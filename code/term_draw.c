// term: the draw list (term_draw.h).
#include "term_draw.h"
#include <string.h>

void DrawList_Begin(DrawList *dl, Arena *arena, s32 w, s32 h)
{
    DrawList zero = {0};
    *dl = zero;
    dl->arena = arena;
    dl->w = w;
    dl->h = h;
    dl->clips[0] = R4(0, 0, w, h);
    dl->hash = 0x7E57u;
}

void Draw_PushClip(DrawList *dl, Rect r)
{
    if (dl->clip_depth + 1 >= (s32)ArrayCount(dl->clips)) return;
    dl->clips[dl->clip_depth + 1] = RectAnd(dl->clips[dl->clip_depth], r);
    dl->clip_depth++;
}

void Draw_PopClip(DrawList *dl)
{
    if (dl->clip_depth > 0) dl->clip_depth--;
}

// The quad batch the next quad goes in: the last one when it has the same clip and texture, else a new one.
static Quad *NextQuad(DrawList *dl, u64 texture)
{
    Rect clip = dl->clips[dl->clip_depth];
    DrawBatch *b = dl->batches.count ? &dl->batches.data[dl->batches.count - 1] : NULL;
    if (!b || b->kind != Batch_Quads || b->texture != texture || memcmp(&b->clip, &clip, sizeof(clip)) != 0) {
        b = Array_Push(dl->arena, dl->batches);
        b->kind = Batch_Quads;
        b->clip = clip;
        b->texture = texture;
        b->first = dl->quads.count;
    }
    b->count++;
    Quad *q = Array_Push(dl->arena, dl->quads);
    return q;
}

static b32 Clipped(DrawList *dl, f32 x0, f32 y0, f32 x1, f32 y1)
{
    Rect c = dl->clips[dl->clip_depth];
    return x1 <= (f32)c.x0 || y1 <= (f32)c.y0 || x0 >= (f32)c.x1 || y0 >= (f32)c.y1 || x1 <= x0 || y1 <= y0;
}

// A color as drawn while the list fades (DrawList.fade).
static u32 Faded(DrawList *dl, u32 c) { return dl->fade > 0 ? ColorAlpha(c, 1.0f - dl->fade) : c; }

static void HashQuad(DrawList *dl, const Quad *q)
{
    dl->hash = Hash_Bytes(q, sizeof(*q), dl->hash);
}

void Draw_RectEx(DrawList *dl, Rect r, u32 fill, u32 border_color, f32 border, f32 radius, const f32 *radii, f32 dash, f32 gap, f32 softness)
{
    f32 grow = softness;   // a shadow reaches past the rect
    if (Clipped(dl, (f32)r.x0 - grow, (f32)r.y0 - grow, (f32)r.x1 + grow, (f32)r.y1 + grow)) return;
    if (!(fill >> 24) && (!(border_color >> 24) || border <= 0)) return;
    Quad *q = NextQuad(dl, 0);
    q->rect[0] = (f32)r.x0; q->rect[1] = (f32)r.y0; q->rect[2] = (f32)r.x1; q->rect[3] = (f32)r.y1;
    q->color = Faded(dl, fill);
    q->border_color = Faded(dl, border_color);
    q->border = border;
    for (s32 k = 0; k < 4; k++) q->radius[k] = radii ? radii[k] : radius;
    q->dash = dash;
    q->gap = gap;
    q->softness = softness;
    q->kind = Quad_Rect;
    HashQuad(dl, q);
}

void Draw_Rect(DrawList *dl, Rect r, u32 color, f32 radius)
{
    Draw_RectEx(dl, r, color, 0, 0, radius, NULL, 0, 0, 0);
}

void Draw_Image(DrawList *dl, Rect r, u64 texture, u32 tint)
{
    if (Clipped(dl, (f32)r.x0, (f32)r.y0, (f32)r.x1, (f32)r.y1) || !texture) return;
    Quad *q = NextQuad(dl, texture);
    q->rect[0] = (f32)r.x0; q->rect[1] = (f32)r.y0; q->rect[2] = (f32)r.x1; q->rect[3] = (f32)r.y1;
    q->uv[2] = q->uv[3] = 1;
    q->color = Faded(dl, tint);
    q->kind = Quad_Image;
    HashQuad(dl, q);
    dl->hash = Hash_Bytes(&texture, sizeof(texture), dl->hash);
}

