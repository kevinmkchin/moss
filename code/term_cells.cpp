// term: terminal cells on the GPU. Each shown tab's grid is resolved into GPU cells (colors from the
// pane's palette, glyph tiles from an atlas drawn on demand with Direct2D/DirectWrite: font fallback,
// color emoji, clusters across cells) and drawn by one pixel shader over its pane, as an external draw
// inside the D3D11 backend's frame. Adapted from clod's clodw renderer.

#define ReleaseCom(P) do { if(P) { (P)->Release(); (P) = 0; } } while(0)

// One cell as the shader reads it. Colors are packed 0x00BBGGRR.
struct gpu_cell
{
    u32 Tile;  // atlas tile: x | y << 16; tile 0 is blank
    u32 Fg;
    u32 Bg;
    u32 Flags; // GPU_*
};

enum
{
    GPU_UNDERLINE   = 1 << 0,
    GPU_STRIKE      = 1 << 1,
    GPU_COLOR       = 1 << 2, // the tile holds its own colors (emoji): drawn as is, not tinted with Fg
    GPU_SEE_THROUGH = 1 << 5, // the background is the terminal's default: drawn at the pane's opacity
    GPU_WIDE        = 1 << 7, // first half of a double-width cell (the cursor covers both)
};

// Matches cbuffer Consts in the shader.
struct alignas(16) gpu_consts
{
    s32 Origin[2];     // the grid's top-left in target pixels
    u32 CellW;
    u32 CellH;
    u32 GridW;
    u32 GridH;
    u32 MarginColor;   // the default background, outside the grid
    u32 UnderlineY;
    u32 StrikeY;
    u32 LineH;
    u32 CursorThick;
    u32 Opacity;       // 0..255
    s32 Sel[4];        // selection start x, y, end x, y (reading order, inclusive); SelOn 0 = none
    u32 SelOn;
    u32 SelColor;
    s32 Cursor[2];
    u32 CursorShape;   // 0 none, 1 block, 2 underline, 3 bar, 4 hollow block
    u32 CursorColor;
    u32 SelBlock;      // the selection is a rectangle (Alt+drag), not reading order
    u32 Pad;
};

#define ATLAS_SIZE 2048
#define GLYPH_MAX_CELLS 16 // widest glyph run: the Direct2D scratch surface is this many cells wide
#define GLYPH_MAX_CPS 16   // a ZWJ family is up to 7; flags with tags run longer

enum
{
    STYLE_BOLD   = 1,
    STYLE_ITALIC = 2,
};

struct glyph_entry
{
    u32 Cps[GLYPH_MAX_CPS];
    u32 Count;   // 0 = empty slot
    u32 Style;
    u32 Cells;   // tiles wide (a cluster spans every cell ConPTY gave its parts)
    u32 Tile;    // first tile: x | y << 16
    bool32 Color;
    bool32 Overflow; // wider than its cells, so drawn shrunk
};

// A tab's cells as last resolved, on the GPU.
struct gpu_grid
{
    u32 TabId;
    u64 Version;           // the TermGrid version resolved
    u64 PaletteHash;
    u64 FontGeneration;
    s32 Cols;
    s32 Rows;
    ID3D11Buffer* Cells;
    ID3D11ShaderResourceView* CellsView;
    u32 Capacity;
    u64 UsedFrame;
};

struct TermCells
{
    ID3D11Device* Device;
    ID3D11DeviceContext* Context;
    TermClient* Client;
    ID3D11VertexShader* VertexShader;
    ID3D11PixelShader* PixelShader;
    ID3D11Buffer* Consts;
    ID3D11Texture2D* Atlas;
    ID3D11ShaderResourceView* AtlasView;
    ID3D11RasterizerState* Rasterizer;
    ID3D11BlendState* Blend;   // premultiplied over what's drawn below (a background image shows through default backgrounds)

    // --- glyphs: Direct2D renders into Scratch, whose tiles are copied into the atlas ---
    ID2D1Factory* D2D;
    IDWriteFactory2* DWrite;
    ID3D11Texture2D* Scratch;
    ID2D1RenderTarget* Draw2D;
    ID2D1SolidColorBrush* White;
    IDWriteTextFormat* Formats[4]; // by STYLE_* bits
    IDWriteTextFormat* Emoji;      // color clusters: the emoji font first, so FE0F gets its color form
    std::wstring Family;
    f32 SizePx;
    s32 CellW;
    s32 CellH;
    s32 Baseline;
    s32 UnderlineY;
    s32 StrikeY;
    s32 LineH;
    u64 FontGeneration;

    std::vector<glyph_entry> Table; // open addressing, power of two
    u32 Used;
    u32 TilesX;
    u32 TilesY;
    u32 NextX;
    u32 NextY;

    std::vector<gpu_grid*> Grids;
    std::vector<gpu_cell> Scratchpad;
    u64 Frame;
    s32 ResolvedThisFrame;
};

static const char CellsShader[] = R"(
struct Cell { uint Tile; uint Fg; uint Bg; uint Flags; };
cbuffer Consts : register(b0)
{
    int2 Origin; uint CellW; uint CellH;
    uint GridW; uint GridH; uint MarginColor; uint UnderlineY;
    uint StrikeY; uint LineH; uint CursorThick; uint Opacity;
    int4 Sel;
    uint SelOn; uint SelColor; int2 Cursor;
    uint CursorShape; uint CursorColor; uint SelBlock; uint Pad2;
};
StructuredBuffer<Cell> Cells : register(t0);
Texture2D<float4> Atlas : register(t1);

float3 Unpack(uint C) { return float3(C & 255, (C >> 8) & 255, (C >> 16) & 255) / 255.0; }

