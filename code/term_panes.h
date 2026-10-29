// term: the window's pane tree. A leaf is a pane with a strip of tabs (one shown); an inner node splits its area
// between its children by ratio, side by side or stacked. Pure: no OS calls.
#ifndef TERM_PANES_H
#define TERM_PANES_H

#include "term_core.h"

enum { Pane_KeyMax = 64 };

typedef struct PaneNode PaneNode;

// A tab in a pane's strip: a terminal tab of the server, or one asked for and not there yet.
typedef struct PaneTab {
    u64 id;                     // the window's own id (UI ids)
    u32 term_id;                // the server's tab id; 0 while its new-tab request is pending
    u32 request;                // the new-tab request that will bring it (0 = none)
    u8 key[Pane_KeyMax];        // lasting id (session / history id): the next run finds it again
    s32 key_len;
    PaneNode *node;
    struct PaneTab *next;       // in its pane's strip
    b32 hidden;                 // not its pane's shown tab
    u32 created_frame;
    f32 inactive_t;             // 0 focused .. 1 dimmed (animated)
    s32 strip_x[2];             // its place in the strip last frame (drags)
    b32 browsing;               // shows its file browser instead of its shell
    struct Browser *browser;    // the browser's state (term_app.c), made when first opened
    String browse_path;         // the browser's folder, kept by the session
    b32 viewing;                // shows a picture over its shell or browser (its viewer)
    struct Viewer *viewer;      // the picture view's state (term_app.c)
    u8 title[64];               // the name the user gave it; empty = the server's
    s32 title_len;
    u8 color;                   // the user's color: 0 none, 1-6 an ANSI color (red .. cyan)
    b32 web;                    // a web tab: a page (the platform hosts it, AppOutput.webs), no server tab
    u8 web_url[1024];           // where its page is (kept in the layout)
    s32 web_url_len;
    u8 web_go[1024];            // where the page goes next (sent with the next frame's AppOutput.webs)
    s32 web_go_len;
    s32 web_step;               // -1 back, 1 forward (next frame)
    b32 web_reload;
    u32 web_popup;              // made for a page's new window (AppWebPopup.id)
    u8 web_title[128];          // the page's title (its tab's name)
    s32 web_title_len;
    u32 web_notices_seen;       // the page's notifications counted so far (AppWebState.notices)
    b32 web_needs;              // it notified while not in front: a dot on its tab until shown
    s32 web_zoom;               // the page's zoom, in steps from 100% (g_WebZooms)
} PaneTab;

struct PaneNode {
    u64 id;
    PaneNode *parent, *first_child, *next, *prev;
    PaneTab *first_tab;         // leaves only
    b32 vertical;               // children side by side (else stacked)
    f32 split_ratio;            // its share of its parent
    f32 rect[4];                // 0..1 of the whole area: x0 y0 x1 y1
    f32 px[4];                  // where it goes, window pixels
    f32 anim_px[4];             // where it's drawn (animated towards px)
    b32 anim_ready;
};

typedef struct Panes {
    Arena arena;                // nodes and tabs; freed ones are reused
    PaneNode *root;
    PaneTab *selected;          // the focused tab (shown in its pane)
    u64 next_id;
    PaneNode *free_nodes;       // (linked through next)
    PaneTab *free_tabs;         // (linked through next)
} Panes;

PaneNode *Panes_NewNode(Panes *p);
PaneTab  *Panes_NewTab(Panes *p, PaneNode *leaf, PaneTab *before);   // before NULL: at the end; shown if the pane was empty
void      Panes_FreeTab(Panes *p, PaneTab *t);   // detached tabs only
void      Panes_Select(Panes *p, PaneTab *t);    // focused and shown in its pane
PaneTab  *Panes_VisibleTab(const PaneNode *leaf);
PaneNode *Panes_NextNode(PaneNode *n);           // depth first
// A new empty pane beside `leaf` (vertical: left / right, else above / below; after: right / below), the
// siblings evened out. Returns it.
PaneNode *Panes_InsertSibling(Panes *p, PaneNode *leaf, b32 vertical, b32 after);
void      Panes_ComputeRects(PaneNode *n);       // children's rects from their ratios
void      Panes_ComputePixels(PaneNode *root, f32 x0, f32 w, f32 h);
void      Panes_EvenChildren(PaneNode *n, b32 recursive);
PaneTab  *Panes_Neighbor(Panes *p, PaneTab *t, s32 dir);      // the next (dir 1) / previous pane's tab, around
PaneTab  *Panes_InDirection(Panes *p, PaneTab *t, s32 dir);   // 0 left 1 right 2 up 3 down: the nearest pane's tab
// Unlinks t from its pane; a pane left empty goes (its parent folds when one child is left). 0 when t was the
// window's last tab (the tree keeps one empty pane then).
b32       Panes_DetachTab(Panes *p, PaneTab *t);
void      Panes_InsertTab(PaneTab *t, PaneNode *leaf, PaneTab *before);
PaneTab  *Panes_FindTerm(Panes *p, u32 term_id);
s32       Panes_TabCount(const Panes *p);

// --- the layout kept across window restarts: a text file, a line per node and per tab ---
typedef struct SessionLayout {
    s32 x, y, w, h;             // the window's restore rect
    b32 maximized;
    b32 sidebar_closed;
} SessionLayout;

String Session_Write(Arena *arena, const Panes *p, const SessionLayout *s);
// Rebuilds the tree (its tabs have keys, no server ids yet). 0, with the tree untouched, when the text isn't one.
b32    Session_Read(Panes *p, String text, SessionLayout *out);

#endif
