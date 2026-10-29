// term: the UI's fonts. Glyphs are drawn with DirectWrite (grayscale antialiasing) into one shared 8-bit atlas the
// first time each is asked for; a codepoint the family lacks comes from the first of a few system fonts that has it.

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

static Glyph DrawGlyph(Font* F, u32 Cp)
{
    Glyph G = {};
    G.codepoint = Cp;
    font_native* N = (font_native*)F->native;
    // the first face that has it
    IDWriteFontFace* Face = N->Faces[0];
    UINT16 Index = 0;
    for(s32 I = 0; I < N->FaceCount && !Index; ++I)
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
    if(Cp == ' ' || Cp == 0xA0) { return G; }

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