// One triangle covering the target; the scissor keeps it to the pane.
float4 VsMain(uint Id : SV_VertexID) : SV_Position
{
    float2 P = float2((Id << 1) & 2, Id & 2);
    return float4(P.x * 2 - 1, 1 - P.y * 2, 0, 1);
}

float4 PsMain(float4 Pos : SV_Position) : SV_Target
{
    int2 Pi = int2(Pos.xy) - Origin;
    float See = Opacity / 255.0;
    if(Pi.x < 0 || Pi.y < 0) { return float4(Unpack(MarginColor) * See, See); }
    uint2 P = uint2(Pi);
    uint2 C = uint2(P.x / CellW, P.y / CellH);
    if(C.x >= GridW || C.y >= GridH) { return float4(Unpack(MarginColor) * See, See); }
    uint2 In = P - C * uint2(CellW, CellH);
    Cell K = Cells[C.y * GridW + C.x];
    float3 Fg = Unpack(K.Fg);
    float3 Bg = Unpack(K.Bg);
    float BgA = (K.Flags & 32) ? See : 1;

    // --- selection: reading order from Sel.xy to Sel.zw ---
    int2 Ci = int2(C);
    bool Selected = SelOn != 0 && (SelBlock != 0 ? (Ci.y >= Sel.y && Ci.y <= Sel.w && Ci.x >= min(Sel.x, Sel.z) && Ci.x <= max(Sel.x, Sel.z)) :
                    (Ci.y > Sel.y || (Ci.y == Sel.y && Ci.x >= Sel.x)) && (Ci.y < Sel.w || (Ci.y == Sel.w && Ci.x <= Sel.z)));
    if(Selected) { Bg = Unpack(SelColor); BgA = 1; }

    // --- a block cursor swaps the cell's colors (both halves of a wide one) ---
    bool OnCursor = CursorShape != 0 && Ci.y == Cursor.y && (Ci.x == Cursor.x || (Ci.x == Cursor.x + 1 && (Cells[C.y * GridW + Cursor.x].Flags & 128)));
    if(OnCursor && CursorShape == 1) { Bg = Unpack(CursorColor); BgA = 1; Fg = Unpack(MarginColor); }

    uint2 Tile = uint2(K.Tile & 0xFFFF, K.Tile >> 16);
    float4 T = K.Tile ? Atlas.Load(int3(Tile * uint2(CellW, CellH) + In, 0)) : float4(0, 0, 0, 0); // premultiplied; tile 0 = blank
    float3 Ink = (K.Flags & 4) ? T.rgb : Fg * T.a;
    float3 Out = Ink + Bg * BgA * (1 - T.a);
    float OutA = T.a + BgA * (1 - T.a);
    bool Under = (K.Flags & 1) && In.y >= UnderlineY && In.y < UnderlineY + LineH;
    bool Strike = (K.Flags & 2) && In.y >= StrikeY && In.y < StrikeY + LineH;
    if(Under || Strike) { Out = Fg; OutA = 1; }
    if(OnCursor && CursorShape != 1)
    {
        uint W = (Cells[C.y * GridW + Cursor.x].Flags & 128) ? 2 : 1;
        uint X = In.x + (Ci.x - Cursor.x) * CellW;
        bool Edge = CursorShape == 2 ? In.y + CursorThick >= CellH :
                    CursorShape == 3 ? (Ci.x == Cursor.x && In.x < CursorThick) :
                    (In.y < 1 || In.y + 1 >= CellH || X < 1 || X + 1 >= CellW * W);
        if(Edge) { Out = Unpack(CursorColor); OutA = 1; }
    }
    return float4(Out, OutA);
}
)";

static u32 PackHex(u32 Hex) { return ((Hex >> 16) & 255) | (Hex & 0xFF00) | ((Hex & 255) << 16); }
static u32 PackRgb(u32 R, u32 G, u32 B) { return R | (G << 8) | (B << 16); }

// A cell color (term_base.h tags) as packed RGB in palette P; untagged = DefaultHex.
static u32 ResolveColor(u32 C, u32 DefaultHex, const TermPalette& P)
{
    if(COLOR_TAG(C) == COLOR_RGB) { return PackHex(C & 0xFFFFFF); }
    if(COLOR_TAG(C) != COLOR_PALETTE) { return PackHex(DefaultHex); }
    u32 I = C & 255;
    if(I < 16) { return PackHex(P.ansi[I]); }
    if(I < 232)
    {
        static const u32 Level[6] = {0, 95, 135, 175, 215, 255};
        u32 N = I - 16;
        return PackRgb(Level[N / 36], Level[(N / 6) % 6], Level[N % 6]);
    }
    u32 Gray = 8 + 10 * (I - 232);
    return PackRgb(Gray, Gray, Gray);
}

static void GlyphCacheReset(TermCells* T)
{
    T->Table.assign(T->Table.size(), glyph_entry{});
    T->Used = 0;
    T->NextX = 1; // tile 0 stays blank
    T->NextY = 0;
}

