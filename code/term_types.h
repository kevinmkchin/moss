// term: what the window's app (term_app.c, pure C) knows of the server's tabs, and what it asks of them.
// The platform side (term_client.cpp) fills these from the server's messages and carries out the requests.
#ifndef TERM_TYPES_H
#define TERM_TYPES_H

#include "term_core.h"

// One cell as the server's emulator keeps it (term_base.h `cell`, same layout).
typedef struct TermCell {
    u32 codepoint;
    u32 fg;         // 0 = the terminal's default, else tagged (TermColor_*)
    u32 bg;
    u16 attr;       // TermAttr_*
    u16 flags;      // TermCell_*
} TermCell;

#define TermColor_Palette 0x01000000u   // + index 0..255
#define TermColor_Rgb     0x02000000u   // + 0xRRGGBB
#define TermColor_Tag(c)  ((c) & 0xFF000000u)

enum {
    TermAttr_Bold = 1 << 0, TermAttr_Dim = 1 << 1, TermAttr_Italic = 1 << 2, TermAttr_Underline = 1 << 3,
    TermAttr_Blink = 1 << 4, TermAttr_Inverse = 1 << 5, TermAttr_Hidden = 1 << 6, TermAttr_Strike = 1 << 7,
};
enum { TermCell_Wide = 1 << 0, TermCell_WideCont = 1 << 1, TermCell_Found = 1 << 2, TermCell_FoundCurrent = 1 << 3, TermCell_Wrapped = 1 << 4, TermCell_LinkShift = 5 };

// A tab as the server lists it (msg_tab). Strings live until the next frame.
enum {
    TermTab_Agent = 1 << 0,       // a CLI agent runs in it (`agent` names it; claude: key is its session id)
    TermTab_Busy = 1 << 1,        // a claude turn is under way (title spinner)
    TermTab_NeedsInput = 1 << 2,  // claude asked for the user and nothing was typed since
    TermTab_Exited = 1 << 3,
    TermTab_Running = 1 << 4,     // a shell running a program
    TermTab_AccountHeld = 1 << 5, // its claude's account changed: stopped, input dropped until AppOutput.account_ok
};
typedef struct TermTab {
    u32 id;          // the server's id (0 = none)
    u32 request;     // the window's new-tab request that opened it (0 = restored)
    u32 flags;       // TermTab_*
    f32 cpu;         // percent of the whole machine
    String key;      // lasting id: claude session id, else the shell's history id
    String name;
    String cwd;
    String notice;   // why it wants a look, while no pane showed it
    String agent;    // "claude", "codex", ... ; empty = a plain shell
    String owner;    // the window it belongs to (its name; empty = the main window)
    String account;  // claude: login method, organization, email, model ('\n' between; empty = no claude)
    String account_was; // TermTab_AccountHeld: the account before it changed, the same way
} TermTab;

// The screen of a tab a pane shows (the latest msg_cells applied). cells: cols * rows, row-major.
enum {
    TermGrid_CursorHidden = 1 << 1, TermGrid_AltScreen = 1 << 2, TermGrid_AppCursorKeys = 1 << 3,
    TermGrid_BracketedPaste = 1 << 4, TermGrid_FocusEvents = 1 << 5, TermGrid_Sync = 1 << 6, TermGrid_MouseSgr = 1 << 7,
    TermGrid_Win32Input = 1 << 8,
};
typedef struct TermGrid {
    u32 tab_id;
    s32 cols, rows;
    const TermCell *cells;
    s32 cursor_x, cursor_y;
    s32 cursor_shape;   // DECSCUSR: 0 default, 1/2 block, 3/4 underline, 5/6 bar (odd blink)
    u32 flags;          // TermGrid_*
    s32 mouse_mode;     // 0, 1000, 1002, 1003
    s32 scroll_offset;  // lines scrolled back (0 = live)
    s32 scrollback;     // lines there are
    u64 version;        // changes whenever anything above does
} TermGrid;

// --- requests from the app ---
typedef struct TermWatch { u32 tab_id; s32 cols, rows; b32 shown; } TermWatch;   // every tab the window has
typedef struct TermInput { u32 tab_id; String bytes; } TermInput;
enum { TermNew_Shell = 0, TermNew_Claude = 1, TermNew_Agent = 2 };
// command: the agent's (TermNew_Agent)
typedef struct TermNewTab { u32 request; u32 kind; s32 cols, rows; String cwd; String session; String command; } TermNewTab;
// A terminal theme: the terminal's colors (0xRRGGBB); the chrome is made from them.
typedef struct TermTheme {
    const char *name;
    b32 light;
    u32 fg, bg, selection, accent;
    u32 ansi[16];
} TermTheme;

typedef struct TermScroll { u32 tab_id; s32 lines; } TermScroll;   // lines > 0 back; TermScroll_ToBottom = live
#define TermScroll_ToBottom (-0x7fffffff)
#define TermScroll_PrevMark (-0x7ffffffe)   // the prompt above the view's top at the top
#define TermScroll_NextMark (-0x7ffffffd)   // the next one (live after the last)
#define TermScroll_Clear (-0x7ffffffc)      // forget the scrollback
#define TermScroll_CopyAll (-0x7ffffffb)    // everything in it as text, to the clipboard

// One message of a claude session's history (its transcript): who said it, when (ISO 8601, UTC), what.
typedef struct TermHistoryEntry { b32 from_user; String time; String text; } TermHistoryEntry;

// A Claude Code session on disk (<claude dir>/projects/<folder>/<id>.jsonl). Strings live until the next frame.
typedef struct TermSession {
    String id;
    String cwd;       // the folder it ran in
    String project;   // that folder's name
    String title;     // its first prompt, on one line
    u64 modified;     // its transcript's last write (FILETIME, UTC)
} TermSession;

// The terminal colours of a theme (0xRRGGBB): what cells with default and palette colours look like.
typedef struct TermPalette {
    u32 fg, bg, selection, cursor;
    u32 found, found_current;   // the find bar's matches' background, and the current one's
    u32 link_hover;             // the OSC 8 link under the mouse with Ctrl held: underlined (0 = none)
    s32 underline_row, underline_x, underline_len;   // a web address under the mouse with Ctrl held (len 0 = none)
    b32 ligatures;              // runs of symbols (-> != ===) drawn together, so a font's ligatures show
    u32 ansi[16];
} TermPalette;

// One pane's terminal as the renderer draws it (RenderCommand_Cells): the grid of `tab_id` at `origin`
// (window pixels), clipped to the command's clip rect, with the app's overlays.
typedef struct TermCellsDraw {
    u32 tab_id;
    s32 origin[2];
    s32 sel[4];          // selection start x, y and end x, y (inclusive, reading order); sel_on = 0: none
    b32 sel_on;
    b32 sel_block;       // a rectangle instead of reading order
    b32 cursor_on;       // draw the cursor (focused pane, blink phase on)
    b32 cursor_hollow;   // the window or pane isn't focused: an outline block
    s32 cursor_shape;    // DECSCUSR as drawn (the program's, else the settings' cursor)
    f32 opacity;         // default backgrounds: 0..1
    TermPalette palette;
} TermCellsDraw;

#endif
