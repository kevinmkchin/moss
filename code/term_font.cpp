// term: the UI's fonts. Glyphs are drawn with DirectWrite (grayscale antialiasing) into one shared 8-bit atlas the
// first time each is asked for; a codepoint the family lacks comes from the first of a few system fonts that has it.
// The reader's fonts (Font.embolden) are drawn finer: from their outlines, unhinted, at quarter-pixel pens.

struct font_native
{
    IDWriteFontFace* Faces[8];  // the family's, then the fallbacks'
    s32 FaceCount;
};

static IDWriteFactory2* FontFactory()
{
    static IDWriteFactory2* Factory = 0;
    if(!Factory) { DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown**)&Factory); }
    return Factory;
}

static IDWriteFontFace* FontFaceFor(const wchar_t* Family, DWRITE_FONT_WEIGHT Weight)
{
    if(wcschr(Family, L'\\'))
    {
        // a font file (moss's own, next to moss.exe): its one face, whatever the weight asked
        IDWriteFontFile* File = 0;
        IDWriteFontFace* FileFace = 0;
        if(GetFileAttributesW(Family) != INVALID_FILE_ATTRIBUTES) { FontFactory()->CreateFontFileReference(Family, 0, &File); }
        if(File) { FontFactory()->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &File, 0, DWRITE_FONT_SIMULATIONS_NONE, &FileFace); File->Release(); }
        return FileFace;
    }
    IDWriteFontCollection* System = 0;
    FontFactory()->GetSystemFontCollection(&System, FALSE);
    if(!System) { return 0; }
    UINT32 Index = 0;
    BOOL Exists = FALSE;
    System->FindFamilyName(Family, &Index, &Exists);
    IDWriteFontFace* Face = 0;
    if(Exists)
    {
        IDWriteFontFamily* Fam = 0;
        System->GetFontFamily(Index, &Fam);
        IDWriteFont* Font = 0;
        if(Fam) { Fam->GetFirstMatchingFont(Weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &Font); }
        if(Font) { Font->CreateFontFace(&Face); Font->Release(); }
        if(Fam) { Fam->Release(); }
    }
    System->Release();
    return Face;
}

// family: the font (or a font file's full path); weight 400 regular, 600 semibold, 700 bold. A family that isn't
// installed uses the next of `families` ("A\0B\0\0"), so icon fonts can name Windows 11's and 10's.
extern "C" b32 Font_Create(Font* F, FontAtlas* Atlas, const wchar_t* Families, f32 SizePx, s32 Weight)
{
    font_native* N = new font_native();
    for(const wchar_t* At = Families; *At && !N->FaceCount; At += wcslen(At) + 1)
    {
        IDWriteFontFace* Face = FontFaceFor(At, (DWRITE_FONT_WEIGHT)Weight);
        if(Face) { N->Faces[N->FaceCount++] = Face; }
    }
    static const wchar_t* Fallbacks[] = {L"Segoe UI", L"Segoe UI Symbol", L"Segoe UI Emoji", L"Microsoft YaHei UI", L"Yu Gothic UI",
                                         L"Malgun Gothic", L"Nirmala UI"};
    for(const wchar_t* Fb : Fallbacks)
    {
        if(N->FaceCount >= (s32)ArrayCount(N->Faces)) { break; }
        IDWriteFontFace* Face = FontFaceFor(Fb, (DWRITE_FONT_WEIGHT)Weight);
        if(Face) { N->Faces[N->FaceCount++] = Face; }
    }
    if(!N->FaceCount) { delete N; return 0; }
    DWRITE_FONT_METRICS M = {};
    N->Faces[0]->GetMetrics(&M);
    f32 Scale = SizePx / (f32)M.designUnitsPerEm;
    Font Zero = {};
    *F = Zero;
    F->native = N;
    F->atlas = Atlas;
    F->size_px = SizePx;
    F->ascent = (f32)M.ascent * Scale;
    F->descent = (f32)M.descent * Scale;
    F->line_h = (f32)(M.ascent + M.descent + M.lineGap) * Scale;
    return 1;
}

extern "C" void Font_Destroy(Font* F)
{
    font_native* N = (font_native*)F->native;
    if(N)
    {
        for(s32 I = 0; I < N->FaceCount; ++I) { N->Faces[I]->Release(); }
        delete N;
    }
    free(F->glyphs);
    Font Zero = {};
    *F = Zero;
}

