// term: immediate-mode UI (term_ui.h).
#include "term_ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void Ui_BeginFrame(Ui *ui, const UiInput *in, DrawList *dl, Arena *frame_arena, u64 now_us)
{
    ui->dt = ui->now_us ? (f32)(now_us - ui->now_us) / 1e6f : 0;
    if (ui->dt > 0.1f) ui->dt = 0.1f;
    ui->now_us = now_us;
    ui->in = *in;
    ui->dl = dl;
    ui->frame_arena = frame_arena;
    ui->frame++;
    ui->animating = 0;
    ui->cursor = Cursor_Arrow;
    ui->copied.count = 0;
    memset(ui->consumed, 0, sizeof(ui->consumed));

    // --- last frame's hit areas become this frame's to look up; this frame fills the other arena ---
    s32 k = (s32)(ui->frame & 1);
    ui->prev_hits.count = ui->hits.count;
    ui->prev_hits.capacity = ui->hits.capacity;
    ui->prev_hits.data = ui->hits.data;
    Arena_Clear(&ui->hit_arenas[k]);
    ui->hits.count = ui->hits.capacity = 0;
    ui->hits.data = NULL;

    // --- hot: the last area registered under the mouse (drawn on top) ---
    ui->hot = 0;
    for (s64 i = ui->prev_hits.count - 1; i >= 0; i--) {
        if (RectHas(ui->prev_hits.data[i].rect, in->mouse[0], in->mouse[1])) { ui->hot = ui->prev_hits.data[i].id; break; }
    }

    // --- a press makes the hot area active; its release (after the frame's signals) ends it ---
    for (s32 b = 0; b < Mouse_Count; b++) {
        if (!in->button[b].pressed) continue;
        ui->active = ui->hot;
        ui->active_button = b;
        ui->press_x = in->mouse[0];
        ui->press_y = in->mouse[1];
        ui->dragging = 0;
        b32 again = ui->hot && ui->hot == ui->last_click_id && now_us - ui->last_click_us <= (u64)(in->double_click_ms ? in->double_click_ms : 500) * 1000;
        ui->click_count = again ? ui->click_count % 3 + 1 : 1;
        ui->last_click_id = ui->hot;
        ui->last_click_us = now_us;
    }
}

void Ui_EndFrame(Ui *ui)
{
    // --- the tooltip: after 600 ms on the same area, by the mouse ---
    if (ui->tip_id && ui->tip_id == ui->hot && !ui->active && ui->tip_font && ui->tip_text.count) {
        if (!ui->tip_since_us) ui->tip_since_us = ui->now_us;
        b32 warm = ui->tip_shown_us && ui->now_us - ui->tip_shown_us < 400000;   // moving from one tooltip to the next: no wait
        if (warm || ui->now_us - ui->tip_since_us >= 600000) {
            Font *f = ui->tip_font;
            s32 pad = (s32)(f->line_h * 0.45f);
            s32 w = Font_Measure(f, ui->tip_text) + 2 * pad, h = (s32)f->line_h + pad;
            s32 x = ui->in.mouse[0] + pad, y = ui->in.mouse[1] + (s32)(f->line_h * 1.4f);
            if (x + w > ui->dl->w) x = ui->dl->w - w - 2;
            if (y + h > ui->dl->h) y = ui->in.mouse[1] - h - pad;
            Rect r = R4(x, y, x + w, y + h);
            Draw_RectEx(ui->dl, r, ui->tip_bg, ui->tip_border, 1, (f32)pad * 0.5f, NULL, 0, 0, 0);
            Draw_Text(ui->dl, f, x + pad, y + pad / 2, ui->tip_fg, ui->tip_text);
            ui->tip_shown_us = ui->now_us;
        } else {
            ui->animating = 1;   // a frame when it's due
        }
    } else {
        ui->tip_id = 0;
        ui->tip_since_us = 0;
    }
    ui->tip_text.count = 0;
    for (s32 b = 0; b < Mouse_Count; b++) {
        if (ui->in.button[b].released && b == ui->active_button) { ui->active = 0; ui->dragging = 0; }
    }
    if (ui->active && !ui->in.button[ui->active_button].down) { ui->active = 0; ui->dragging = 0; }
}

u64 Ui_IdS(String s)
{
    return Hash_Bytes(s.data, (u64)s.count, 0x1D5EEDull) | 1;
}