extern "C" TermCells* TermCells_Create(void* Device, void* Context, TermClient* Client)
{
    TermCells* T = new TermCells();
    T->Device = (ID3D11Device*)Device;
    T->Context = (ID3D11DeviceContext*)Context;
    T->Client = Client;
    HRESULT Hr = S_OK;

    // --- shaders, compiled from the embedded source ---
    ID3DBlob* Code = 0;
    ID3DBlob* Errors = 0;
    Hr = D3DCompile(CellsShader, sizeof(CellsShader) - 1, "cells", 0, 0, "VsMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
    if(FAILED(Hr)) { Logf("cells", "shader: %s", Errors ? (const char*)Errors->GetBufferPointer() : "?"); ReleaseCom(Errors); return T; }
    T->Device->CreateVertexShader(Code->GetBufferPointer(), Code->GetBufferSize(), 0, &T->VertexShader);
    ReleaseCom(Code);
    Hr = D3DCompile(CellsShader, sizeof(CellsShader) - 1, "cells", 0, 0, "PsMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
    if(FAILED(Hr)) { Logf("cells", "shader: %s", Errors ? (const char*)Errors->GetBufferPointer() : "?"); ReleaseCom(Errors); return T; }
    T->Device->CreatePixelShader(Code->GetBufferPointer(), Code->GetBufferSize(), 0, &T->PixelShader);
    ReleaseCom(Code);

    D3D11_BUFFER_DESC CDesc = {};
    CDesc.ByteWidth = sizeof(gpu_consts);
    CDesc.Usage = D3D11_USAGE_DYNAMIC;
    CDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    T->Device->CreateBuffer(&CDesc, 0, &T->Consts);
    D3D11_RASTERIZER_DESC Rd = {};
    Rd.FillMode = D3D11_FILL_SOLID;
    Rd.CullMode = D3D11_CULL_NONE;
    Rd.ScissorEnable = TRUE;
    T->Device->CreateRasterizerState(&Rd, &T->Rasterizer);
    D3D11_BLEND_DESC Bd = {};
    Bd.RenderTarget[0].BlendEnable = TRUE;
    Bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    Bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    Bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    Bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    Bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    Bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    Bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    T->Device->CreateBlendState(&Bd, &T->Blend);

    // --- glyph atlas: premultiplied BGRA tiles, filled by copies from the Direct2D scratch ---
    D3D11_TEXTURE2D_DESC TDesc = {};
    TDesc.Width = ATLAS_SIZE;
    TDesc.Height = ATLAS_SIZE;
    TDesc.MipLevels = 1;
    TDesc.ArraySize = 1;
    TDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    TDesc.SampleDesc.Count = 1;
    TDesc.Usage = D3D11_USAGE_DEFAULT;
    TDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Hr = T->Device->CreateTexture2D(&TDesc, 0, &T->Atlas);
    if(FAILED(Hr)) { LogHr("cells", "atlas texture", Hr); return T; }
    T->Device->CreateShaderResourceView(T->Atlas, 0, &T->AtlasView);

    // --- Direct2D / DirectWrite ---
    Hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), 0, (void**)&T->D2D);
    if(FAILED(Hr)) { LogHr("cells", "D2D1CreateFactory", Hr); }
    Hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown**)&T->DWrite);
    if(FAILED(Hr)) { LogHr("cells", "DWriteCreateFactory", Hr); }
    T->Table.resize(4096);
    return T;
}

