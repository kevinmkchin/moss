// term: the window's app <-> platform contract. The app (term_app.c) is pure: each frame it reads AppFrameInput
// (input, window state, the server's tabs and the grids it shows, folder listings), fills its DrawList and AppOutput,
// and asks the platform for every side effect (server requests, clipboard, window commands, session file) through
// AppOutput. The window loop and the headless script runner both drive it.
#ifndef TERM_APP_H
#define TERM_APP_H

#include "term_ui.h"
#include "term_files.h"
#include "term_types.h"

// The UI's fonts, made by the platform for the window's DPI (term_font.cpp): Segoe UI, Segoe Fluent Icons.
enum { Font_Ui = 0, Font_UiSmall = 1, Font_UiBold = 2, Font_Icon = 3, Font_IconSmall = 4, Font_Count = 5 };

typedef struct AppPlace { String name, path; } AppPlace;
// A picture for the image viewer, decoded (scaled down to fit 4K) and on the GPU; failed: it couldn't be read.
typedef struct AppImage { String path; u64 texture; s32 w, h, full_w, full_h; b32 failed; } AppImage;   // a known folder: Home, Desktop, Documents, ...

typedef struct AppInit {
    String session_text;        // the saved layout, empty when none
    String settings_theme;      // settings.txt `theme`
    f32 font_size_pt;           // settings.txt `font_size`, 0 = default
    String agent_command;       // settings.txt `agent`: what Ctrl+Shift+A starts (empty = claude)
    b32 headless;               // no window: no blink timers
    f32 opacity;                // settings.txt `opacity` (20-100 %, as 0..1): terminals' default backgrounds
    String term_font;           // settings.txt `font`: the terminal font family
    String agents;              // settings.txt `agents` (the server's agent names; empty = its defaults)
    b32 hide_hidden;            // settings.txt `hidden_files = off`
    b32 folder_sizes_off;       // settings.txt `folder_sizes = off`
    b32 notifications_off;      // settings.txt `notifications = off`
    b32 vs_env_off;             // settings.txt `vs_env = off`
    String window_name;         // this window's name (`--window`): it shows the tabs the server says are its; empty = the main window
    u8 backdrop;                // settings.txt `backdrop`: 0 none, 1 acrylic, 2 mica
    String settings_text;       // settings.txt whole: its `key.<command> = <keys>` lines change the key bindings
    u8 cursor;                  // settings.txt `cursor`: 0 block, 1 bar, 2 underline (when the program asks for none)
    b32 cursor_blink;           // settings.txt `cursor_blink = on`
    b32 ligatures;              // settings.txt `ligatures = on`
    b32 copy_on_select_off;     // settings.txt `copy_on_select = off`
    b32 paste_warning_off;      // settings.txt `paste_warning = off`
    b32 win32_input_off;        // settings.txt `win32_input = off`: keys as plain VT even when ConPTY asks for more
    b32 explorer_menu;          // Explorer has "Open in moss" (registered for this user)
    b32 start_with_windows;     // term's server starts at sign-in (the user's Run key)
    b32 good_programmer;        // the global CLAUDE.md has the "Be a good programmer" block
    s32 background_strength;    // settings.txt `background_strength`: how much of the background image shows (percent; 0 = the default 30, -1 = off)
    String environment;         // the window's environment, NAME=value a line (%NAME% in typed paths)
    s32 place_count;            // the known folders, for the go-to box's Places
    AppPlace places[8];
    String paths_text;          // paths.txt: `recent <path>` (newest first) and `bookmark <path>` lines
} AppInit;

