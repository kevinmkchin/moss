// term: the C++ side (server, server client, GPU, fonts, terminal cell renderer, images) as the C platform layer
// (term_win32.c) calls it.
#ifndef TERM_PLATFORM_H
#define TERM_PLATFORM_H

#include "term_app.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- the GPU: one D3D11 device; a window's composition swap chain, or (hwnd 0) a texture for headless runs ---
typedef struct Gpu Gpu;
Gpu  *Gpu_Create(void *hwnd, s32 w, s32 h);
void  Gpu_Destroy(Gpu *g);
void *Gpu_Device(Gpu *g);
void *Gpu_Context(Gpu *g);
void *Gpu_LatencyHandle(Gpu *g);   // the swap chain's frame latency waitable (0 headless)
void  Gpu_SetExternalDraw(Gpu *g, void (*fn)(void *user, const void *data, Rect clip, s32 target_w, s32 target_h), void *user);
void  Gpu_Render(Gpu *g, const DrawList *dl, const FontAtlas *atlas, u32 clear, s32 w, s32 h);   // resizes the target to w x h first
void  Gpu_Present(Gpu *g);
u8   *Gpu_ReadPixels(Gpu *g, Arena *arena, s32 *w, s32 *h);   // the last frame, RGBA rows
void  Gpu_Times(Gpu *g, f32 *total_ms, f32 *cells_ms);           // a recent frame's GPU time, and its terminal cells'
u64   Gpu_CreateImage(Gpu *g, const u32 *bgra_premultiplied, s32 w, s32 h);   // mipmapped; 0 on failure
void  Gpu_FreeImage(Gpu *g, u64 image);

// --- the UI's fonts (DirectWrite), glyphs drawn into one shared atlas as they're asked for ---
void  FontAtlas_Init(FontAtlas *a);
void  FontAtlas_Reset(FontAtlas *a);   // every glyph drawn again on use (a new DPI)
b32   Font_Create(Font *f, FontAtlas *atlas, const wchar_t *families, f32 size_px, s32 weight);   // families: "A\0B\0\0", the first installed
void  Font_Destroy(Font *f);

int  Term_ServerMain(void);                                     // `term --server`
int  Term_VtReplayMain(const wchar_t *file, int cols, int rows); // `term --vt-replay FILE`: the screen after feeding it
void Term_PrepareUser(void);    // first run: ~\.moss from the defaults, the bundled font installed
String Term_ReadSetting(Arena *arena, const char *key);          // settings.txt, empty when unset
void Term_WriteSetting(const char *key, String value);
void Term_ResetSettings(void);                  // settings.txt as moss ships it, the old one kept as settings-old.txt
b32  Term_GoodProgrammer(void);                 // the global CLAUDE.md has the "Be a good programmer" block (moss's or clod's)
b32  Term_SetGoodProgrammer(b32 on);            // puts it in (from ~\.moss\good-programmer.md) or takes it out; 0 = failed
String Term_ReadFile(Arena *arena, const wchar_t *path);
void Term_WriteFileAtomic(const wchar_t *path, String data);
void Term_RaiseForTyping(void);
void Term_LatencyMark(const char *what);   // MOSS_LATENCY=1: timestamps per hop (tools/latency.py)