// Picks the font and derives the cell size from its metrics at SizePx. Name is a face name as Windows'
// font dialog gives it (e.g. "Cascadia Mono") or a DirectWrite family; unknown names fall back to
// Consolas. The atlas is laid out in cells, so the cache starts over.
extern "C" void TermCells_SetFont(TermCells* T, String FamilyUtf8, f32 SizePx)
{
    std::wstring FaceName = Utf8ToWide(std::string((const char*)FamilyUtf8.data, (size_t)FamilyUtf8.count));
    if(FaceName == T->Family && SizePx == T->SizePx) { return; }
    if(!T->DWrite || !T->D2D) { return; }
    TIMED_BLOCK(SetFont);
    T->Family = FaceName;
    T->SizePx = SizePx;
    for(IDWriteTextFormat*& Format : T->Formats) { ReleaseCom(Format); }
    ReleaseCom(T->Emoji);

    // --- the regular face: through the GDI name first, then as a DirectWrite family ---
    IDWriteFont* Font = 0;
    IDWriteGdiInterop* Gdi = 0;
    T->DWrite->GetGdiInterop(&Gdi);
    LOGFONTW Lf = {};
    wcsncpy_s(Lf.lfFaceName, FaceName.c_str(), _TRUNCATE);
    Lf.lfWeight = FW_NORMAL;
    Lf.lfCharSet = DEFAULT_CHARSET;
    if(FAILED(Gdi->CreateFontFromLOGFONT(&Lf, &Font))) { Font = 0; }
    ReleaseCom(Gdi);
    if(!Font)
    {
        IDWriteFontCollection* System = 0;
        T->DWrite->GetSystemFontCollection(&System, TRUE); // TRUE: sees a font installed moments ago
        UINT32 Index = 0;
        BOOL Exists = FALSE;
        System->FindFamilyName(FaceName.c_str(), &Index, &Exists);
        if(!Exists) { System->FindFamilyName(L"Consolas", &Index, &Exists); }
        IDWriteFontFamily* Family = 0;
        System->GetFontFamily(Index, &Family);
        Family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &Font);
        ReleaseCom(Family);
        ReleaseCom(System);
    }
    DWRITE_FONT_STRETCH Stretch = Font->GetStretch();
    std::wstring Name;
    {
        IDWriteFontFamily* Family = 0;
        IDWriteLocalizedStrings* Names = 0;
        Font->GetFontFamily(&Family);
        Family->GetFamilyNames(&Names);
        UINT32 Index = 0;
        BOOL Exists = FALSE;
        Names->FindLocaleName(L"en-us", &Index, &Exists);
        UINT32 Len = 0;
        Names->GetStringLength(Exists ? Index : 0, &Len);
        Name.resize(Len + 1);
        Names->GetString(Exists ? Index : 0, &Name[0], Len + 1);
        Name.resize(Len);
        ReleaseCom(Names);
        ReleaseCom(Family);
    }

    // --- metrics from the regular face ---
    IDWriteFontFace* Face = 0;
    Font->CreateFontFace(&Face);
    DWRITE_FONT_METRICS M = {};
    Face->GetMetrics(&M);
    UINT32 Zero = '0';
    UINT16 Glyph = 0;
    Face->GetGlyphIndices(&Zero, 1, &Glyph);
    DWRITE_GLYPH_METRICS GM = {};
    Face->GetDesignGlyphMetrics(&Glyph, 1, &GM, FALSE);
    float Scale = SizePx / (float)M.designUnitsPerEm;
    float Ascent = M.ascent * Scale;
    float Descent = M.descent * Scale;
    float Gap = M.lineGap * Scale;
    T->CellW = Max(1, (s32)ceilf(GM.advanceWidth * Scale - 0.05f));
    T->CellH = Max(1, (s32)ceilf(Ascent + Descent + Gap));
    T->Baseline = (s32)roundf(Ascent + Gap / 2);
    T->LineH = Max(1, (s32)roundf(M.underlineThickness * Scale));
    T->UnderlineY = Min(T->CellH - T->LineH, T->Baseline - (s32)roundf(M.underlinePosition * Scale));
    T->StrikeY = T->Baseline - (s32)roundf(M.strikethroughPosition * Scale);
    ReleaseCom(Face);
    ReleaseCom(Font);

    // --- one text format per style, all on the same baseline so fallback fonts line up ---
    for(u32 Style = 0; Style < 4; ++Style)
    {
        T->DWrite->CreateTextFormat(Name.c_str(), 0, (Style & STYLE_BOLD) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                    (Style & STYLE_ITALIC) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                    Stretch, SizePx, L"", &T->Formats[Style]);
        T->Formats[Style]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        T->Formats[Style]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, (float)T->CellH, (float)T->Baseline);
    }
    T->DWrite->CreateTextFormat(L"Segoe UI Emoji", 0, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                SizePx, L"", &T->Emoji);
    T->Emoji->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    T->Emoji->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, (float)T->CellH, (float)T->Baseline);

    // --- the Direct2D scratch surface: wide enough for the longest cluster ---
    ReleaseCom(T->White);
    ReleaseCom(T->Draw2D);
    ReleaseCom(T->Scratch);
    D3D11_TEXTURE2D_DESC TDesc = {};
    TDesc.Width = (UINT)(T->CellW * GLYPH_MAX_CELLS);
    TDesc.Height = (UINT)T->CellH;
    TDesc.MipLevels = 1;
    TDesc.ArraySize = 1;
    TDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    TDesc.SampleDesc.Count = 1;
    TDesc.Usage = D3D11_USAGE_DEFAULT;
    TDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    T->Device->CreateTexture2D(&TDesc, 0, &T->Scratch);
    IDXGISurface* Surface = 0;
    T->Scratch->QueryInterface(__uuidof(IDXGISurface), (void**)&Surface);
    // 96 DPI: one Direct2D unit is one pixel; font sizes are given in pixels.
    D2D1_RENDER_TARGET_PROPERTIES Props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    HRESULT Hr = T->D2D->CreateDxgiSurfaceRenderTarget(Surface, &Props, &T->Draw2D);
    if(FAILED(Hr)) { LogHr("cells", "CreateDxgiSurfaceRenderTarget", Hr); }
    ReleaseCom(Surface);
    if(T->Draw2D)
    {
        T->Draw2D->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        T->Draw2D->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &T->White);
    }
    T->TilesX = ATLAS_SIZE / T->CellW;
    T->TilesY = ATLAS_SIZE / T->CellH;
    GlyphCacheReset(T);
    T->FontGeneration += 1;
    Logf("cells", "font %s %.1f px: cells %dx%d", WideToUtf8(Name).c_str(), (double)SizePx, T->CellW, T->CellH);
}

extern "C" void TermCells_CellSize(TermCells* T, s32* W, s32* H)
{
    *W = T->CellW;
    *H = T->CellH;
}

// Emoji draw in their own colors; everything else is a coverage mask tinted with the cell color.
static bool32 IsColorGlyph(const u32* Cps, u32 Count)
{
    for(u32 I = 0; I < Count; ++I) { if(Cps[I] == 0xFE0F) { return true; } }
    u32 Cp = Cps[0];
    // The emoji blocks only: higher planes hold CJK and private-use icons (Nerd Font) the user's font draws.
    return (Cp >= 0x1F000 && Cp <= 0x1FAFF) || (Cp >= 0x2300 && Cp <= 0x2BFF && CharWidth(Cp) == 2);
}

