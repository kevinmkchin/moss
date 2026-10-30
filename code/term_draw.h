// term: what a frame draws, in order (later on top): rounded rectangles, text, images and terminal cells, each
// clipped. The app fills a DrawList; the GPU side (term_gpu.cpp) draws it.
#ifndef TERM_DRAW_H
#define TERM_DRAW_H

#include "term_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Rect { s32 x0, y0, x1, y1; } Rect;

// Colors are 0xAARRGGBB, straight alpha.
#define RGBA(r, g, b, a) (((u32)(a) << 24) | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

// --- fonts: a glyph is drawn into the shared atlas the first time it's asked for (term_font.cpp) ---
typedef struct Glyph {
    u32 codepoint;
    u16 x, y, w, h;      // in the atlas (w 0: nothing to draw, a space)
    s16 left, top;       // from the pen: where the bitmap starts (top: up from the baseline)
    f32 advance;
} Glyph;

typedef struct FontAtlas {
    u8 *pixels;          // w x h coverage, 8 bits
    s32 w, h;
    s32 pen_x, pen_y, row_h;   // where the next glyph goes
    u32 version;         // changes whenever pixels do: the GPU uploads again
} FontAtlas;

typedef struct Font {
    void *native;        // the platform's (DirectWrite)
    FontAtlas *atlas;
    f32 size_px, ascent, descent, line_h;
    f32 contrast;        // coverage darkened (DirectWrite's enhanced contrast: a' = a(k+1) / (ak+1)); 0 = as drawn
    s32 glyph_count, glyph_cap;
    Glyph *glyphs;       // open addressing by codepoint (cap a power of two)
} Font;

Glyph *Font_Glyph(Font *font, u32 codepoint);   // term_font.cpp
s32    Font_Measure(Font *font, String text);   // width in px
s64    Font_Fit(Font *font, String text, s32 max_w);   // bytes of text that fit in max_w

// --- the list ---
enum { Quad_Rect = 0, Quad_Glyph = 1, Quad_Image = 2 };
typedef struct Quad {
    f32 rect[4];         // x0 y0 x1 y1, px
    f32 uv[4];           // glyphs and images: texture coordinates
    u32 color;           // fill; a glyph's color; an image's tint
    u32 border_color;
    f32 radius[4];       // corners: top left, top right, bottom right, bottom left
    f32 border;          // px; 0 = none
    f32 dash, gap;       // a dashed border, px (0: solid)
    f32 softness;        // px: a soft edge (shadows)
    u32 kind;            // Quad_*
} Quad;

enum { Batch_Quads = 0, Batch_External = 1 };
typedef struct DrawBatch {
    u32 kind;
    Rect clip;
    u64 texture;         // Batch_Quads: an image's texture (0: the font atlas)
    s64 first, count;    // quads
    const void *external;   // Batch_External: what the external drawer gets (term: a TermCellsDraw)
} DrawBatch;

typedef struct DrawList {
    Arena *arena;
    Array(Quad) quads;
    Array(DrawBatch) batches;
    Rect clips[32];
    s32 clip_depth;
    s32 w, h;            // the target
    u64 hash;            // of everything drawn (with what externals said changed): an equal frame isn't drawn again
    f32 fade;            // what's drawn now is this much see-through (0 as given, 1 gone): a popup fading in
} DrawList;

void DrawList_Begin(DrawList *dl, Arena *arena, s32 w, s32 h);
void Draw_PushClip(DrawList *dl, Rect r);   // intersected with the current one
void Draw_PopClip(DrawList *dl);
void Draw_Rect(DrawList *dl, Rect r, u32 color, f32 radius);
// Fill and border (dashed with dash > 0), each corner its own radius (NULL: all `radius`), a shadow when softness > 0.
void Draw_RectEx(DrawList *dl, Rect r, u32 fill, u32 border_color, f32 border, f32 radius, const f32 *radii, f32 dash, f32 gap, f32 softness);
void Draw_Image(DrawList *dl, Rect r, u64 texture, u32 tint);
// Text from the pen (x, top of the line); returns the pen's x after it. `marks` (codepoint indices, sorted) draw in
// mark_color.
s32  Draw_Text(DrawList *dl, Font *font, s32 x, s32 y, u32 color, String text);
s32  Draw_TextMarked(DrawList *dl, Font *font, s32 x, s32 y, u32 color, String text, const u32 *marks, s64 mark_count, u32 mark_color);
// Text in a rect: vertically centred, left / centre / right (align 0, 0.5, 1), "..." when it doesn't fit.
void Draw_TextIn(DrawList *dl, Font *font, Rect r, u32 color, String text, f32 align);
// The terminal's cells (or anything else drawn outside the quads): `data` lives until the frame is drawn; `hash`
// says when it changed.
void Draw_External(DrawList *dl, Rect r, const void *data, u64 hash);

static inline Rect R4(s32 x0, s32 y0, s32 x1, s32 y1) { Rect r = { x0, y0, x1, y1 }; return r; }
static inline s32 RectW(Rect r) { return r.x1 - r.x0; }
static inline s32 RectH(Rect r) { return r.y1 - r.y0; }
static inline b32 RectHas(Rect r, s32 x, s32 y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }
static inline Rect RectShrink(Rect r, s32 d) { Rect o = { r.x0 + d, r.y0 + d, r.x1 - d, r.y1 - d }; return o; }
static inline Rect RectAnd(Rect a, Rect b)
{
    Rect o = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    if (o.x1 < o.x0) o.x1 = o.x0;
    if (o.y1 < o.y0) o.y1 = o.y0;
    return o;
}
// Cut a strip off one side of r (r keeps the rest).
static inline Rect CutLeft(Rect *r, s32 w)   { Rect o = { r->x0, r->y0, r->x0 + w < r->x1 ? r->x0 + w : r->x1, r->y1 }; r->x0 = o.x1; return o; }
static inline Rect CutRight(Rect *r, s32 w)  { Rect o = { r->x1 - w > r->x0 ? r->x1 - w : r->x0, r->y0, r->x1, r->y1 }; r->x1 = o.x0; return o; }
static inline Rect CutTop(Rect *r, s32 h)    { Rect o = { r->x0, r->y0, r->x1, r->y0 + h < r->y1 ? r->y0 + h : r->y1 }; r->y0 = o.y1; return o; }
static inline Rect CutBottom(Rect *r, s32 h) { Rect o = { r->x0, r->y1 - h > r->y0 ? r->y1 - h : r->y0, r->x1, r->y1 }; r->y1 = o.y0; return o; }
// A w x h rect centred in r.
static inline Rect RectCenter(Rect r, s32 w, s32 h)
{
    Rect o = { r.x0 + (RectW(r) - w) / 2, r.y0 + (RectH(r) - h) / 2, 0, 0 };
    o.x1 = o.x0 + w;
    o.y1 = o.y0 + h;
    return o;
}

static inline u32 ColorMix(u32 a, u32 b, f32 t)   // a towards b, alpha too
{
    u32 out = 0;
    for (s32 s = 0; s < 32; s += 8) {
        f32 x = (f32)((a >> s) & 0xFF), y = (f32)((b >> s) & 0xFF);
        out |= (u32)(x + (y - x) * t + 0.5f) << s;
    }
    return out;
}
static inline u32 ColorAlpha(u32 c, f32 a) { return (c & 0xFFFFFF) | ((u32)(a * (f32)(c >> 24) + 0.5f) << 24); }

#ifdef __cplusplus
}
#endif

#endif