u64 Ui_Id(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    String s = { (u8 *)buf, n < 0 ? 0 : Min(n, (int)sizeof(buf) - 1) };
    return Ui_IdS(s);
}

UiSignal Ui_Hit(Ui *ui, u64 id, Rect r)
{
    UiSignal s = {0};
    Rect clipped = RectAnd(r, ui->dl->clips[ui->dl->clip_depth]);
    if (RectW(clipped) > 0 && RectH(clipped) > 0) {
        UiHit *h = Array_Push(&ui->hit_arenas[ui->frame & 1], ui->hits);
        h->id = id;
        h->rect = clipped;
    }
    s.hovered = ui->hot == id && (!ui->active || ui->active == id);   // while something is held, nothing else lights up
    for (s32 b = 0; b < Mouse_Count; b++) {
        if (ui->in.button[b].pressed && ui->active == id && ui->active_button == b) {
            s.pressed = 1;
            s.button = b;
            s.clicks = ui->click_count;
        }
    }
    if (ui->active == id) {
        s.button = ui->active_button;
        s.held = ui->in.button[ui->active_button].down;
        s.released = ui->in.button[ui->active_button].released;
        s.clicked = s.released && s.hovered;
    }
    return s;
}

f32 Ui_Animate(Ui *ui, u64 id, f32 from, f32 target, f32 rate)
{
    u32 cap = (u32)ArrayCount(ui->anims);
    u32 at = (u32)(id & (cap - 1));
    UiAnim *a = NULL, *free_slot = NULL;
    for (u32 probe = 0; probe < 16 && !a; probe++) {
        UiAnim *x = &ui->anims[(at + probe) & (cap - 1)];
        if (x->id == id) a = x;
        else if (!free_slot && (!x->id || x->frame + 2 < ui->frame)) free_slot = x;   // one nothing has used lately
    }
    if (!a && !free_slot) return target;
    if (!a) { a = free_slot; a->id = id; a->value = from; }
    if (a->frame + 1 < ui->frame) a->value = from;   // not asked for last frame: it starts again
    a->frame = ui->frame;
    f32 t = 1.0f - expf(-rate * ui->dt);
    a->value += (target - a->value) * t;
    if (fabsf(target - a->value) < 0.002f * (fabsf(target) + 1.0f)) a->value = target;
    else ui->animating = 1;
    return a->value;
}

void Ui_Tooltip(Ui *ui, u64 id, String text)
{
    if (ui->hot != id) return;
    if (ui->tip_id != id) { ui->tip_id = id; ui->tip_since_us = 0; }
    ui->tip_text = Str_PushCopy(ui->frame_arena, text);
}

b32 Ui_TakeKey(Ui *ui, u8 key, u8 modifiers)
{
    for (s64 i = 0; i < ui->in.event_count && i < 256; i++) {
        const UiKeyEvent *e = &ui->in.events[i];
        if (ui->consumed[i] || !e->down || e->key != key || (e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift)) != modifiers) continue;
        ui->consumed[i] = 1;
        return 1;
    }
    return 0;
}

void TextEdit_Set(TextEdit *e, String s)
{
    e->count = (s32)Min(s.count, (s64)sizeof(e->buffer));
    memcpy(e->buffer, s.data, (size_t)e->count);
    e->mark = 0;
    e->cursor = e->count;
}