// The first of Cells tiles for this text in this style, drawing it into the atlas on first use.
// Overflow reports a glyph wider than its cells (drawn shrunk). Returns false when the atlas is full
// (reset the cache and resolve again).
static bool32 GlyphLookup(TermCells* T, const u32* Cps, u32 Count, u32 Style, u32 Cells, u32* Tile, bool32* Color, bool32* Overflow)
{
    Count = Min((s32)Count, GLYPH_MAX_CPS);
    Cells = Clamp((s32)Cells, 1, GLYPH_MAX_CELLS);
    u32 Hash = 2166136261u ^ (Style * 31 + Cells);
    for(u32 I = 0; I < Count; ++I) { Hash = (Hash ^ Cps[I]) * 16777619u; }
    u32 Mask = (u32)T->Table.size() - 1;
    u32 Slot = Hash & Mask;
    for(;;)
    {
        glyph_entry& E = T->Table[Slot];
        if(!E.Count) { break; }
        if(E.Count == Count && E.Style == Style && E.Cells == Cells && memcmp(E.Cps, Cps, Count * sizeof(u32)) == 0)
        {
            *Tile = E.Tile;
            *Color = E.Color;
            *Overflow = E.Overflow;
            return true;
        }
        Slot = (Slot + 1) & Mask;
    }
    if(!T->Draw2D) { *Tile = 0; *Color = false; *Overflow = false; return true; }

    // --- a free run of tiles in the atlas; the table stays at most half full ---
    if(T->NextX + Cells > T->TilesX) { T->NextX = 0; T->NextY += 1; }
    if(T->NextY >= T->TilesY || T->Used * 2 >= T->Table.size()) { return false; }
    u32 X = T->NextX;
    u32 Y = T->NextY;
    T->NextX += Cells;

    // --- draw it with Direct2D, white on transparent, and copy its tiles into the atlas ---
    // A lone symbol asks for text presentation (U+FE0E), or fallback picks a color emoji. A glyph
    // wider than its cells (often from a fallback font) is scaled to fit.
    bool32 IsColor = IsColorGlyph(Cps, Count);
    std::wstring Text;
    for(u32 I = 0; I < Count; ++I)
    {
        u32 Cp = Cps[I];
        if(Cp >= 0x10000) { Text += (wchar_t)(0xD800 + ((Cp - 0x10000) >> 10)); Text += (wchar_t)(0xDC00 + ((Cp - 0x10000) & 0x3FF)); }
        else { Text += (wchar_t)Cp; }
    }
    // Only symbols that also have an emoji form: box drawing, blocks and the like keep the main font.
    u32 C0 = Cps[0];
    bool32 HasEmojiForm = (C0 >= 0x2190 && C0 <= 0x21FF) || (C0 >= 0x2300 && C0 <= 0x23FF) || (C0 >= 0x25A0 && C0 <= 0x25FF) ||
                          (C0 >= 0x2600 && C0 <= 0x27BF) || (C0 >= 0x2900 && C0 <= 0x297F) || (C0 >= 0x2B00 && C0 <= 0x2BFF) ||
                          C0 == 0x3030 || C0 == 0x303D || C0 == 0x3297 || C0 == 0x3299;
    if(!IsColor && Count == 1 && HasEmojiForm && CharWidth(C0) == 1) { Text += (wchar_t)0xFE0E; }
    float BoxW = (float)(Cells * T->CellW);
    IDWriteTextLayout* Layout = 0;
    T->DWrite->CreateTextLayout(Text.data(), (UINT32)Text.size(), IsColor ? T->Emoji : T->Formats[Style & 3], 10000.0f, (float)T->CellH, &Layout);
    DWRITE_TEXT_METRICS TM = {};
    Layout->GetMetrics(&TM);
    float Fit = (TM.widthIncludingTrailingWhitespace > BoxW + 0.5f) ? BoxW / TM.widthIncludingTrailingWhitespace : 1.0f;
    T->Draw2D->BeginDraw();
    T->Draw2D->Clear(D2D1::ColorF(0, 0, 0, 0));
    // Squeeze sideways only: a too-wide glyph keeps its height and stays on the line. Emoji keep
    // their proportions.
    T->Draw2D->SetTransform(D2D1::Matrix3x2F::Scale(Fit, IsColor ? Fit : 1.0f, D2D1::Point2F(0.0f, (float)T->Baseline)));
    T->Draw2D->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), Layout, T->White,
                              IsColor ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT : D2D1_DRAW_TEXT_OPTIONS_NONE);
    T->Draw2D->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT DrawHr = T->Draw2D->EndDraw();
    if(FAILED(DrawHr))
    {
        char Hex[96] = {};
        for(u32 I = 0, N = 0; I < Count && N + 10 < sizeof(Hex); ++I) { N += (u32)snprintf(Hex + N, sizeof(Hex) - N, "%X ", Cps[I]); }
        LogHr("cells", "drawing a glyph", DrawHr);
        Logf("cells", "the glyph: %s(%s)", Hex, IsColor ? "color" : "mask");
    }
    ReleaseCom(Layout);
    D3D11_BOX Src = {0, 0, 0, Cells * (UINT)T->CellW, (UINT)T->CellH, 1};
    T->Context->CopySubresourceRegion(T->Atlas, 0, X * T->CellW, Y * T->CellH, 0, T->Scratch, 0, &Src);

    glyph_entry& E = T->Table[Slot];
    memcpy(E.Cps, Cps, Count * sizeof(u32));
    E.Count = Count;
    E.Style = Style;
    E.Cells = Cells;
    E.Tile = X | (Y << 16);
    E.Color = IsColor;
    E.Overflow = Fit < 1.0f;
    T->Used += 1;
    *Tile = E.Tile;
    *Color = E.Color;
    *Overflow = E.Overflow;
    return true;
}