// Room for a w x h bitmap in the atlas: shelf by shelf. False when it's full.
static bool32 AtlasPlace(FontAtlas* A, s32 W, s32 H, s32* X, s32* Y)
{
    if(A->pen_x + W + 1 > A->w) { A->pen_x = 1; A->pen_y += A->row_h + 1; A->row_h = 0; }
    if(A->pen_y + H + 1 > A->h) { return false; }
    *X = A->pen_x;
    *Y = A->pen_y;
    A->pen_x += W + 1;
    A->row_h = Max(A->row_h, H);
    return true;
}

// The reader's fine glyphs are painted from their outlines with Direct2D into one 8-bit bitmap kept for it (software,
// 256 x 256: a bigger glyph is drawn the usual way).
struct glyph_painter
{
    ID2D1Factory* D2D;
    IWICImagingFactory* Wic;
    IWICBitmap* Bitmap;
    ID2D1RenderTarget* Target;
    ID2D1SolidColorBrush* Brush;
    ID2D1StrokeStyle* Round;
    bool32 Tried;
};
static const s32 PainterSize = 256;

static glyph_painter* Painter()
{
    static glyph_painter P = {};
    if(P.Tried) { return P.Target ? &P : 0; }
    P.Tried = true;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), 0, (void**)&P.D2D);
    CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&P.Wic));
    if(!P.D2D || !P.Wic) { return 0; }
    P.Wic->CreateBitmap(PainterSize, PainterSize, GUID_WICPixelFormat8bppAlpha, WICBitmapCacheOnLoad, &P.Bitmap);
    D2D1_RENDER_TARGET_PROPERTIES Props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                       D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if(P.Bitmap) { P.D2D->CreateWicBitmapRenderTarget(P.Bitmap, Props, &P.Target); }
    if(!P.Target) { return 0; }
    P.Target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    P.Target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &P.Brush);
    D2D1_STROKE_STYLE_PROPERTIES Stroke = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    P.D2D->CreateStrokeStyle(Stroke, 0, 0, &P.Round);
    return P.Brush ? &P : 0;
}

// A fine glyph's bitmap: its outline unhinted, its pen a quarter pixel in (Phase 0..3), filled and stroked Embolden px
// wider on every side, so every stem gains the same; false when it can't be (no painter, too big: drawn the usual way).
static bool32 PaintFineGlyph(Font* F, IDWriteFontFace* Face, UINT16 Index, s32 Phase, Glyph* G)
{
    glyph_painter* P = Painter();
    if(!P) { return false; }
    ID2D1PathGeometry* Geo = 0;
    ID2D1GeometrySink* Sink = 0;
    P->D2D->CreatePathGeometry(&Geo);
    if(Geo) { Geo->Open(&Sink); }
    if(!Sink) { if(Geo) { Geo->Release(); } return false; }
    Face->GetGlyphRunOutline(F->size_px, &Index, 0, 0, 1, FALSE, FALSE, Sink);
    Sink->Close();
    Sink->Release();
    D2D1_RECT_F Bounds = {};
    Geo->GetBounds(0, &Bounds);
    if(!(Bounds.right > Bounds.left && Bounds.bottom > Bounds.top)) { Geo->Release(); return true; }   // nothing to draw
    f32 Ox = (f32)Phase / 4.0f, Pad = F->embolden + 1.0f;
    s32 X0 = (s32)floorf(Bounds.left + Ox - Pad), Y0 = (s32)floorf(Bounds.top - Pad);
    s32 X1 = (s32)ceilf(Bounds.right + Ox + Pad), Y1 = (s32)ceilf(Bounds.bottom + Pad);
    s32 W = X1 - X0, H = Y1 - Y0, X = 0, Y = 0;
    if(W > PainterSize || H > PainterSize) { Geo->Release(); return false; }
    P->Target->BeginDraw();
    P->Target->Clear(D2D1::ColorF(0, 0, 0, 0));
    P->Target->SetTransform(D2D1::Matrix3x2F::Translation(Ox - (f32)X0, -(f32)Y0));
    P->Target->FillGeometry(Geo, P->Brush);
    if(F->embolden > 0) { P->Target->DrawGeometry(Geo, P->Brush, F->embolden * 2.0f, P->Round); }
    HRESULT Hr = P->Target->EndDraw();
    Geo->Release();
    if(FAILED(Hr) || !AtlasPlace(F->atlas, W, H, &X, &Y)) { return true; }
    WICRect Rect = {0, 0, W, H};
    IWICBitmapLock* Lock = 0;
    P->Bitmap->Lock(&Rect, WICBitmapLockRead, &Lock);
    if(!Lock) { return true; }
    UINT Stride = 0, Size = 0;
    BYTE* Data = 0;
    Lock->GetStride(&Stride);
    Lock->GetDataPointer(&Size, &Data);
    for(s32 Row = 0; Data && Row < H; ++Row)
    {
        memcpy(F->atlas->pixels + (size_t)(Y + Row) * F->atlas->w + X, Data + (size_t)Row * Stride, (size_t)W);
    }
    Lock->Release();
    F->atlas->version++;
    G->x = (u16)X; G->y = (u16)Y; G->w = (u16)W; G->h = (u16)H;
    G->left = (s16)X0;
    G->top = (s16)-Y0;
    return true;
}

