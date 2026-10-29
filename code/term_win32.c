// term: the platform layer. One frame step (StepFrame: the app's frame, its requests, glyphs, render) is driven
// either by the window loop (Win32 messages, frame pacing, present) or by the headless script runner (no window,
// synthetic input, fixed ticks, captures).
//
//   term                 the window, attached to the background server (started when there is none)
//   term --test          a window with its own throwaway server (no tray, no saved tabs)
//   term --server        the background server itself
//   term --headless --script FILE [--size WxH] [--dpi N] [--session FILE]
//                        no window: plays the script against a private server, logs to stdout
//   term --vt-replay FILE [--size COLSxROWS]   the emulator's screen after a recorded byte stream
#include "term_app.h"
#include "term_platform.h"
#include "term_debug.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <imm.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <wchar.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Platform_MaxEvents = 256, Platform_TextBytes = 4096, Platform_CursorCount = 5 };
enum { Wake_Files = WM_APP + 1 };   // the lister or the watcher has something
enum { Test_Command = WM_APP + 3 }; // a test window runs the next of MOSS_TEST_COMMAND's commands (names, ; between)

typedef struct Platform {
    HWND hwnd;
    // --- the image viewer's pictures: decoded on a worker thread (the first one the app wants that isn't here), made
    //     textures on this one; 8 kept, the least recently wanted goes ---
    struct { u8 path[1024]; s32 path_len; u64 texture; s32 w, h, full_w, full_h; b32 failed; u32 used_frame; } images[8];
    AppImage image_list[8];
    CRITICAL_SECTION image_lock;
    HANDLE image_wake, image_thread;
    u8 image_job[1024];           // being decoded (0 long: nothing)
    s32 image_job_len;
    u8 *image_done;               // the worker's result (malloc'd BGRA), when image_done_ready
    s32 image_done_w, image_done_h, image_done_fw, image_done_fh;
    b32 image_done_ready;
    u32 view_tab;                 // `view <file>` in a tab, for the next frame's input
    u8 view_path[1024];
    s32 view_path_len;
    String editor;                // the terminal text editor (settings.txt `editor`, else the first found)
    u8 pasted_image[MAX_PATH * 3]; // the clipboard picture saved last frame, for the next frame's input
    s32 pasted_image_len;
    WCHAR accounts_path[MAX_PATH]; // accounts.txt (MOSS_ACCOUNTS_FILE; "NUL": none)
    u8 accounts_text[8192];       // as last read
    s32 accounts_len;
    u64 accounts_write;           // its write time when read (0: not there)
    u64 accounts_checked_us;
    u32 accounts_version;         // counts up with each read that changed it
    b32 headless;
    b32 quit;
    b32 shown;
    b32 in_frame;
    b32 woke_from_wait;           // the frame follows an idle wait: animations don't jump
    b32 in_size_move;
    b32 fullscreen;
    WINDOWPLACEMENT windowed;     // before fullscreen; the restored layout's maximized state
    u32 frame;
    u32 wake_frame;               // a frame is wanted when frame == wake_frame
    u32 dpi;
    u32 refresh_hz;
    HMONITOR monitor;
    HANDLE pace_timer;
    u64 wake_at_us;               // the app asked for a frame by then (cursor blink, spinner)

    // --- input since the last frame ---
    UiInput input;
    u8 vk_to_key[256];
    u16 dead_char;
    u16 high_surrogate;
    s64 event_count;
    UiKeyEvent events[Platform_MaxEvents];
    u32 event_lparam[Platform_MaxEvents];
    s64 text_used;
    u8 text[Platform_TextBytes];
    b32 caption_down[Mouse_Count];   // a press on the title band (DefWindowProc's move loop swallows its release)
    s32 caption_down_pos[2];
    LPARAM last_hit_test;
    b32 focused;
    s32 posted_mouse[2];          // the last WM_MOUSEMOVE's position (what a test window uses)

    // --- the app's last answers ---
    s32 caption_height;
    s32 resize_border;
    s32 min_width, min_height;
    s32 caption_rect_count;
    Rect caption_rects[App_MaxCaptionClientRects];
    u8 cursor;
    HCURSOR cursors[Platform_CursorCount];
    WCHAR title[512];

    s32 client_w, client_h;
    b32 resized;
    Gpu *gpu;
    FontAtlas atlas;
    Font fonts[Font_Count];
    u32 fonts_dpi;
    u64 presented_hash;
    u64 last_frame_us, last_present_us;
    b32 present_deferred;         // something new waits for the next present slot
    b32 rendered_not_presented;
    b32 last_changed;             // the last frame drew something new
    b32 last_animating;           // the last frame asked for more (animations, pending requests)
    App *app;

    TermClient *client;
    TermCells *cells;
    String term_font;             // settings `font`
    u8 term_font_buf[128];        // the font picked in the settings (term_font points here then)
    s64 mono_font_count;          // installed monospace families, listed when the settings first ask
    String *mono_fonts;
    b32 mono_fonts_listed;
    f32 term_font_px;
    b32 want_clipboard;           // the app asked for the clipboard text
    u8 fake_clipboard[8192];      // headless runs' clipboard
    s32 fake_clipboard_len;
    u64 frames_drawn, frames_skipped;
    u64 reconnect_at_us;          // the next try while the server is gone
    String cwd;
    s32 ime_caret[4];             // where IME composition goes (the focused terminal's cursor)
    u64 background;               // the background image's texture (0 none), its size, its file (background_buf)
    s32 background_w, background_h;
    String background_path;
    u8 background_buf[1024];
    b32 test_window;              // MOSS_TEST_WINDOW: off screen, driven by posted messages
    s32 test_commands_run;        // MOSS_TEST_COMMAND's commands run so far (one per Test_Command)
    b32 closing;                  // the server quit for good: the window is closing
    String window_name;           // `--window NAME`: one of the other windows (empty = the main one)
    WCHAR instance[64];           // the server instance (MOSS_INSTANCE, empty = the user's): windows only trade tabs within one
    u32 tear_offs;                // windows torn off in this run (headless names them w1, w2, ...)
    b32 start_at;                 // `--at x,y,w,h`: where a torn-off window opens
    s32 start_rect[4];
    ATOM id_atom;                 // the window's term.id property
    b32 explorer_menu;            // Explorer's "Open in moss" is registered (at start)
    AppInit reset_settings;       // settings.txt read again after a reset, for the app's next frame (reset_ready)
    b32 reset_ready;
    b32 open_request;             // a tab asked for from outside (`-d DIR`, `-- command`, WM_COPYDATA from another term)
    u8 open_buf[2048];            // cwd \0 command
    String open_cwd, open_command;
    u8 backdrop;                  // applied to the window: AppOutput.backdrop, dark
    b32 dark;
    b32 backdrop_set;
    b32 drag_over;                // another program drags files over the window (DropTarget)
    u32 drop_effect;              // the app's answer for a drop at the mouse (last frame)
    Arena drop_arena;
    String *dropped;              // dropped files, for the next frame
    s64 dropped_count;

    b32 foreign_drag;             // another moss window's tab is dragged over this one (its WM_COPYDATA), at foreign_at
    b32 foreign_drop;             // and dropped here, for the next frame
    s32 foreign_at[2];            // the pointer, screen pixels
    u32 foreign_tab;              // its server tab (0: a web tab)
    u8 foreign_buf[1400];         // its label \0 web address
    String foreign_label, foreign_url;
    HWND drag_target;             // the moss window our dragged tab is over (told where the pointer is)
    u64 torn_web;                 // a web tab whose page opened in another window, for the next frame (it goes here)
    String open_web;              // `--web URL`: the first tab, a web page (a web tab torn off another window)

    Web *web;                     // web pages in tabs (WebView2), 0 without it or in headless runs
    AppWebState web_states[App_MaxWebs];
    AppWebPopup web_popups[8];

    Arena arena;
    Arena scratch;
    WString session_path;         // NUL-terminated
    WString paths_path;           // paths.txt (recents, bookmarks), NUL-terminated; empty: not saved
    WString sessions_meta_path;   // sessions.txt (the sidebar's names, pins, archived); empty: not saved
    String sessions_meta_text;    // as read at start, handed to the first frame
} Platform;

static void Pump(Platform *p)
{
    MSG msg = {0};
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) p->quit = 1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void UpdateMonitor(Platform *p)
{
    HMONITOR mon = MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon == p->monitor && p->refresh_hz) return;
    p->monitor = mon;
    p->refresh_hz = 60;
    MONITORINFOEXW mi = {0};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {0};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, (MONITORINFO *)&mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        p->refresh_hz = dm.dmDisplayFrequency;
    }
    Logf("window", "monitor %d x %d, %u Hz, %u dpi", (int)(mi.rcMonitor.right - mi.rcMonitor.left), (int)(mi.rcMonitor.bottom - mi.rcMonitor.top), p->refresh_hz, p->dpi);
}

// The UI fonts for the window's DPI: made again (and the glyph atlas emptied) when it changes.
static void MakeFonts(Platform *p)
{
    if (p->fonts_dpi == p->dpi) return;
    if (!p->atlas.pixels) FontAtlas_Init(&p->atlas);
    else FontAtlas_Reset(&p->atlas);
    for (s32 i = 0; i < Font_Count; i++) Font_Destroy(&p->fonts[i]);
    f32 pt = (f32)p->dpi / 72.0f;
    static const wchar_t ui[] = L"Segoe UI\0", icons[] = L"Segoe Fluent Icons\0Segoe MDL2 Assets\0";
    Font_Create(&p->fonts[Font_Ui], &p->atlas, ui, 9 * pt, 400);
    Font_Create(&p->fonts[Font_UiSmall], &p->atlas, ui, 8 * pt, 400);
    Font_Create(&p->fonts[Font_UiBold], &p->atlas, ui, 9 * pt, 600);
    Font_Create(&p->fonts[Font_Icon], &p->atlas, icons, 9 * pt, 400);
    Font_Create(&p->fonts[Font_IconSmall], &p->atlas, icons, 7 * pt, 400);
    Font_Create(&p->fonts[Font_Mono], &p->atlas, L"Cascadia Mono\0Consolas\0", 8.5f * pt, 400);   // code in the reader
    p->fonts_dpi = p->dpi;
}

static UiKeyEvent *PushEvent(Platform *p, u8 key, b32 down, b32 up, u32 repeat_count, u32 lparam, u8 mods)
{
    if (p->event_count >= Platform_MaxEvents) return NULL;
    s64 i = p->event_count++;
    UiKeyEvent zero = {0};
    UiKeyEvent *e = &p->events[i];
    *e = zero;
    e->key = key;
    e->down = (u8)(down != 0);
    e->up = (u8)(up != 0);
    e->repeat_count = repeat_count ? repeat_count : 1;
    e->modifiers = mods;
    p->event_lparam[i] = lparam;
    // what a console program would get (win32-input-mode): the key, its scan code, which modifiers and locks
    e->scan = (u16)((lparam >> 16) & 0xFF);
    e->control_state = (u16)((GetKeyState(VK_RMENU) < 0 ? 0x1 : 0) | (GetKeyState(VK_LMENU) < 0 ? 0x2 : 0) | (GetKeyState(VK_RCONTROL) < 0 ? 0x4 : 0) |
                             (GetKeyState(VK_LCONTROL) < 0 ? 0x8 : 0) | (GetKeyState(VK_SHIFT) < 0 ? 0x10 : 0) | ((GetKeyState(VK_NUMLOCK) & 1) ? 0x20 : 0) |
                             ((GetKeyState(VK_SCROLL) & 1) ? 0x40 : 0) | ((GetKeyState(VK_CAPITAL) & 1) ? 0x80 : 0) | ((lparam & (1 << 24)) ? 0x100 : 0));
    return e;
}

static u8 KeyMods(void)
{
    return (u8)((GetKeyState(VK_SHIFT) < 0 ? Mod_Shift : 0) | (GetKeyState(VK_CONTROL) < 0 ? Mod_Ctrl : 0) | (GetKeyState(VK_MENU) < 0 ? Mod_Alt : 0));
}

// Text typed with this event (WM_CHAR / WM_SYSCHAR), kept with the key that made it when they belong together.
static void PushText(Platform *p, u32 ch, u32 lparam, u8 mods)
{
    if (p->text_used + 4 > Platform_TextBytes) return;
    String text = { p->text + p->text_used, Utf8_Encode(p->text + p->text_used, ch) };
    p->text_used += text.count;
    s64 last = p->event_count - 1;
    if (last >= 0 && p->events[last].down && !p->events[last].text.count && p->event_lparam[last] == lparam) {
        p->events[last].text = text;
    } else {
        UiKeyEvent *e = PushEvent(p, 0, 1, 0, 1, lparam, mods);
        if (e) e->text = text;
    }
}

static void SetClipboardUtf8(HWND hwnd, String text, Arena *a)
{
    WString w = Utf8ToUtf16(a, text);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(w.count + 1) * sizeof(u16));
    if (!mem) return;
    u16 *dst = (u16 *)GlobalLock(mem);
    memcpy(dst, w.data, (size_t)w.count * sizeof(u16));
    dst[w.count] = 0;
    GlobalUnlock(mem);
    if (!OpenClipboard(hwnd)) { GlobalFree(mem); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    CloseClipboard();
}

// The clipboard's picture (a screenshot, an image copied) saved as %TEMP%\moss-paste\paste-<time>.png, when it has one
// and no text: its path (UTF-8), else empty. 24 and 32 bit pictures (CF_DIB); a 32 bit one with no alpha at all is
// opaque.
static String SaveClipboardImage(Platform *p)
{
    if (!OpenClipboard(p->hwnd)) return (String){0};
    String out = {0};
    HANDLE h = IsClipboardFormatAvailable(CF_UNICODETEXT) ? NULL : GetClipboardData(CF_DIB);
    const BITMAPINFOHEADER *bi = h ? (const BITMAPINFOHEADER *)GlobalLock(h) : NULL;
    if (bi && (bi->biBitCount == 32 || bi->biBitCount == 24) && (bi->biCompression == BI_RGB || bi->biCompression == BI_BITFIELDS) &&
        bi->biWidth > 0 && bi->biWidth <= 16384 && bi->biHeight != 0 && abs(bi->biHeight) <= 16384) {
        s32 w = bi->biWidth, hgt = abs(bi->biHeight), bpp = bi->biBitCount / 8;
        s32 stride = ((w * bi->biBitCount + 31) / 32) * 4;
        const u8 *bits = (const u8 *)bi + bi->biSize + (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0);
        u8 *rgba = (u8 *)malloc((size_t)w * hgt * 4);
        b32 any_alpha = 0;
        for (s32 y = 0; rgba && y < hgt; y++) {
            const u8 *row = bits + (size_t)(bi->biHeight > 0 ? hgt - 1 - y : y) * stride;   // bottom-up unless the height is negative
            for (s32 x = 0; x < w; x++) {
                u8 *o = rgba + ((size_t)y * w + x) * 4;
                o[0] = row[x * bpp + 2]; o[1] = row[x * bpp + 1]; o[2] = row[x * bpp + 0];
                o[3] = bpp == 4 ? row[x * bpp + 3] : 255;
                any_alpha |= o[3] != 0;
            }
        }
        for (s64 i = 0; rgba && !any_alpha && i < (s64)w * hgt; i++) rgba[i * 4 + 3] = 255;
        WCHAR dir[MAX_PATH] = {0}, path[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, dir);
        wcscat_s(dir, MAX_PATH, L"moss-paste");
        CreateDirectoryW(dir, NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf_s(path, MAX_PATH, L"%s\\paste-%04d%02d%02d-%02d%02d%02d-%03d.png", dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        if (rgba && Image_SavePng(path, rgba, w, hgt)) out = Utf16ToUtf8(&p->scratch, (WString){ (u16 *)path, (s64)wcslen(path) });
        free(rgba);
        Logf("app", "pasted picture: %dx%d%s", w, hgt, out.count ? "" : " (not saved)");
    }
    if (bi) GlobalUnlock(h);
    CloseClipboard();
    return out;
}

static String ReadClipboardUtf8(HWND hwnd, Arena *a)
{
    String out = {0};
    if (!OpenClipboard(hwnd)) return out;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const u16 *text = data ? (const u16 *)GlobalLock(data) : NULL;
    if (text) {
        WString w = { (u16 *)text, (s64)wcslen((const wchar_t *)text) };
        out = Utf16ToUtf8(a, w);
        GlobalUnlock(data);
    }
    CloseClipboard();
    return out;
}

// Files on the clipboard as Explorer copies them (CF_HDROP), their paths as text too.
static void SetClipboardFiles(HWND hwnd, const String *paths, s64 count, Arena *a)
{
    TextBuilder tb = { a, NULL, 0, 0 };
    s64 chars = 0;
    WString *w = Arena_PushArray(a, WString, count + 1);
    for (s64 i = 0; i < count; i++) {
        w[i] = Utf8ToUtf16(a, paths[i]);
        chars += w[i].count + 1;
        TB_Format(&tb, "%s%.*s", i ? "\r\n" : "", STR_ARG(paths[i]));
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (size_t)(chars + 1) * sizeof(u16));
    if (!mem) return;
    DROPFILES *df = (DROPFILES *)GlobalLock(mem);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    u16 *dst = (u16 *)(df + 1);
    for (s64 i = 0; i < count; i++) {
        memcpy(dst, w[i].data, (size_t)w[i].count * sizeof(u16));
        dst += w[i].count + 1;
    }
    GlobalUnlock(mem);
    String text = TB_String(&tb);
    WString wt = Utf8ToUtf16(a, text);
    HGLOBAL tmem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wt.count + 1) * sizeof(u16));
    if (tmem) {
        u16 *t = (u16 *)GlobalLock(tmem);
        memcpy(t, wt.data, (size_t)wt.count * sizeof(u16));
        t[wt.count] = 0;
        GlobalUnlock(tmem);
    }
    if (!OpenClipboard(hwnd)) { GlobalFree(mem); if (tmem) GlobalFree(tmem); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_HDROP, mem)) GlobalFree(mem);
    if (tmem && !SetClipboardData(CF_UNICODETEXT, tmem)) GlobalFree(tmem);
    CloseClipboard();
}

// --- the browser's file operations: renames and new folders at once; deletes and pastes on a thread of their
//     own (Windows shows its progress window for long ones). The watcher brings every result into the browser ---
typedef struct FileOpJob { HWND hwnd; UINT func; FILEOP_FLAGS flags; WCHAR *from; WCHAR *to; } FileOpJob;

static DWORD WINAPI FileOpThread(void *arg)
{
    FileOpJob *job = (FileOpJob *)arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SHFILEOPSTRUCTW op = {0};
    op.hwnd = job->hwnd;
    op.wFunc = job->func;
    op.pFrom = job->from;
    op.pTo = job->to;
    op.fFlags = job->flags;
    int r = SHFileOperationW(&op);
    if (r) Logf("files", "file operation %u failed (%d)", job->func, r);
    if (SUCCEEDED(com)) CoUninitialize();
    free(job->from);
    free(job->to);
    free(job);
    return 0;
}

// One path as SHFileOperation takes it (double-NUL-terminated), on the heap: the thread frees it.
static WCHAR *HeapPathList(Arena *a, String path)
{
    WString w = Utf8ToUtf16(a, path);
    WCHAR *out = (WCHAR *)calloc((size_t)w.count + 2, sizeof(WCHAR));
    if (out) memcpy(out, w.data, (size_t)w.count * sizeof(WCHAR));
    return out;
}

// --- Claude accounts: accounts.txt (MOSS_ACCOUNTS_FILE; headless and test runs have none unless a test sets one) ---