typedef struct AppFrameInput {
    UiInput input;
    s32 client_w, client_h;
    u32 dpi;
    b32 resized, foreground, maximized, focused;   // focused: the window has the keyboard
    u64 now_us;
    u64 dt_us;                  // 0 for the first frame after an idle wait
    s32 window_rect[4];         // restore rect x, y, w, h
    Font *fonts;                // Font_Count of them, for this DPI

    // --- the terminal side (term_client.cpp) ---
    b32 connected;              // attached to the server
    String status;              // why not (or a message to show once), may be empty
    s64 tab_count;
    const TermTab *tabs;        // every tab of the server
    s64 grid_count;
    const TermGrid *grids;      // the tabs the last watch showed, as received
    s32 cell_w, cell_h;         // terminal cell size in pixels at this DPI and font size
    b32 tabs_received;          // the server has sent its tab list since the window connected
    String clipboard_text;      // only when the app asked for it last frame (AppOutput.read_clipboard)
    s64 session_count;          // Claude Code's sessions, newest first (while AppOutput.want_sessions)
    const TermSession *sessions;
    b32 open_request;           // a tab asked for from outside (term -d DIR [-- command]); until AppOutput.open_taken
    String open_cwd, open_command;
    u32 find_tab;               // the last find result: its tab, the matches in its scrollback and screen, the current one (1-based)
    s32 find_count, find_index;
    s64 find_mark_count;        // where the matches are, as a share of the lines (0..65535)
    const u16 *find_marks;
    u32 history_tab;            // the last claude history read (after AppOutput.history_get): its tab, a count up per read
    u32 history_version;
    s64 history_count;
    const TermHistoryEntry *history;
    s64 mono_font_count;        // the installed monospace font families, sorted (after AppOutput.want_fonts)
    const String *mono_fonts;
    u64 background;             // the background image's texture (settings.txt `background_image`; 0 = none), its size and file
    s32 background_w, background_h;
    String background_path;
    // --- the file browsers ---
    FilesIn files;              // listings and changed folders since the last frame
    s32 utc_offset_minutes;     // local time - UTC (dates)
    u64 wall_time;              // now, UTC FILETIME (100 ns ticks since 1601)
    u32 view_tab;               // `view <file>` was typed in this tab (0: none); view_path is the file or folder
    String view_path;
    s32 image_count;            // the viewer's pictures that are ready (asked with AppOutput.image_wants)
    const AppImage *images;
    String editor;              // the terminal text editor Enter on a text file runs (settings.txt `editor`, else the first found: nvim, vim, ...; empty: none)
    b32 drag_over;              // another program drags files over the window (at the mouse)
    s64 drop_count;             // files it dropped this frame (at the mouse)
    const String *dropped;
    const AppInit *settings;    // settings.txt read again after AppOutput.reset_settings (one frame); else NULL
    // --- what the debug overlay shows (Ctrl+Alt+Shift+F12) ---
    struct {
        f32 work_ms_avg, work_ms_max;      // the last 120 frames' work
        u64 frames_drawn, frames_skipped;
        u64 ipc_in, ipc_out;               // bytes from / to the server
        s32 glyphs, atlas_percent;         // terminal glyphs drawn, atlas used
        f32 gpu_ms, gpu_cells_ms;          // a recent frame on the GPU, and its terminal cells
    } debug;
} AppFrameInput;

enum { AppWindow_None = 0, AppWindow_Minimize = 1, AppWindow_ToggleMaximize = 2, AppWindow_Close = 3, AppWindow_Fullscreen = 4, AppWindow_ToggleTopmost = 5 };
enum { App_MaxCaptionClientRects = 64 };

// What the browser asks of the disk (never while its tab is busy).
enum { FileOp_None = 0, FileOp_Delete = 1, FileOp_Rename = 2, FileOp_NewFolder = 3, FileOp_Paste = 4, FileOp_CopyIn = 5 };
typedef struct FileOp {
    u32 kind;
    String path;    // Delete: to the Recycle Bin; Rename: the item; NewFolder: the folder made; Paste: where the clipboard's files go
    String name;    // Rename: the new name; CopyIn: the files that go into path, one a line
} FileOp;

