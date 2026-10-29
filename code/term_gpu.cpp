// term: the window's GPU side. Direct3D 11 draws a DrawList (term_draw.h): every quad is one instance of one
// shader (rounded rectangles with borders and soft edges by signed distance, glyphs from the font atlas, images),
// batch by batch with its clip as the scissor; terminal cells are drawn by term_cells.cpp in their batch's place.
// The swap chain goes through DirectComposition with premultiplied alpha (see-through backgrounds), flip-discard,
// one frame of latency; headless runs draw into a texture instead and read it back.

struct gpu_quad_consts
{
    float TargetW, TargetH;
    u32 First;
    u32 Pad;
};

struct gpu_image { ID3D11Texture2D* Texture; ID3D11ShaderResourceView* View; };

struct Gpu
{
    HWND Window;
    ID3D11Device* Device;
    ID3D11DeviceContext* Context;
    IDXGISwapChain2* SwapChain;
    HANDLE Latency;
    IDCompositionDevice* Comp;
    IDCompositionTarget* CompTarget;
    IDCompositionVisual* CompVisual;   // the swap chain's, on top
    IDCompositionVisual* Root;         // under it: web pages (term_web.cpp)
    ID3D11Texture2D* Offscreen;   // headless: the frame is drawn here
    ID3D11RenderTargetView* Target;
    s32 W, H;

    ID3D11VertexShader* Vs;
    ID3D11PixelShader* Ps;
    ID3D11Buffer* Consts;
    ID3D11Buffer* Quads;
    ID3D11ShaderResourceView* QuadsView;
    u32 QuadCapacity;
    ID3D11Texture2D* Atlas;
    ID3D11ShaderResourceView* AtlasView;
    u32 AtlasVersion;
    ID3D11SamplerState* Linear;
    ID3D11BlendState* Blend;
    ID3D11RasterizerState* Raster;
    std::map<u64, gpu_image> Images;
    u64 NextImage;

    // GPU timings: timestamp queries a few frames deep, read back when ready (the debug overlay)
    struct { ID3D11Query *Disjoint, *Begin, *End, *CellsBegin[16], *CellsEnd[16]; s32 Cells; bool32 Issued; } Timings[4];
    u32 TimingFrame;
    f32 GpuMs, CellsMs;

    void (*External)(void* User, const void* Data, Rect Clip, s32 TargetW, s32 TargetH);
    void* ExternalUser;
};

static const char GpuShader[] = R"(
struct Quad
{
    float4 Rect; float4 Uv; uint Color; uint Border; float4 Radius;
    float BorderW; float Dash; float Gap; float Soft; uint Kind;
};
cbuffer Consts : register(b0) { float2 Target; uint First; uint Pad; };
StructuredBuffer<Quad> Quads : register(t0);
Texture2D<float> Atlas : register(t1);
Texture2D<float4> Image : register(t2);
SamplerState Linear : register(s0);

struct V { float4 Pos : SV_Position; float2 P : PIXEL; float2 Uv : UV; nointerpolation uint I : INDEX; };

float4 Unpack(uint C) { return float4((C >> 16) & 255, (C >> 8) & 255, C & 255, C >> 24) / 255.0; }

V VsMain(uint Vid : SV_VertexID, uint Iid : SV_InstanceID)
{
    Quad Q = Quads[First + Iid];
    float Grow = Q.Kind == 0 ? Q.Soft + 1 : 0;   // soft edges reach past the rect
    float2 C = float2((0x32 >> Vid) & 1, (0x2C >> Vid) & 1);   // two triangles: (0,0)(1,0)(0,1) (0,1)(1,0)(1,1)
    float2 P = lerp(Q.Rect.xy - Grow, Q.Rect.zw + Grow, C);
    V O;
    O.Pos = float4(P / Target * float2(2, -2) + float2(-1, 1), 0, 1);
    O.P = P;
    O.Uv = lerp(Q.Uv.xy, Q.Uv.zw, C);
    O.I = First + Iid;
    return O;
}

// Signed distance to a rounded box (half size H, corner radius per quadrant), from its centre.
float Box(float2 P, float2 H, float4 R)
{
    float Rad = P.x < 0 ? (P.y < 0 ? R.x : R.w) : (P.y < 0 ? R.y : R.z);
    float2 Q = abs(P) - H + Rad;
    return length(max(Q, 0)) + min(max(Q.x, Q.y), 0) - Rad;
}