// accounts.txt again when its write time changed (looked at once a second), for the app.
static void ReadAccounts(Platform *p)
{
    u64 now = Time_NowMicros();
    if (p->accounts_version && now - p->accounts_checked_us < 1000000) return;
    p->accounts_checked_us = now;
    WIN32_FILE_ATTRIBUTE_DATA fad = {0};
    u64 write = GetFileAttributesExW(p->accounts_path, GetFileExInfoStandard, &fad) ?
                (((u64)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime | 1) : 0;
    if (p->accounts_version && write == p->accounts_write) return;
    p->accounts_write = write;
    String text = write ? Term_ReadFile(&p->scratch, p->accounts_path) : (String){0};
    p->accounts_len = (s32)Min(text.count, (s64)sizeof(p->accounts_text));
    memcpy(p->accounts_text, text.data, (size_t)p->accounts_len);
    p->accounts_version++;
    p->wake_frame = p->frame;
}

// A new Claude account (the palette's "Add"): its config folder next to accounts.txt (accounts\<name>), the main setup's
// settings copied in (settings.json, CLAUDE.md, agents, commands, skills, output-styles; never its login, sessions or
// history), and its line in accounts.txt. The main setup: CLAUDE_CONFIG_DIR, else ~\.claude. The app checked the name
// (letters, digits, - and _).
static void AddAccount(Platform *p, String name)
{
    if (!wcscmp(p->accounts_path, L"NUL")) { Logf("headless", "add account %.*s: no accounts file in this run", STR_ARG(name)); return; }
    WCHAR base[MAX_PATH] = {0}, dir[MAX_PATH] = {0}, from[MAX_PATH] = {0};
    wcscpy_s(base, MAX_PATH, p->accounts_path);
    WCHAR *slash = wcsrchr(base, L'\\');
    if (slash) *slash = 0;
    swprintf_s(dir, MAX_PATH, L"%s\\accounts\\%s", base, (const WCHAR *)Utf8ToUtf16(&p->scratch, name).data);
    if (!GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", from, MAX_PATH)) {
        GetEnvironmentVariableW(L"USERPROFILE", from, MAX_PATH);
        wcscat_s(from, MAX_PATH, L"\\.claude");
    }
    SHCreateDirectoryExW(NULL, dir, NULL);
    static const WCHAR *const files[] = { L"settings.json", L"CLAUDE.md" };
    static const WCHAR *const folders[] = { L"agents", L"commands", L"skills", L"output-styles" };
    for (s32 i = 0; i < (s32)ArrayCount(files); i++) {
        WCHAR a[MAX_PATH], b[MAX_PATH];
        swprintf_s(a, MAX_PATH, L"%s\\%s", from, files[i]);
        swprintf_s(b, MAX_PATH, L"%s\\%s", dir, files[i]);
        CopyFileW(a, b, TRUE);
    }
    for (s32 i = 0; i < (s32)ArrayCount(folders); i++) {
        WCHAR a[MAX_PATH + 2] = {0}, b[MAX_PATH + 2] = {0};   // double-NUL-terminated lists
        swprintf_s(a, MAX_PATH, L"%s\\%s", from, folders[i]);
        if (GetFileAttributesW(a) == INVALID_FILE_ATTRIBUTES) continue;
        wcscpy_s(b, MAX_PATH, dir);
        SHFILEOPSTRUCTW op = {0};
        op.wFunc = FO_COPY;
        op.pFrom = a;
        op.pTo = b;
        op.fFlags = FOF_NO_UI;
        if (SHFileOperationW(&op)) Logf("app", "add account: copying %S failed", folders[i]);
    }
    String old = Term_ReadFile(&p->scratch, p->accounts_path);
    String head = old.count ? (String){0} : S("# moss: Claude Code accounts. The default one is Claude Code's own (~\\.claude); each other is a\r\n"
                                              "# config folder. Lines: account <name> = <config folder>, theme <name> = <theme>, folder <path> = <name>\r\n"
                                              "# (claudes in that folder must use that account, and it is used nowhere else).\r\n");
    String sep = old.count && old.data[old.count - 1] != '\n' ? S("\r\n") : (String){0};
    String line = Str_PushFormat(&p->scratch, "%.*s%.*s%.*saccount %.*s = %.*s\r\n", STR_ARG(head), STR_ARG(old), STR_ARG(sep), STR_ARG(name),
                                 STR_ARG(Utf16ToUtf8(&p->scratch, (WString){ (u16 *)dir, (s64)wcslen(dir) })));
    Term_WriteFileAtomic(p->accounts_path, line);
    p->accounts_checked_us = 0;
    p->accounts_write = 0;
    Logf("app", "added the account %.*s: %S", STR_ARG(name), dir);
}

// An account's theme in accounts.txt (the accounts palette's arrows): its `theme <name> = ...` lines replaced by one with
// the new theme, or none (empty: the window's).
static void SetAccountTheme(Platform *p, String name, String theme)
{
    if (!wcscmp(p->accounts_path, L"NUL")) { Logf("headless", "account %.*s theme %.*s: no accounts file in this run", STR_ARG(name), STR_ARG(theme)); return; }
    String old = Term_ReadFile(&p->scratch, p->accounts_path);
    TextBuilder tb = { &p->scratch, NULL, 0, 0 };
    for (s64 at = 0; at < old.count;) {
        s64 end = at;
        while (end < old.count && old.data[end] != '\n') end++;
        String line = { old.data + at, end - at + (end < old.count ? 1 : 0) };
        at = end + 1;
        // "theme <name> =" (spaces and case not counting): dropped
        s64 k = 0;
        while (k < line.count && (line.data[k] == ' ' || line.data[k] == '\t')) k++;
        b32 drop = line.count - k > 6 && Str_EqualNoCase((String){ line.data + k, 5 }, S("theme")) && (line.data[k + 5] == ' ' || line.data[k + 5] == '\t');
        if (drop) {
            k += 5;
            while (k < line.count && (line.data[k] == ' ' || line.data[k] == '\t')) k++;
            s64 n0 = k;
            while (k < line.count && line.data[k] != '=' && line.data[k] != ' ' && line.data[k] != '\t') k++;
            drop = Str_EqualNoCase((String){ line.data + n0, k - n0 }, name);
        }
        if (!drop) TB_Append(&tb, line);
    }
    if (tb.count && tb.data[tb.count - 1] != '\n') TB_Append(&tb, S("\r\n"));
    if (theme.count) TB_Format(&tb, "theme %.*s = %.*s\r\n", STR_ARG(name), STR_ARG(theme));
    Term_WriteFileAtomic(p->accounts_path, (String){ tb.data, tb.count });
    p->accounts_checked_us = 0;
    p->accounts_write = 0;
    Logf("app", "account %.*s theme: %.*s", STR_ARG(name), theme.count ? (int)theme.count : 8, theme.count ? (const char *)theme.data : "window's");
}

static void RunFileOps(Platform *p, const FileOp *ops, s64 count)
{
    static const char *const names[6] = { "", "delete", "rename", "new folder", "paste into", "copy into" };
    for (s64 i = 0; i < count; i++) {
        const FileOp *o = &ops[i];
        Logf("files", "%s %.*s%s%.*s", names[o->kind % 6], STR_ARG(o->path), o->name.count ? " <- " : "", STR_ARG(o->name));
        WString path = Utf8ToUtf16(&p->scratch, o->path);
        if (o->kind == FileOp_Rename) {
            s64 cut = o->path.count;
            while (cut > 0 && o->path.data[cut - 1] != '\\') cut--;
            String to = Str_PushFormat(&p->scratch, "%.*s%.*s", (int)cut, (const char *)o->path.data, STR_ARG(o->name));
            if (!MoveFileExW((LPCWSTR)path.data, (LPCWSTR)Utf8ToUtf16(&p->scratch, to).data, 0)) Logf("files", "rename failed (%lu)", GetLastError());
        } else if (o->kind == FileOp_NewFolder) {
            if (!CreateDirectoryW((LPCWSTR)path.data, NULL)) Logf("files", "new folder failed (%lu)", GetLastError());
        } else if (o->kind == FileOp_Delete || o->kind == FileOp_Paste || o->kind == FileOp_CopyIn) {
            FileOpJob *job = (FileOpJob *)calloc(1, sizeof(FileOpJob));
            if (!job) continue;
            job->hwnd = p->hwnd;
            // headless runs never show Windows' dialogs, and delete for good (test files, not the user's Recycle Bin)
            FILEOP_FLAGS quiet = p->headless ? FOF_NO_UI : 0;
            if (o->kind == FileOp_CopyIn) {
                // the dropped files (one a line) into the folder; a copy, never a move
                WString list = Utf8ToUtf16(&p->scratch, o->name);
                WCHAR *from = (WCHAR *)calloc((size_t)list.count + 2, sizeof(WCHAR));
                if (!from) { free(job); continue; }
                for (s64 k = 0; k < list.count; k++) from[k] = list.data[k] == '\n' ? 0 : (WCHAR)list.data[k];
                job->func = FO_COPY;
                job->from = from;
                job->to = HeapPathList(&p->scratch, o->path);
                job->flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION | quiet;
            } else if (o->kind == FileOp_Delete) {
                job->func = FO_DELETE;
                job->from = HeapPathList(&p->scratch, o->path);
                job->flags = p->headless ? quiet : (FOF_ALLOWUNDO | FOF_NOCONFIRMATION);
            } else {
                // the clipboard's files (Explorer's copy, or its cut: a move), into the folder
                b32 got = !p->headless && OpenClipboard(p->hwnd);
                HDROP drop = got ? (HDROP)GetClipboardData(CF_HDROP) : NULL;
                UINT n = drop ? DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) : 0;
                s64 chars = 0;
                for (UINT k = 0; k < n; k++) chars += DragQueryFileW(drop, k, NULL, 0) + 1;
                WCHAR *from = n ? (WCHAR *)calloc((size_t)chars + 1, sizeof(WCHAR)) : NULL;
                for (UINT k = 0, at = 0; from && k < n; k++) at += DragQueryFileW(drop, k, from + at, (UINT)(chars - at)) + 1;
                HANDLE effect = got ? GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)) : NULL;
                DWORD *e = effect ? (DWORD *)GlobalLock(effect) : NULL;
                b32 move = e && (*e & DROPEFFECT_MOVE);
                if (e) GlobalUnlock(effect);
                if (got) CloseClipboard();
                if (!from) { Logf("files", "nothing to paste: no files on the clipboard"); free(job); continue; }
                job->func = move ? FO_MOVE : FO_COPY;
                job->from = from;
                job->to = HeapPathList(&p->scratch, o->path);
                job->flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION | quiet;
            }
            HANDLE th = CreateThread(NULL, 0, FileOpThread, job, 0, NULL);
            if (th) CloseHandle(th);
            else { free(job->from); free(job->to); free(job); }
        }
    }
}