// --- the connection to the server and the tabs' grids ---
typedef struct TermClient TermClient;
TermClient *TermClient_Create(String window_name);   // empty name: the main window
void  TermClient_SetOwner(TermClient *c, u32 tab_id, String owner);   // the tab now belongs to that window
// Connects (starting the server when there is none) and attaches. 0 with *error set on failure.
b32   TermClient_Connect(TermClient *c, void *hwnd, String cwd, Arena *arena, String *error);
void *TermClient_ReadEvent(TermClient *c);       // HANDLE that signals when the server sent something
b32   TermClient_Poll(TermClient *c);            // applies what arrived; 1 when anything changed
void  TermClient_Fill(TermClient *c, AppFrameInput *in);   // tabs, grids, connection state (valid until the next Poll)
void  TermClient_Apply(TermClient *c, const AppOutput *out);   // the app's requests, to the server
// What programs passed to the terminal since the last call: OSC 52 clipboard text (frame arena copy) and bells.
String TermClient_TakeClipboard(TermClient *c, Arena *arena);
String TermClient_TakeLink(TermClient *c, Arena *arena);   // a link's URI the server sent (asked with AppOutput.link_id)
b32   TermClient_TakeBell(TermClient *c);
String TermClient_TakeView(TermClient *c, Arena *arena, u32 *tab_id);   // `view <file>` in a tab: its path (empty: none)
b32   TermClient_ShouldClose(TermClient *c);   // the server said goodbye for good (quit from the tray): close, don't reconnect
// The visible text of a tab's grid, rows joined with \n (trailing blanks dropped); empty when not shown.
String TermClient_ScreenText(TermClient *c, u32 tab_id, Arena *arena, b32 with_attrs);
void  TermClient_Stats(TermClient *c, u64 *bytes_in, u64 *bytes_out, u64 *messages_in);
void  TermClient_Debug(TermClient *c, u32 what);   // 1 crash the server, 2 quit it (tests)
// The cell where UTF-8 `text` first appears on a tab's screen (row by row); 0 when it doesn't.
b32   TermClient_FindText(TermClient *c, u32 tab_id, String text, s32 *col, s32 *row);
void  TermClient_Destroy(TermClient *c);

// --- terminal cells on the GPU (an external draw of the D3D11 backend) ---
typedef struct TermCells TermCells;
TermCells *TermCells_Create(void *device, void *context, TermClient *client);
// The terminal font (a face or family name, size in pixels); the cell size follows.
void  TermCells_SetFont(TermCells *t, String family, f32 size_px);
void  TermCells_CellSize(TermCells *t, s32 *w, s32 *h);
// Before rendering: glyphs and GPU cells for every TermCellsDraw in the render list (Direct2D draws glyphs,
// which can't happen in the middle of the backend's pass).
void  TermCells_Prepare(TermCells *t, const DrawList *dl);
void  TermCells_Draw(void *user, const void *data, Rect clip, s32 target_w, s32 target_h);   // Gpu_SetExternalDraw's; user = the TermCells
void  TermCells_Stats(TermCells *t, s32 *glyphs, s32 *atlas_percent, s32 *resolved_this_frame);
s64   TermCells_MonoFonts(TermCells *t, Arena *arena, String **out);
void  TermCells_Destroy(TermCells *t);

// --- web pages in tabs (WebView2; term_web.cpp): in the window's composition tree, under its frame ---
typedef struct Web Web;
Web *Web_Create(void *hwnd, Gpu *g, u32 wake_message, const wchar_t *data_folder);   // NULL: no WebView2 loader
void Web_Update(Web *w, const AppWeb *webs, s32 count, u64 focus);   // the app's pages: made, placed, navigated, closed
void Web_Presented(Web *w);                                          // after a present: pages it covers are hidden / closed
s32  Web_Fill(Web *w, Arena *arena, AppWebState *out, s32 max);      // their state
s32  Web_Popups(Web *w, Arena *arena, AppWebPopup *out, s32 max);    // new windows they asked for, waiting for tabs
b32  Web_HasFocus(Web *w);                                           // a page has the keyboard
void *Web_Cursor(Web *w);                                            // the page under the mouse's cursor (HCURSOR), or 0
b32  Web_Mouse(Web *w, u32 msg, u64 wparam, s64 lparam);            // a mouse message, to a page if it's one's (1)
void Web_Destroy(Web *w);
int  Web_ProbeMain(const wchar_t *url, const wchar_t *png);           // `moss --web-probe URL OUT.png`

// --- images (WIC) ---
b32   Image_SavePng(const wchar_t *path, const u8 *rgba, s32 w, s32 h);
u8   *Image_LoadPng(const wchar_t *path, Arena *arena, s32 *w, s32 *h);   // RGBA8 rows, NULL on failure
// Any image Windows reads (PNG, JPEG, GIF, BMP, TIFF, ICO, WebP/HEIF with their codecs), its first frame scaled
// down to fit max_w x max_h: premultiplied RGBA8 rows (malloc'd: free()), NULL on failure. Any thread.
u8   *Image_Decode(const wchar_t *path, s32 max_w, s32 max_h, s32 *w, s32 *h, s32 *full_w, s32 *full_h);

#ifdef __cplusplus
}
#endif

#endif