float4 PsMain(V In) : SV_Target
{
    Quad Q = Quads[In.I];
    float4 Fill = Unpack(Q.Color);
    if(Q.Kind == 1)
    {
        float A = Atlas.Sample(Linear, In.Uv) * Fill.a;
        return float4(Fill.rgb * A, A);
    }
    if(Q.Kind == 2)
    {
        float4 I = Image.Sample(Linear, In.Uv);   // premultiplied
        return I * Fill.a;
    }
    float2 Half = (Q.Rect.zw - Q.Rect.xy) * 0.5;
    float2 P = In.P - (Q.Rect.xy + Half);
    float D = Box(P, Half, min(Q.Radius, min(Half.x, Half.y)));
    float Inside = Q.Soft > 0 ? 1 - smoothstep(-Q.Soft, Q.Soft, D) : saturate(0.5 - D);
    float4 Out = float4(Fill.rgb * Fill.a, Fill.a) * Inside;
    if(Q.BorderW > 0)
    {
        float Ring = saturate(0.5 - D) - saturate(0.5 - (D + Q.BorderW));
        if(Q.Dash > 0)
        {
            // dashes along the nearer edge: x along the top and bottom, y along the sides
            float2 E = abs(P) - Half;
            float Along = E.y > E.x ? P.x + Half.x : P.y + Half.y;
            Ring *= fmod(Along, Q.Dash + Q.Gap) < Q.Dash ? 1 : 0;
        }
        float4 B = Unpack(Q.Border);
        float A = B.a * Ring;
        Out = float4(B.rgb * A, A) + Out * (1 - A);
    }
    return Out;
}
)";

static void GpuLog(const char* What, HRESULT Hr)
{
    Logf("gpu", "%s failed (0x%08X)", What, (u32)Hr);
}

static void GpuTargets(Gpu* G, s32 W, s32 H)
{
    ReleaseCom(G->Target);
    G->W = Max(1, W);
    G->H = Max(1, H);
    ID3D11Texture2D* Back = 0;
    if(G->SwapChain)
    {
        G->SwapChain->ResizeBuffers(0, (UINT)G->W, (UINT)G->H, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        G->SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&Back);
    }
    else
    {
        ReleaseCom(G->Offscreen);
        D3D11_TEXTURE2D_DESC T = {};
        T.Width = (UINT)G->W;
        T.Height = (UINT)G->H;
        T.MipLevels = 1;
        T.ArraySize = 1;
        T.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        T.SampleDesc.Count = 1;
        T.BindFlags = D3D11_BIND_RENDER_TARGET;
        G->Device->CreateTexture2D(&T, 0, &G->Offscreen);
        Back = G->Offscreen;
        if(Back) { Back->AddRef(); }
    }
    if(Back) { G->Device->CreateRenderTargetView(Back, 0, &G->Target); Back->Release(); }
}