// The grid as GPU cells: colors resolved, glyph tiles looked up (drawn on first use), clusters joined.
static void ResolveGrid(TermCells* T, const TermGrid& G, const TermPalette& Pal, std::vector<gpu_cell>& Gpu)
{
    TIMED_BLOCK(ResolveGrid);
    size_t N = (size_t)G.cols * G.rows;
    Gpu.assign(N, gpu_cell{});
    const cell* Grid = (const cell*)G.cells;
    // A full atlas empties the glyph cache and the pass runs again.
    for(s32 Pass = 0; Pass < 2; ++Pass)
    {
        bool32 AtlasFull = false;
        for(s32 Y = 0; Y < G.rows && !AtlasFull; ++Y)
        {
            const cell* Row = Grid + (size_t)Y * G.cols;
            for(s32 X = 0; X < G.cols && !AtlasFull; ++X)
            {
                size_t I = (size_t)Y * G.cols + X;
                const cell& K = Grid[I];
                u32 Fg = ResolveColor(K.Fg, Pal.fg, Pal);
                u32 Bg = ResolveColor(K.Bg, Pal.bg, Pal);
                if(K.Attr & ATTR_INVERSE) { u32 Swap = Fg; Fg = Bg; Bg = Swap; }
                if(K.Attr & ATTR_DIM) { Fg = ((Fg >> 1) & 0x7F7F7F) + ((Bg >> 1) & 0x7F7F7F); } // halfway to the background
                if(K.Flags & CELL_FOUND) { Bg = PackHex(Pal.found); }
                bool32 Linked = (Pal.link_hover && (u32)(K.Flags >> CELL_LINK_SHIFT) == Pal.link_hover) ||
                                (Pal.underline_len && Y == Pal.underline_row && X >= Pal.underline_x && X < Pal.underline_x + Pal.underline_len);
                if(K.Flags & CELL_FOUND_CURRENT) { Bg = PackHex(Pal.found_current); Fg = PackHex(Pal.bg); }
                u32 Flags = ((K.Attr & ATTR_UNDERLINE) || Linked ? GPU_UNDERLINE : 0) | ((K.Attr & ATTR_STRIKE) ? GPU_STRIKE : 0);
                bool32 Wide = (K.Flags & CELL_WIDE) && X + 1 < G.cols;
                if(Wide) { Flags |= GPU_WIDE; }
                u32 Cp = K.Codepoint;
                // Blanks, controls, orphaned second halves, hidden text, and marks or joiners with no base before them.
                bool32 Blank = Cp <= ' ' || (K.Flags & CELL_WIDE_CONT) || (K.Attr & ATTR_HIDDEN) || IsClusterExtender(Cp) ||
                               ((K.Flags & CELL_WIDE) && !Wide);

                // --- the cluster starting here: the base plus the cells after it that join it (marks,
                // selectors, joiners and what they join, skin tones, keycaps, a flag's second letter) ---
                u32 Cps[GLYPH_MAX_CPS] = {};
                u32 Count = 0;
                s32 Span = Wide ? 2 : 1;
                s32 MaxSpan = Min(GLYPH_MAX_CELLS, (s32)T->TilesX); // a glyph's tiles share one atlas row
                if(!Blank)
                {
                    Cps[Count++] = Cp;
                    bool32 Flag = IsRegionalIndicator(Cp);
                    for(;;)
                    {
                        s32 Nx = X + Span;
                        if(Nx >= G.cols || Count >= GLYPH_MAX_CPS) { break; }
                        u32 Next = Row[Nx].Codepoint;
                        s32 Add = ((Row[Nx].Flags & CELL_WIDE) && Nx + 1 < G.cols) ? 2 : 1;
                        // After a joiner only an emoji or symbol joins, never e.g. a box-drawing line.
                        bool32 AfterZwj = Cps[Count - 1] == 0x200D && (Next >= 0x1F000 || (Next >= 0x2600 && Next <= 0x27BF));
                        // Ligatures: up to four programming symbols of the same look drawn as one run (-> => != ===).
                        bool32 Liga = Pal.ligatures && Count < 4 && Cps[Count - 1] < 0x80 && Next < 0x80 &&
                                      strchr("!#$%&*+-./:;<=>?@\\^_|~", (int)Cps[Count - 1]) && strchr("!#$%&*+-./:;<=>?@\\^_|~", (int)Next) &&
                                      Row[Nx].Fg == K.Fg && Row[Nx].Bg == K.Bg && Row[Nx].Attr == K.Attr && !(Row[Nx].Flags & (CELL_FOUND | CELL_FOUND_CURRENT));
                        bool32 Joins = Next > ' ' && (IsClusterExtender(Next) || AfterZwj || Liga || (Flag && Count == 1 && IsRegionalIndicator(Next)));
                        if(!Joins || Span + Add > MaxSpan) { break; }
                        Cps[Count++] = Next;
                        Span += Add;
                    }
                }

                u32 Tile = 0;
                bool32 Color = false;
                if(!Blank)
                {
                    u32 Style = ((K.Attr & ATTR_BOLD) ? STYLE_BOLD : 0) | ((K.Attr & ATTR_ITALIC) ? STYLE_ITALIC : 0);
                    bool32 Overflow = false;
                    if(!GlyphLookup(T, Cps, Count, Style, Span, &Tile, &Color, &Overflow)) { AtlasFull = true; break; }
                    // A glyph wider than its cells may spill into a following blank cell of the same style
                    // and draw full size; otherwise it stays squeezed.
                    s32 Nx = X + Span;
                    bool32 UnderCursor = !(G.flags & TermGrid_CursorHidden) && G.cursor_y == Y && G.cursor_x == Nx;
                    bool32 Room = Overflow && !UnderCursor && Nx < G.cols && Span < MaxSpan && (Row[Nx].Codepoint == 0 || Row[Nx].Codepoint == ' ') &&
                                  !Row[Nx].Flags && Row[Nx].Fg == K.Fg && Row[Nx].Bg == K.Bg && Row[Nx].Attr == K.Attr;
                    if(Room)
                    {
                        Span += 1;
                        if(!GlyphLookup(T, Cps, Count, Style, Span, &Tile, &Color, &Overflow)) { AtlasFull = true; break; }
                    }
                }
                if(Color) { Flags |= GPU_COLOR; }
                if(COLOR_TAG(K.Bg) == 0 && !(K.Attr & ATTR_INVERSE) && !(K.Flags & (CELL_FOUND | CELL_FOUND_CURRENT))) { Flags |= GPU_SEE_THROUGH; } // the terminal's own background
                for(s32 Part = 0; Part < Span && I + Part < N; ++Part)
                {
                    Gpu[I + Part] = gpu_cell{Tile ? Tile + (u32)Part : 0, Fg, Bg, Part == 0 ? Flags : (Flags & ~(u32)GPU_WIDE)};
                }
                X += Span - 1;
            }
        }
        if(!AtlasFull) { break; }
        GlyphCacheReset(T);
        T->FontGeneration += 1; // every grid resolves again
    }
}

