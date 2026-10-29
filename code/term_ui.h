// term: immediate-mode UI. The app lays out with rects (CutLeft & co. in term_draw.h), draws into the frame's
// DrawList and asks Ui_Hit what the mouse did to each id. Hover comes from last frame's hit areas (the last one
// registered under the mouse, i.e. the one drawn on top), so call order doesn't decide who wins.
#ifndef TERM_UI_H
#define TERM_UI_H

#include "term_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

// Key codes (the platform's VK table).
enum {
    Key_Space = 5, Key_Tab = 6, Key_Enter = 7, Key_Backspace = 8, Key_Delete = 9, Key_Escape = 10, Key_PageUp = 11,
    Key_PageDown = 12, Key_Home = 13, Key_End = 14, Key_Left = 15, Key_Right = 16, Key_Up = 17, Key_Down = 18,
    Key_Equals = 20, Key_Minus = 21, Key_Comma = 23, Key_Slash = 24, Key_KeypadEnter = 32, Key_Insert = 46, Key_0 = 48,
    Key_A = 65, Key_F1 = 94,
};
enum { Mod_Shift = 1, Mod_Ctrl = 2, Mod_Alt = 4 };
enum { Mouse_Left = 0, Mouse_Right = 1, Mouse_Middle = 2, Mouse_Count = 3 };
enum { Cursor_Arrow = 0, Cursor_Hand, Cursor_IBeam, Cursor_SizeWE, Cursor_SizeNS };

typedef struct UiKeyEvent {
    u8 key;              // Key_* (0: text with no key, e.g. from an IME)
    u8 modifiers;        // Mod_*
    u8 down, up;
    u32 repeat_count;
    String text;         // the character it typed (UTF-8), if any
    u16 vk, scan;        // the Windows key (win32-input-mode)
    u16 control_state;   // KEY_EVENT_RECORD's dwControlKeyState
} UiKeyEvent;

typedef struct UiButton { u8 down, pressed, released; } UiButton;

typedef struct UiInput {
    s32 mouse[2];        // window px (far outside when the mouse isn't over the window)
    UiButton button[Mouse_Count];
    s32 wheel[2];        // 120 a notch; [1] > 0: up
    s32 side;            // the mouse's side buttons this frame: -1 back (XBUTTON1), +1 forward (XBUTTON2)
    u8 modifiers;
    s64 event_count;
    UiKeyEvent *events;
    u32 double_click_ms;
    s32 wheel_scroll_lines;
    String clipboard;    // the clipboard's text, when the frame has a Ctrl+V or Shift+Insert
} UiInput;

typedef struct UiSignal {
    b32 hovered;         // on top under the mouse
    b32 pressed;         // a button went down on it this frame (button, clicks: 1, 2, 3)
    b32 held;            // pressed on it and still down
    b32 released;        // let go after a press on it
    b32 clicked;         // let go over it after a press on it
    s32 button, clicks;
} UiSignal;

typedef struct UiHit { u64 id; Rect rect; } UiHit;
typedef struct UiAnim { u64 id; f32 value; u32 frame; } UiAnim;

// An edited line of text: buffer, caret and the other end of the selection.
typedef struct TextEdit { s32 count, cursor, mark; u8 buffer[1024]; } TextEdit;

typedef struct Ui {
    UiInput in;
    DrawList *dl;
    Arena *frame_arena;
    u64 now_us;
    f32 dt;
    u32 frame;
    Arena hit_arenas[2];
    Array(UiHit) hits;   // this frame's
    Array(UiHit) prev_hits;
    u64 hot, active;
    s32 active_button;
    s32 press_x, press_y;   // where the active press began
    u64 last_click_id, last_click_us;
    s32 click_count;
    b32 dragging;        // the app's: the active press became a drag
    u8 cursor;           // Cursor_* for the platform
    b32 animating;       // an animation hasn't settled: keep frames coming
    u8 consumed[256];    // key events something took
    UiAnim anims[512];
    u64 tip_id, tip_since_us;
    u64 tip_shown_us;    // a tooltip was last on screen then: the next one shows at once (warm) for a moment
    String tip_text;
    Font *tip_font;
    u32 tip_fg, tip_bg, tip_border;
    String copied;       // a text edit's Ctrl+C / Ctrl+X: the app puts it on the clipboard
} Ui;

void     Ui_BeginFrame(Ui *ui, const UiInput *in, DrawList *dl, Arena *frame_arena, u64 now_us);
void     Ui_EndFrame(Ui *ui);   // the tooltip, last (on top)
u64      Ui_Id(const char *fmt, ...);
u64      Ui_IdS(String s);
static inline u64 Ui_IdN(u64 base, u64 n) { return (base ^ (n * 0x9E3779B97F4A7C15ull)) + 0x632BE59BD9B4E019ull; }
UiSignal Ui_Hit(Ui *ui, u64 id, Rect r);
// Eases towards target, `rate` per second (15: snappy); a new one (or one unused for a frame) starts at `from`.
f32      Ui_Animate(Ui *ui, u64 id, f32 from, f32 target, f32 rate);
void     Ui_Tooltip(Ui *ui, u64 id, String text);             // shown after a moment hovering id
// The first key press (not yet taken) of key with exactly these modifiers: taken, 1.
b32      Ui_TakeKey(Ui *ui, u8 key, u8 modifiers);
// A one-line edit in r. While `focused` it takes the keys (typing, arrows, Home/End, Shift to select, Ctrl+A,
// Backspace/Delete, Ctrl+C/X/V); returns 1 when the text changed. Draws the text, selection and caret.
b32      Ui_TextEdit(Ui *ui, u64 id, TextEdit *e, Rect r, Font *font, b32 focused, String hint, u32 color, u32 hint_color, u32 selection_color);
static inline String TextEdit_String(TextEdit *e) { String s = { e->buffer, e->count }; return s; }
void     TextEdit_Set(TextEdit *e, String s);   // all of it selected

// Edited text of many lines (the prompt composer): buffer, caret, selection, the first line shown, and undo (the text
// as it was before each burst of editing, the newest last).
#define TextArea_Max 16384
#define TextArea_Undos 8
typedef struct TextArea {
    s32 count, cursor, mark;
    s32 top;
    u8 buffer[TextArea_Max];
    u8 undo[TextArea_Undos][TextArea_Max];
    s32 undo_count[TextArea_Undos], undo_cursor[TextArea_Undos];
    s32 undo_len;
    u64 undo_us;              // the last edit (a pause starts a new burst)
} TextArea;
// A many-line edit in r, its lines wrapped to the width. While `focused` it takes the keys TextEdit does, and Enter (a
// new line), Up / Down, Ctrl+Home / End, Ctrl+Z (undo), the wheel scrolls; Ctrl+Enter is left for the caller. Returns 1
// when the text changed. Draws the text, selection and caret.
b32      Ui_TextArea(Ui *ui, u64 id, TextArea *e, Rect r, Font *font, b32 focused, String hint, u32 color, u32 hint_color, u32 selection_color);
s32      TextArea_Lines(Ui *ui, TextArea *e, Font *font, s32 width);   // how many lines it wraps to in width px
void     TextArea_Insert(TextArea *e, String s);                       // at the caret, over the selection (undoable)
static inline String TextArea_String(TextArea *e) { String s = { e->buffer, e->count }; return s; }

#ifdef __cplusplus
}
#endif

#endif