// Key: the codepoint, or (top bit set, Font_Shape's) a face's glyph: its face in bits 16..18, its index in 0..15; for a
// fine font the pen's quarter pixel (0..3) in bits 24..25.
static Glyph DrawGlyph(Font* F, u32 Key)
{
    Glyph G = {};
    G.codepoint = Key;
    b32 Shaped = (Key & 0x80000000u) != 0;
    u32 Cp = Shaped ? 0 : Key & 0xFFFFFF;
    s32 Phase = (s32)((Key >> 24) & 3);
    font_native* N = (font_native*)F->native;
    // the first face that has it (a shaped glyph names its own)
    IDWriteFontFace* Face = N->Faces[0];
    UINT16 Index = 0;
    if(Shaped)
    {
        s32 I = (s32)((Key >> 16) & 7);
        Face = N->Faces[I < N->FaceCount ? I : 0];
        Index = (UINT16)(Key & 0xFFFF);
    }
    for(s32 I = 0; !Shaped && I < N->FaceCount && !Index; ++I)
    {
        UINT32 C = Cp;
        N->Faces[I]->GetGlyphIndices(&C, 1, &Index);
        if(Index) { Face = N->Faces[I]; }
    }
    DWRITE_FONT_METRICS M = {};
    Face->GetMetrics(&M);
    DWRITE_GLYPH_METRICS GM = {};
    Face->GetDesignGlyphMetrics(&Index, 1, &GM, FALSE);
    G.advance = (f32)GM.advanceWidth * F->size_px / (f32)M.designUnitsPerEm;
    if(!Shaped && (Cp == ' ' || Cp == 0xA0)) { return G; }
    if(F->embolden > 0 && Index && PaintFineGlyph(F, Face, Index, Phase, &G)) { return G; }

    DWRITE_GLYPH_RUN Run = {};
    FLOAT Advance = 0;
    DWRITE_GLYPH_OFFSET Offset = {};
    Run.fontFace = Face;
    Run.fontEmSize = F->size_px;
    Run.glyphCount = 1;
    Run.glyphIndices = &Index;
    Run.glyphAdvances = &Advance;
    Run.glyphOffsets = &Offset;
    IDWriteGlyphRunAnalysis* Analysis = 0;
    FontFactory()->CreateGlyphRunAnalysis(&Run, 0, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
                                          DWRITE_GRID_FIT_MODE_ENABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &Analysis);
    if(!Analysis) { return G; }
    RECT B = {};
    Analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &B);
    s32 W = B.right - B.left, H = B.bottom - B.top;
    s32 X = 0, Y = 0;
    if(W > 0 && H > 0 && AtlasPlace(F->atlas, W, H, &X, &Y))
    {
        std::vector<u8> Bits((size_t)W * H);
        Analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &B, Bits.data(), (UINT32)Bits.size());
        for(s32 Row = 0; Row < H; ++Row)
        {
            memcpy(F->atlas->pixels + (size_t)(Y + Row) * F->atlas->w + X, Bits.data() + (size_t)Row * W, (size_t)W);
        }
        F->atlas->version++;
        G.x = (u16)X; G.y = (u16)Y; G.w = (u16)W; G.h = (u16)H;
        G.left = (s16)B.left;
        G.top = (s16)-B.top;
    }
    Analysis->Release();
    return G;
}