static gpu_grid* GpuGrid(TermCells* T, u32 TabId)
{
    for(gpu_grid* G : T->Grids) { if(G->TabId == TabId) { return G; } }
    gpu_grid* G = new gpu_grid();
    G->TabId = TabId;
    T->Grids.push_back(G);
    return G;
}

extern "C" void TermCells_Prepare(TermCells* T, const DrawList* Dl)
{
    TIMED_BLOCK(CellsPrepare);
    T->Frame += 1;
    T->ResolvedThisFrame = 0;
    AppFrameInput In = {};
    TermClient_Fill(T->Client, &In);
    for(s64 B = 0; B < Dl->batches.count; ++B)
    {
        const DrawBatch& Batch = Dl->batches.data[B];
        if(Batch.kind != Batch_External || !Batch.external) { continue; }
        const TermCellsDraw* D = (const TermCellsDraw*)Batch.external;
        const TermGrid* G = 0;
        for(s64 I = 0; I < In.grid_count; ++I) { if(In.grids[I].tab_id == D->tab_id) { G = &In.grids[I]; } }
        if(!G || G->cols <= 0 || G->rows <= 0) { continue; }
        gpu_grid* GG = GpuGrid(T, D->tab_id);
        GG->UsedFrame = T->Frame;
        u64 PaletteHash = Hash_Bytes(&D->palette, sizeof(D->palette), 0);
        if(GG->Version == G->version && GG->PaletteHash == PaletteHash && GG->FontGeneration == T->FontGeneration) { continue; }
        ResolveGrid(T, *G, D->palette, T->Scratchpad);
        T->ResolvedThisFrame += 1;
        u32 Count = (u32)Max(1, (s32)T->Scratchpad.size());
        if(Count > GG->Capacity)
        {
            ReleaseCom(GG->CellsView);
            ReleaseCom(GG->Cells);
            GG->Capacity = Count + Count / 2;
            D3D11_BUFFER_DESC BDesc = {};
            BDesc.ByteWidth = GG->Capacity * sizeof(gpu_cell);
            BDesc.Usage = D3D11_USAGE_DEFAULT;
            BDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            BDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            BDesc.StructureByteStride = sizeof(gpu_cell);
            T->Device->CreateBuffer(&BDesc, 0, &GG->Cells);
            D3D11_SHADER_RESOURCE_VIEW_DESC VDesc = {};
            VDesc.Format = DXGI_FORMAT_UNKNOWN;
            VDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            VDesc.Buffer.NumElements = GG->Capacity;
            T->Device->CreateShaderResourceView(GG->Cells, &VDesc, &GG->CellsView);
        }
        if(!T->Scratchpad.empty())
        {
            D3D11_BOX Box = {0, 0, 0, (UINT)(T->Scratchpad.size() * sizeof(gpu_cell)), 1, 1};
            T->Context->UpdateSubresource(GG->Cells, 0, &Box, T->Scratchpad.data(), 0, 0);
        }
        GG->Version = G->version;
        GG->PaletteHash = PaletteHash;
        GG->FontGeneration = T->FontGeneration;
        GG->Cols = G->cols;
        GG->Rows = G->rows;
    }
    // --- grids no pane has drawn for a while ---
    for(size_t I = 0; I < T->Grids.size();)
    {
        gpu_grid* G = T->Grids[I];
        if(T->Frame - G->UsedFrame < 600) { ++I; continue; }
        ReleaseCom(G->CellsView);
        ReleaseCom(G->Cells);
        delete G;
        T->Grids.erase(T->Grids.begin() + (s64)I);
    }
}