s32 Draw_TextMarked(DrawList *dl, Font *font, s32 x, s32 y, u32 color, String text, const u32 *marks, s64 mark_count, u32 mark_color)
{
    f32 pen = (f32)x;
    s32 baseline = y + (s32)(font->ascent + 0.5f);
    Rect clip = dl->clips[dl->clip_depth];
    s64 index = 0, mark = 0;
    for (s64 at = 0; at < text.count; index++) {
        u32 cp = Utf8_Decode(text, &at);
        if (cp == '\t') cp = ' ';
        Glyph *g = Font_Glyph(font, cp);
        if (!g) continue;
        while (mark < mark_count && marks[mark] < (u32)index) mark++;
        u32 c = mark < mark_count && marks[mark] == (u32)index ? mark_color : color;
        if (g->w && pen < (f32)clip.x1) {
            f32 gx = (f32)(s32)(pen + 0.5f) + (f32)g->left, gy = (f32)(baseline - g->top);
            if (!Clipped(dl, gx, gy, gx + g->w, gy + g->h)) {
                Quad *q = NextQuad(dl, 0);
                q->rect[0] = gx; q->rect[1] = gy; q->rect[2] = gx + g->w; q->rect[3] = gy + g->h;
                f32 iw = 1.0f / (f32)font->atlas->w, ih = 1.0f / (f32)font->atlas->h;
                q->uv[0] = (f32)g->x * iw; q->uv[1] = (f32)g->y * ih;
                q->uv[2] = (f32)(g->x + g->w) * iw; q->uv[3] = (f32)(g->y + g->h) * ih;
                q->color = Faded(dl, c);
                q->kind = Quad_Glyph;
                HashQuad(dl, q);
            }
        }
        pen += g->advance;
    }
    return (s32)(pen + 0.5f);
}

s32 Draw_Text(DrawList *dl, Font *font, s32 x, s32 y, u32 color, String text)
{
    return Draw_TextMarked(dl, font, x, y, color, text, NULL, 0, 0);
}

void Draw_TextIn(DrawList *dl, Font *font, Rect r, u32 color, String text, f32 align)
{
    s32 y = r.y0 + (RectH(r) - (s32)(font->line_h + 0.5f)) / 2;
    s32 w = Font_Measure(font, text);
    if (w <= RectW(r)) {
        Draw_Text(dl, font, r.x0 + (s32)((f32)(RectW(r) - w) * align), y, color, text);
        return;
    }
    // too long: what fits, then an ellipsis
    String dots = S("...");
    s32 dots_w = Font_Measure(font, dots);
    String head = { text.data, Font_Fit(font, text, RectW(r) - dots_w) };
    s32 pen = Draw_Text(dl, font, r.x0, y, color, head);
    Draw_PushClip(dl, r);
    Draw_Text(dl, font, pen, y, color, dots);
    Draw_PopClip(dl);
}

void Draw_External(DrawList *dl, Rect r, const void *data, u64 hash)
{
    DrawBatch *b = Array_Push(dl->arena, dl->batches);
    b->kind = Batch_External;
    b->clip = RectAnd(dl->clips[dl->clip_depth], r);
    b->external = data;
    dl->hash = Hash_Bytes(&b->clip, sizeof(b->clip), dl->hash ^ hash);
}

s32 Font_Measure(Font *font, String text)
{
    f32 w = 0;
    for (s64 at = 0; at < text.count;) {
        u32 cp = Utf8_Decode(text, &at);
        Glyph *g = Font_Glyph(font, cp == '\t' ? ' ' : cp);
        if (g) w += g->advance;
    }
    return (s32)(w + 0.999f);
}

s64 Font_Fit(Font *font, String text, s32 max_w)
{
    f32 w = 0;
    for (s64 at = 0; at < text.count;) {
        s64 start = at;
        u32 cp = Utf8_Decode(text, &at);
        Glyph *g = Font_Glyph(font, cp == '\t' ? ' ' : cp);
        if (g) w += g->advance;
        if (w > (f32)max_w) return start;
    }
    return text.count;
}