extern "C" Glyph* Font_Glyph(Font* F, u32 Cp)
{
    if(!F || !F->native) { return 0; }
    // --- the table: open addressing by codepoint, grown at half full ---
    if(F->glyph_count * 2 >= F->glyph_cap)
    {
        s32 Cap = F->glyph_cap ? F->glyph_cap * 2 : 256;
        Glyph* Grown = (Glyph*)calloc((size_t)Cap, sizeof(Glyph));
        for(s32 I = 0; I < F->glyph_cap; ++I)
        {
            Glyph* G = &F->glyphs[I];
            if(!G->codepoint && !G->advance) { continue; }
            u32 At = (G->codepoint * 2654435761u) & (u32)(Cap - 1);
            while(Grown[At].codepoint || Grown[At].advance) { At = (At + 1) & (u32)(Cap - 1); }
            Grown[At] = *G;
        }
        free(F->glyphs);
        F->glyphs = Grown;
        F->glyph_cap = Cap;
    }
    u32 At = (Cp * 2654435761u) & (u32)(F->glyph_cap - 1);
    for(;;)
    {
        Glyph* G = &F->glyphs[At];
        if(G->codepoint == Cp && (G->advance || G->w)) { return G; }
        if(!G->codepoint && !G->advance && !G->w)
        {
            *G = DrawGlyph(F, Cp);
            if(!G->advance && !G->w) { G->advance = 0.0001f; }   // a codepoint with nothing: still a hit next time
            F->glyph_count++;
            return G;
        }
        At = (At + 1) & (u32)(F->glyph_cap - 1);
    }
}

// The atlas for every UI font, 2048 x 2048. A full atlas leaves later glyphs blank (it holds thousands).
extern "C" void FontAtlas_Init(FontAtlas* A)
{
    FontAtlas Zero = {};
    *A = Zero;
    A->w = A->h = 2048;
    A->pixels = (u8*)calloc((size_t)A->w * A->h, 1);
    A->pen_x = A->pen_y = 1;
    A->version = 1;
}

// Every font forgets its glyphs (the DPI changed: sizes do too) and the atlas starts over.
extern "C" void FontAtlas_Reset(FontAtlas* A)
{
    memset(A->pixels, 0, (size_t)A->w * A->h);
    A->pen_x = A->pen_y = 1;
    A->row_h = 0;
    A->version++;
}

// --- shaping (the reader's fonts): a run's script for the text analyzer, which asks through these two ---
struct shape_source : public IDWriteTextAnalysisSource
{
    const WCHAR* Text;
    UINT32 Length;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** Out) override { *Out = 0; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 Pos, WCHAR const** Out, UINT32* Len) override
    {
        *Out = Pos < Length ? Text + Pos : 0;
        *Len = Pos < Length ? Length - Pos : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 Pos, WCHAR const** Out, UINT32* Len) override
    {
        *Out = Pos && Pos <= Length ? Text : 0;
        *Len = Pos && Pos <= Length ? Pos : 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override { return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 Pos, UINT32* Len, WCHAR const** Name) override { *Len = Length - Pos; *Name = L"en-us"; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 Pos, UINT32* Len, IDWriteNumberSubstitution** Sub) override { *Len = Length - Pos; *Sub = 0; return S_OK; }
};

struct shape_sink : public IDWriteTextAnalysisSink
{
    DWRITE_SCRIPT_ANALYSIS Script;
    bool32 Got;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** Out) override { *Out = 0; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32, UINT32, DWRITE_SCRIPT_ANALYSIS const* S) override { if(!Got) { Script = *S; Got = true; } return S_OK; }
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32, UINT32, DWRITE_LINE_BREAKPOINT const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32, UINT32, UINT8, UINT8) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32, IDWriteNumberSubstitution*) override { return S_OK; }
};