// Files dragged out of a browser, as Explorer drags them (one data object of their shell items; all in one
// folder). Copy or link only: a move would take them off this disk. Modal: OLE runs its own loop until the drop.
static void DragFilesOut(Platform *p, const String *paths, s64 count)
{
    s64 cut = paths[0].count;
    while (cut > 0 && paths[0].data[cut - 1] != '\\') cut--;
    WString folder = Utf8ToUtf16(&p->scratch, (String){ paths[0].data, cut });
    PIDLIST_ABSOLUTE folder_id = ILCreateFromPathW((PCWSTR)folder.data);
    PIDLIST_ABSOLUTE *full = Arena_PushArray(&p->scratch, PIDLIST_ABSOLUTE, count);
    PCUITEMID_CHILD *children = Arena_PushArray(&p->scratch, PCUITEMID_CHILD, count);
    s64 n = 0;
    for (s64 i = 0; i < count; i++) {
        full[n] = ILCreateFromPathW((PCWSTR)Utf8ToUtf16(&p->scratch, paths[i]).data);
        if (full[n]) { children[n] = (PCUITEMID_CHILD)ILFindLastID((PCUIDLIST_RELATIVE)full[n]); n++; }
    }
    IDataObject *data = NULL;
    if (folder_id && n && SUCCEEDED(SHCreateDataObject(folder_id, (UINT)n, children, NULL, &IID_IDataObject, (void **)&data))) {
        DWORD effect = 0;
        HRESULT hr = SHDoDragDrop(p->hwnd, data, NULL, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        Logf("files", "dragged %lld item(s) out (%s)", (long long)n, hr == DRAGDROP_S_DROP ? "dropped" : "cancelled");
        data->lpVtbl->Release(data);
    }
    for (s64 i = 0; i < n; i++) ILFree(full[i]);
    if (folder_id) ILFree(folder_id);
    // the button came up inside OLE's loop
    p->input.button[Mouse_Left].down = 0;
    p->input.button[Mouse_Left].released = 1;
    ReleaseCapture();
}

// --- files dragged in from other programs (Explorer and the like). OLE calls these on the window's thread
//     while our loop runs; the app decides what a drop under the mouse does (AppOutput.drop_effect, a frame
//     behind) and gets the dropped paths in the next frame's input ---
typedef struct DropTarget { IDropTarget base; LONG refs; Platform *p; } DropTarget;

static HRESULT STDMETHODCALLTYPE Drop_QueryInterface(IDropTarget *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Drop_AddRef(IDropTarget *self) { return (ULONG)InterlockedIncrement(&((DropTarget *)self)->refs); }
static ULONG STDMETHODCALLTYPE Drop_Release(IDropTarget *self) { return (ULONG)InterlockedDecrement(&((DropTarget *)self)->refs); }

static DWORD DropEffect(Platform *p) { return p->drop_effect ? DROPEFFECT_COPY : DROPEFFECT_NONE; }

static HRESULT STDMETHODCALLTYPE Drop_DragEnter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    (void)keys; (void)pt;
    Platform *p = ((DropTarget *)self)->p;
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    p->drag_over = data->lpVtbl->QueryGetData(data, &fmt) == S_OK;
    p->wake_frame = p->frame;
    *effect = p->drag_over ? DropEffect(p) : DROPEFFECT_NONE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Drop_DragOver(IDropTarget *self, DWORD keys, POINTL pt, DWORD *effect)
{
    (void)keys; (void)pt;
    Platform *p = ((DropTarget *)self)->p;
    p->wake_frame = p->frame;
    *effect = p->drag_over ? DropEffect(p) : DROPEFFECT_NONE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Drop_DragLeave(IDropTarget *self)
{
    Platform *p = ((DropTarget *)self)->p;
    p->drag_over = 0;
    p->wake_frame = p->frame;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Drop_Drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    (void)keys; (void)pt;
    Platform *p = ((DropTarget *)self)->p;
    *effect = DROPEFFECT_NONE;
    p->drag_over = 0;
    p->wake_frame = p->frame;
    if (!p->drop_effect) return S_OK;
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {0};
    if (data->lpVtbl->GetData(data, &fmt, &medium) != S_OK) return S_OK;
    HDROP drop = (HDROP)medium.hGlobal;
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    Arena_Clear(&p->drop_arena);
    p->dropped = Arena_PushArray(&p->drop_arena, String, n + 1);
    p->dropped_count = 0;
    for (UINT k = 0; k < n; k++) {
        WCHAR path[2048] = {0};
        UINT len = DragQueryFileW(drop, k, path, 2048);
        if (len) p->dropped[p->dropped_count++] = Utf16ToUtf8(&p->drop_arena, (WString){ (u16 *)path, (s64)len });
    }
    ReleaseStgMedium(&medium);
    Logf("files", "%lld item(s) dropped on the window", (long long)p->dropped_count);
    *effect = DROPEFFECT_COPY;
    return S_OK;
}

static IDropTargetVtbl g_DropVtbl = { Drop_QueryInterface, Drop_AddRef, Drop_Release, Drop_DragEnter, Drop_DragOver, Drop_DragLeave, Drop_Drop };
static DropTarget g_Drop;

static void ToggleFullscreen(Platform *p)
{
    HWND hwnd = p->hwnd;
    if (!p->fullscreen) {
        p->windowed.length = sizeof(p->windowed);
        GetWindowPlacement(hwnd, &p->windowed);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        p->fullscreen = 1;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &p->windowed);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        p->fullscreen = 0;
    }
}

// Opens what a Ctrl+click in a terminal named. http(s): the browser. A file (a file:// URI, or a path relative to cwd,
// maybe ending in :line[:col] or (line,col)): settings.txt `editor` when set ("code -g" gets "path:line:col"; {file},
// {line} and {col} are filled in where it has them), else its own program, except programs and scripts, which
// Explorer shows instead of running. A folder: Explorer. Nothing when the path isn't there.
static void OpenLink(Platform *p, String link, String cwd)
{
    Arena *a = &p->scratch;
    const char *who = p->headless ? "headless" : "app";
    if ((link.count > 7 && memcmp(link.data, "http://", 7) == 0) || (link.count > 8 && memcmp(link.data, "https://", 8) == 0)) {
        Logf(who, "open %.*s", STR_ARG(link));
        if (!p->headless) ShellExecuteW(p->hwnd, L"open", (LPCWSTR)Utf8ToUtf16(a, link).data, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    // --- the path: a file:// URI decoded, or the text ---
    TextBuilder tb = { a, NULL, 0, 0 };
    b32 uri = link.count > 7 && memcmp(link.data, "file://", 7) == 0;
    s64 from = 0;
    if (uri) {
        from = 7;
        while (from < link.count && link.data[from] != '/') from++;   // file://host/... : the host goes
        if (from + 2 < link.count && link.data[from + 2] == ':') from++;   // file:///C:/...
    }
    for (s64 i = from; i < link.count; i++) {
        u8 c = link.data[i];
        if (uri && c == '%' && i + 2 < link.count) {
            char hex[3] = { (char)link.data[i + 1], (char)link.data[i + 2], 0 };
            c = (u8)strtol(hex, NULL, 16);
            i += 2;
        }
        if (c == '/') c = '\\';
        TB_Append(&tb, (String){ &c, 1 });
    }
    String path = TB_String(&tb);
    // --- a line and column at the end: path:12:3, path:12, path(12,3) ---
    s32 line = 0, col = 0;
    for (s32 pass = 0; pass < 2 && path.count; pass++) {
        WString w = Utf8ToUtf16(a, path);
        WCHAR full[MAX_PATH * 2] = {0};
        b32 absolute = path.count >= 2 && (path.data[1] == ':' || (path.data[0] == '\\' && path.data[1] == '\\'));
        if (absolute || !cwd.count) wcsncpy_s(full, ArrayCount(full), (const WCHAR *)w.data, _TRUNCATE);
        else swprintf_s(full, ArrayCount(full), L"%s\\%s", (const WCHAR *)Utf8ToUtf16(a, cwd).data, (const WCHAR *)w.data);
        DWORD attrs = GetFileAttributesW(full);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            String shown = Utf16ToUtf8(a, (WString){ (u16 *)full, (s64)wcslen(full) });
            if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                Logf(who, "open folder %.*s", STR_ARG(shown));
                if (!p->headless) ShellExecuteW(p->hwnd, L"open", full, NULL, NULL, SW_SHOWNORMAL);
                return;
            }
            String editor = Term_ReadSetting(a, "editor");
            const WCHAR *dot = wcsrchr(full, L'.'), *slash = wcsrchr(full, L'\\');
            const WCHAR *ext = dot && (!slash || dot > slash) ? dot : L"";
            static const WCHAR *const runs[] = { L".exe", L".com", L".bat", L".cmd", L".ps1", L".vbs", L".vbe", L".js", L".jse", L".wsf",
                                                 L".wsh", L".msi", L".msp", L".lnk", L".scr", L".pif", L".hta", L".cpl", L".reg", L".jar" };
            b32 program = 0;
            for (s32 i = 0; i < (s32)ArrayCount(runs); i++) program |= _wcsicmp(ext, runs[i]) == 0;
            if (editor.count) {
                // the editor: {file} {line} {col} filled in, else "path:line:col" after it
                TextBuilder cb = { a, NULL, 0, 0 };
                b32 placed = 0;
                for (s64 i = 0; i < editor.count; i++) {
                    String rest = { editor.data + i, editor.count - i };
                    if (rest.count >= 6 && memcmp(rest.data, "{file}", 6) == 0) { TB_Format(&cb, "%.*s", STR_ARG(shown)); i += 5; placed = 1; }
                    else if (rest.count >= 6 && memcmp(rest.data, "{line}", 6) == 0) { TB_Format(&cb, "%d", Max(line, 1)); i += 5; }
                    else if (rest.count >= 5 && memcmp(rest.data, "{col}", 5) == 0) { TB_Format(&cb, "%d", Max(col, 1)); i += 4; }
                    else TB_Append(&cb, (String){ editor.data + i, 1 });
                }
                if (!placed) {
                    TB_Format(&cb, " \"%.*s", STR_ARG(shown));
                    if (line) TB_Format(&cb, ":%d", line);
                    if (line && col) TB_Format(&cb, ":%d", col);
                    TB_Append(&cb, S("\""));
                }
                String command = TB_String(&cb);
                Logf(who, "open %.*s with: %.*s", STR_ARG(shown), STR_ARG(command));
                if (!p->headless) {
                    WCHAR run[4096] = {0};
                    swprintf_s(run, ArrayCount(run), L"cmd.exe /d /c %s", (const WCHAR *)Utf8ToUtf16(a, command).data);
                    STARTUPINFOW si = { sizeof(si) };
                    PROCESS_INFORMATION pi = {0};
                    if (CreateProcessW(NULL, run, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                        AllowSetForegroundWindow(pi.dwProcessId);
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                }
            } else if (program) {
                Logf(who, "show %.*s in Explorer (a program: not run)", STR_ARG(shown));
                if (!p->headless) {
                    WCHAR args[MAX_PATH * 2 + 16] = {0};
                    swprintf_s(args, ArrayCount(args), L"/select,\"%s\"", full);
                    ShellExecuteW(p->hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
                }
            } else {
                Logf(who, "open file %.*s (line %d)", STR_ARG(shown), line);
                if (!p->headless) ShellExecuteW(p->hwnd, L"open", full, NULL, NULL, SW_SHOWNORMAL);
            }
            return;
        }
        // not there as written: take a line (and column) off the end and look again
        s64 end = path.count;
        if (path.data[end - 1] == ')') {
            s64 open_at = end - 1;
            while (open_at > 0 && path.data[open_at] != '(') open_at--;
            if (open_at <= 0) break;
            String inner = { path.data + open_at + 1, end - open_at - 2 };
            line = (s32)atoi((const char *)Str_PushCopy(a, inner).data);
            const u8 *comma = memchr(inner.data, ',', (size_t)inner.count);
            col = comma ? atoi((const char *)comma + 1) : 0;
            path.count = open_at;
        } else {
            s32 nums[2] = {0};
            s32 found = 0;
            while (found < 2) {
                s64 k = path.count;
                while (k > 0 && path.data[k - 1] >= '0' && path.data[k - 1] <= '9') k--;
                if (k == path.count || k < 2 || path.data[k - 1] != ':') break;
                nums[found++] = (s32)atoi((const char *)Str_PushCopy(a, (String){ path.data + k, path.count - k }).data);
                path.count = k - 1;
            }
            if (!found) break;
            line = found == 2 ? nums[1] : nums[0];
            col = found == 2 ? nums[0] : 0;
        }
    }
    Logf(who, "nothing to open at %.*s", STR_ARG(link));
}

// Starts another term window: `term --window NAME` (it shows the tabs the server says are that window's), at rect
// when given, its first tab a web page at web_url when given.
static void SpawnWindow(Platform *p, String name, const s32 *rect, String web_url)
{
    WCHAR exe[MAX_PATH] = {0}, cmd[MAX_PATH + 1400] = {0};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WString wname = Utf8ToUtf16(&p->scratch, name);
    if (rect) swprintf_s(cmd, ArrayCount(cmd), L"\"%s\" --window %s --at %d,%d,%d,%d", exe, (const WCHAR *)wname.data, rect[0], rect[1], rect[2], rect[3]);
    else swprintf_s(cmd, ArrayCount(cmd), L"\"%s\" --window %s", exe, (const WCHAR *)wname.data);
    if (web_url.count) {
        WString wurl = Utf8ToUtf16(&p->scratch, (String){ web_url.data, Min(web_url.count, 1200) });
        size_t at = wcslen(cmd);
        swprintf_s(cmd + at, ArrayCount(cmd) - at, L" --web \"%s\"", (const WCHAR *)wurl.data);
    }
    Logf(p->headless ? "headless" : "window", "open window %.*s", STR_ARG(name));
    if (p->headless) return;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Logf("window", "CreateProcess failed (%lu)", GetLastError());
    }
}

// Another moss window of this server instance at a screen point (its name in *name), else NULL.
static HWND MossWindowAt(Platform *p, POINT pt, String *name)
{
    HWND w = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    ATOM atom = w && w != p->hwnd ? (ATOM)(uintptr_t)GetPropW(w, L"moss.id") : 0;
    WCHAR id[160] = {0};
    WCHAR *bar = atom && GlobalGetAtomNameW(atom, id, 160) ? wcschr(id, L'|') : NULL;
    if (!bar || (size_t)(bar - id) != wcslen(p->instance) || wcsncmp(id, p->instance, (size_t)(bar - id)) != 0) return NULL;
    if (name) *name = Utf16ToUtf8(&p->scratch, (WString){ (u16 *)(bar + 1), (s64)wcslen(bar + 1) });
    return w;
}

// Tells another moss window about a tab of ours dragged over it (WM_COPYDATA 0x7E4E): kind 0 over it, 1 gone, 2 dropped;
// the pointer (screen), the server tab (0: a web tab), label \0 web address. Its answer: 1 when it took a drop.
static LRESULT SendDrag(Platform *p, HWND to, u32 kind, POINT pt, u32 tab, String label, String url)
{
    u8 buf[16 + sizeof(p->foreign_buf)] = {0};
    u32 head[4] = { kind, (u32)pt.x, (u32)pt.y, tab };
    memcpy(buf, head, sizeof(head));
    s64 n = sizeof(head);
    s64 label_n = Min(label.count, 200), url_n = Min(url.count, (s64)sizeof(p->foreign_buf) - 202);
    while (label_n < label.count && label_n && (label.data[label_n] & 0xC0) == 0x80) label_n--;   // whole characters
    memcpy(buf + n, label.data, (size_t)label_n);
    n += label_n;
    buf[n++] = 0;
    memcpy(buf + n, url.data, (size_t)url_n);
    n += url_n;
    COPYDATASTRUCT cd = { 0x7E4E, (DWORD)n, buf };
    DWORD_PTR result = 0;
    SendMessageTimeoutW(to, WM_COPYDATA, (WPARAM)p->hwnd, (LPARAM)&cd, SMTO_ABORTIFHUNG, 500, &result);
    return (LRESULT)result;
}

// The background image behind the terminals: any image Windows reads, scaled down to fit 4K, on the GPU. An empty path
// (or one that won't load) takes it away. 1 when it loaded.
// settings.txt's part of AppInit (strings in p->arena): at start, and again after a reset (AppOutput.reset_settings).
// Headless runs take the defaults, never the user's settings: the same script draws the same frames.
static void ReadSettings(Platform *p, AppInit *init, b32 headless)
{
    init->opacity = 1.0f;
    if (!headless) {
        init->settings_theme = Term_ReadSetting(&p->arena, "theme");
        init->agent_command = Term_ReadSetting(&p->arena, "agent");
        String font_size = Term_ReadSetting(&p->arena, "font_size");
        init->font_size_pt = font_size.count ? (f32)atof((const char *)font_size.data) : 0;
        String opacity = Term_ReadSetting(&p->arena, "opacity");
        if (opacity.count) init->opacity = (f32)atof((const char *)opacity.data) / 100.0f;
        init->term_font = Term_ReadSetting(&p->arena, "font");
        init->agents = Term_ReadSetting(&p->arena, "agents");
        init->hide_hidden = Str_Equal(Term_ReadSetting(&p->arena, "hidden_files"), S("off"));
        init->folder_sizes_off = Str_Equal(Term_ReadSetting(&p->arena, "folder_sizes"), S("off"));
        String strength = Term_ReadSetting(&p->arena, "background_strength");
        init->background_strength = !strength.count ? 0 : atoi((const char *)strength.data) > 0 ? atoi((const char *)strength.data) : -1;   // -1: off
        init->notifications_off = Str_Equal(Term_ReadSetting(&p->arena, "notifications"), S("off"));
        init->vs_env_off = Str_Equal(Term_ReadSetting(&p->arena, "vs_env"), S("off"));
        WCHAR home[MAX_PATH] = {0}, settings_path[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        swprintf_s(settings_path, MAX_PATH, L"%s\\.moss\\settings.txt", home);
        init->settings_text = Term_ReadFile(&p->arena, settings_path);
        String cursor = Term_ReadSetting(&p->arena, "cursor");
        init->cursor = Str_EqualNoCase(cursor, S("bar")) ? 1 : Str_EqualNoCase(cursor, S("underline")) ? 2 : 0;
        init->cursor_blink = Str_EqualNoCase(Term_ReadSetting(&p->arena, "cursor_blink"), S("on"));
        init->ligatures = Str_EqualNoCase(Term_ReadSetting(&p->arena, "ligatures"), S("on"));
        init->copy_on_select_off = Str_EqualNoCase(Term_ReadSetting(&p->arena, "copy_on_select"), S("off"));
        init->dim_unfocused_off = Str_EqualNoCase(Term_ReadSetting(&p->arena, "dim_unfocused"), S("off"));
        init->paste_warning_off = Str_EqualNoCase(Term_ReadSetting(&p->arena, "paste_warning"), S("off"));
        init->win32_input_off = Str_EqualNoCase(Term_ReadSetting(&p->arena, "win32_input"), S("off"));
        String backdrop = Term_ReadSetting(&p->arena, "backdrop");
        init->backdrop = Str_EqualNoCase(backdrop, S("acrylic")) ? 1 : Str_EqualNoCase(backdrop, S("mica")) ? 2 : 0;
    }
    if (!init->term_font.count) init->term_font = S("Cascadia Mono");   // ships with Windows 11; Consolas when missing
}

static b32 LoadBackground(Platform *p, String path)
{
    if (p->background) Gpu_FreeImage(p->gpu, p->background);
    p->background = 0;
    p->background_w = p->background_h = 0;
    p->background_path = (String){ p->background_buf, Min(path.count, (s64)sizeof(p->background_buf)) };
    memcpy(p->background_buf, path.data, (size_t)p->background_path.count);
    if (!path.count) return 0;
    s32 w = 0, h = 0, full_w = 0, full_h = 0;
    u8 *rgba = Image_Decode((const wchar_t *)Utf8ToUtf16(&p->scratch, path).data, 3840, 2160, &w, &h, &full_w, &full_h);
    if (!rgba) { Logf("window", "background: can't read %.*s", STR_ARG(path)); return 0; }
    for (s64 i = 0; i < (s64)w * h; i++) { u8 t = rgba[i * 4]; rgba[i * 4] = rgba[i * 4 + 2]; rgba[i * 4 + 2] = t; }   // RGBA -> BGRA
    p->background = Gpu_CreateImage(p->gpu, (const u32 *)rgba, w, h);
    free(rgba);
    p->background_w = w;
    p->background_h = h;
    p->wake_frame = p->frame;
    Logf("window", "background: %.*s, %d x %d (drawn at %d x %d)", STR_ARG(path), full_w, full_h, w, h);
    return p->background != 0;
}

// The image viewer's decoder: one picture at a time (image_job), any size scaled down to fit 4K, BGRA premultiplied.
static DWORD WINAPI ImageWorker(void *param)
{
    Platform *p = (Platform *)param;
    for (;;) {
        WaitForSingleObject(p->image_wake, INFINITE);
        WCHAR path[1024] = {0};
        EnterCriticalSection(&p->image_lock);
        MultiByteToWideChar(CP_UTF8, 0, (const char *)p->image_job, p->image_job_len, path, 1023);
        LeaveCriticalSection(&p->image_lock);
        s32 w = 0, h = 0, fw = 0, fh = 0;
        u8 *px = Image_Decode(path, 3840, 2160, &w, &h, &fw, &fh);
        for (s64 i = 0; px && i < (s64)w * h; i++) { u8 t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }   // RGBA -> BGRA
        EnterCriticalSection(&p->image_lock);
        p->image_done = px;
        p->image_done_w = w; p->image_done_h = h; p->image_done_fw = fw; p->image_done_fh = fh;
        p->image_done_ready = 1;
        LeaveCriticalSection(&p->image_lock);
        PostMessageW(p->hwnd, Wake_Files, 0, 0);
    }
}

// A decoded picture into the cache (a free slot, else the least recently wanted one), as a texture.
static void ImageCachePut(Platform *p, String path, u8 *bgra, s32 w, s32 h, s32 fw, s32 fh)
{
    s32 slot = 0;
    for (s32 i = 1; i < (s32)ArrayCount(p->images); i++) {
        if (!p->images[slot].path_len) break;
        if (!p->images[i].path_len || p->images[i].used_frame < p->images[slot].used_frame) slot = i;
    }
    if (p->images[slot].texture) Gpu_FreeImage(p->gpu, p->images[slot].texture);
    memset(&p->images[slot], 0, sizeof(p->images[slot]));
    p->images[slot].path_len = (s32)Min(path.count, (s64)sizeof(p->images[slot].path));
    memcpy(p->images[slot].path, path.data, (size_t)p->images[slot].path_len);
    p->images[slot].texture = bgra ? Gpu_CreateImage(p->gpu, (const u32 *)bgra, w, h) : 0;
    p->images[slot].failed = !p->images[slot].texture;
    p->images[slot].w = w; p->images[slot].h = h; p->images[slot].full_w = fw; p->images[slot].full_h = fh;
    p->images[slot].used_frame = p->frame;
    Logf("window", "view: %.*s %s (%d x %d)", STR_ARG(path), bgra ? "decoded" : "can't be read", fw, fh);
}

// --- one frame: the app, its requests, glyphs, rendering ---------------------------------------------------------

static const char *const g_PhaseNames[] = { "server", "app", "glyphs", "render", "present" };

// Runs the app on fi, carries out its requests, renders. paint: draw now even if nothing changed (WM_PAINT).
static void StepFrame(Platform *p, AppFrameInput *fi, b32 paint, b32 force_render)
{
    u64 t0 = Time_NowMicros();
    u64 phase[5] = {0};
    Debug_SetFrame(p->frame);

    // --- the server's latest; while it's gone, a new one every 2 s (it restores the tabs from tabs.txt) ---
    b32 closing = p->client && TermClient_ShouldClose(p->client);   // quit from the tray: no new server
    if (closing && !p->headless && p->hwnd && !p->closing) {
        p->closing = 1;
        Logf("window", "the server said goodbye: closing");
        PostMessageW(p->hwnd, WM_CLOSE, 0, 0);
    }
    if (p->client && !closing && !TermClient_ReadEvent(p->client) && Time_NowMicros() >= p->reconnect_at_us) {
        p->reconnect_at_us = Time_NowMicros() + 2000000;
        String error = {0};
        Logf("client", "connecting to the server again");
        TermClient_Connect(p->client, p->hwnd, p->cwd, &p->scratch, &error);
    }
    if (p->client) {
        TermClient_Poll(p->client);
        TermClient_Fill(p->client, fi);
    }
    if (p->cells) TermCells_CellSize(p->cells, &fi->cell_w, &fi->cell_h);
    MakeFonts(p);
    fi->fonts = p->fonts;
    fi->open_request = p->open_request;
    fi->open_cwd = p->open_cwd;
    fi->open_command = p->open_command;
    fi->mono_font_count = p->mono_font_count;
    fi->mono_fonts = p->mono_fonts;
    fi->background = p->background;
    fi->background_w = p->background_w;
    fi->background_h = p->background_h;
    fi->background_path = p->background_path;
    // --- the image viewer: `view` typed in a tab, the pictures that are ready, the editor ---
    fi->view_tab = p->view_tab;
    fi->view_path = (String){ p->view_path, p->view_path_len };
    p->view_tab = 0;
    s32 ready = 0;
    for (s32 i = 0; i < (s32)ArrayCount(p->images); i++) {
        if (!p->images[i].path_len) continue;
        p->image_list[ready++] = (AppImage){ { p->images[i].path, p->images[i].path_len }, p->images[i].texture, p->images[i].w, p->images[i].h,
                                             p->images[i].full_w, p->images[i].full_h, p->images[i].failed };
    }
    fi->image_count = ready;
    fi->images = p->image_list;
    fi->editor = p->editor;
    fi->sessions_meta_text = p->sessions_meta_text;
    p->sessions_meta_text = (String){0};   // once
    fi->pasted_image = Str_PushCopy(&p->scratch, (String){ p->pasted_image, p->pasted_image_len });
    p->pasted_image_len = 0;
    ReadAccounts(p);
    fi->accounts_text = (String){ p->accounts_text, p->accounts_len };
    fi->accounts_version = p->accounts_version;
    fi->settings = p->reset_ready ? &p->reset_settings : NULL;
    p->reset_ready = 0;
    fi->web_count = Web_Fill(p->web, &p->scratch, p->web_states, App_MaxWebs);
    fi->webs = p->web_states;
    fi->web_popup_count = Web_Popups(p->web, &p->scratch, p->web_popups, (s32)ArrayCount(p->web_popups));
    fi->web_popups = p->web_popups;
    fi->web_torn_off = p->torn_web;
    p->torn_web = 0;
    fi->open_web = p->open_web;
    // --- another window's tab dragged over this one: the mouse is where that window says (it has the mouse). A drag
    //     whose window went away ends with the button ---
    if (p->foreign_drag && !p->test_window && GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) >= 0) p->foreign_drag = 0;
    if ((p->foreign_drag || p->foreign_drop) && p->hwnd) {
        POINT fp = { p->foreign_at[0], p->foreign_at[1] };
        ScreenToClient(p->hwnd, &fp);
        fi->input.mouse[0] = fp.x;
        fi->input.mouse[1] = fp.y;
    }
    fi->foreign_drag = p->foreign_drag;
    fi->foreign_drop = p->foreign_drop;
    fi->foreign_tab = p->foreign_tab;
    fi->foreign_label = p->foreign_label;
    fi->foreign_url = p->foreign_url;
    p->foreign_drop = 0;
    // --- the debug overlay's numbers ---
    Debug_RecentWork(120, &fi->debug.work_ms_avg, &fi->debug.work_ms_max);
    fi->debug.frames_drawn = p->frames_drawn;
    fi->debug.frames_skipped = p->frames_skipped;
    if (p->client) { u64 msgs = 0; TermClient_Stats(p->client, &fi->debug.ipc_in, &fi->debug.ipc_out, &msgs); }
    if (p->cells) { s32 resolved = 0; TermCells_Stats(p->cells, &fi->debug.glyphs, &fi->debug.atlas_percent, &resolved); }
    if (p->gpu) Gpu_Times(p->gpu, &fi->debug.gpu_ms, &fi->debug.gpu_cells_ms);
    if (p->want_clipboard) {
        // headless runs have a clipboard of their own (copies, the script's `clipboard`), never the user's
        fi->clipboard_text = p->headless ? Str_PushCopy(&p->scratch, (String){ p->fake_clipboard, p->fake_clipboard_len }) : ReadClipboardUtf8(p->hwnd, &p->scratch);
        p->want_clipboard = 0;
    }
    for (s64 i = 0; i < fi->input.event_count; i++) {
        // a text edit's paste (Ctrl+V, Shift+Insert) gets the clipboard with the frame that has it
        const UiKeyEvent *e = &fi->input.events[i];
        b32 paste = e->down && (((e->modifiers & Mod_Ctrl) && e->key == Key_A + 'V' - 'A') || ((e->modifiers & Mod_Shift) && e->key == Key_Insert));
        if (!paste) continue;
        fi->input.clipboard = p->headless ? Str_PushCopy(&p->scratch, (String){ p->fake_clipboard, p->fake_clipboard_len }) : ReadClipboardUtf8(p->hwnd, &p->scratch);
        break;
    }
    fi->drag_over = p->drag_over;
    fi->drop_count = p->dropped_count;
    fi->dropped = p->dropped;
    // --- the browsers' listings and changes; the clock for their dates (fixed in headless runs, so captures don't
    //     depend on the day) ---
    Files_Drain(&fi->files, &p->scratch);
    if (p->headless) {
        fi->wall_time = 134259984000000000ull;   // 2026-06-15 12:00 UTC
    } else {
        FILETIME utc = {0}, local = {0};
        GetSystemTimeAsFileTime(&utc);
        FileTimeToLocalFileTime(&utc, &local);
        u64 u = ((u64)utc.dwHighDateTime << 32) | utc.dwLowDateTime, l = ((u64)local.dwHighDateTime << 32) | local.dwLowDateTime;
        fi->wall_time = u;
        fi->utc_offset_minutes = (s32)(((s64)l - (s64)u) / 600000000);
    }
    u64 t1 = Time_NowMicros();
    phase[0] = t1 - t0;

    AppOutput out = {0};
    App_Frame(p->app, fi, &out);
    Web_Update(p->web, out.webs, out.web_count, out.web_focus);
    u64 t2 = Time_NowMicros();
    phase[1] = t2 - t1;

    // --- the terminal font, requests, the cells' glyphs ---
    BEGIN_TIMED_BLOCK(Glyphs);
    if (out.term_font.count && !Str_Equal(out.term_font, p->term_font)) {
        p->term_font = (String){ p->term_font_buf, Min(out.term_font.count, (s64)sizeof(p->term_font_buf)) };
        memcpy(p->term_font_buf, out.term_font.data, (size_t)p->term_font.count);
        if (p->cells) TermCells_SetFont(p->cells, p->term_font, p->term_font_px);
        p->wake_frame = p->frame;   // the panes' sizes in cells change
    }
    if (p->cells && out.term_font_px > 0 && out.term_font_px != p->term_font_px) {
        p->term_font_px = out.term_font_px;
        TermCells_SetFont(p->cells, p->term_font, out.term_font_px);
        p->wake_frame = p->frame;
    }
    if (out.want_fonts && !p->mono_fonts_listed && p->cells) {
        p->mono_fonts_listed = 1;
        p->mono_font_count = TermCells_MonoFonts(p->cells, &p->arena, &p->mono_fonts);
        Logf("app", "%d monospace fonts", (int)p->mono_font_count);
        p->wake_frame = p->frame;
    }
    if (p->client) TermClient_Apply(p->client, &out);
    for (s64 i = 0; i < out.file_request_count; i++) Files_Request(&out.file_requests[i]);
    Files_Watch(out.file_watches, out.file_watch_count);
    if (p->cells && out.draw) TermCells_Prepare(p->cells, out.draw);
    END_TIMED_BLOCK(Glyphs);
    u64 t3 = Time_NowMicros();
    phase[2] = t3 - t2;

    // --- render and present: unchanged frames are skipped, presents paced to the refresh rate (at most 6 ms apart).
    //     A frame after a quiet spell (an echo) goes out at once ---
    p->present_deferred = 0;
    p->rendered_not_presented = 1;
    u64 now = Time_NowMicros();
    u64 hash = out.draw ? Hash_Bytes(&out.clear_color, sizeof(out.clear_color), out.draw->hash) ^ ((u64)p->client_w << 32 | (u64)p->client_h) : 0;
    p->last_changed = hash != p->presented_hash;
    p->last_animating = out.keep_animating;
    if (out.draw && (p->last_changed || paint || force_render || p->resized)) {
        u64 interval = Min(1000000ull / Max(1u, p->refresh_hz), 6000ull);
        if (!paint && !force_render && !p->resized && p->presented_hash && now - p->last_present_us < interval) {
            p->present_deferred = 1;
            p->frames_skipped++;
        } else {
            BEGIN_TIMED_BLOCK(Render);
            Gpu_Render(p->gpu, out.draw, &p->atlas, out.clear_color, p->client_w, p->client_h);
            END_TIMED_BLOCK(Render);
            u64 t4 = Time_NowMicros();
            phase[3] = t4 - t3;
            Gpu_Present(p->gpu);
            Web_Presented(p->web);
            Term_LatencyMark("client_present");
            phase[4] = Time_NowMicros() - t4;
            p->resized = 0;
            p->rendered_not_presented = 0;
            p->last_present_us = Time_NowMicros();
            p->presented_hash = hash;
            p->frames_drawn++;
        }
        p->wake_frame = p->frame;   // one more frame after anything new was drawn
    } else {
        p->frames_skipped++;
    }

    // --- side effects ---
    if (out.clipboard_set && out.clipboard_text.count && !p->headless) SetClipboardUtf8(p->hwnd, out.clipboard_text, &p->scratch);
    if (out.clipboard_set && p->headless) {
        Logf("headless", "clipboard: %.*s", STR_ARG(out.clipboard_text));
        p->fake_clipboard_len = (s32)Min(out.clipboard_text.count, (s64)sizeof(p->fake_clipboard));
        memcpy(p->fake_clipboard, out.clipboard_text.data, (size_t)p->fake_clipboard_len);
    }
    if (p->client) {
        String program_clip = TermClient_TakeClipboard(p->client, &p->scratch);
        if (program_clip.count && !p->headless) SetClipboardUtf8(p->hwnd, program_clip, &p->scratch);
        if (program_clip.count && p->headless) {
            Logf("headless", "clipboard from a program: %d bytes", (int)program_clip.count);
            p->fake_clipboard_len = (s32)Min(program_clip.count, (s64)sizeof(p->fake_clipboard));
            memcpy(p->fake_clipboard, program_clip.data, (size_t)p->fake_clipboard_len);
        }
        u32 view_tab = 0;
        String view = TermClient_TakeView(p->client, &p->scratch, &view_tab);
        if (view_tab) {
            p->view_tab = view_tab;
            p->view_path_len = (s32)Min(view.count, (s64)sizeof(p->view_path));
            memcpy(p->view_path, view.data, (size_t)p->view_path_len);
            p->wake_frame = p->frame;
        }
        String link = TermClient_TakeLink(p->client, &p->scratch);
        if (link.count) OpenLink(p, link, (String){0});
        if (TermClient_TakeBell(p->client) && p->hwnd && GetForegroundWindow() != p->hwnd) {
            FLASHWINFO flash = { sizeof(flash), p->hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
            FlashWindowEx(&flash);
        }
    }
    if (out.read_clipboard) { p->want_clipboard = 1; p->wake_frame = p->frame; }
    if (out.save_clipboard_image) {
        // headless runs have no pictures on their clipboard
        String saved = p->headless ? (String){0} : SaveClipboardImage(p);
        p->pasted_image_len = (s32)Min(saved.count, (s64)sizeof(p->pasted_image));
        memcpy(p->pasted_image, saved.data, (size_t)p->pasted_image_len);
        if (saved.count) p->wake_frame = p->frame;
    }
    {
        // --- the viewer's pictures: one the worker decoded becomes a texture; the first wanted one that isn't here is
        //     decoded next (at once in headless runs) ---
        EnterCriticalSection(&p->image_lock);
        b32 done = p->image_done_ready;
        u8 *px = p->image_done;
        s32 w = p->image_done_w, h = p->image_done_h, fw = p->image_done_fw, fh = p->image_done_fh;
        p->image_done_ready = 0;
        p->image_done = NULL;
        LeaveCriticalSection(&p->image_lock);
        if (done) {
            ImageCachePut(p, (String){ p->image_job, p->image_job_len }, px, w, h, fw, fh);
            free(px);
            p->image_job_len = 0;
        }
        for (s32 k = 0; k < out.image_want_count; k++) {
            String want = out.image_wants[k];
            b32 here = 0;
            for (s32 i = 0; i < (s32)ArrayCount(p->images) && !here; i++) {
                if (p->images[i].path_len != want.count || memcmp(p->images[i].path, want.data, (size_t)want.count) != 0) continue;
                p->images[i].used_frame = p->frame;
                here = 1;
            }
            if (here || p->image_job_len || want.count >= (s64)sizeof(p->image_job)) continue;
            if (p->headless) {
                s32 iw = 0, ih = 0, ifw = 0, ifh = 0;
                u8 *ipx = Image_Decode((const wchar_t *)Utf8ToUtf16(&p->scratch, want).data, 3840, 2160, &iw, &ih, &ifw, &ifh);
                for (s64 i = 0; ipx && i < (s64)iw * ih; i++) { u8 t = ipx[i * 4]; ipx[i * 4] = ipx[i * 4 + 2]; ipx[i * 4 + 2] = t; }
                ImageCachePut(p, want, ipx, iw, ih, ifw, ifh);
                free(ipx);
                p->wake_frame = p->frame;
                continue;
            }
            if (!p->image_thread) {
                p->image_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
                p->image_thread = CreateThread(NULL, 0, ImageWorker, p, 0, NULL);
            }
            EnterCriticalSection(&p->image_lock);
            memcpy(p->image_job, want.data, (size_t)want.count);
            p->image_job_len = (s32)want.count;
            LeaveCriticalSection(&p->image_lock);
            SetEvent(p->image_wake);
        }
    }
    if (out.open_url.count) OpenLink(p, out.open_url, (String){0});
    if (out.open_path.count) OpenLink(p, out.open_path, out.open_path_cwd);
    if (out.open_file.count) {
        Logf("app", "open %.*s", STR_ARG(out.open_file));
        if (!p->headless) ShellExecuteW(p->hwnd, L"open", (LPCWSTR)Utf8ToUtf16(&p->scratch, out.open_file).data, NULL, NULL, SW_SHOWNORMAL);
    }
    if (out.file_op_count) RunFileOps(p, out.file_ops, out.file_op_count);
    p->drop_effect = out.drop_effect;
    p->dropped_count = 0;
    if (out.drag_file_count) {
        if (p->headless) Logf("headless", "drag out: %.*s%s", STR_ARG(out.drag_files[0]), out.drag_file_count > 1 ? " ..." : "");
        else DragFilesOut(p, out.drag_files, out.drag_file_count);
    }
    if (out.clipboard_file_count) {
        Logf(p->headless ? "headless" : "app", "clipboard files: %.*s%s", STR_ARG(out.clipboard_files[0]), out.clipboard_file_count > 1 ? " ..." : "");
        if (!p->headless) SetClipboardFiles(p->hwnd, out.clipboard_files, out.clipboard_file_count, &p->scratch);
    }
    if (out.session_text.count && p->session_path.count) Term_WriteFileAtomic((const wchar_t *)p->session_path.data, out.session_text);
    if (out.add_account.count) AddAccount(p, out.add_account);
    if (out.sessions_meta_text.count && p->sessions_meta_path.count) Term_WriteFileAtomic((const wchar_t *)p->sessions_meta_path.data, out.sessions_meta_text);
    if (out.account_theme_name.count) SetAccountTheme(p, out.account_theme_name, out.account_theme);
    if (out.paths_text.count && p->paths_path.count) Term_WriteFileAtomic((const wchar_t *)p->paths_path.data, out.paths_text);
    for (s32 i = 0; i < out.setting_count && !p->headless; i++) {
        char key[64] = {0};
        memcpy(key, out.setting_keys[i].data, (size_t)Min(out.setting_keys[i].count, 63));
        Term_WriteSetting(key, out.setting_values[i]);
    }
    if (p->hwnd && (out.backdrop != p->backdrop || out.dark != p->dark || !p->backdrop_set)) {
        // --- what Windows draws behind the see-through parts: the frame extended over the whole window, then the
        //     system backdrop (Windows 11; older ones keep the desktop) and the frame's dark mode ---
        p->backdrop = out.backdrop;
        p->dark = out.dark;
        p->backdrop_set = 1;
        s32 all = p->backdrop ? -1 : 0;
        MARGINS m = { all, all, all, all };
        DwmExtendFrameIntoClientArea(p->hwnd, &m);
        BOOL dark = p->dark;
        DwmSetWindowAttribute(p->hwnd, 20, &dark, sizeof(dark));                    // DWMWA_USE_IMMERSIVE_DARK_MODE
        DWORD type = p->backdrop == 1 ? 3 : p->backdrop == 2 ? 2 : 1;              // acrylic, Mica, none
        DwmSetWindowAttribute(p->hwnd, 38, &type, sizeof(type));                    // DWMWA_SYSTEMBACKDROP_TYPE
        Logf("window", "backdrop %d (%s)", p->backdrop, p->dark ? "dark" : "light");
    }
    // --- a tab leaves this window: into the moss window under the mouse (same server; told where it was dropped), else
    //     a new window there. A web tab's page opens there again (it reloads) and its tab here goes ---
    POINT drag_pt = {0};
    if (!p->headless) GetCursorPos(&drag_pt);
    if (p->test_window) { drag_pt.x = p->posted_mouse[0]; drag_pt.y = p->posted_mouse[1]; ClientToScreen(p->hwnd, &drag_pt); }
    if ((out.tear_off_tab || out.tear_off_web) && p->client) {
        String to = {0};
        HWND target = out.tear_off_drop && !p->headless ? MossWindowAt(p, drag_pt, &to) : NULL;
        if (target) {
            Logf("window", "tab %u%s to the window %.*s", out.tear_off_tab, out.tear_off_web ? " (web)" : "", STR_ARG(to));
            b32 took = SendDrag(p, target, 2, drag_pt, out.tear_off_tab, (String){0}, out.tear_off_url) != 0;
            if (out.tear_off_tab) TermClient_SetOwner(p->client, out.tear_off_tab, to);
            if (out.tear_off_web && took) p->torn_web = out.tear_off_web;
            if (!p->test_window) SetForegroundWindow(target);   // test windows never take the foreground
        } else if (!out.tear_off_last) {
            // a new window the size of this one, its first tab under the mouse (from a drag), else just below this one
            p->tear_offs++;
            if (p->headless) {
                to = Str_PushFormat(&p->scratch, "w%u", p->tear_offs);
            } else {
                FILETIME ft = {0};
                GetSystemTimeAsFileTime(&ft);
                to = Str_PushFormat(&p->scratch, "w%llx", (((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000);
            }
            RECT wr = {0};
            if (p->hwnd) GetWindowRect(p->hwnd, &wr);
            s32 rect[4] = { wr.left + MulDiv(40, (int)p->dpi, 96), wr.top + MulDiv(40, (int)p->dpi, 96), wr.right - wr.left, wr.bottom - wr.top };
            if (out.tear_off_drop) { rect[0] = drag_pt.x - MulDiv(80, (int)p->dpi, 96); rect[1] = drag_pt.y - MulDiv(16, (int)p->dpi, 96); }
            if (out.tear_off_tab) TermClient_SetOwner(p->client, out.tear_off_tab, to);
            SpawnWindow(p, to, rect, out.tear_off_web ? (out.tear_off_url.count ? out.tear_off_url : S("about:blank")) : (String){0});
            if (out.tear_off_web) p->torn_web = out.tear_off_web;
        }
    }
    // --- our tab dragged over another moss window: it shows where the tab would land (told again on each move, and
    //     when the pointer leaves it) ---
    HWND drag_over = out.drag_out && !p->headless ? MossWindowAt(p, drag_pt, NULL) : NULL;
    if (p->drag_target && p->drag_target != drag_over) SendDrag(p, p->drag_target, 1, drag_pt, 0, (String){0}, (String){0});
    if (drag_over) SendDrag(p, drag_over, 0, drag_pt, 0, out.drag_out_label, (String){0});
    p->drag_target = drag_over;
    if (out.open_web_taken) p->open_web = (String){0};
    for (s64 i = 0; i < out.open_window_count; i++) {
        // the main window's start: windows that own tabs and aren't open (each holds its mutex while it is)
        WString w = Utf8ToUtf16(&p->scratch, out.open_windows[i]);
        WCHAR mutex[128] = {0};
        swprintf_s(mutex, 128, L"Local\\moss-window-%s", (const WCHAR *)w.data);
        HANDLE open = OpenMutexW(SYNCHRONIZE, FALSE, mutex);
        if (open) { CloseHandle(open); continue; }
        SpawnWindow(p, out.open_windows[i], NULL, (String){0});
    }
    if (out.open_taken) p->open_request = 0;
    if (out.start_with_windows) {
        // --- the server at sign-in (hidden: its tabs come back, the tray icon waits) ---
        HKEY run = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &run) == ERROR_SUCCESS) {
            if (out.start_with_windows == 1) {
                WCHAR exe[MAX_PATH] = {0}, command[MAX_PATH + 16] = {0};
                GetModuleFileNameW(NULL, exe, MAX_PATH);
                swprintf_s(command, ArrayCount(command), L"\"%s\" --server", exe);
                RegSetValueExW(run, L"moss", 0, REG_SZ, (const BYTE *)command, (DWORD)((wcslen(command) + 1) * 2));
            } else {
                RegDeleteValueW(run, L"moss");
            }
            RegCloseKey(run);
        }
        Logf("window", "start with Windows %s", out.start_with_windows == 1 ? "on" : "off");
    }
    if (out.explorer_menu) {
        // --- "Open in moss" on folders and folder backgrounds, for this user ---
        static const WCHAR *const keys[2] = { L"Software\\Classes\\Directory\\shell\\moss", L"Software\\Classes\\Directory\\Background\\shell\\moss" };
        WCHAR exe[MAX_PATH] = {0}, command[MAX_PATH + 32] = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        for (s32 k = 0; k < 2; k++) {
            if (out.explorer_menu == 2) { RegDeleteTreeW(HKEY_CURRENT_USER, keys[k]); continue; }
            HKEY key = NULL, sub = NULL;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, keys[k], 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) continue;
            const WCHAR *label = L"Open in moss";
            RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)label, (DWORD)((wcslen(label) + 1) * 2));
            RegSetValueExW(key, L"Icon", 0, REG_SZ, (const BYTE *)exe, (DWORD)((wcslen(exe) + 1) * 2));
            if (RegCreateKeyExW(key, L"command", 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL) == ERROR_SUCCESS) {
                swprintf_s(command, ArrayCount(command), L"\"%s\" -d \"%%%c\"", exe, k ? L'V' : L'1');
                RegSetValueExW(sub, NULL, 0, REG_SZ, (const BYTE *)command, (DWORD)((wcslen(command) + 1) * 2));
                RegCloseKey(sub);
            }
            RegCloseKey(key);
        }
        Logf("window", "Explorer menu %s", out.explorer_menu == 1 ? "added" : "removed");
    }
    if (out.good_programmer) {
        // --- "Be a good programmer": the principles into the global CLAUDE.md, or out (headless runs never touch it) ---
        const char *state = out.good_programmer == 1 ? "on" : "off";
        if (p->headless) Logf("headless", "be a good programmer %s (CLAUDE.md not written)", state);
        else if (Term_SetGoodProgrammer(out.good_programmer == 1)) Logf("window", "be a good programmer %s", state);
        else Logf("window", "be a good programmer %s: couldn't update the global CLAUDE.md", state);
    }
    if (out.notify_title.count) Logf("window", "a web tab's notification, to the tray");
    if (out.reset_settings) {
        // --- settings.txt back to moss's own (headless runs never write it), read again for the app's next frame ---
        if (p->headless) Logf("headless", "reset settings (settings.txt not written)");
        else { Term_ResetSettings(); Logf("window", "settings reset to moss's defaults"); }
        p->reset_settings = (AppInit){0};
        ReadSettings(p, &p->reset_settings, p->headless);
        p->reset_ready = 1;
        LoadBackground(p, p->headless ? (String){0} : Term_ReadSetting(&p->arena, "background_image"));
        p->wake_frame = p->frame;
    }
    if (out.choose_background) {
        // --- a file dialog for the background image (headless runs never show one) ---
        WCHAR file[1024] = {0};
        OPENFILENAMEW ofn = { sizeof(ofn) };
        ofn.hwndOwner = p->hwnd;
        ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.heic\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = 1024;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (p->headless) Logf("headless", "choose a background image (no dialog)");
        else if (GetOpenFileNameW(&ofn)) {
            String path = Utf16ToUtf8(&p->scratch, (WString){ (u16 *)file, (s64)wcslen(file) });
            if (LoadBackground(p, path)) Term_WriteSetting("background_image", path);
        }
    }
    if (out.open_settings) {
        WCHAR home[MAX_PATH] = {0}, path[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        swprintf_s(path, MAX_PATH, out.open_settings == 1 ? L"%s\\.moss\\settings.txt" : L"%s\\.moss", home);
        Logf(p->headless ? "headless" : "app", "open the settings %s", out.open_settings == 1 ? "file" : "folder");
        if (!p->headless) ShellExecuteW(p->hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }
    if (out.log_profile) Debug_LogProfile(Debug_ProfileFrames - 1, "requested");
    if (out.keep_animating) p->wake_frame = p->frame;
    p->wake_at_us = out.wake_at_us;
    memcpy(p->ime_caret, out.ime_caret, sizeof(p->ime_caret));
    if (p->client && !TermClient_ReadEvent(p->client) && (!p->wake_at_us || p->reconnect_at_us < p->wake_at_us)) p->wake_at_us = p->reconnect_at_us;
    p->caption_height = out.caption_height;
    p->resize_border = out.resize_border;
    p->caption_rect_count = Clamp(out.caption_client_rect_count, 0, App_MaxCaptionClientRects);
    for (s32 i = 0; i < p->caption_rect_count; i++) p->caption_rects[i] = out.caption_client_rects[i];
    p->min_width = out.min_width;
    p->min_height = out.min_height;

    if (p->headless) {
        if (out.window_command == AppWindow_Close) { Logf("headless", "the app closed the window"); p->quit = 1; }
    } else if (!paint) {
        HWND hwnd = p->hwnd;
        if (out.cursor < Platform_CursorCount && out.cursor != p->cursor) {
            p->cursor = out.cursor;
            SetCursor(p->cursors[p->cursor]);
        }
        if (out.window_title.count) {
            WString wt = Utf8ToUtf16(&p->scratch, out.window_title);
            if (wt.count < (s64)ArrayCount(p->title) && wcscmp((const WCHAR *)wt.data, p->title) != 0) {
                wcscpy_s(p->title, ArrayCount(p->title), (const WCHAR *)wt.data);
                SetWindowTextW(hwnd, p->title);
            }
        }
        if (out.set_window_placement && !p->shown && out.placement_rect[2] > 0 && out.placement_rect[3] > 0) {
            RECT r = { out.placement_rect[0], out.placement_rect[1], out.placement_rect[0] + out.placement_rect[2],
                       out.placement_rect[1] + out.placement_rect[3] };
            WINDOWPLACEMENT wp = {0};
            wp.length = sizeof(wp);
            GetWindowPlacement(hwnd, &wp);
            if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) OffsetRect(&r, wp.rcNormalPosition.left - r.left, wp.rcNormalPosition.top - r.top);
            wp.rcNormalPosition = r;
            wp.showCmd = SW_HIDE;
            SetWindowPlacement(hwnd, &wp);
            if (out.placement_maximized) p->windowed.showCmd = SW_SHOWMAXIMIZED;
        }
        b32 maximized = IsZoomed(hwnd) != 0;
        if (out.window_command == AppWindow_Minimize) ShowWindow(hwnd, SW_MINIMIZE);
        else if (out.window_command == AppWindow_ToggleMaximize) ShowWindow(hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
        else if (out.window_command == AppWindow_Close) DestroyWindow(hwnd);
        else if (out.window_command == AppWindow_Fullscreen) ToggleFullscreen(p);
        else if (out.window_command == AppWindow_ToggleTopmost) {
            b32 top = (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            SetWindowPos(hwnd, top ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            Logf("window", "always on top %s", top ? "off" : "on");
        }
    }

    u64 work = Time_NowMicros() - t0;
    Debug_FrameEnd(work, phase);
}

// --- the window --------------------------------------------------------------------------------------------------

static void FillWindowInput(Platform *p, AppFrameInput *fi, b32 paint, u64 now, u64 dt)
{
    HWND hwnd = p->hwnd;
    b32 maximized = IsZoomed(hwnd) != 0, minimized = IsIconic(hwnd) != 0;
    if (!minimized) {
        RECT cr = {0};
        GetClientRect(hwnd, &cr);
        s32 w = Max(1, cr.right - cr.left), h = Max(1, cr.bottom - cr.top);
        if (w != p->client_w || h != p->client_h) { p->resized = 1; fi->resized = 1; }
        p->client_w = w;
        p->client_h = h;
    }
    // --- the mouse: where it is over the window (far off when it isn't, unless a button is held) ---
    b32 any_button = 0;
    for (s32 b = 0; b < Mouse_Count; b++) any_button |= p->input.button[b].down || p->input.button[b].released;
    POINT cp = {0};
    GetCursorPos(&cp);
    if (p->test_window) {
        // a test window off screen: the mouse is where the posted messages say
        cp.x = p->posted_mouse[0];
        cp.y = p->posted_mouse[1];
        ClientToScreen(hwnd, &cp);
    }
    if (WindowFromPoint(cp) == hwnd || any_button || p->test_window) {
        ScreenToClient(hwnd, &cp);
        p->input.mouse[0] = cp.x;
        p->input.mouse[1] = cp.y;
    } else {
        p->input.mouse[0] = p->input.mouse[1] = -100000;
    }
    if (paint) {
        // a repaint (a resize drag): the mouse, no input (it waits for a real frame)
        fi->input.mouse[0] = p->input.mouse[0];
        fi->input.mouse[1] = p->input.mouse[1];
        fi->input.double_click_ms = p->input.double_click_ms;
        fi->input.wheel_scroll_lines = p->input.wheel_scroll_lines;
    } else {
        p->input.modifiers = KeyMods();
        fi->input = p->input;
        fi->input.event_count = p->event_count;
        fi->input.events = p->events;
        // --- a click on the title band: DefWindowProc's move loop swallows it; when the window didn't move it's a
        //     whole click for the app ---
        RECT wr = {0};
        GetWindowRect(hwnd, &wr);
        for (s32 b = 0; b < Mouse_Count; b++) {
            if (!p->caption_down[b]) continue;
            p->caption_down[b] = 0;
            if (abs(wr.left - p->caption_down_pos[0]) < GetSystemMetrics(SM_CXDRAG) && abs(wr.top - p->caption_down_pos[1]) < GetSystemMetrics(SM_CYDRAG)) {
                fi->input.button[b].down = 1;
                fi->input.button[b].pressed = 1;
                fi->input.button[b].released = 1;
            }
        }
    }
    fi->client_w = p->client_w;
    fi->client_h = p->client_h;
    fi->dpi = p->dpi;
    fi->foreground = GetForegroundWindow() == hwnd;
    fi->focused = p->focused || Web_HasFocus(p->web);   // a page's keyboard is still the window's
    fi->maximized = maximized;
    fi->now_us = now;
    fi->dt_us = dt;
    WINDOWPLACEMENT wp = {0};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp)) {
        fi->window_rect[0] = wp.rcNormalPosition.left;
        fi->window_rect[1] = wp.rcNormalPosition.top;
        fi->window_rect[2] = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        fi->window_rect[3] = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
}

static void RunFrame(Platform *p, b32 paint)
{
    if (p->in_frame || p->quit || !p->app || !p->gpu) return;
    p->in_frame = 1;
    u64 now = Time_NowMicros();
    u64 dt = (p->woke_from_wait || !p->last_frame_us) ? 0 : now - p->last_frame_us;
    p->last_frame_us = now;
    p->woke_from_wait = 0;
    p->frame++;
    Arena_Clear(&p->scratch);
    AppFrameInput fi = {0};
    FillWindowInput(p, &fi, paint, now, dt);
    StepFrame(p, &fi, paint, 0);
    p->in_frame = 0;
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        ((Platform *)cs->lpCreateParams)->hwnd = hwnd;
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    Platform *p = (Platform *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!p) return DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
    case Wake_Files:
        p->wake_frame = p->frame;
        return 0;
    case Test_Command: {
        WCHAR command[512] = {0};
        if (!p->test_window || !GetEnvironmentVariableW(L"MOSS_TEST_COMMAND", command, 512)) return 0;
        String name = Utf16ToUtf8(&p->scratch, (WString){ (u16 *)command, (s64)wcslen(command) });
        for (s32 k = 0; k < p->test_commands_run && name.count; k++) {
            s64 at = 0;
            while (at < name.count && name.data[at] != ';') at++;
            name = at < name.count ? (String){ name.data + at + 1, name.count - at - 1 } : (String){0};
        }
        for (s64 at = 0; at < name.count; at++) if (name.data[at] == ';') { name.count = at; break; }
        p->test_commands_run++;
        Logf("window", "test command %.*s: %s", STR_ARG(name), App_RunCommandByName(p->app, name) ? "ran" : "unknown");
        p->wake_frame = p->frame;
        return 0;
    }
    case WM_DESTROY:
        RevokeDragDrop(hwnd);
        p->quit = 1;
        PostQuitMessage(0);
        return 0;
    case WM_MOVE:
        UpdateMonitor(p);
        break;
    case WM_SIZE:
        p->wake_frame = p->frame;
        RunFrame(p, 1);   // live resizing: the move loop runs no frames of ours
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps = {0};
        BeginPaint(hwnd, &ps);
        RunFrame(p, 1);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        p->focused = msg == WM_SETFOCUS;
        if (!p->focused && !IsChild(hwnd, (HWND)wparam)) ReleaseCapture();   // the keyboard going to a page (a child) keeps a drag
        p->wake_frame = p->frame;
        break;
    case WM_ACTIVATE:
        p->wake_frame = p->frame;
        break;
    case WM_COPYDATA: {
        // another `moss -d DIR [-- command]` handing its tab to this window: cwd \0 command (UTF-8)
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lparam;
        if (cd && cd->dwData == 0x7E4D && cd->cbData < sizeof(p->open_buf)) {
            memcpy(p->open_buf, cd->lpData, cd->cbData);
            s64 n = (s64)cd->cbData, zero = 0;
            while (zero < n && p->open_buf[zero]) zero++;
            p->open_cwd = (String){ p->open_buf, zero };
            p->open_command = zero < n ? (String){ p->open_buf + zero + 1, n - zero - 1 } : (String){0};
            p->open_request = 1;
            p->wake_frame = p->frame;
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        if (cd && cd->dwData == 0x7E4E && cd->cbData >= 16 && cd->cbData <= 16 + sizeof(p->foreign_buf)) {
            // another moss window's tab dragged over this one (SendDrag): kind, the pointer, its server tab, label \0 url
            u32 head[4] = {0};
            memcpy(head, cd->lpData, sizeof(head));
            p->foreign_drag = head[0] == 0;
            p->foreign_at[0] = (s32)head[1];
            p->foreign_at[1] = (s32)head[2];
            if (head[0] != 1) {
                s64 n = (s64)cd->cbData - 16, zero = 0;
                memcpy(p->foreign_buf, (const u8 *)cd->lpData + 16, (size_t)n);
                while (zero < n && p->foreign_buf[zero]) zero++;
                p->foreign_tab = head[3];
                p->foreign_label = (String){ p->foreign_buf, zero };
                p->foreign_url = zero < n ? (String){ p->foreign_buf + zero + 1, n - zero - 1 } : (String){0};
            }
            if (head[0] == 2) {
                p->foreign_drop = 1;
                Logf("window", "a tab dropped here from another window (%s)", head[3] ? "server tab" : "web");
            }
            p->wake_frame = p->frame;
            return TRUE;
        }
        break;
    }
    case WM_HOTKEY:
        // the summon key (settings `summon`): brings the window up from anywhere, or puts it away when it's in front
        if (wparam == 1) {
            if (GetForegroundWindow() == hwnd && !IsIconic(hwnd)) ShowWindow(hwnd, SW_MINIMIZE);
            else { if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        }
        return 0;
    case WM_ENDSESSION:
        if (wparam) p->closing = 1;   // logging off: the windows come back with their tabs, none hands them on
        break;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) { HCURSOR page = (HCURSOR)Web_Cursor(p->web); SetCursor(page ? page : p->cursors[p->cursor % Platform_CursorCount]); return TRUE; }
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lparam;
        if (p->min_width > 0) mmi->ptMinTrackSize.x = p->min_width;
        if (p->min_height > 0) mmi->ptMinTrackSize.y = p->min_height;
        return 0;
    }
    case WM_NCCALCSIZE: {
        // our own title band: the client area keeps the sizing frame on the left, right and bottom, and the top only
        // when maximized (the window then reaches past the monitor's edge)
        if (p->fullscreen || !wparam) break;
        RECT *r = &((NCCALCSIZE_PARAMS *)lparam)->rgrc[0];
        s32 pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, p->dpi);
        s32 fx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, p->dpi) + pad, fy = GetSystemMetricsForDpi(SM_CYSIZEFRAME, p->dpi) + pad;
        r->left += fx;
        r->right -= fx;
        r->bottom -= fy;
        if (IsZoomed(hwnd)) {
            r->top += fy;
            // an auto-hidden taskbar at the bottom still needs a pixel to come up by
            APPBARDATA abd = { sizeof(abd) };
            if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) r->bottom -= 1;
        }
        return 0;
    }
    case WM_NCHITTEST: {
        if (p->fullscreen) return HTCLIENT;
        if (lparam != p->last_hit_test) p->wake_frame = p->frame;   // the mouse moved over the title band
        p->last_hit_test = lparam;
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &pt);
        if (pt.y < 0 || pt.y >= p->caption_height) break;
        // the title band: the window's drag handle, except the app's tabs and buttons; its top edge resizes
        LRESULT hit = HTCAPTION;
        for (s32 i = 0; i < p->caption_rect_count; i++) if (RectHas(p->caption_rects[i], pt.x, pt.y)) hit = HTCLIENT;
        if (IsZoomed(hwnd)) return hit;
        RECT cr = {0};
        GetClientRect(hwnd, &cr);
        b32 top = pt.y < p->resize_border;
        if (pt.x < 0) return top ? HTTOPLEFT : HTLEFT;
        if (pt.x >= cr.right) return top ? HTTOPRIGHT : HTRIGHT;
        return top ? HTTOP : hit;
    }
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
        if (wparam == HTCAPTION) {
            s32 b = msg == WM_NCLBUTTONDOWN ? Mouse_Left : msg == WM_NCRBUTTONDOWN ? Mouse_Right : Mouse_Middle;
            RECT wr = {0};
            GetWindowRect(hwnd, &wr);
            p->caption_down_pos[0] = wr.left;
            p->caption_down_pos[1] = wr.top;
            p->caption_down[b] = 1;
            p->wake_frame = p->frame;
            POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd, &pt);
            p->input.mouse[0] = pt.x;
            p->input.mouse[1] = pt.y;
        }
        break;
    case WM_NCRBUTTONUP:
        if (wparam == HTCAPTION) return 0;   // the app's settings, not the system menu
        break;
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        p->in_size_move = msg == WM_ENTERSIZEMOVE;
        p->wake_frame = p->frame;
        break;
    case WM_DPICHANGED: {
        p->dpi = HIWORD(wparam);
        Logf("window", "dpi %u", p->dpi);
        RECT *r = (RECT *)lparam;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        p->wake_frame = p->frame;
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        if (wparam == VK_F4 && (lparam & (1 << 29))) break;   // Alt+F4
        p->wake_frame = p->frame;
        u32 vk = (u32)wparam & 0xFF;
        if (vk == VK_RETURN && (lparam & (1 << 24))) vk = VK_SEPARATOR;   // the keypad's Enter
        u8 key = p->vk_to_key[vk];
        if (key == 1 || key == 2 || key == 4) return 0;   // modifiers travel as flags
        b32 up = ((u32)lparam >> 31) & 1;
        if (!up) Term_LatencyMark("key");
        UiKeyEvent *e = PushEvent(p, key, !up, up, (u32)(lparam & 0xFFFF), (u32)lparam, KeyMods());
        if (e) e->vk = (u16)wparam;
        if (msg == WM_SYSKEYDOWN && wparam == VK_SPACE) break;   // Alt+Space: the window menu
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR: {
        // typed text, control characters (Ctrl+letter) included; Alt+key arrives as WM_SYSCHAR (the app gives it ESC)
        p->wake_frame = p->frame;
        u32 ch = (u32)wparam;
        u16 dead = p->dead_char;
        p->dead_char = 0;
        if (msg == WM_SYSCHAR && ch == ' ') break;   // Alt+Space: the window menu
        if (ch >= 0xD800 && ch < 0xDC00) { p->high_surrogate = (u16)ch; return 0; }
        if (ch >= 0xDC00 && ch < 0xE000) {
            if (!p->high_surrogate) return 0;
            ch = 0x10000 + (((u32)p->high_surrogate - 0xD800) << 10) + (ch - 0xDC00);
        }
        p->high_surrogate = 0;
        if (ch == dead) return 0;
        if (ch == ' ' && GetKeyState(VK_CONTROL) < 0) return 0;   // Ctrl+Space: the key sends NUL
        PushText(p, ch, (u32)lparam, KeyMods());
        return 0;
    }
    case WM_DEADCHAR:
        p->dead_char = (u16)wparam;
        return 0;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION: {
        // IME (Chinese, Japanese, Korean input): composition and candidates at the focused terminal's cursor, in the
        // terminal's font; the finished text arrives as WM_CHAR
        HIMC imc = ImmGetContext(hwnd);
        if (imc) {
            POINT at = { p->ime_caret[0], p->ime_caret[1] };
            COMPOSITIONFORM form = { CFS_POINT, at, {0} };
            ImmSetCompositionWindow(imc, &form);
            CANDIDATEFORM cand = { 0, CFS_CANDIDATEPOS, { at.x, at.y + p->ime_caret[3] }, {0} };
            ImmSetCandidateWindow(imc, &cand);
            LOGFONTW lf = {0};
            lf.lfHeight = -Max(p->ime_caret[3], 8);
            WString face = Utf8ToUtf16(&p->scratch, p->term_font);
            wcsncpy_s(lf.lfFaceName, LF_FACESIZE, (const wchar_t *)face.data, _TRUNCATE);
            ImmSetCompositionFontW(imc, &lf);
            ImmReleaseContext(hwnd, imc);
        }
        break;
    }
    case WM_MOUSEMOVE:
        Web_Mouse(p->web, msg, wparam, lparam);   // the page under the mouse sees it too; the app keeps its hover
        p->wake_frame = p->frame;
        p->posted_mouse[0] = GET_X_LPARAM(lparam);
        p->posted_mouse[1] = GET_Y_LPARAM(lparam);
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    case WM_MOUSELEAVE:
        Web_Mouse(p->web, msg, wparam, lparam);
        p->wake_frame = p->frame;
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: {
        Web_Mouse(p->web, msg, wparam, lparam);
        p->wake_frame = p->frame;
        s32 b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? Mouse_Left : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? Mouse_Right : Mouse_Middle;
        p->input.button[b].down = 1;
        p->input.button[b].pressed = 1;
        p->posted_mouse[0] = GET_X_LPARAM(lparam);
        p->posted_mouse[1] = GET_Y_LPARAM(lparam);
        if (p->test_window) Logf("window", "test click %d at %d, %d", (int)b, p->posted_mouse[0], p->posted_mouse[1]);
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        Web_Mouse(p->web, msg, wparam, lparam);
        p->wake_frame = p->frame;
        s32 b = msg == WM_LBUTTONUP ? Mouse_Left : msg == WM_RBUTTONUP ? Mouse_Right : Mouse_Middle;
        p->input.button[b].released = 1;
        p->input.button[b].down = 0;
        if (!p->input.button[0].down && !p->input.button[1].down && !p->input.button[2].down) ReleaseCapture();
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (Web_Mouse(p->web, msg, wparam, lparam)) return 0;
        p->wake_frame = p->frame;
        p->input.wheel[1] += GET_WHEEL_DELTA_WPARAM(wparam);
        return 0;
    case WM_XBUTTONDOWN:
        if (Web_Mouse(p->web, msg, wparam, lparam)) return TRUE;
        p->wake_frame = p->frame;
        p->input.side = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? -1 : 1;
        return TRUE;
    case WM_XBUTTONUP:
        Web_Mouse(p->web, msg, wparam, lparam);
        return TRUE;
    case WM_MOUSEHWHEEL:
        if (Web_Mouse(p->web, msg, wparam, lparam)) return 0;
        p->wake_frame = p->frame;
        p->input.wheel[0] -= GET_WHEEL_DELTA_WPARAM(wparam);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The platform's key codes (term_ui.h's Key_*) for Windows' virtual keys.
static void InitKeyTable(Platform *p)
{
    u8 *t = p->vk_to_key;
    t[VK_SHIFT] = 1; t[VK_CONTROL] = 2; t[VK_MENU] = 4; t[VK_SPACE] = 5; t[VK_TAB] = 6; t[VK_RETURN] = 7;
    t[VK_BACK] = 8; t[VK_DELETE] = 9; t[VK_ESCAPE] = 10; t[VK_PRIOR] = 11; t[VK_NEXT] = 12; t[VK_HOME] = 13;
    t[VK_END] = 14; t[VK_LEFT] = 15; t[VK_RIGHT] = 16; t[VK_UP] = 17; t[VK_DOWN] = 18; t[VK_OEM_1] = 19;
    t[VK_OEM_PLUS] = 20; t[VK_OEM_MINUS] = 21; t[VK_OEM_PERIOD] = 22; t[VK_OEM_COMMA] = 23; t[VK_OEM_2] = 24;
    t[VK_OEM_5] = 25; t[VK_OEM_3] = 26; t[VK_OEM_7] = 27; t[VK_OEM_4] = 28; t[VK_OEM_6] = 29;
    t[VK_MULTIPLY] = 30; t[VK_ADD] = 31; t[VK_SEPARATOR] = 32; t[VK_SUBTRACT] = 33; t[VK_DIVIDE] = 34;
    t[VK_DECIMAL] = 34; t[VK_INSERT] = 46; t[VK_APPS] = 93;
    for (s32 i = 0; i < 10; i++) { t[VK_NUMPAD0 + i] = (u8)(36 + i); t['0' + i] = (u8)(48 + i); }
    for (s32 i = 0; i < 26; i++) t['A' + i] = (u8)(65 + i);
    for (s32 i = 0; i < 24; i++) t[VK_F1 + i] = (u8)(94 + i);
}

// --- headless: a script plays the user ------------------------------------------------------------------

typedef struct Headless {
    Platform *p;
    u64 clock_us;          // the fake clock: ticks of 1/120 s, so animations land on the same frames every run
    u32 tick;
    s32 mouse[2];
    b32 buttons[Mouse_Count];
    b32 prev_buttons[Mouse_Count];
    s16 wheel;
    s32 side;                    // `side back|forward`: the mouse's side buttons
    u8 mods_held;          // modifiers held now ("key" for one tick, "mods" until changed)
    u8 mods_sticky;        // what "mods" set (a key press goes back to it)
    UiKeyEvent events[64];
    s64 event_count;
    u8 text[1024];
    s64 text_used;
    b32 failed;            // a waitfor timed out, a label wasn't found, a compare differed
} Headless;

static void HeadlessTick(Headless *h)
{
    Platform *p = h->p;
    p->frame++;
    h->tick++;
    Arena_Clear(&p->scratch);
    h->clock_us += 8333;
    AppFrameInput fi = {0};
    fi.input.mouse[0] = h->mouse[0];
    fi.input.mouse[1] = h->mouse[1];
    fi.input.wheel_scroll_lines = 3;
    fi.input.double_click_ms = 500;
    fi.input.wheel[1] = h->wheel;
    fi.input.side = h->side;
    fi.input.modifiers = h->mods_held;
    for (s32 b = 0; b < Mouse_Count; b++) {
        if (h->buttons[b] && !h->prev_buttons[b]) {
            fi.input.button[b].pressed = 1;
        }
        if (h->buttons[b]) fi.input.button[b].down = 1;
        if (!h->buttons[b] && h->prev_buttons[b]) fi.input.button[b].released = 1;
        h->prev_buttons[b] = h->buttons[b];
    }
    fi.input.event_count = h->event_count;
    fi.input.events = h->events;
    fi.client_w = p->client_w;
    fi.client_h = p->client_h;
    fi.dpi = p->dpi;
    fi.foreground = 1;
    fi.focused = 1;
    fi.resized = p->resized;
    fi.now_us = h->clock_us;
    fi.dt_us = 8333;
    fi.window_rect[2] = p->client_w;
    fi.window_rect[3] = p->client_h;
    StepFrame(p, &fi, 0, 1);
    h->event_count = 0;
    h->text_used = 0;
    h->wheel = 0;
    h->side = 0;
}

static b32 NextToken(String *line, String *tok)
{
    while (line->count && line->data[0] == ' ') { line->data++; line->count--; }
    if (!line->count) return 0;
    s64 n = 0;
    while (n < line->count && line->data[n] != ' ') n++;
    *tok = (String){ line->data, n };
    line->data += n;
    line->count -= n;
    return 1;
}

static String Trim(String s)
{
    while (s.count && (s.data[0] == ' ' || s.data[0] == '\t')) { s.data++; s.count--; }
    while (s.count && (s.data[s.count - 1] == ' ' || s.data[s.count - 1] == '\t' || s.data[s.count - 1] == '\r')) s.count--;
    return s;
}

static s64 ParseInt(String s)
{
    char buf[32] = {0};
    memcpy(buf, s.data, (size_t)Min(s.count, 31));
    return _strtoi64(buf, NULL, 10);
}

// "Ctrl+Shift+T", "Alt+Shift+=", "Enter", "F11", "a" -> key code, modifiers, the text it types.
static b32 ParseKey(String k, u8 *key, u8 *mods, u32 *text)
{
    *key = 0; *mods = 0; *text = 0;
    for (;;) {
        s64 plus = -1;
        for (s64 i = 0; i + 1 < k.count; i++) if (k.data[i] == '+') { plus = i; break; }
        if (plus < 0) break;
        String m = { k.data, plus };
        if (Str_EqualNoCase(m, S("ctrl"))) *mods |= Mod_Ctrl;
        else if (Str_EqualNoCase(m, S("shift"))) *mods |= Mod_Shift;
        else if (Str_EqualNoCase(m, S("alt"))) *mods |= Mod_Alt;
        else return 0;
        k.data += plus + 1;
        k.count -= plus + 1;
    }
    static const struct { const char *name; u8 key; u32 text; } named[] = {
        { "space", 5, ' ' }, { "tab", 6, '\t' }, { "enter", 7, '\r' }, { "backspace", 8, 8 }, { "delete", 9, 0 }, { "del", 9, 0 },
        { "escape", 10, 27 }, { "esc", 10, 27 }, { "pageup", 11, 0 }, { "pgup", 11, 0 }, { "pagedown", 12, 0 }, { "pgdn", 12, 0 },
        { "home", 13, 0 }, { "end", 14, 0 }, { "left", 15, 0 }, { "right", 16, 0 }, { "up", 17, 0 }, { "down", 18, 0 },
        { "=", 20, '=' }, { "-", 21, '-' }, { ",", 23, ',' }, { "/", 24, '/' }, { "insert", 46, 0 },
    };
    for (s32 i = 0; i < (s32)ArrayCount(named); i++) {
        if (Str_EqualNoCase(k, Str_FromCStr(named[i].name))) { *key = named[i].key; *text = named[i].text; break; }
    }
    if (!*key && k.count == 1) {
        u8 c = k.data[0];
        if (c >= 'a' && c <= 'z') c = (u8)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') { *key = (u8)(65 + c - 'A'); *text = (*mods & Mod_Shift) ? c : (u32)(c - 'A' + 'a'); }
        else if (c >= '0' && c <= '9') { *key = (u8)(48 + c - '0'); *text = c; }
    }
    if (!*key && (k.data[0] == 'F' || k.data[0] == 'f') && k.count >= 2 && k.count <= 3) {
        s32 n = (s32)ParseInt((String){ k.data + 1, k.count - 1 });
        if (n >= 1 && n <= 24) { *key = (u8)(94 + n - 1); *text = 0; }
    }
    if (!*key) return 0;
    // what Windows would type with it: Ctrl+letter is the control character, Ctrl+other keys nothing
    if ((*mods & Mod_Ctrl) && *key >= 65 && *key < 91) *text = (u32)(*key - 64);
    else if ((*mods & Mod_Ctrl) && *text >= ' ') *text = 0;
    if (*key >= 11 && *key <= 18) *text = 0;
    return 1;
}

static b32 ParseNamedArg(String line, const char *name, String *value)
{
    String tok;
    s64 nlen = (s64)strlen(name);
    while (NextToken(&line, &tok)) {
        if (tok.count > nlen && memcmp(tok.data, name, (size_t)nlen) == 0 && tok.data[nlen] == '=') {
            *value = (String){ tok.data + nlen + 1, tok.count - nlen - 1 };
            return 1;
        }
    }
    return 0;
}

// The text of a pane's terminal (pane by order), "" when it has none.
static String PaneScreen(Platform *p, s32 pane, b32 attrs)
{
    s32 rect[4] = {0};
    u32 term_id = 0;
    if (!App_PaneRect(p->app, pane, rect, &term_id) || !term_id) return (String){0};
    return TermClient_ScreenText(p->client, term_id, &p->scratch, attrs);
}

static b32 Contains(String hay, String needle)
{
    for (s64 i = 0; i + needle.count <= hay.count; i++) if (memcmp(hay.data + i, needle.data, (size_t)needle.count) == 0) return 1;
    return 0;
}

static WString PathArg(Arena *a, String s)
{
    return Utf8ToUtf16(a, s);
}

static void CompareImages(Headless *h, String a, String b, String diff)
{
    Platform *p = h->p;
    s32 aw = 0, ah = 0, bw = 0, bh = 0;
    u8 *pa = Image_LoadPng((const wchar_t *)PathArg(&p->scratch, a).data, &p->scratch, &aw, &ah);
    u8 *pb = Image_LoadPng((const wchar_t *)PathArg(&p->scratch, b).data, &p->scratch, &bw, &bh);
    if (!pa || !pb || aw != bw || ah != bh) {
        Logf("script", "compare %.*s %.*s: DIFFERENT (%s)", STR_ARG(a), STR_ARG(b), !pa || !pb ? "missing" : "sizes differ");
        h->failed = 1;
        return;
    }
    s64 count = 0, max_err = 0, max_at = 0;
    f64 sum_sq = 0;
    u8 *d = diff.count ? (u8 *)Arena_Push(&p->scratch, (u64)aw * ah * 4, 16) : NULL;
    for (s64 i = 0; i < (s64)aw * ah; i++) {
        s32 e = 0;
        for (s32 c = 0; c < 3; c++) e = Max(e, abs((s32)pa[i * 4 + c] - (s32)pb[i * 4 + c]));
        if (e) { count++; sum_sq += (f64)e * e; }
        if (e > max_err) { max_err = e; max_at = i; }
        if (d) { u8 v = (u8)Min(255, e * 4); d[i * 4] = d[i * 4 + 1] = d[i * 4 + 2] = v; d[i * 4 + 3] = 255; }
    }
    if (d) Image_SavePng((const wchar_t *)PathArg(&p->scratch, diff).data, d, aw, ah);
    if (!count) {
        Logf("script", "compare %.*s %.*s: same", STR_ARG(a), STR_ARG(b));
    } else {
        Logf("script", "compare %.*s %.*s: DIFFERENT, %lld of %lld pixels, max %lld at %lld,%lld, rmse %.2f", STR_ARG(a), STR_ARG(b),
             (long long)count, (long long)aw * ah, (long long)max_err, (long long)(max_at % aw), (long long)(max_at / aw),
             sqrt(sum_sq / (f64)((s64)aw * ah)));
        h->failed = 1;
    }
}

static void HeadlessKey(Headless *h, String k)
{
    u8 key = 0, mods = 0;
    u32 text = 0;
    if (!ParseKey(k, &key, &mods, &text)) { Logf("script", "key: unknown key %.*s", STR_ARG(k)); h->failed = 1; return; }
    Term_LatencyMark("key");
    UiKeyEvent *e = &h->events[h->event_count++];
    memset(e, 0, sizeof(*e));
    e->key = key;
    e->down = 1;
    e->repeat_count = 1;
    e->modifiers = mods;
    // the Windows key behind it, as a real press would have (win32-input-mode)
    for (u32 vk = 1; vk < 256 && !e->vk; vk++) if (h->p->vk_to_key[vk] == key && vk != VK_SEPARATOR) e->vk = (u16)vk;
    e->scan = (u16)MapVirtualKeyW(e->vk, MAPVK_VK_TO_VSC);
    e->control_state = (u16)(((mods & Mod_Alt) ? 0x2 : 0) | ((mods & Mod_Ctrl) ? 0x8 : 0) | ((mods & Mod_Shift) ? 0x10 : 0));
    if (text) {
        String t = { h->text + h->text_used, Utf8_Encode(h->text + h->text_used, text) };
        h->text_used += t.count;
        e->text = t;
    }
    h->mods_held = mods;
    HeadlessTick(h);
    UiKeyEvent *up = &h->events[h->event_count++];
    memset(up, 0, sizeof(*up));
    up->key = key;
    up->up = 1;
    up->modifiers = mods;
    h->mods_held = h->mods_sticky;
    HeadlessTick(h);
}

// Ticks until the client has something or ms pass (real time: the server's answers take real time).
static void HeadlessWaitServer(Headless *h, u32 ms)
{
    HANDLE ev = (HANDLE)TermClient_ReadEvent(h->p->client);
    DWORD r = ev ? WaitForSingleObject(ev, ms) : (Sleep(ms), WAIT_TIMEOUT);
    if (r == WAIT_OBJECT_0) Term_LatencyMark("client_wake");
}

static int HeadlessMain(Platform *p, String script_text, String script_name)
{
    Headless h = {0};
    h.p = p;
    h.mouse[0] = p->client_w / 2;
    h.mouse[1] = p->client_h / 2;
    Logf("headless", "script %.*s, %d x %d at %u dpi", STR_ARG(script_name), p->client_w, p->client_h, p->dpi);
    HeadlessTick(&h);
    s32 line_no = 0;
    for (s64 at = 0; at < script_text.count && !p->quit; ) {
        s64 end = at;
        while (end < script_text.count && script_text.data[end] != '\n') end++;
        String line = Trim((String){ script_text.data + at, end - at });
        at = end + 1;
        line_no++;
        if (!line.count || line.data[0] == '#') continue;
        Debug_SetFrame(p->frame);
        Logf("script", "%.*s", STR_ARG(line));
        String rest = line, cmd = {0};
        NextToken(&rest, &cmd);
        rest = Trim(rest);
        if (Str_Equal(cmd, S("wait"))) {
            s64 n = rest.count ? ParseInt(rest) : 1;
            for (s64 i = 0; i < n && !p->quit; i++) HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("sleep"))) {
            // real time, the server working meanwhile; ticks keep the app current
            u64 until = Time_NowMicros() + (u64)ParseInt(rest) * 1000;
            while (Time_NowMicros() < until && !p->quit) { HeadlessWaitServer(&h, 16); HeadlessTick(&h); }
        } else if (Str_Equal(cmd, S("waitfor"))) {
            String text = rest;
            String v = {0};
            s32 pane = 0;
            u64 timeout_ms = 10000;
            // "waitfor some text pane=1 timeout=5000": options at the end
            for (;;) {
                s64 sp = text.count - 1;
                while (sp >= 0 && text.data[sp] != ' ') sp--;
                String last = { text.data + sp + 1, text.count - sp - 1 };
                if (ParseNamedArg(last, "pane", &v)) pane = (s32)ParseInt(v);
                else if (ParseNamedArg(last, "timeout", &v)) timeout_ms = (u64)ParseInt(v);
                else break;
                text.count = Max(0, sp);
                text = Trim(text);
            }
            u64 until = Time_NowMicros() + timeout_ms * 1000;
            b32 found = 0;
            while (!p->quit) {
                if (Contains(PaneScreen(p, pane, 0), text)) { found = 1; break; }
                if (Time_NowMicros() > until) break;
                HeadlessWaitServer(&h, 16);
                HeadlessTick(&h);
            }
            if (!found) {
                Logf("script", "waitfor: timed out after %llu ms, pane %d shows:\n%.*s", (unsigned long long)timeout_ms, pane, STR_ARG(PaneScreen(p, pane, 0)));
                h.failed = 1;
            }
        } else if (Str_Equal(cmd, S("waitlabel"))) {
            // until a UI target with that label exists (a tab's name arriving from its shell)
            u64 until = Time_NowMicros() + 10000000;
            s32 x = 0, y = 0;
            while (!p->quit && !App_FindLabel(p->app, rest, &x, &y) && Time_NowMicros() < until) { HeadlessWaitServer(&h, 16); HeadlessTick(&h); }
            if (!App_FindLabel(p->app, rest, &x, &y)) { Logf("script", "waitlabel: timed out (%.*s)", STR_ARG(rest)); h.failed = 1; }
        } else if (Str_Equal(cmd, S("waitgone"))) {
            // until no UI target has that label (a deleted file's row)
            u64 until = Time_NowMicros() + 10000000;
            s32 x = 0, y = 0;
            while (!p->quit && App_FindLabel(p->app, rest, &x, &y) && Time_NowMicros() < until) { HeadlessWaitServer(&h, 16); HeadlessTick(&h); }
            if (App_FindLabel(p->app, rest, &x, &y)) { Logf("script", "waitgone: timed out (%.*s)", STR_ARG(rest)); h.failed = 1; }
        } else if (Str_Equal(cmd, S("waitidle"))) {
            // two quiet ticks in a row: nothing new drawn, nothing animating
            s32 quiet = 0, i = 0;
            for (; i < 600 && quiet < 2; i++) {
                HeadlessWaitServer(&h, 5);
                HeadlessTick(&h);
                quiet = (!p->last_changed && !p->last_animating) ? quiet + 1 : 0;
            }
            if (quiet < 2) Logf("script", "waitidle: still busy after %d ticks (changed %d, animating %d)", i, p->last_changed, p->last_animating);
        } else if (Str_Equal(cmd, S("key"))) {
            HeadlessKey(&h, rest);
        } else if (Str_Equal(cmd, S("drop"))) {
            // files dropped at the mouse as another program would (paths relative to the run's folder)
            Arena_Clear(&p->drop_arena);
            p->dropped = Arena_PushArray(&p->drop_arena, String, 64);
            p->dropped_count = 0;
            String tok = {0}, args = rest;
            while (NextToken(&args, &tok) && p->dropped_count < 64) {
                WCHAR full[2048] = {0};
                GetFullPathNameW((LPCWSTR)Utf8ToUtf16(&p->scratch, tok).data, 2048, full, NULL);
                p->dropped[p->dropped_count++] = Utf16ToUtf8(&p->drop_arena, (WString){ (u16 *)full, (s64)wcslen(full) });
            }
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("type"))) {
            for (s64 i = 0; i < rest.count; ) {
                u32 cp = Utf8_Decode(rest, &i);
                if (h.event_count >= (s64)ArrayCount(h.events) - 2 || h.text_used + 4 > (s64)sizeof(h.text)) HeadlessTick(&h);
                UiKeyEvent *e = &h.events[h.event_count++];
                memset(e, 0, sizeof(*e));
                e->down = 1;
                e->repeat_count = 1;
                e->text = (String){ h.text + h.text_used, Utf8_Encode(h.text + h.text_used, cp) };
                h.text_used += e->text.count;
            }
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("cursor")) || Str_Equal(cmd, S("cursorby"))) {
            String a = {0}, b = {0};
            NextToken(&rest, &a);
            NextToken(&rest, &b);
            s32 x = (s32)ParseInt(a), y = (s32)ParseInt(b);
            if (Str_Equal(cmd, S("cursorby"))) { x += h.mouse[0]; y += h.mouse[1]; }
            h.mouse[0] = x;
            h.mouse[1] = y;
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("cursorui"))) {
            s32 x = 0, y = 0;
            if (App_FindLabel(p->app, rest, &x, &y)) { h.mouse[0] = x; h.mouse[1] = y; Logf("script", "cursor %d %d", x, y); HeadlessTick(&h); }
            else { Logf("script", "cursor: not found (%.*s)", STR_ARG(rest)); h.failed = 1; }
        } else if (Str_Equal(cmd, S("cursorpane"))) {
            String a = {0}, b = {0}, c = {0};
            NextToken(&rest, &a);
            b32 at_cell = NextToken(&rest, &b) && NextToken(&rest, &c);
            s32 rect[4] = {0};
            u32 id = 0;
            s32 cw = 1, ch = 1;
            TermCells_CellSize(p->cells, &cw, &ch);
            if (App_PaneRect(p->app, (s32)ParseInt(a), rect, &id)) {
                h.mouse[0] = at_cell ? rect[0] + (s32)ParseInt(b) * cw + cw / 2 : (rect[0] + rect[2]) / 2;
                h.mouse[1] = at_cell ? rect[1] + (s32)ParseInt(c) * ch + ch / 2 : (rect[1] + rect[3]) / 2;
                Logf("script", "cursor %d %d", h.mouse[0], h.mouse[1]);
                HeadlessTick(&h);
            } else { Logf("script", "cursor: not found (pane %.*s)", STR_ARG(a)); h.failed = 1; }
        } else if (Str_Equal(cmd, S("cursortext"))) {
            // "cursortext some text [pane=N]": onto the first cell of that text on the pane's screen
            String text = rest, v = {0};
            s32 pane = 0;
            s64 sp = text.count - 1;
            while (sp >= 0 && text.data[sp] != ' ') sp--;
            if (ParseNamedArg((String){ text.data + sp + 1, text.count - sp - 1 }, "pane", &v)) { pane = (s32)ParseInt(v); text.count = Max(0, sp); }
            s32 rect[4] = {0}, col = 0, row = 0, cw = 1, ch = 1;
            u32 id = 0;
            TermCells_CellSize(p->cells, &cw, &ch);
            if (App_PaneRect(p->app, pane, rect, &id) && TermClient_FindText(p->client, id, text, &col, &row)) {
                h.mouse[0] = rect[0] + col * cw + cw / 2;
                h.mouse[1] = rect[1] + row * ch + ch / 2;
                Logf("script", "cursor %d %d (cell %d, %d)", h.mouse[0], h.mouse[1], col, row);
                HeadlessTick(&h);
            } else { Logf("script", "cursor: not found (text %.*s on pane %d)", STR_ARG(text), pane); h.failed = 1; }
        } else if (Str_Equal(cmd, S("lmb")) || Str_Equal(cmd, S("rmb")) || Str_Equal(cmd, S("mmb"))) {
            s32 b = cmd.data[0] == 'l' ? Mouse_Left : cmd.data[0] == 'r' ? Mouse_Right : Mouse_Middle;
            h.buttons[b] = Str_Equal(rest, S("down"));
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("clipboard"))) {
            // "clipboard some text" (\n for a line break): what a paste gets
            s32 n = 0;
            for (s64 i = 0; i < rest.count && n < (s32)sizeof(p->fake_clipboard); i++) {
                if (rest.data[i] == '\\' && i + 1 < rest.count && rest.data[i + 1] == 'n') { p->fake_clipboard[n++] = '\n'; i++; }
                else p->fake_clipboard[n++] = rest.data[i];
            }
            p->fake_clipboard_len = n;
        } else if (Str_Equal(cmd, S("mods"))) {
            // held for the clicks that follow: "mods ctrl", "mods ctrl+shift", "mods none"
            h.mods_held = 0;
            if (Contains(rest, S("ctrl"))) h.mods_held |= Mod_Ctrl;
            if (Contains(rest, S("shift"))) h.mods_held |= Mod_Shift;
            if (Contains(rest, S("alt"))) h.mods_held |= Mod_Alt;
            h.mods_sticky = h.mods_held;
        } else if (Str_Equal(cmd, S("wheel"))) {
            h.wheel = (s16)(ParseInt(rest) * 120);
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("side"))) {
            h.side = Str_Equal(rest, S("back")) ? -1 : 1;
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("capture"))) {
            s32 w = 0, hh = 0;
            u8 *px = Gpu_ReadPixels(p->gpu, &p->scratch, &w, &hh);
            if (px && Image_SavePng((const wchar_t *)PathArg(&p->scratch, rest).data, px, w, hh)) Logf("script", "captured %.*s (%d x %d)", STR_ARG(rest), w, hh);
            else { Logf("script", "capture failed: %.*s", STR_ARG(rest)); h.failed = 1; }
        } else if (Str_Equal(cmd, S("compare"))) {
            String a = {0}, b = {0}, d = {0};
            NextToken(&rest, &a);
            NextToken(&rest, &b);
            NextToken(&rest, &d);
            CompareImages(&h, a, b, d);
        } else if (Str_Equal(cmd, S("dump"))) {
            String what = {0}, v = {0};
            NextToken(&rest, &what);
            if (Str_Equal(what, S("layout"))) {
                Logf("dump", "layout:\n%.*s", STR_ARG(App_DumpLayout(p->app, &p->scratch)));
            } else if (Str_Equal(what, S("screen"))) {
                s32 pane = ParseNamedArg(rest, "pane", &v) ? (s32)ParseInt(v) : 0;
                b32 attrs = Contains(rest, S("attrs"));
                Logf("dump", "screen of pane %d:\n%.*s", pane, STR_ARG(PaneScreen(p, pane, attrs)));
            } else if (Str_Equal(what, S("tabs"))) {
                AppFrameInput fi = {0};
                TermClient_Fill(p->client, &fi);
                for (s64 i = 0; i < fi.tab_count; i++) {
                    const TermTab *t = &fi.tabs[i];
                    Logf("dump", "tab %u: %.*s | agent %.*s | flags 0x%x | cpu %.0f%% | cwd %.*s%s%.*s | key %.*s", t->id, STR_ARG(t->name), STR_ARG(t->agent),
                         t->flags, (double)t->cpu, STR_ARG(t->cwd), t->owner.count ? " | window " : "", STR_ARG(t->owner), STR_ARG(t->key));
                }
            } else if (Str_Equal(what, S("commands"))) {
                Logf("dump", "commands:\n%.*s", STR_ARG(App_CommandNames(p->app, &p->scratch)));
            }
        } else if (Str_Equal(cmd, S("cmd"))) {
            if (!App_RunCommandByName(p->app, rest)) { Logf("script", "cmd: unknown command %.*s", STR_ARG(rest)); h.failed = 1; }
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("dpi"))) {
            p->dpi = (u32)ParseInt(rest);
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("size"))) {
            String a = {0};
            NextToken(&rest, &a);
            s32 w = 0, hh = 0;
            char buf[32] = {0};
            memcpy(buf, a.data, (size_t)Min(a.count, 31));
            if (sscanf_s(buf, "%dx%d", &w, &hh) == 2) { p->client_w = w; p->client_h = hh; p->resized = 1; }
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("background"))) {
            // "background FILE": the background image (relative to the run's folder), as choosing it in the settings
            WCHAR full[2048] = {0};
            GetFullPathNameW((LPCWSTR)Utf8ToUtf16(&p->scratch, rest).data, 2048, full, NULL);
            if (!LoadBackground(p, Utf16ToUtf8(&p->scratch, (WString){ (u16 *)full, (s64)wcslen(full) }))) h.failed = 1;
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("profile"))) {
            Debug_LogProfile(rest.count ? (int)ParseInt(rest) : Debug_ProfileFrames - 1, "script");
        } else if (Str_Equal(cmd, S("stats"))) {
            u64 bin = 0, bout = 0, msgs = 0;
            TermClient_Stats(p->client, &bin, &bout, &msgs);
            s32 glyphs = 0, atlas = 0, resolved = 0;
            TermCells_Stats(p->cells, &glyphs, &atlas, &resolved);
            f32 gpu_ms = 0, cells_ms = 0;
            Gpu_Times(p->gpu, &gpu_ms, &cells_ms);
            Logf("stats", "frames drawn %llu, skipped %llu; glyphs %d, atlas %d%% full; ipc in %llu bytes (%llu messages), out %llu bytes; gpu %.3f ms (cells %.3f ms)",
                 (unsigned long long)p->frames_drawn, (unsigned long long)p->frames_skipped, glyphs,
                 atlas, (unsigned long long)bin, (unsigned long long)msgs, (unsigned long long)bout, (double)gpu_ms, (double)cells_ms);
        } else if (Str_Equal(cmd, S("log"))) {
            // already echoed above
        } else if (Str_Equal(cmd, S("crashserver"))) {
            TermClient_Debug(p->client, 1);
            HeadlessWaitServer(&h, 2000);
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("quitserver"))) {
            TermClient_Debug(p->client, 2);
            HeadlessWaitServer(&h, 2000);
            HeadlessTick(&h);
        } else if (Str_Equal(cmd, S("crash"))) {
            Debug_Crash();
        } else if (Str_Equal(cmd, S("exit"))) {
            break;
        } else {
            Logf("script", "line %d: unknown command %.*s", line_no, STR_ARG(cmd));
            h.failed = 1;
        }
    }
    Logf("headless", "done after %u ticks%s", h.tick, h.failed ? ", with failures" : "");
    return h.failed ? 2 : 0;
}