extern "C" void TermCells_Draw(void* User, const void* Data, Rect Clip, s32 TargetW, s32 TargetH)
{
    TermCells* T = (TermCells*)User;
    const TermCellsDraw* D = (const TermCellsDraw*)Data;
    gpu_grid* G = 0;
    for(gpu_grid* X : T->Grids) { if(X->TabId == D->tab_id) { G = X; } }
    if(!G || !G->CellsView || !T->PixelShader) { return; }
    gpu_consts Consts = {};
    Consts.Origin[0] = D->origin[0];
    Consts.Origin[1] = D->origin[1];
    Consts.CellW = (u32)T->CellW;
    Consts.CellH = (u32)T->CellH;
    Consts.GridW = (u32)G->Cols;
    Consts.GridH = (u32)G->Rows;
    Consts.MarginColor = PackHex(D->palette.bg);
    Consts.UnderlineY = (u32)T->UnderlineY;
    Consts.StrikeY = (u32)T->StrikeY;
    Consts.LineH = (u32)T->LineH;
    Consts.CursorThick = (u32)Max(2, T->CellW / 6);
    Consts.Opacity = (u32)Clamp((s32)(D->opacity * 255.0f + 0.5f), 0, 255);
    memcpy(Consts.Sel, D->sel, sizeof(Consts.Sel));
    Consts.SelOn = D->sel_on ? 1 : 0;
    Consts.SelBlock = D->sel_block ? 1 : 0;
    Consts.SelColor = PackHex(D->palette.selection);
    AppFrameInput In = {};
    TermClient_Fill(T->Client, &In);
    for(s64 I = 0; I < In.grid_count; ++I)
    {
        const TermGrid& Gr = In.grids[I];
        if(Gr.tab_id != D->tab_id) { continue; }
        Consts.Cursor[0] = Gr.cursor_x;
        Consts.Cursor[1] = Gr.cursor_y;
        s32 Shape = D->cursor_shape ? D->cursor_shape : Gr.cursor_shape;
        Consts.CursorShape = !D->cursor_on ? 0 : D->cursor_hollow ? 4 : (Shape == 3 || Shape == 4) ? 2 : (Shape == 5 || Shape == 6) ? 3 : 1;
    }
    Consts.CursorColor = PackHex(D->palette.cursor);
    D3D11_MAPPED_SUBRESOURCE Mapped = {};
    if(SUCCEEDED(T->Context->Map(T->Consts, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
    {
        memcpy(Mapped.pData, &Consts, sizeof(Consts));
        T->Context->Unmap(T->Consts, 0);
    }

    // --- one full-target triangle, scissored to the pane ---
    D3D11_VIEWPORT View = {0.0f, 0.0f, (float)TargetW, (float)TargetH, 0.0f, 1.0f};
    T->Context->RSSetViewports(1, &View);
    T->Context->RSSetState(T->Rasterizer);
    D3D11_RECT Scissor = {Clip.x0, Clip.y0, Clip.x1, Clip.y1};
    T->Context->RSSetScissorRects(1, &Scissor);
    float Factor[4] = {};
    T->Context->OMSetBlendState(T->Blend, Factor, 0xffffffff);
    T->Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    T->Context->IASetInputLayout(0);
    T->Context->VSSetShader(T->VertexShader, 0, 0);
    T->Context->PSSetShader(T->PixelShader, 0, 0);
    T->Context->PSSetConstantBuffers(0, 1, &T->Consts);
    ID3D11ShaderResourceView* Views[2] = {G->CellsView, T->AtlasView};
    T->Context->PSSetShaderResources(0, 2, Views);
    T->Context->Draw(3, 0);
    ID3D11ShaderResourceView* None[2] = {};
    T->Context->PSSetShaderResources(0, 2, None);
}

extern "C" void TermCells_Stats(TermCells* T, s32* Glyphs, s32* AtlasPercent, s32* Resolved)
{
    *Glyphs = (s32)T->Used;
    u32 Total = Max((s32)(T->TilesX * T->TilesY), 1);
    *AtlasPercent = (s32)((T->NextY * T->TilesX + T->NextX) * 100 / Total);
    *Resolved = T->ResolvedThisFrame;
}

// The installed monospace font families (the settings' font list), sorted, in Arena.
extern "C" s64 TermCells_MonoFonts(TermCells* T, Arena* A, String** Out)
{
    *Out = 0;
    IDWriteFontCollection* Collection = 0;
    if(!T->DWrite || FAILED(T->DWrite->GetSystemFontCollection(&Collection, FALSE))) { return 0; }
    TIMED_BLOCK(MonoFonts);
    UINT32 Families = Collection->GetFontFamilyCount();
    String* List = (String*)Arena_Push(A, sizeof(String) * (u64)(Families + 1), 8);
    s64 Count = 0;
    for(UINT32 I = 0; I < Families; I++)
    {
        IDWriteFontFamily* Family = 0;
        IDWriteFont* Font = 0;
        IDWriteFont1* Font1 = 0;
        IDWriteLocalizedStrings* Names = 0;
        if(SUCCEEDED(Collection->GetFontFamily(I, &Family)) &&
           SUCCEEDED(Family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &Font)) &&
           SUCCEEDED(Font->QueryInterface(__uuidof(IDWriteFont1), (void**)&Font1)) && Font1->IsMonospacedFont() && !Font->IsSymbolFont() &&
           SUCCEEDED(Family->GetFamilyNames(&Names)))
        {
            UINT32 Index = 0, Length = 0;
            BOOL Exists = FALSE;
            if(FAILED(Names->FindLocaleName(L"en-us", &Index, &Exists)) || !Exists) { Index = 0; }
            wchar_t Name[128] = {};
            if(SUCCEEDED(Names->GetStringLength(Index, &Length)) && Length < 128 && SUCCEEDED(Names->GetString(Index, Name, 128)))
            {
                s32 Bytes = WideCharToMultiByte(CP_UTF8, 0, Name, (int)Length, 0, 0, 0, 0);
                u8* Utf8 = (u8*)Arena_Push(A, (u64)Bytes + 1, 1);
                WideCharToMultiByte(CP_UTF8, 0, Name, (int)Length, (char*)Utf8, Bytes, 0, 0);
                Utf8[Bytes] = 0;
                if(Name[0] != L'@') { List[Count++] = String{ Utf8, Bytes }; }   // '@': vertical-text CJK families
            }
        }
        ReleaseCom(Names);
        ReleaseCom(Font1);
        ReleaseCom(Font);
        ReleaseCom(Family);
    }
    ReleaseCom(Collection);
    std::sort(List, List + Count, [](const String& X, const String& Y) {
        return _strnicmp((const char*)X.data, (const char*)Y.data, (size_t)(X.count < Y.count ? X.count : Y.count) + 1) < 0;
    });
    *Out = List;
    return Count;
}

extern "C" void TermCells_Destroy(TermCells* T)
{
    for(gpu_grid* G : T->Grids) { ReleaseCom(G->CellsView); ReleaseCom(G->Cells); delete G; }
    for(IDWriteTextFormat*& Format : T->Formats) { ReleaseCom(Format); }
    ReleaseCom(T->Emoji);
    ReleaseCom(T->White);
    ReleaseCom(T->Draw2D);
    ReleaseCom(T->Scratch);
    ReleaseCom(T->DWrite);
    ReleaseCom(T->D2D);
    ReleaseCom(T->AtlasView);
    ReleaseCom(T->Atlas);
    ReleaseCom(T->Rasterizer);
    ReleaseCom(T->Blend);
    ReleaseCom(T->Consts);
    ReleaseCom(T->PixelShader);
    ReleaseCom(T->VertexShader);
    delete T;
}