// hwnd 0: headless (a texture, read back by Gpu_ReadPixels).
extern "C" Gpu* Gpu_Create(void* Hwnd, s32 W, s32 H)
{
    Gpu* G = new Gpu();
    HWND Window = (HWND)Hwnd;
    G->Window = Window;
    UINT Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL Levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT Hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, Flags, Levels, ArrayCount(Levels), D3D11_SDK_VERSION, &G->Device, 0, &G->Context);
    if(FAILED(Hr)) { Hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0, Flags, Levels, ArrayCount(Levels), D3D11_SDK_VERSION, &G->Device, 0, &G->Context); }
    if(FAILED(Hr)) { GpuLog("D3D11CreateDevice", Hr); delete G; return 0; }

    if(Window)
    {
        // --- a composition swap chain (premultiplied alpha: see-through backgrounds), shown by DirectComposition ---
        IDXGIDevice1* Dxgi = 0;
        IDXGIAdapter* Adapter = 0;
        IDXGIFactory2* Factory = 0;
        G->Device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&Dxgi);
        Dxgi->SetMaximumFrameLatency(1);
        Dxgi->GetAdapter(&Adapter);
        Adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&Factory);
        DXGI_SWAP_CHAIN_DESC1 Desc = {};
        Desc.Width = (UINT)Max(1, W);
        Desc.Height = (UINT)Max(1, H);
        Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        Desc.SampleDesc.Count = 1;
        Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        Desc.BufferCount = 2;
        Desc.Scaling = DXGI_SCALING_STRETCH;
        Desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        Desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        IDXGISwapChain1* Chain = 0;
        Hr = Factory->CreateSwapChainForComposition(G->Device, &Desc, 0, &Chain);
        if(Chain) { Chain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&G->SwapChain); Chain->Release(); }
        if(G->SwapChain)
        {
            G->SwapChain->SetMaximumFrameLatency(1);
            G->Latency = G->SwapChain->GetFrameLatencyWaitableObject();
        }
        if(SUCCEEDED(Hr)) { Hr = DCompositionCreateDevice(Dxgi, __uuidof(IDCompositionDevice), (void**)&G->Comp); }
        if(SUCCEEDED(Hr)) { Hr = G->Comp->CreateTargetForHwnd(Window, TRUE, &G->CompTarget); }
        if(SUCCEEDED(Hr)) { Hr = G->Comp->CreateVisual(&G->CompVisual); }
        if(SUCCEEDED(Hr)) { Hr = G->CompVisual->SetContent(G->SwapChain); }
        if(SUCCEEDED(Hr)) { Hr = G->Comp->CreateVisual(&G->Root); }
        if(SUCCEEDED(Hr)) { Hr = G->Root->AddVisual(G->CompVisual, FALSE, 0); }
        if(SUCCEEDED(Hr)) { Hr = G->CompTarget->SetRoot(G->Root); }
        if(SUCCEEDED(Hr)) { Hr = G->Comp->Commit(); }
        ReleaseCom(Factory);
        ReleaseCom(Adapter);
        ReleaseCom(Dxgi);
        if(FAILED(Hr) || !G->SwapChain) { GpuLog("swap chain", Hr); return G; }
    }
    GpuTargets(G, W, H);

    // --- the shader ---
    ID3DBlob* Code = 0;
    ID3DBlob* Errors = 0;
    Hr = D3DCompile(GpuShader, sizeof(GpuShader) - 1, "moss", 0, 0, "VsMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
    if(FAILED(Hr)) { Logf("gpu", "shader: %s", Errors ? (const char*)Errors->GetBufferPointer() : "?"); return G; }
    G->Device->CreateVertexShader(Code->GetBufferPointer(), Code->GetBufferSize(), 0, &G->Vs);
    ReleaseCom(Code);
    Hr = D3DCompile(GpuShader, sizeof(GpuShader) - 1, "moss", 0, 0, "PsMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
    if(FAILED(Hr)) { Logf("gpu", "shader: %s", Errors ? (const char*)Errors->GetBufferPointer() : "?"); return G; }
    G->Device->CreatePixelShader(Code->GetBufferPointer(), Code->GetBufferSize(), 0, &G->Ps);
    ReleaseCom(Code);

    D3D11_BUFFER_DESC C = {};
    C.ByteWidth = sizeof(gpu_quad_consts);
    C.Usage = D3D11_USAGE_DYNAMIC;
    C.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    C.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    G->Device->CreateBuffer(&C, 0, &G->Consts);
    D3D11_SAMPLER_DESC S = {};
    S.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    S.AddressU = S.AddressV = S.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    S.MaxLOD = D3D11_FLOAT32_MAX;
    G->Device->CreateSamplerState(&S, &G->Linear);
    D3D11_BLEND_DESC B = {};
    B.RenderTarget[0].BlendEnable = TRUE;
    B.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    B.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    B.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    B.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    B.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    B.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    B.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    G->Device->CreateBlendState(&B, &G->Blend);
    D3D11_RASTERIZER_DESC Ra = {};
    Ra.FillMode = D3D11_FILL_SOLID;
    Ra.CullMode = D3D11_CULL_NONE;
    Ra.ScissorEnable = TRUE;
    G->Device->CreateRasterizerState(&Ra, &G->Raster);
    return G;
}

extern "C" void Gpu_Destroy(Gpu* G)
{
    if(!G) { return; }
    for(auto& I : G->Images) { ReleaseCom(I.second.View); ReleaseCom(I.second.Texture); }
    for(auto& T : G->Timings)
    {
        ReleaseCom(T.Disjoint); ReleaseCom(T.Begin); ReleaseCom(T.End);
        for(s32 K = 0; K < 16; ++K) { ReleaseCom(T.CellsBegin[K]); ReleaseCom(T.CellsEnd[K]); }
    }
    ReleaseCom(G->Raster);
    ReleaseCom(G->Blend);
    ReleaseCom(G->Linear);
    ReleaseCom(G->AtlasView);
    ReleaseCom(G->Atlas);
    ReleaseCom(G->QuadsView);
    ReleaseCom(G->Quads);
    ReleaseCom(G->Consts);
    ReleaseCom(G->Ps);
    ReleaseCom(G->Vs);
    ReleaseCom(G->Target);
    ReleaseCom(G->Offscreen);
    ReleaseCom(G->CompVisual);
    ReleaseCom(G->Root);
    ReleaseCom(G->CompTarget);
    ReleaseCom(G->Comp);
    ReleaseCom(G->SwapChain);
    ReleaseCom(G->Context);
    ReleaseCom(G->Device);
    delete G;
}

extern "C" void* Gpu_Device(Gpu* G) { return G->Device; }
extern "C" void* Gpu_Context(Gpu* G) { return G->Context; }
extern "C" void* Gpu_LatencyHandle(Gpu* G) { return G ? G->Latency : 0; }

extern "C" void Gpu_SetExternalDraw(Gpu* G, void (*Fn)(void* User, const void* Data, Rect Clip, s32 TargetW, s32 TargetH), void* User)
{
    G->External = Fn;
    G->ExternalUser = User;
}

// An image's texture: premultiplied BGRA, w x h, top row first, mipmapped (any size samples smoothly).
extern "C" u64 Gpu_CreateImage(Gpu* G, const u32* Pixels, s32 W, s32 H)
{
    D3D11_TEXTURE2D_DESC T = {};
    T.Width = (UINT)W;
    T.Height = (UINT)H;
    T.MipLevels = 0;
    T.ArraySize = 1;
    T.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    T.SampleDesc.Count = 1;
    T.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    T.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    gpu_image I = {};
    if(FAILED(G->Device->CreateTexture2D(&T, 0, &I.Texture))) { return 0; }
    G->Device->CreateShaderResourceView(I.Texture, 0, &I.View);
    G->Context->UpdateSubresource(I.Texture, 0, 0, Pixels, (UINT)W * 4, 0);
    G->Context->GenerateMips(I.View);
    u64 Id = ++G->NextImage;
    G->Images[Id] = I;
    return Id;
}

extern "C" void Gpu_FreeImage(Gpu* G, u64 Id)
{
    auto It = G->Images.find(Id);
    if(It == G->Images.end()) { return; }
    ReleaseCom(It->second.View);
    ReleaseCom(It->second.Texture);
    G->Images.erase(It);
}

extern "C" void Gpu_Render(Gpu* G, const DrawList* Dl, const FontAtlas* Atlas, u32 Clear, s32 W, s32 H)
{
    if(!G || !G->Vs) { return; }
    if(W != G->W || H != G->H || !G->Target) { GpuTargets(G, W, H); }
    if(!G->Target) { return; }
    ID3D11DeviceContext* X = G->Context;

    // --- the font atlas, when glyphs were added ---
    if(Atlas && Atlas->version != G->AtlasVersion)
    {
        if(!G->Atlas)
        {
            D3D11_TEXTURE2D_DESC T = {};
            T.Width = (UINT)Atlas->w;
            T.Height = (UINT)Atlas->h;
            T.MipLevels = 1;
            T.ArraySize = 1;
            T.Format = DXGI_FORMAT_R8_UNORM;
            T.SampleDesc.Count = 1;
            T.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            G->Device->CreateTexture2D(&T, 0, &G->Atlas);
            if(G->Atlas) { G->Device->CreateShaderResourceView(G->Atlas, 0, &G->AtlasView); }
        }
        if(G->Atlas) { X->UpdateSubresource(G->Atlas, 0, 0, Atlas->pixels, (UINT)Atlas->w, 0); }
        G->AtlasVersion = Atlas->version;
    }

    // --- the quads, all at once ---
    u32 Count = (u32)Dl->quads.count;
    if(Count > G->QuadCapacity)
    {
        ReleaseCom(G->QuadsView);
        ReleaseCom(G->Quads);
        G->QuadCapacity = Count + Count / 2 + 256;
        D3D11_BUFFER_DESC B = {};
        B.ByteWidth = G->QuadCapacity * sizeof(Quad);
        B.Usage = D3D11_USAGE_DYNAMIC;
        B.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        B.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        B.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        B.StructureByteStride = sizeof(Quad);
        G->Device->CreateBuffer(&B, 0, &G->Quads);
        D3D11_SHADER_RESOURCE_VIEW_DESC V = {};
        V.Format = DXGI_FORMAT_UNKNOWN;
        V.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        V.Buffer.NumElements = G->QuadCapacity;
        G->Device->CreateShaderResourceView(G->Quads, &V, &G->QuadsView);
    }
    D3D11_MAPPED_SUBRESOURCE M = {};
    if(Count && SUCCEEDED(X->Map(G->Quads, 0, D3D11_MAP_WRITE_DISCARD, 0, &M)))
    {
        memcpy(M.pData, Dl->quads.data, Count * sizeof(Quad));
        X->Unmap(G->Quads, 0);
    }

    // --- GPU timings: this slot's queries from 4 frames ago read back if the GPU is done with them (else this frame
    //     goes untimed), then issued again around this frame and each terminal's cells ---
    auto& T = G->Timings[G->TimingFrame++ % 4];
    b32 Timed = 1;
    if(T.Issued)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT D = {};
        UINT64 B0 = 0, E0 = 0;
        if(X->GetData(T.Disjoint, &D, sizeof(D), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && X->GetData(T.Begin, &B0, sizeof(B0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
           X->GetData(T.End, &E0, sizeof(E0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
        {
            if(!D.Disjoint && D.Frequency)
            {
                UINT64 Cells = 0;
                for(s32 K = 0; K < T.Cells; ++K)
                {
                    UINT64 CB = 0, CE = 0;
                    X->GetData(T.CellsBegin[K], &CB, sizeof(CB), 0);
                    X->GetData(T.CellsEnd[K], &CE, sizeof(CE), 0);
                    Cells += CE - CB;
                }
                G->GpuMs = (f32)((f64)(E0 - B0) * 1000.0 / (f64)D.Frequency);
                G->CellsMs = (f32)((f64)Cells * 1000.0 / (f64)D.Frequency);
            }
            T.Issued = 0;
        }
        else
        {
            Timed = 0;
        }
    }
    if(Timed && !T.Disjoint)
    {
        D3D11_QUERY_DESC Q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        G->Device->CreateQuery(&Q, &T.Disjoint);
        Q.Query = D3D11_QUERY_TIMESTAMP;
        G->Device->CreateQuery(&Q, &T.Begin);
        G->Device->CreateQuery(&Q, &T.End);
        for(s32 K = 0; K < 16; ++K) { G->Device->CreateQuery(&Q, &T.CellsBegin[K]); G->Device->CreateQuery(&Q, &T.CellsEnd[K]); }
    }
    Timed = Timed && T.Disjoint && T.Begin && T.End;
    T.Cells = 0;
    if(Timed) { X->Begin(T.Disjoint); X->End(T.Begin); }

    float C[4] = {(f32)((Clear >> 16) & 255) / 255.0f, (f32)((Clear >> 8) & 255) / 255.0f, (f32)(Clear & 255) / 255.0f, (f32)(Clear >> 24) / 255.0f};
    for(s32 K = 0; K < 3; ++K) { C[K] *= C[3]; }   // premultiplied
    X->ClearRenderTargetView(G->Target, C);
    D3D11_VIEWPORT View = {0, 0, (f32)G->W, (f32)G->H, 0, 1};

    // --- batch by batch: quads with their texture, or the external drawer ---
    for(s64 I = 0; I < Dl->batches.count; ++I)
    {
        const DrawBatch* B = &Dl->batches.data[I];
        D3D11_RECT Sc = {B->clip.x0, B->clip.y0, B->clip.x1, B->clip.y1};
        if(Sc.right <= Sc.left || Sc.bottom <= Sc.top) { continue; }
        X->OMSetRenderTargets(1, &G->Target, 0);
        if(B->kind == Batch_External)
        {
            b32 Time = Timed && T.Cells < 16 && T.CellsBegin[T.Cells] && T.CellsEnd[T.Cells];
            if(Time) { X->End(T.CellsBegin[T.Cells]); }
            if(G->External) { G->External(G->ExternalUser, B->external, B->clip, G->W, G->H); }
            if(Time) { X->End(T.CellsEnd[T.Cells]); T.Cells++; }
            continue;
        }
        if(!B->count) { continue; }
        gpu_quad_consts Q = {(f32)G->W, (f32)G->H, (u32)B->first, 0};
        if(SUCCEEDED(X->Map(G->Consts, 0, D3D11_MAP_WRITE_DISCARD, 0, &M))) { memcpy(M.pData, &Q, sizeof(Q)); X->Unmap(G->Consts, 0); }
        X->RSSetViewports(1, &View);
        X->RSSetState(G->Raster);
        X->RSSetScissorRects(1, &Sc);
        float Factor[4] = {};
        X->OMSetBlendState(G->Blend, Factor, 0xffffffff);
        X->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        X->IASetInputLayout(0);
        X->VSSetShader(G->Vs, 0, 0);
        X->PSSetShader(G->Ps, 0, 0);
        X->VSSetConstantBuffers(0, 1, &G->Consts);
        X->PSSetConstantBuffers(0, 1, &G->Consts);
        ID3D11ShaderResourceView* ImageView = 0;
        if(B->texture)
        {
            auto It = G->Images.find(B->texture);
            if(It != G->Images.end()) { ImageView = It->second.View; }
        }
        ID3D11ShaderResourceView* Views[3] = {G->QuadsView, G->AtlasView, ImageView};
        X->VSSetShaderResources(0, 1, Views);
        X->PSSetShaderResources(0, 3, Views);
        X->PSSetSamplers(0, 1, &G->Linear);
        X->DrawInstanced(6, (UINT)B->count, 0, 0);
    }
    ID3D11ShaderResourceView* None[3] = {};
    X->VSSetShaderResources(0, 1, None);
    X->PSSetShaderResources(0, 3, None);
    if(Timed) { X->End(T.End); X->End(T.Disjoint); T.Issued = 1; }
}

extern "C" void Gpu_Present(Gpu* G)
{
    if(G && G->SwapChain) { G->SwapChain->Present(0, 0); }
}

// Why the device is gone (a driver installed or restarted, a GPU reset), else 0: nothing drawn shows any more.
extern "C" u32 Gpu_LostReason(Gpu* G)
{
    return G && G->Device ? (u32)G->Device->GetDeviceRemovedReason() : 0;
}

// The last frame (headless), RGBA, w x h, in arena memory.
extern "C" u8* Gpu_ReadPixels(Gpu* G, Arena* A, s32* W, s32* H)
{
    if(!G || !G->Offscreen) { return 0; }
    D3D11_TEXTURE2D_DESC T = {};
    G->Offscreen->GetDesc(&T);
    T.BindFlags = 0;
    T.Usage = D3D11_USAGE_STAGING;
    T.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* Staging = 0;
    if(FAILED(G->Device->CreateTexture2D(&T, 0, &Staging))) { return 0; }
    G->Context->CopyResource(Staging, G->Offscreen);
    D3D11_MAPPED_SUBRESOURCE M = {};
    u8* Out = 0;
    if(SUCCEEDED(G->Context->Map(Staging, 0, D3D11_MAP_READ, 0, &M)))
    {
        Out = (u8*)Arena_Push(A, (u64)T.Width * T.Height * 4, 16);
        for(u32 Y = 0; Y < T.Height; ++Y)
        {
            const u8* Row = (const u8*)M.pData + (size_t)Y * M.RowPitch;
            u8* To = Out + (size_t)Y * T.Width * 4;
            for(u32 X = 0; X < T.Width; ++X)
            {
                To[X * 4 + 0] = Row[X * 4 + 2];
                To[X * 4 + 1] = Row[X * 4 + 1];
                To[X * 4 + 2] = Row[X * 4 + 0];
                To[X * 4 + 3] = 255;
            }
        }
        G->Context->Unmap(Staging, 0);
        *W = (s32)T.Width;
        *H = (s32)T.Height;
    }
    Staging->Release();
    return Out;
}

// The GPU time of a recent frame: all of it, and the terminals' cells in it (ms; 0 until the first reading).
extern "C" void Gpu_Times(Gpu* G, f32* Total, f32* Cells)
{
    *Total = G ? G->GpuMs : 0;
    *Cells = G ? G->CellsMs : 0;
}