typedef struct AppOutput {
    DrawList *draw;             // the frame (the app's memory, valid until its next frame)
    u32 clear_color;            // behind everything (transparent where terminals are see-through)
    u32 window_command;         // AppWindow_*
    s32 caption_height;         // title band for WM_NCHITTEST
    s32 resize_border;
    s32 caption_client_rect_count;
    Rect caption_client_rects[App_MaxCaptionClientRects];   // parts of the caption band that are client (tabs, buttons)
    s32 min_width, min_height;
    String window_title;
    b32 keep_animating;
    u64 wake_at_us;             // run a frame by then even if nothing happens (cursor blink); 0 = none
    u8 cursor;                  // Cursor_*

    // --- server requests (frame arena; term_client.cpp sends them) ---
    b32 term_watch_changed;     // send the watch list
    s64 term_watch_count; TermWatch *term_watches;
    u32 history_get;            // read this tab's claude history (AppFrameInput.history_*)
    u32 account_ok;             // this tab's claude may go on with the account it changed to (TermTab_AccountHeld)
    s64 input_count;    TermInput *inputs;
    s64 new_tab_count;  TermNewTab *new_tabs;
    s64 close_count;    u32 *closes;
    s64 scroll_count;   TermScroll *scrolls;
    u32 focus_tab;              // the tab with the keyboard focus (0 = none); sent when it changes
    f32 term_font_px;           // the terminal font size in pixels the cells should use (the platform makes the glyphs again on change)
    b32 want_sessions;          // the sessions list is on screen: the server keeps it current
    b32 want_fonts;             // list the monospace fonts (AppFrameInput.mono_fonts), once
    b32 find_send;              // the find bar's text (empty: off) for find_tab, and a step (FIND_* flags)
    u32 find_tab, find_flags;
    String find_text;
    String term_font;           // non-empty: switch the terminal font to this family
    // --- file browser requests ---
    s64 file_request_count; FileRequest *file_requests;   // list these folders
    s64 file_watch_count; FileWatch *file_watches;        // exactly these folders are watched (every frame), sizes measured in some
    String open_file;           // open a file with its program (ShellExecute "open")
    s32 image_want_count;       // the viewer's pictures, the shown ones first, then their neighbours (decoded in that order)
    String image_wants[6];
    s64 clipboard_file_count; String *clipboard_files;    // put these files on the clipboard (copy), with their paths as text
    s64 file_op_count; FileOp *file_ops;
    s64 drag_file_count; String *drag_files;              // drag these files out of the window (a copy; OLE, modal)
    u32 drop_effect;            // what a drop at the mouse would do: 0 refused, 1 copy into a browser's folder, 2 type the paths

    // --- other side effects ---
    b32 read_clipboard;         // put the clipboard text in the next frame's input
    b32 clipboard_set;          String clipboard_text;
    String open_url;            // ShellExecute an http(s) link
    String open_path;           // a path Ctrl+clicked in a terminal (maybe with :line:col), relative to open_path_cwd
    String open_path_cwd;
    u32 link_tab, link_id;      // an OSC 8 link Ctrl+clicked: the server sends its URI to open (TermClient_TakeLink)
    String session_text;        // non-empty: save the layout
    String paths_text;          // non-empty: save paths.txt (recent folders, bookmarks)
    b32 set_window_placement;   // the saved window place (first frame after a restored layout)
    s32 placement_rect[4];      // x, y, w, h
    b32 placement_maximized;
    s32 setting_count;          // write these to settings.txt
    String setting_keys[4], setting_values[4];
    u8 backdrop;                // what Windows draws behind see-through parts: 0 nothing, 1 acrylic, 2 Mica
    b32 dark;                   // the theme is dark (the window frame and acrylic's tint follow)
    u8 open_settings;           // 1: open settings.txt, 2: its folder
    b32 choose_background;      // pick a background image (a file dialog; the platform loads it and saves the setting)
    b32 open_taken;             // the outside request's tab was asked for
    u8 explorer_menu;           // 1: add "Open in moss" to Explorer, 2: take it away
    u8 start_with_windows;      // 1: start term's server at sign-in, 2: don't
    u8 good_programmer;         // 1: the principles into the global CLAUDE.md, 2: out of it
    b32 reset_settings;         // settings.txt back to moss's own (the old one kept), read again: AppFrameInput.settings
    u32 tear_off_tab;           // this tab goes to another window: the one under the mouse (tear_off_drop), else a new one
    b32 tear_off_drop;          // dragged out of the window: where the mouse is decides
    b32 tear_off_last;          // it's the window's only tab: only into another window
    s64 open_window_count; String *open_windows;   // windows the main one opens at start: their tabs wait for them
    s32 ime_caret[4];           // the focused terminal's cursor cell (window px x, y, w, h): IME composition goes there
    // --- debug ---
    b32 log_profile;            // Ctrl+Alt+Shift+F10
} AppOutput;

typedef struct App App;
App   *App_Create(const AppInit *init);
void   App_Frame(App *app, const AppFrameInput *in, AppOutput *out);
String App_SessionText(App *app);   // the final save at exit (empty: unchanged)
void   App_Destroy(App *app);

// --- for the headless script runner ---
// The centre of the UI target named `label` in the last frame; 0 when none.
b32    App_FindLabel(App *app, String label, s32 *x, s32 *y);
// A pane's terminal rect (window pixels) and its tab, by pane order (0 = first, depth first); 0 when none.
b32    App_PaneRect(App *app, s32 pane, s32 rect[4], u32 *term_id);
// Text dumps for the log: the pane tree, rects, tabs and focus.
String App_DumpLayout(App *app, Arena *arena);
// Runs a bound command by name ("split_right", "theme Nord", ...); 0 when unknown.
b32    App_RunCommandByName(App *app, String line);
// Every command's name, one per line (for `cmd` in scripts).
String App_CommandNames(App *app, Arena *arena);

#endif