// A word character for Ctrl+arrows and Ctrl+Backspace (anything above ASCII counts).
static b32 WordByte(u8 c) { return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

// The caret one codepoint (or, with ctrl, one word) to the left / right of at, in buf's count bytes.
static s32 StepIn(const u8 *buf, s32 count, s32 at, s32 dir, b32 word)
{
    if (dir < 0) {
        if (word) {
            while (at > 0 && !WordByte(buf[at - 1])) at--;
            while (at > 0 && WordByte(buf[at - 1])) at--;
            return at;
        }
        if (at > 0) at--;
        while (at > 0 && (buf[at] & 0xC0) == 0x80) at--;
        return at;
    }
    if (word) {
        while (at < count && WordByte(buf[at])) at++;
        while (at < count && !WordByte(buf[at])) at++;
        return at;
    }
    if (at < count) at++;
    while (at < count && (buf[at] & 0xC0) == 0x80) at++;
    return at;
}

static s32 Step(TextEdit *e, s32 at, s32 dir, b32 word) { return StepIn(e->buffer, e->count, at, dir, word); }

// --- the many-line edit (TextArea) ---

typedef struct AreaLine { s32 from, to; } AreaLine;   // a line on screen: bytes [from, to), its '\n' not included

// The lines text wraps to in width px: at each '\n', else after the last space that fits (a word wider than the line
// where it stops fitting, a codepoint at least). An empty text is one empty line.
static s32 AreaLayout(Arena *a, String text, Font *font, s32 width, AreaLine **out)
{
    AreaLine *lines = Arena_PushArray(a, AreaLine, text.count + 1);
    s32 n = 0;
    for (s64 at = 0;;) {
        s64 end = at;
        while (end < text.count && text.data[end] != '\n') end++;
        s64 p = at;
        do {
            s64 fit = p + Font_Fit(font, (String){ text.data + p, end - p }, width);
            if (fit < end) {
                s64 brk = fit;
                while (brk > p && text.data[brk - 1] != ' ') brk--;
                if (brk > p) fit = brk;
                if (fit == p) { fit = p + 1; while (fit < end && (text.data[fit] & 0xC0) == 0x80) fit++; }
            }
            lines[n++] = (AreaLine){ (s32)p, (s32)fit };
            p = fit;
        } while (p < end);
        if (end >= text.count) break;
        at = end + 1;
    }
    *out = lines;
    return n;
}

// The line a byte is on (a wrap point belongs to the line it starts).
static s32 AreaLineOf(const AreaLine *lines, s32 n, s32 at)
{
    s32 k = 0;
    while (k + 1 < n && lines[k + 1].from <= at) k++;
    return k;
}

// Before an edit: the text as it is goes on the undo list, unless this edit goes on the same burst (within a second).
static void AreaSnapshot(TextArea *e, u64 now_us)
{
    b32 burst = e->undo_len && now_us - e->undo_us < 1000000;
    e->undo_us = now_us;
    if (burst) return;
    if (e->undo_len == TextArea_Undos) {
        memmove(e->undo[0], e->undo[1], sizeof(e->undo[0]) * (TextArea_Undos - 1));
        memmove(e->undo_count, e->undo_count + 1, sizeof(s32) * (TextArea_Undos - 1));
        memmove(e->undo_cursor, e->undo_cursor + 1, sizeof(s32) * (TextArea_Undos - 1));
        e->undo_len--;
    }
    memcpy(e->undo[e->undo_len], e->buffer, (size_t)e->count);
    e->undo_count[e->undo_len] = e->count;
    e->undo_cursor[e->undo_len] = e->cursor;
    e->undo_len++;
}

static void AreaReplace(TextArea *e, s32 from, s32 to, String with)
{
    s64 n = Min(with.count, (s64)TextArea_Max - (e->count - (to - from)));
    if (n < 0) n = 0;
    memmove(e->buffer + from + n, e->buffer + to, (size_t)(e->count - to));
    memcpy(e->buffer + from, with.data, (size_t)n);
    e->count += (s32)n - (to - from);
    e->cursor = e->mark = from + (s32)n;
}

void TextArea_Insert(TextArea *e, String s)
{
    AreaSnapshot(e, 0);
    e->undo_us = 0;   // its own burst
    AreaReplace(e, Min(e->cursor, e->mark), Max(e->cursor, e->mark), s);
}

s32 TextArea_Lines(Ui *ui, TextArea *e, Font *font, s32 width)
{
    AreaLine *lines = NULL;
    return AreaLayout(ui->frame_arena, (String){ e->buffer, e->count }, font, Max(1, width), &lines);
}

b32 Ui_TextArea(Ui *ui, u64 id, TextArea *e, Rect r, Font *font, b32 focused, String hint, u32 color, u32 hint_color, u32 selection_color)
{
    b32 changed = 0;
    UiSignal s = Ui_Hit(ui, id, r);
    if (s.hovered) ui->cursor = Cursor_IBeam;
    e->cursor = Clamp(e->cursor, 0, e->count);
    e->mark = Clamp(e->mark, 0, e->count);
    s32 lh = Max(1, (s32)(font->line_h + 0.5f)), rows = Max(1, RectH(r) / lh);
    AreaLine *lines = NULL;
    s32 n = AreaLayout(ui->frame_arena, (String){ e->buffer, e->count }, font, Max(1, RectW(r)), &lines);

    // --- the mouse: a press puts the caret there, a drag selects, a double click takes the word; the wheel scrolls ---
    if ((s.pressed || s.held) && s.button == Mouse_Left) {
        s32 k = Clamp((ui->in.mouse[1] - r.y0) / lh + e->top, 0, n - 1);
        s32 at = lines[k].from + (s32)Font_Fit(font, (String){ e->buffer + lines[k].from, lines[k].to - lines[k].from }, ui->in.mouse[0] - r.x0);
        e->cursor = at;
        if (s.pressed) e->mark = at;
        if (s.pressed && s.clicks >= 2) {
            e->mark = StepIn(e->buffer, e->count, StepIn(e->buffer, e->count, at, 1, 1), -1, 1);
            e->cursor = StepIn(e->buffer, e->count, e->mark, 1, 1);
        }
    }
    if (s.hovered && ui->in.wheel[1]) e->top -= ui->in.wheel[1] * 3 / 120;

    // --- the keys ---
    for (s64 i = 0; focused && i < ui->in.event_count && i < 256; i++) {
        const UiKeyEvent *ev = &ui->in.events[i];
        if (ui->consumed[i] || !ev->down) continue;
        b32 ctrl = (ev->modifiers & Mod_Ctrl) != 0, shift = (ev->modifiers & Mod_Shift) != 0;
        s32 lo = Min(e->cursor, e->mark), hi = Max(e->cursor, e->mark);
        b32 taken = 1, edit = 0;
        String with = {0};
        if (ev->key == Key_Left || ev->key == Key_Right) {
            s32 to = !shift && lo != hi ? (ev->key == Key_Left ? lo : hi) : StepIn(e->buffer, e->count, e->cursor, ev->key == Key_Left ? -1 : 1, ctrl);
            e->cursor = to;
            if (!shift) e->mark = to;
        } else if (ev->key == Key_Up || ev->key == Key_Down) {
            // the line above / below, as near the same x as it goes
            s32 k = AreaLineOf(lines, n, e->cursor), t = k + (ev->key == Key_Up ? -1 : 1);
            s32 to = t < 0 ? 0 : t >= n ? e->count : 0;
            if (t >= 0 && t < n) {
                s32 x = Font_Measure(font, (String){ e->buffer + lines[k].from, e->cursor - lines[k].from });
                to = lines[t].from + (s32)Font_Fit(font, (String){ e->buffer + lines[t].from, lines[t].to - lines[t].from }, x);
            }
            e->cursor = to;
            if (!shift) e->mark = to;
        } else if (ev->key == Key_Home || ev->key == Key_End) {
            s32 k = AreaLineOf(lines, n, e->cursor);
            e->cursor = ctrl ? (ev->key == Key_Home ? 0 : e->count) : (ev->key == Key_Home ? lines[k].from : lines[k].to);
            if (!shift) e->mark = e->cursor;
        } else if (ev->key == Key_Backspace || ev->key == Key_Delete) {
            if (lo == hi) {
                if (ev->key == Key_Backspace) lo = StepIn(e->buffer, e->count, lo, -1, ctrl);
                else hi = StepIn(e->buffer, e->count, hi, 1, ctrl);
            }
            edit = lo != hi;
        } else if ((ev->key == Key_Enter || ev->key == Key_KeypadEnter) && !ctrl) {
            with = S("\n");
            edit = 1;
        } else if (ctrl && ev->key == Key_A) {
            e->mark = 0;
            e->cursor = e->count;
        } else if (ctrl && !shift && ev->key == Key_A + 'Z' - 'A') {
            if (e->undo_len) {
                e->undo_len--;
                e->count = e->undo_count[e->undo_len];
                memcpy(e->buffer, e->undo[e->undo_len], (size_t)e->count);
                e->cursor = e->mark = Min(e->undo_cursor[e->undo_len], e->count);
                e->undo_us = 0;
                changed = 1;
            }
        } else if (ctrl && (ev->key == Key_A + 'C' - 'A' || ev->key == Key_A + 'X' - 'A')) {
            if (lo != hi) ui->copied = Str_PushCopy(ui->frame_arena, (String){ e->buffer + lo, hi - lo });
            edit = lo != hi && ev->key == Key_A + 'X' - 'A';
        } else if ((ctrl && ev->key == Key_A + 'V' - 'A') || (shift && ev->key == Key_Insert)) {
            // line ends as '\n'
            String paste = ui->in.clipboard;
            u8 *clean = Arena_PushArray(ui->frame_arena, u8, paste.count + 1);
            s64 m = 0;
            for (s64 k = 0; k < paste.count; k++) if (paste.data[k] != '\r') clean[m++] = paste.data[k];
            with = (String){ clean, m };
            edit = 1;
        } else if (ev->text.count && !ctrl && ev->text.data[0] >= 0x20 && ev->text.data[0] != 0x7F) {
            with = ev->text;
            edit = 1;
        } else {
            taken = 0;
        }
        if (edit) {
            AreaSnapshot(e, ui->now_us);
            AreaReplace(e, lo, hi, with);
            changed = 1;
        }
        if (taken) ui->consumed[i] = 1;
        if (taken) n = AreaLayout(ui->frame_arena, (String){ e->buffer, e->count }, font, Max(1, RectW(r)), &lines);
    }

    // --- in view: the caret's line (after a key or a click), else where the wheel left it ---
    s32 caret_line = AreaLineOf(lines, n, e->cursor);
    if (focused && (changed || s.pressed || s.held || ui->in.event_count)) {
        if (caret_line < e->top) e->top = caret_line;
        if (caret_line >= e->top + rows) e->top = caret_line - rows + 1;
    }
    e->top = Clamp(e->top, 0, Max(0, n - rows));

    // --- draw: the selection, the lines (or the hint), the caret ---
    Draw_PushClip(ui->dl, r);
    s32 lo = Min(e->cursor, e->mark), hi = Max(e->cursor, e->mark);
    for (s32 k = e->top; k < n && k < e->top + rows + 1; k++) {
        s32 y = r.y0 + (k - e->top) * lh;
        String line = { e->buffer + lines[k].from, lines[k].to - lines[k].from };
        if (focused && lo != hi && hi > lines[k].from && lo <= lines[k].to) {
            s32 a = Font_Measure(font, (String){ line.data, Max(lo, lines[k].from) - lines[k].from });
            s32 b = Font_Measure(font, (String){ line.data, Min(hi, lines[k].to) - lines[k].from }) + (hi > lines[k].to ? lh / 3 : 0);
            Draw_Rect(ui->dl, R4(r.x0 + a, y, r.x0 + b, y + lh), selection_color, 2);
        }
        if (line.count) Draw_Text(ui->dl, font, r.x0, y, color, line);
    }
    if (!e->count && hint.count) Draw_Text(ui->dl, font, r.x0, r.y0, hint_color, hint);
    if (focused) {
        s32 y = r.y0 + (caret_line - e->top) * lh;
        s32 cx = r.x0 + Font_Measure(font, (String){ e->buffer + lines[caret_line].from, e->cursor - lines[caret_line].from });
        Draw_Rect(ui->dl, R4(cx, y + 1, cx + Max(1, (s32)(font->size_px / 12)), y + lh - 1), color, 0);
    }
    Draw_PopClip(ui->dl);
    return changed;
}

static void EditReplace(TextEdit *e, s32 from, s32 to, String with)
{
    s64 n = Min(with.count, (s64)sizeof(e->buffer) - (e->count - (to - from)));
    if (n < 0) n = 0;
    memmove(e->buffer + from + n, e->buffer + to, (size_t)(e->count - to));
    memcpy(e->buffer + from, with.data, (size_t)n);
    e->count += (s32)n - (to - from);
    e->cursor = e->mark = from + (s32)n;
}

b32 Ui_TextEdit(Ui *ui, u64 id, TextEdit *e, Rect r, Font *font, b32 focused, String hint, u32 color, u32 hint_color, u32 selection_color)
{
    b32 changed = 0;
    UiSignal s = Ui_Hit(ui, id, r);
    if (s.hovered) ui->cursor = Cursor_IBeam;
    e->cursor = Clamp(e->cursor, 0, e->count);
    e->mark = Clamp(e->mark, 0, e->count);
    s32 scroll_x = 0;
    s32 caret_px = Font_Measure(font, (String){ e->buffer, e->cursor });
    if (caret_px > RectW(r) - 2) scroll_x = caret_px - (RectW(r) - 2);

    // --- the mouse: a press puts the caret there, a drag selects, a double click selects all ---
    if ((s.pressed || s.held) && s.button == Mouse_Left) {
        s32 x = ui->in.mouse[0] - r.x0 + scroll_x;
        s32 at = (s32)Font_Fit(font, (String){ e->buffer, e->count }, x);
        if (at < e->count) {   // nearer the next codepoint's left edge?
            s32 left = Font_Measure(font, (String){ e->buffer, at });
            s64 next = at;
            Utf8_Decode((String){ e->buffer, e->count }, &next);
            s32 right = Font_Measure(font, (String){ e->buffer, (s64)next });
            if (x - left > right - x) at = (s32)next;
        }
        e->cursor = at;
        if (s.pressed) e->mark = s.clicks >= 2 ? 0 : at;
        if (s.pressed && s.clicks >= 2) e->cursor = e->count;
    }

    // --- the keys ---
    for (s64 i = 0; focused && i < ui->in.event_count && i < 256; i++) {
        const UiKeyEvent *ev = &ui->in.events[i];
        if (ui->consumed[i] || !ev->down) continue;
        b32 ctrl = (ev->modifiers & Mod_Ctrl) != 0, shift = (ev->modifiers & Mod_Shift) != 0;
        s32 lo = Min(e->cursor, e->mark), hi = Max(e->cursor, e->mark);
        b32 taken = 1;
        if (ev->key == Key_Left || ev->key == Key_Right) {
            s32 to = !shift && lo != hi ? (ev->key == Key_Left ? lo : hi) : Step(e, e->cursor, ev->key == Key_Left ? -1 : 1, ctrl);
            e->cursor = to;
            if (!shift) e->mark = to;
        } else if (ev->key == Key_Home || ev->key == Key_End) {
            e->cursor = ev->key == Key_Home ? 0 : e->count;
            if (!shift) e->mark = e->cursor;
        } else if (ev->key == Key_Backspace || ev->key == Key_Delete) {
            if (lo == hi) {
                if (ev->key == Key_Backspace) lo = Step(e, lo, -1, ctrl);
                else hi = Step(e, hi, 1, ctrl);
            }
            if (lo != hi) { EditReplace(e, lo, hi, (String){0}); changed = 1; }
        } else if (ctrl && ev->key == Key_A) {
            e->mark = 0;
            e->cursor = e->count;
        } else if (ctrl && (ev->key == Key_A + 'C' - 'A' || ev->key == Key_A + 'X' - 'A')) {
            if (lo != hi) ui->copied = Str_PushCopy(ui->frame_arena, (String){ e->buffer + lo, hi - lo });
            if (lo != hi && ev->key == Key_A + 'X' - 'A') { EditReplace(e, lo, hi, (String){0}); changed = 1; }
        } else if ((ctrl && ev->key == Key_A + 'V' - 'A') || (shift && ev->key == Key_Insert)) {
            String paste = ui->in.clipboard;
            for (s64 k = 0; k < paste.count; k++) if (paste.data[k] == '\r' || paste.data[k] == '\n') { paste.count = k; break; }
            EditReplace(e, lo, hi, paste);
            changed = 1;
        } else if (ev->text.count && !ctrl && ev->text.data[0] >= 0x20 && ev->text.data[0] != 0x7F) {
            EditReplace(e, lo, hi, ev->text);
            changed = 1;
        } else {
            taken = 0;
        }
        if (taken) ui->consumed[i] = 1;
    }

    // --- draw: the selection, the text (or the hint), the caret ---
    caret_px = Font_Measure(font, (String){ e->buffer, e->cursor });
    scroll_x = caret_px > RectW(r) - 2 ? caret_px - (RectW(r) - 2) : 0;
    s32 y = r.y0 + (RectH(r) - (s32)(font->line_h + 0.5f)) / 2;
    Draw_PushClip(ui->dl, r);
    if (focused && e->cursor != e->mark) {
        s32 a = Font_Measure(font, (String){ e->buffer, Min(e->cursor, e->mark) });
        s32 b = Font_Measure(font, (String){ e->buffer, Max(e->cursor, e->mark) });
        Draw_Rect(ui->dl, R4(r.x0 + a - scroll_x, y, r.x0 + b - scroll_x, y + (s32)font->line_h), selection_color, 2);
    }
    if (e->count) Draw_Text(ui->dl, font, r.x0 - scroll_x, y, color, (String){ e->buffer, e->count });
    else if (hint.count) Draw_Text(ui->dl, font, r.x0, y, hint_color, hint);
    if (focused) {
        s32 cx = r.x0 + caret_px - scroll_x;
        Draw_Rect(ui->dl, R4(cx, y + 1, cx + Max(1, (s32)(font->size_px / 12)), y + (s32)font->line_h - 1), color, 0);
    }
    Draw_PopClip(ui->dl);
    return changed;
}