// Text shaped with the font's own features (kerning, ligatures; DirectWrite's defaults): runs of the codepoints each face
// has (the family's, else a fallback's), each run analyzed and shaped. Cached by font and text (all of it goes once it
// holds half a million glyphs).
extern "C" s32 Font_Shape(Font* F, String Text, FontShaped* Out, s32 Cap)
{
    font_native* N = (font_native*)F->native;
    if(!N || Text.count <= 0) { return 0; }
    static std::map<u64, std::vector<FontShaped>> Cache;
    static size_t CacheGlyphs = 0;
    u64 Key = Hash_Bytes(Text.data, (u64)Text.count, Hash_Bytes(&F->size_px, sizeof(F->size_px), (u64)(uintptr_t)N));
    auto Hit = Cache.find(Key);
    if(Hit == Cache.end())
    {
        // --- UTF-16, each unit's byte and codepoint index; each codepoint's face ---
        std::vector<WCHAR> W;
        std::vector<s32> Byte, Index, FaceOf;
        s32 Cps = 0;
        for(s64 At = 0; At < Text.count; ++Cps)
        {
            s64 Start = At;
            u32 Cp = Utf8_Decode(Text, &At);
            if(Cp == '\t') { Cp = ' '; }
            s32 Face = 0;
            for(s32 I = 0; I < N->FaceCount; ++I)
            {
                UINT16 G = 0;
                UINT32 C = Cp;
                N->Faces[I]->GetGlyphIndices(&C, 1, &G);
                if(G) { Face = I; break; }
            }
            WCHAR Units[2] = {(WCHAR)Cp, 0};
            s32 Count = 1;
            if(Cp >= 0x10000) { Units[0] = (WCHAR)(0xD800 + ((Cp - 0x10000) >> 10)); Units[1] = (WCHAR)(0xDC00 + ((Cp - 0x10000) & 0x3FF)); Count = 2; }
            for(s32 K = 0; K < Count; ++K) { W.push_back(Units[K]); Byte.push_back((s32)Start); Index.push_back(Cps); FaceOf.push_back(Face); }
        }
        // --- each run of one face, shaped ---
        static IDWriteTextAnalyzer* Analyzer = 0;
        if(!Analyzer) { FontFactory()->CreateTextAnalyzer(&Analyzer); }
        std::vector<FontShaped> Glyphs;
        for(UINT32 A = 0; A < (UINT32)W.size() && Analyzer;)
        {
            UINT32 B = A;
            while(B < (UINT32)W.size() && FaceOf[B] == FaceOf[A]) { B++; }
            UINT32 Len = B - A;
            IDWriteFontFace* Face = N->Faces[FaceOf[A]];
            shape_source Src = {};
            Src.Text = W.data() + A;
            Src.Length = Len;
            shape_sink Sink = {};
            Analyzer->AnalyzeScript(&Src, 0, Len, &Sink);
            UINT32 Max = Len * 3 + 16, Count = 0;
            std::vector<UINT16> Cluster(Len), Ids(Max);
            std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> TextProps(Len);
            std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> GlyphProps(Max);
            HRESULT Hr = Analyzer->GetGlyphs(W.data() + A, Len, Face, FALSE, FALSE, &Sink.Script, L"en-us", 0, 0, 0, 0, Max,
                                             Cluster.data(), TextProps.data(), Ids.data(), GlyphProps.data(), &Count);
            std::vector<FLOAT> Advances(Count ? Count : 1);
            std::vector<DWRITE_GLYPH_OFFSET> Offsets(Count ? Count : 1);
            if(SUCCEEDED(Hr))
            {
                Hr = Analyzer->GetGlyphPlacements(W.data() + A, Cluster.data(), TextProps.data(), Len, Ids.data(), GlyphProps.data(), Count, Face,
                                                  F->size_px, FALSE, FALSE, &Sink.Script, L"en-us", 0, 0, 0, Advances.data(), Offsets.data());
            }
            if(FAILED(Hr)) { Cache[Key]; Hit = Cache.find(Key); break; }   // can't: drawn codepoint by codepoint
            // each glyph's cluster: the first unit that maps to it, else the glyph's before it (the same cluster)
            std::vector<s32> Unit(Count, -1);
            for(s32 P = (s32)Len - 1; P >= 0; --P) { if(Cluster[P] < Count) { Unit[Cluster[P]] = P; } }
            for(UINT32 G = 0; G < Count; ++G)
            {
                if(Unit[G] < 0) { Unit[G] = G ? Unit[G - 1] : 0; }
                FontShaped S = {};
                S.key = 0x80000000u | ((u32)FaceOf[A] << 16) | Ids[G];
                S.byte = Byte[A + Unit[G]];
                S.index = Index[A + Unit[G]];
                S.advance = Advances[G];
                S.dx = Offsets[G].advanceOffset;
                S.dy = -Offsets[G].ascenderOffset;
                Glyphs.push_back(S);
            }
            A = B;
        }
        if(Hit == Cache.end())
        {
            if(CacheGlyphs > 500000) { Cache.clear(); CacheGlyphs = 0; }
            CacheGlyphs += Glyphs.size() + 1;
            Hit = Cache.emplace(Key, std::move(Glyphs)).first;
        }
    }
    if(Hit->second.empty()) { return -1; }
    if((s32)Hit->second.size() > Cap) { return -1; }
    memcpy(Out, Hit->second.data(), Hit->second.size() * sizeof(FontShaped));
    return (s32)Hit->second.size();
}