// --- entry -----------------------------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, PWSTR cmd_line, int show_cmd)
{
    (void)prev_instance; (void)cmd_line; (void)show_cmd;
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    b32 headless = 0, test = 0;
    const WCHAR *probe_url = NULL, *probe_png = NULL;
    const WCHAR *script = NULL, *session = NULL, *replay = NULL, *instance_name = NULL, *window_name = NULL, *at = NULL, *open_dir = NULL, *web_url = NULL;
    WCHAR open_command[1024] = {0};   // `term -- command line`: a tab running it
    s32 size_w = 1280, size_h = 800, dpi_arg = 96;
    if (argc >= 2 && wcscmp(argv[1], L"view") == 0) {
        // --- `moss view [file or folder]` in a tab: OSC 7777;view;<full path> to the console it runs in (the tab's);
        //     the window shows it over the tab ---
        WCHAR full[2048] = {0};
        if (!GetFullPathNameW(argc >= 3 ? argv[2] : L".", 2048, full, NULL)) return 1;
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return 1;
        HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        WCHAR osc[2200] = {0};
        swprintf_s(osc, 2200, L"\x1b]7777;view;%s\x1b\\", full);
        DWORD written = 0;
        WriteConsoleW(out, osc, (DWORD)wcslen(osc), &written, NULL);
        return 0;
    }
    for (s32 i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--server") == 0) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return Term_ServerMain();
        }
        if (wcscmp(argv[i], L"--headless") == 0) headless = 1;
        else if (wcscmp(argv[i], L"--test") == 0) test = 1;
        else if (wcscmp(argv[i], L"--script") == 0 && i + 1 < argc) script = argv[++i];
        else if (wcscmp(argv[i], L"--session") == 0 && i + 1 < argc) session = argv[++i];
        else if (wcscmp(argv[i], L"--instance") == 0 && i + 1 < argc) instance_name = argv[++i];
        else if (wcscmp(argv[i], L"--window") == 0 && i + 1 < argc) window_name = argv[++i];
        else if ((wcscmp(argv[i], L"-d") == 0 || wcscmp(argv[i], L"--cwd") == 0) && i + 1 < argc) open_dir = argv[++i];
        else if (wcscmp(argv[i], L"--") == 0) {
            // the rest is the command line, quoted again where it has spaces
            for (s32 k = i + 1; k < argc; k++) {
                b32 quote = wcschr(argv[k], L' ') != NULL;
                swprintf_s(open_command + wcslen(open_command), ArrayCount(open_command) - wcslen(open_command), L"%s%s%s%s",
                           k > i + 1 ? L" " : L"", quote ? L"\"" : L"", argv[k], quote ? L"\"" : L"");
            }
            break;
        }
        else if (wcscmp(argv[i], L"--at") == 0 && i + 1 < argc) at = argv[++i];
        else if (wcscmp(argv[i], L"--web") == 0 && i + 1 < argc) web_url = argv[++i];
        else if (wcscmp(argv[i], L"--vt-replay") == 0 && i + 1 < argc) replay = argv[++i];
        else if (wcscmp(argv[i], L"--web-probe") == 0 && i + 2 < argc) { probe_url = argv[++i]; probe_png = argv[++i]; }
        else if (wcscmp(argv[i], L"--size") == 0 && i + 1 < argc) swscanf_s(argv[++i], L"%dx%d", &size_w, &size_h);
        else if (wcscmp(argv[i], L"--dpi") == 0 && i + 1 < argc) dpi_arg = _wtoi(argv[++i]);
    }
    if (replay) {
        Debug_Init("replay", 1);
        return Term_VtReplayMain(replay, size_w == 1280 ? 120 : size_w, size_h == 800 ? 40 : size_h);
    }

    if (probe_url) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Debug_Init("webprobe", 1);
        OleInitialize(NULL);
        return Web_ProbeMain(probe_url, probe_png);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Time_NowMicros();
    // --- one window: a second `term` brings the first one up instead (the server serves one window at a time, and
    //     both would write one session file). The mutex is held until this process exits ---
    if (window_name && !headless) {
        // another window (torn off): one process per name
        WCHAR mutex[128] = {0};
        swprintf_s(mutex, 128, L"Local\\moss-window-%s", window_name);
        HANDLE one = CreateMutexW(NULL, FALSE, mutex);
        if (one && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    }
    if (!headless && !test && !instance_name && !replay && !window_name) {
        HANDLE one = CreateMutexW(NULL, FALSE, L"Local\\moss-window");
        if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND other = NULL;
            for (s32 tries = 0; tries < 40 && !other; tries++) {   // it may be starting: up to 2 s
                for (HWND w = FindWindowExW(NULL, NULL, L"moss", NULL); w && !other; w = FindWindowExW(NULL, w, L"moss", NULL)) {
                    if (GetPropW(w, L"moss.main")) other = w;
                }
                if (!other) Sleep(50);
            }
            if (other) {
                if (open_dir || open_command[0]) {
                    // a tab for the running window: cwd \0 command, UTF-8
                    char data[2048] = {0};
                    WCHAR full[MAX_PATH] = {0};
                    if (open_dir) GetFullPathNameW(open_dir, MAX_PATH, full, NULL);
                    s32 n = WideCharToMultiByte(CP_UTF8, 0, full, -1, data, 1024, NULL, NULL);
                    s32 m = WideCharToMultiByte(CP_UTF8, 0, open_command, -1, data + n, 1000, NULL, NULL);
                    COPYDATASTRUCT cd = { 0x7E4D, (DWORD)(n + Max(m - 1, 0)), data };
                    DWORD pid = 0;
                    GetWindowThreadProcessId(other, &pid);
                    AllowSetForegroundWindow(pid);
                    SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
                } else {
                    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
                    SetForegroundWindow(other);
                }
            }
            return 0;
        }
    }
    Debug_Init(headless ? "headless" : "window", headless);
    Debug_NameThread("main");
    Debug_SetPhaseNames(g_PhaseNames, 5);
    Debug_SetSlowFrameUs(headless ? 50000 : 8000);
    if (instance_name) {
        // a named private server that outlives this run (headless session tests): no tray icon
        SetEnvironmentVariableW(L"MOSS_INSTANCE", instance_name);
        SetEnvironmentVariableW(L"MOSS_NO_TRAY", L"1");
    } else if (headless || test) {
        // a private server for this run (inherited by the server the client starts), which ends with us
        WCHAR name[64];
        swprintf_s(name, 64, L"%s-%lu", headless ? L"headless" : L"test", GetCurrentProcessId());
        SetEnvironmentVariableW(L"MOSS_INSTANCE", name);
        SetEnvironmentVariableW(L"MOSS_EPHEMERAL", L"1");
    }
    // the user's Claude accounts and folder rules aren't a test's (its server inherits this): none unless the test has some
    if ((headless || test) && !GetEnvironmentVariableW(L"MOSS_ACCOUNTS_FILE", NULL, 0)) SetEnvironmentVariableW(L"MOSS_ACCOUNTS_FILE", L"NUL");
    OleInitialize(NULL);   // COM (apartment) and OLE: drag and drop, the clipboard
    Term_RaiseForTyping();
    Term_PrepareUser();

    Arena boot = {0};
    Platform *p = Arena_PushStruct(&boot, Platform);
    p->arena = boot;
    p->headless = headless;
    InitializeCriticalSection(&p->image_lock);
    GetEnvironmentVariableW(L"MOSS_INSTANCE", p->instance, 64);
    if (window_name) p->window_name = Utf16ToUtf8(&p->arena, (WString){ (u16 *)window_name, (s64)wcslen(window_name) });
    if (web_url) p->open_web = Utf16ToUtf8(&p->arena, (WString){ (u16 *)web_url, (s64)wcslen(web_url) });
    p->start_at = at && swscanf_s(at, L"%d,%d,%d,%d", &p->start_rect[0], &p->start_rect[1], &p->start_rect[2], &p->start_rect[3]) == 4;
    p->focused = 1;
    InitKeyTable(p);
    if (open_dir || open_command[0]) {
        // asked for a tab: it opens once the server's tabs are in
        WCHAR full[MAX_PATH] = {0};
        if (open_dir) GetFullPathNameW(open_dir, MAX_PATH, full, NULL);
        s32 n = WideCharToMultiByte(CP_UTF8, 0, full, -1, (char *)p->open_buf, 1024, NULL, NULL);
        s32 m = WideCharToMultiByte(CP_UTF8, 0, open_command, -1, (char *)p->open_buf + n, 1000, NULL, NULL);
        p->open_cwd = (String){ p->open_buf, Max(n - 1, 0) };
        p->open_command = (String){ p->open_buf + n, Max(m - 1, 0) };
        p->open_request = 1;
    }
    {
        HKEY key = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\moss", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            p->explorer_menu = 1;
            RegCloseKey(key);
        }
    }

    // --- settings and the session ---
    AppInit init = {0};
    init.headless = headless;
    ReadSettings(p, &init, headless);
    p->term_font = init.term_font;
    init.good_programmer = !headless && Term_GoodProgrammer();
    init.window_name = p->window_name;
    init.explorer_menu = p->explorer_menu;
    {
        HKEY run = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &run) == ERROR_SUCCESS) {
            init.start_with_windows = RegQueryValueExW(run, L"moss", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
            RegCloseKey(run);
        }
    }
    {
        // accounts.txt (not a setting: a reset keeps it)
        WCHAR home[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        if (!GetEnvironmentVariableW(L"MOSS_ACCOUNTS_FILE", p->accounts_path, MAX_PATH)) swprintf_s(p->accounts_path, MAX_PATH, L"%s\\.moss\\accounts.txt", home);
    }
    if (session) {
        p->session_path = (WString){ (u16 *)session, (s64)wcslen(session) };
    } else if (!headless && !test) {
        WCHAR local[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        WCHAR path[MAX_PATH] = {0};
        swprintf_s(path, MAX_PATH, L"%s\\moss", local);
        CreateDirectoryW(path, NULL);
        // a named instance (`--instance NAME`) keeps its own, never the user's
        const WCHAR *dash = instance_name ? L"-" : L"", *inst = instance_name ? instance_name : L"";
        if (window_name) swprintf_s(path, MAX_PATH, L"%s\\moss\\window-%s%s%s.txt", local, window_name, dash, inst);
        else swprintf_s(path, MAX_PATH, L"%s\\moss\\layout%s%s.txt", local, dash, inst);
        WString w = { (u16 *)path, (s64)wcslen(path) };
        p->session_path = Utf8ToUtf16(&p->arena, Utf16ToUtf8(&p->arena, w));
    }
    if (p->session_path.count) init.session_text = Term_ReadFile(&p->arena, (const wchar_t *)p->session_path.data);

    // --- the editor Enter on a text file runs in its tab: settings.txt `editor` (`off`: the file's own program),
    //     else the first terminal editor found: on PATH, or with Git for Windows (its usr\bin, which the tabs' PATH has) ---
    {
        String editor = headless ? (String){0} : Term_ReadSetting(&p->arena, "editor");
        if (Str_EqualNoCase(editor, S("off"))) editor = (String){0};
        else if (!editor.count) {
            WCHAR git[MAX_PATH] = {0}, git_bin[MAX_PATH] = {0};
            if (SearchPathW(NULL, L"git.exe", NULL, MAX_PATH, git, NULL)) {
                // ...\Git\cmd\git.exe -> ...\Git\usr\bin
                WCHAR *slash = wcsrchr(git, L'\\');
                if (slash) *slash = 0;
                slash = wcsrchr(git, L'\\');
                if (slash) { *slash = 0; swprintf_s(git_bin, MAX_PATH, L"%s\\usr\\bin", git); }
            }
            static const WCHAR *const editors[] = { L"nvim", L"vim", L"hx", L"micro", L"nano", L"edit", L"emacs" };
            for (s32 i = 0; i < (s32)ArrayCount(editors) && !editor.count; i++) {
                WCHAR found[MAX_PATH] = {0};
                b32 on_path = SearchPathW(NULL, editors[i], L".exe", MAX_PATH, found, NULL) != 0;
                b32 with_git = !on_path && git_bin[0] && SearchPathW(git_bin, editors[i], L".exe", MAX_PATH, found, NULL) != 0;
                if (on_path || with_git) editor = Utf16ToUtf8(&p->arena, (WString){ (u16 *)editors[i], (s64)wcslen(editors[i]) });
            }
            if (Str_Equal(editor, S("emacs"))) editor = S("emacs -nw");
        }
        p->editor = editor;
        Logf("window", "text editor: %.*s", editor.count ? (int)editor.count : 4, editor.count ? (const char *)editor.data : "none");
    }

    // --- the go-to box: the environment (%NAME%), the known folders (Places), recents and bookmarks (paths.txt;
    //     headless and --test runs start with none and save nothing) ---
    {
        WCHAR *block = GetEnvironmentStringsW();
        Array(u8) env = {0};
        for (WCHAR *v = block; v && *v; v += wcslen(v) + 1) {
            if (*v == L'=') continue;   // cmd's per-drive folders ("=C:=C:\...")
            String line = Utf16ToUtf8(&p->arena, (WString){ (u16 *)v, (s64)wcslen(v) });
            for (s64 i = 0; i < line.count; i++) *Array_Push(&p->arena, env) = line.data[i];
            *Array_Push(&p->arena, env) = '\n';
        }
        if (block) FreeEnvironmentStringsW(block);
        init.environment = (String){ env.data, env.count };
        static const struct { const char *name; const KNOWNFOLDERID *id; } known[] = {
            { "Home", &FOLDERID_Profile }, { "Desktop", &FOLDERID_Desktop }, { "Documents", &FOLDERID_Documents },
            { "Downloads", &FOLDERID_Downloads }, { "Pictures", &FOLDERID_Pictures }, { "Music", &FOLDERID_Music },
            { "Videos", &FOLDERID_Videos },
        };
        for (s32 i = 0; i < (s32)ArrayCount(known) && init.place_count < (s32)ArrayCount(init.places); i++) {
            PWSTR path = NULL;
            if (SUCCEEDED(SHGetKnownFolderPath(known[i].id, 0, NULL, &path)) && path) {
                init.places[init.place_count].name = Str_FromCStr(known[i].name);
                init.places[init.place_count++].path = Utf16ToUtf8(&p->arena, (WString){ (u16 *)path, (s64)wcslen(path) });
            }
            CoTaskMemFree(path);
        }
        if (!headless && !test) {
            WCHAR local[MAX_PATH] = {0}, path[MAX_PATH] = {0};
            GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            swprintf_s(path, MAX_PATH, L"%s\\moss\\paths%s%s.txt", local, instance_name ? L"-" : L"", instance_name ? instance_name : L"");
            p->paths_path = Utf8ToUtf16(&p->arena, Utf16ToUtf8(&p->arena, (WString){ (u16 *)path, (s64)wcslen(path) }));
            init.paths_text = Term_ReadFile(&p->arena, (const wchar_t *)p->paths_path.data);
            swprintf_s(path, MAX_PATH, L"%s\\moss\\sessions%s%s.txt", local, instance_name ? L"-" : L"", instance_name ? instance_name : L"");
            p->sessions_meta_path = Utf8ToUtf16(&p->arena, Utf16ToUtf8(&p->arena, (WString){ (u16 *)path, (s64)wcslen(path) }));
            p->sessions_meta_text = Term_ReadFile(&p->arena, (const wchar_t *)p->sessions_meta_path.data);
        }
    }

    // --- input configuration ---
    {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        p->input.double_click_ms = GetDoubleClickTime();
        p->input.wheel_scroll_lines = (s32)(lines == WHEEL_PAGESCROLL ? 3 : lines);
    }

    HWND hwnd = NULL;
    if (headless) {
        p->dpi = (u32)dpi_arg;
        p->client_w = size_w;
        p->client_h = size_h;
        p->refresh_hz = 120;
    } else {
        p->dpi = GetDpiForSystem();
        static const LPCWSTR cursor_ids[Platform_CursorCount] = { IDC_ARROW, IDC_HAND, IDC_IBEAM, IDC_SIZEWE, IDC_SIZENS };   // Cursor_*
        for (s32 i = 0; i < Platform_CursorCount; i++) p->cursors[i] = LoadCursorW(NULL, cursor_ids[i]);
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS | CS_OWNDC;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = p->cursors[0];
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"moss";
        RegisterClassExW(&wc);
        s32 win_w = MulDiv(1200, (int)p->dpi, 96), win_h = MulDiv(760, (int)p->dpi, 96);
        hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"moss", test ? L"moss TEST" : L"moss", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
                               NULL, NULL, instance, p);
        if (!hwnd) { Logf("window", "CreateWindowEx failed (%lu)", GetLastError()); return 1; }
        if (!test && !window_name) SetPropW(hwnd, L"moss.main", (HANDLE)1);   // what a second `term` looks for
        // which server and window: where a tab dragged out of another window can go
        WCHAR id[160] = {0};
        swprintf_s(id, 160, L"%s|%s", p->instance, window_name ? window_name : L"");
        p->id_atom = GlobalAddAtomW(id);
        // --- the summon key, for the main window: settings `summon` (e.g. win+`, ctrl+alt+t; off = none), Win+` when unset ---
        if (!window_name && !test) {
            String key = Term_ReadSetting(&p->arena, "summon");
            if (!key.count) key = S("win+`");
            UINT mods = MOD_NOREPEAT, vk = 0;
            while (key.count) {
                s64 plus = 0;
                while (plus < key.count && (key.data[plus] != '+' || plus == 0)) plus++;
                String part = { key.data, plus };
                if (Str_EqualNoCase(part, S("win"))) mods |= MOD_WIN;
                else if (Str_EqualNoCase(part, S("ctrl"))) mods |= MOD_CONTROL;
                else if (Str_EqualNoCase(part, S("alt"))) mods |= MOD_ALT;
                else if (Str_EqualNoCase(part, S("shift"))) mods |= MOD_SHIFT;
                else if (part.count == 1) vk = part.data[0] == '`' ? VK_OEM_3 : (UINT)VkKeyScanA((char)part.data[0]) & 0xFF;
                else if ((part.data[0] == 'f' || part.data[0] == 'F') && part.count <= 3) vk = VK_F1 + (UINT)atoi((const char *)Str_PushCopy(&p->scratch, (String){ part.data + 1, part.count - 1 }).data) - 1;
                key.data += Min(plus + 1, key.count);
                key.count -= Min(plus + 1, key.count);
            }
            if (vk && !RegisterHotKey(hwnd, 1, mods, vk)) Logf("window", "the summon key is taken by another program (%lu)", GetLastError());
        }
        SetPropW(hwnd, L"moss.id", (HANDLE)(uintptr_t)p->id_atom);
        p->dpi = GetDpiForWindow(hwnd);
        if (!p->dpi) p->dpi = 96;
        UpdateMonitor(p);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        RECT cr = {0};
        GetClientRect(hwnd, &cr);
        p->client_w = Max(1, cr.right - cr.left);
        p->client_h = Max(1, cr.bottom - cr.top);
    }

    p->gpu = (Gpu *)Gpu_Create(hwnd, p->client_w, p->client_h);
    if (!p->gpu) {
        if (!headless) MessageBoxW(NULL, L"Direct3D 11 could not be initialised.", L"moss", MB_OK | MB_ICONERROR);
        return 1;
    }
    p->app = App_Create(&init);
    if (hwnd) {
        g_Drop.base.lpVtbl = &g_DropVtbl;
        g_Drop.p = p;
        g_Drop.refs = 1;
        HRESULT dr = RegisterDragDrop(hwnd, &g_Drop.base);
        if (FAILED(dr)) LogHr("window", "RegisterDragDrop", dr);
    }
    Files_Start(hwnd, Wake_Files);
    if (hwnd) {
        // web pages' data (logins, cookies): the user's in %LOCALAPPDATA%\moss\web, a test's or instance's its own
        WCHAR folder[MAX_PATH] = {0};
        if (p->instance[0]) {
            WCHAR temp[MAX_PATH] = {0};
            GetTempPathW(MAX_PATH, temp);
            swprintf_s(folder, MAX_PATH, L"%smoss-web-%s", temp, p->instance);
        } else {
            WCHAR local[MAX_PATH] = {0};
            GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            swprintf_s(folder, MAX_PATH, L"%s\\moss\\web", local);
        }
        WCHAR place[64] = {0};
        if (GetEnvironmentVariableW(L"MOSS_TEST_WINDOW", place, 64)) {
            // a test window is off screen: pages there would count as covered and stop drawing
            SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"--disable-features=CalculateNativeWinOcclusion");
        }
        p->web = Web_Create(hwnd, p->gpu, Wake_Files, folder);
    }

    // --- the server ---
    p->client = TermClient_Create(p->window_name);
    {
        WCHAR cwd[MAX_PATH] = {0};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        WString w = { (u16 *)cwd, (s64)wcslen(cwd) };
        String error = {0};
        p->cwd = Utf16ToUtf8(&p->arena, w);
        p->reconnect_at_us = Time_NowMicros() + 2000000;
        TermClient_Connect(p->client, hwnd, p->cwd, &p->arena, &error);
    }
    p->cells = TermCells_Create(Gpu_Device(p->gpu), Gpu_Context(p->gpu), p->client);
    Gpu_SetExternalDraw(p->gpu, TermCells_Draw, p->cells);
    p->term_font_px = 12.0f * (f32)p->dpi / 72.0f;
    if (!headless) LoadBackground(p, Term_ReadSetting(&p->scratch, "background_image"));
    TermCells_SetFont(p->cells, p->term_font, p->term_font_px);

    int result = 0;
    if (headless) {
        // --- the script ---
        String text = script ? Term_ReadFile(&p->arena, script) : (String){0};
        if (!text.count) Logf("headless", "no script (or an empty one): --script FILE");
        WString sw = { (u16 *)(script ? script : L""), script ? (s64)wcslen(script) : 0 };
        result = HeadlessMain(p, text, Utf16ToUtf8(&p->arena, sw));
    } else {
        p->pace_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!p->pace_timer) p->pace_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        // --- main loop: frames on events only; presents paced; the server's pipe wakes it too ---
        while (!p->quit) {
            if (!p->shown && p->frame >= 2) {
                p->shown = 1;
                // shown after two frames are drawn; cloaked meanwhile, then one more frame at its final size (a maximized
                // start, a restored rect), so the first thing on screen is a finished frame
                BOOL cloak = TRUE;
                DwmSetWindowAttribute(hwnd, 13, &cloak, sizeof(cloak));   // DWMWA_CLOAK
                // MOSS_TEST_WINDOW=x,y,w,h: a test window at that rect (off screen is fine), shown without
                // taking the foreground, so automated checks of the window path don't get in the user's way
                WCHAR place[64] = {0};
                s32 r[4] = {0};
                b32 test_place = GetEnvironmentVariableW(L"MOSS_TEST_WINDOW", place, 64) > 0 && swscanf_s(place, L"%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4;
                if (test_place && p->start_at) memcpy(r, p->start_rect, sizeof(r));   // a window torn off a test window stays a test window
                if (p->start_at && !test_place) {
                    SetWindowPos(hwnd, NULL, p->start_rect[0], p->start_rect[1], p->start_rect[2], p->start_rect[3], SWP_NOZORDER);
                    ShowWindow(hwnd, SW_SHOWNORMAL);
                    SetForegroundWindow(hwnd);
                } else if (test_place) {
                    SetWindowPos(hwnd, NULL, r[0], r[1], r[2], r[3], SWP_NOZORDER | SWP_NOACTIVATE);
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    p->focused = 1;   // posted keys arrive without the keyboard focus
                    p->test_window = 1;
                } else {
                    ShowWindow(hwnd, p->windowed.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
                }
                SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
                RunFrame(p, 1);
                cloak = FALSE;
                DwmSetWindowAttribute(hwnd, 13, &cloak, sizeof(cloak));
                Logf("window", "shown %.1f ms after start", (double)Time_NowMicros() / 1000.0);
            }
            if (!p->shown) p->wake_frame = p->frame;

            // --- end the previous frame's input ---
            for (s32 b = 0; b < Mouse_Count; b++) {
                UiButton *btn = &p->input.button[b];
                btn->pressed = 0;
                btn->released = 0;
                if (btn->down && !p->focused && !Web_HasFocus(p->web)) { btn->released = 1; btn->down = 0; }   // a page's keyboard is still the window's
            }
            p->input.wheel[0] = 0;
            p->input.wheel[1] = 0;
            p->input.side = 0;
            p->input.modifiers = 0;
            p->event_count = 0;
            p->text_used = 0;

            HANDLE latency = (HANDLE)Gpu_LatencyHandle(p->gpu);
            if (latency && !p->rendered_not_presented) WaitForSingleObjectEx(latency, 1000, TRUE);

            // --- idle until a message, the server, a wake-up time or a deferred present asks for a frame ---
            u64 interval = Min(1000000ull / Max(1, p->refresh_hz), 6000ull);
            while (!p->quit) {
                u64 now = Time_NowMicros();
                if (p->wake_at_us && now >= p->wake_at_us) { p->wake_at_us = 0; p->wake_frame = p->frame; }
                b32 wanted = p->frame == p->wake_frame || p->present_deferred;
                u64 due = p->present_deferred ? p->last_present_us + interval : 0;
                if (wanted && now >= due) break;
                HANDLE handles[2];
                DWORD count = 0;
                HANDLE server = (HANDLE)TermClient_ReadEvent(p->client);
                if (server) handles[count++] = server;
                u64 wake = wanted ? due : p->wake_at_us;
                if (wake && p->pace_timer) {
                    LARGE_INTEGER t = {0};
                    t.QuadPart = -(LONGLONG)(wake > now ? wake - now : 0) * 10;
                    if (SetWaitableTimer(p->pace_timer, &t, 0, NULL, NULL, FALSE)) handles[count++] = p->pace_timer;
                }
                DWORD r = MsgWaitForMultipleObjectsEx(count, count ? handles : NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (server && r == WAIT_OBJECT_0) {
                    // the server sent something: apply it now, a frame shows it
                    if (TermClient_Poll(p->client)) p->wake_frame = p->frame;
                }
                if (!wanted) p->woke_from_wait = 1;
                Pump(p);
            }
            Pump(p);
            if (p->quit) break;
            RunFrame(p, 0);
        }
    }

    // --- shutdown: a window the user closed hands its tabs to the main window and forgets its layout; else the final
    //     session save ---
    if (p->window_name.count && !p->closing && p->client) {
        AppFrameInput fi = {0};
        TermClient_Fill(p->client, &fi);
        for (s64 i = 0; i < fi.tab_count; i++) {
            if (Str_Equal(fi.tabs[i].owner, p->window_name)) TermClient_SetOwner(p->client, fi.tabs[i].id, (String){0});
        }
        if (p->session_path.count) DeleteFileW((const wchar_t *)p->session_path.data);
        Logf(headless ? "headless" : "window", "tabs handed back to the main window");
    } else {
        String session_text = App_SessionText(p->app);
        if (session_text.count && p->session_path.count) Term_WriteFileAtomic((const wchar_t *)p->session_path.data, session_text);
    }
    if (p->id_atom) GlobalDeleteAtom(p->id_atom);
    Logf(headless ? "headless" : "window", "exit (%d)", result);
    Files_Stop();
    Web_Destroy(p->web);
    TermCells_Destroy(p->cells);
    TermClient_Destroy(p->client);
    App_Destroy(p->app);
    for (s32 i = 0; i < Font_Count; i++) Font_Destroy(&p->fonts[i]);
    Gpu_Destroy(p->gpu);
    return result;
}
