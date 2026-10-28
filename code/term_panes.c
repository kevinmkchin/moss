// term: the window's pane tree (term_panes.h).
#include "term_panes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PaneNode *Panes_NewNode(Panes *p)
{
    PaneNode *n = p->free_nodes;
    if (n) p->free_nodes = n->next;
    else n = Arena_PushStruct(&p->arena, PaneNode);
    PaneNode zero = {0};
    *n = zero;
    n->id = ++p->next_id;
    n->split_ratio = 1;
    n->rect[2] = n->rect[3] = 1;
    return n;
}

static void FreeNode(Panes *p, PaneNode *n)
{
    n->next = p->free_nodes;
    p->free_nodes = n;
}

void Panes_InsertTab(PaneTab *t, PaneNode *leaf, PaneTab *before)
{
    t->node = leaf;
    t->hidden = leaf->first_tab != NULL;   // an empty pane shows it
    PaneTab **link = &leaf->first_tab;
    while (*link && *link != before) link = &(*link)->next;
    t->next = *link;
    *link = t;
}

PaneTab *Panes_NewTab(Panes *p, PaneNode *leaf, PaneTab *before)
{
    PaneTab *t = p->free_tabs;
    if (t) p->free_tabs = t->next;
    else t = Arena_PushStruct(&p->arena, PaneTab);
    PaneTab zero = {0};
    *t = zero;
    t->id = ++p->next_id;
    Panes_InsertTab(t, leaf, before);
    return t;
}

void Panes_FreeTab(Panes *p, PaneTab *t)
{
    if (p->selected == t) p->selected = NULL;
    t->next = p->free_tabs;
    p->free_tabs = t;
}

void Panes_Select(Panes *p, PaneTab *t)
{
    p->selected = t;
    if (!t) return;
    for (PaneTab *o = t->node->first_tab; o; o = o->next) o->hidden = o != t;
}

PaneTab *Panes_VisibleTab(const PaneNode *leaf)
{
    for (PaneTab *t = leaf->first_tab; t; t = t->next) if (!t->hidden) return t;
    return leaf->first_tab;
}

PaneNode *Panes_NextNode(PaneNode *n)
{
    if (n->first_child) return n->first_child;
    while (n && !n->next) n = n->parent;
    return n ? n->next : NULL;
}

// Puts `fresh` where `old` is among its siblings (or as the root).
static void Replace(Panes *p, PaneNode *old, PaneNode *fresh)
{
    fresh->parent = old->parent;
    fresh->prev = old->prev;
    fresh->next = old->next;
    fresh->split_ratio = old->split_ratio;
    memcpy(fresh->rect, old->rect, sizeof(fresh->rect));   // its place too (a group folding into its last child: the child fills it)
    if (old->prev) old->prev->next = fresh;
    else if (old->parent) old->parent->first_child = fresh;
    if (old->next) old->next->prev = fresh;
    if (p->root == old) p->root = fresh;
    old->parent = old->prev = old->next = NULL;
}

PaneNode *Panes_InsertSibling(Panes *p, PaneNode *leaf, b32 vertical, b32 after)
{
    PaneNode *fresh = Panes_NewNode(p);
    PaneNode *group = leaf->parent;
    if (!group || group->vertical != vertical) {
        // a group of the two takes the leaf's place
        group = Panes_NewNode(p);
        group->vertical = vertical;
        Replace(p, leaf, group);
        group->first_child = leaf;
        leaf->parent = group;
    }
    fresh->parent = group;
    if (after) {
        fresh->prev = leaf;
        fresh->next = leaf->next;
        if (leaf->next) leaf->next->prev = fresh;
        leaf->next = fresh;
    } else {
        fresh->next = leaf;
        fresh->prev = leaf->prev;
        if (leaf->prev) leaf->prev->next = fresh;
        else group->first_child = fresh;
        leaf->prev = fresh;
    }
    Panes_EvenChildren(group, 0);
    Panes_ComputeRects(p->root);
    return fresh;
}

void Panes_EvenChildren(PaneNode *n, b32 recursive)
{
    s32 count = 0;
    for (PaneNode *c = n->first_child; c; c = c->next) count++;
    for (PaneNode *c = n->first_child; c; c = c->next) {
        c->split_ratio = 1.0f / (f32)count;
        if (recursive) Panes_EvenChildren(c, 1);
    }
}

void Panes_ComputeRects(PaneNode *n)
{
    f32 total = 0;
    for (PaneNode *c = n->first_child; c; c = c->next) total += c->split_ratio > 0 ? c->split_ratio : 0.01f;
    f32 at = 0;
    for (PaneNode *c = n->first_child; c; c = c->next) {
        f32 share = (c->split_ratio > 0 ? c->split_ratio : 0.01f) / total;
        memcpy(c->rect, n->rect, sizeof(c->rect));
        s32 axis = n->vertical ? 0 : 1;
        f32 span = n->rect[axis + 2] - n->rect[axis];
        c->rect[axis] = n->rect[axis] + span * at;
        c->rect[axis + 2] = c->next ? n->rect[axis] + span * (at + share) : n->rect[axis + 2];
        at += share;
        Panes_ComputeRects(c);
    }
}

void Panes_ComputePixels(PaneNode *root, f32 x0, f32 w, f32 h)
{
    for (PaneNode *n = root; n; n = Panes_NextNode(n)) {
        n->px[0] = x0 + n->rect[0] * w;
        n->px[1] = n->rect[1] * h;
        n->px[2] = x0 + n->rect[2] * w;
        n->px[3] = n->rect[3] * h;
    }
}

PaneTab *Panes_Neighbor(Panes *p, PaneTab *t, s32 dir)
{
    PaneNode *leaves[256];
    s32 count = 0, at = 0;
    for (PaneNode *n = p->root; n && count < 256; n = Panes_NextNode(n)) {
        if (n->first_child || !n->first_tab) continue;
        if (t && n == t->node) at = count;
        leaves[count++] = n;
    }
    if (!count) return NULL;
    return Panes_VisibleTab(leaves[((at + dir) % count + count) % count]);
}

PaneTab *Panes_InDirection(Panes *p, PaneTab *t, s32 dir)
{
    if (!t) return NULL;
    PaneNode *me = t->node, *best = NULL;
    f32 cx = (me->px[0] + me->px[2]) * 0.5f, cy = (me->px[1] + me->px[3]) * 0.5f, best_d = 1e30f;
    for (PaneNode *n = p->root; n; n = Panes_NextNode(n)) {
        if (n->first_child || !n->first_tab || n == me) continue;
        // on that side of me, and overlapping me across
        b32 side = dir == 0 ? n->px[2] <= me->px[0] + 1 : dir == 1 ? n->px[0] >= me->px[2] - 1 :
                   dir == 2 ? n->px[3] <= me->px[1] + 1 : n->px[1] >= me->px[3] - 1;
        b32 across = dir < 2 ? n->px[1] < me->px[3] && n->px[3] > me->px[1] : n->px[0] < me->px[2] && n->px[2] > me->px[0];
        if (!side || !across) continue;
        f32 dx = (n->px[0] + n->px[2]) * 0.5f - cx, dy = (n->px[1] + n->px[3]) * 0.5f - cy;
        f32 d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = n; }
    }
    return best ? Panes_VisibleTab(best) : NULL;
}

b32 Panes_DetachTab(Panes *p, PaneTab *t)
{
    PaneNode *leaf = t->node;
    PaneTab **link = &leaf->first_tab;
    PaneTab *before = NULL;
    while (*link && *link != t) { before = *link; link = &(*link)->next; }
    if (*link) *link = t->next;
    if (!t->hidden) {
        // the one beside it shows now
        PaneTab *heir = t->next ? t->next : before;
        if (heir) heir->hidden = 0;
    }
    t->next = NULL;
    t->node = NULL;
    if (p->selected == t) p->selected = NULL;
    if (!leaf->first_tab && leaf->parent) {
        // the empty pane goes; a group left with one child is replaced by it
        PaneNode *group = leaf->parent;
        if (leaf->prev) leaf->prev->next = leaf->next;
        else group->first_child = leaf->next;
        if (leaf->next) leaf->next->prev = leaf->prev;
        FreeNode(p, leaf);
        if (group->first_child && !group->first_child->next) {
            PaneNode *only = group->first_child;
            only->parent = NULL;
            Replace(p, group, only);
            FreeNode(p, group);
        } else {
            Panes_EvenChildren(group, 0);
        }
        Panes_ComputeRects(p->root);
    }
    if (!p->selected) {
        for (PaneNode *n = p->root; n && !p->selected; n = Panes_NextNode(n)) {
            if (!n->first_child && n->first_tab) p->selected = Panes_VisibleTab(n);
        }
    }
    return Panes_TabCount(p) > 0;
}

PaneTab *Panes_FindTerm(Panes *p, u32 term_id)
{
    for (PaneNode *n = p->root; n && term_id; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) if (t->term_id == term_id) return t;
    }
    return NULL;
}

s32 Panes_TabCount(const Panes *p)
{
    s32 count = 0;
    for (PaneNode *n = p->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) count++;
    }
    return count;
}

// --- session: "term-layout 1", a "window" line, then a line per node ("split" / "pane" with its depth) and per
//     tab of a pane ("tab"), depth first. Text fields are the line's last, tab-separated ---

String Session_Write(Arena *arena, const Panes *p, const SessionLayout *s)
{
    TextBuilder tb = { arena, NULL, 0, 0 };
    TB_Format(&tb, "term-layout 1\nwindow %d %d %d %d %d %d\n", s->x, s->y, s->w, s->h, s->maximized ? 1 : 0, s->sidebar_closed ? 1 : 0);
    for (PaneNode *n = p->root; n; n = Panes_NextNode(n)) {
        s32 depth = 0;
        for (PaneNode *a = n->parent; a; a = a->parent) depth++;
        if (n->first_child) { TB_Format(&tb, "split %d %d %.4f\n", depth, n->vertical ? 1 : 0, (double)n->split_ratio); continue; }
        TB_Format(&tb, "pane %d %.4f\n", depth, (double)n->split_ratio);
        for (PaneTab *t = n->first_tab; t; t = t->next) {
            TB_Format(&tb, "tab %d %d %d %d\t%.*s\t%.*s\t%.*s\n", t == p->selected ? 1 : 0, t->hidden ? 0 : 1, t->browsing ? 1 : 0, (int)t->color,
                      t->key_len, (const char *)t->key, t->title_len, (const char *)t->title, STR_ARG(t->browse_path));
        }
    }
    return TB_String(&tb);
}

// The next line of text (without its newline), moving *at past it.
static String NextLine(String text, s64 *at)
{
    s64 start = *at;
    while (*at < text.count && text.data[*at] != '\n') (*at)++;
    String line = { text.data + start, *at - start };
    if (*at < text.count) (*at)++;
    if (line.count && line.data[line.count - 1] == '\r') line.count--;
    return line;
}

// The tab-separated field k of a line (the first is the numbers before the first tab).
static String Field(String line, s32 k)
{
    s64 at = 0;
    for (s32 i = 0; i < k && at <= line.count; i++) {
        while (at < line.count && line.data[at] != '\t') at++;
        at++;
    }
    if (at > line.count) { String none = {0}; return none; }
    s64 end = at;
    while (end < line.count && line.data[end] != '\t') end++;
    String f = { line.data + at, end - at };
    return f;
}

b32 Session_Read(Panes *p, String text, SessionLayout *out)
{
    s64 at = 0;
    String head = NextLine(text, &at);
    if (!Str_Equal(head, S("term-layout 1"))) return 0;
    Panes fresh = {0};
    fresh.arena = p->arena;   // reused: the old tree's memory (it's thrown away)
    fresh.next_id = p->next_id;
    PaneNode *at_depth[64] = {0};
    PaneNode *leaf = NULL;
    PaneTab *selected = NULL;
    while (at < text.count) {
        String line = NextLine(text, &at);
        char buf[256];
        s64 n = Min(Field(line, 0).count, (s64)sizeof(buf) - 1);
        memcpy(buf, line.data, (size_t)n);
        buf[n] = 0;
        s32 a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        float ratio = 1;
        if (sscanf(buf, "window %d %d %d %d %d %d", &a, &b, &c, &d, &e, &f) == 6) {
            out->x = a; out->y = b; out->w = c; out->h = d; out->maximized = e; out->sidebar_closed = f;
            continue;
        }
        b32 split = sscanf(buf, "split %d %d %f", &a, &b, &ratio) == 3;
        b32 pane = !split && sscanf(buf, "pane %d %f", &a, &ratio) == 2;
        if (split || pane) {
            if (a < 0 || a >= 63 || (a > 0 && !at_depth[a - 1]) || (a == 0 && fresh.root)) return 0;
            PaneNode *node = Panes_NewNode(&fresh);
            node->vertical = split && b;
            node->split_ratio = ratio;
            if (a == 0) fresh.root = node;
            else {
                PaneNode *parent = at_depth[a - 1];
                node->parent = parent;
                PaneNode *last = parent->first_child;
                while (last && last->next) last = last->next;
                if (last) { last->next = node; node->prev = last; } else parent->first_child = node;
            }
            at_depth[a] = node;
            for (s32 k = a + 1; k < 64; k++) at_depth[k] = NULL;
            leaf = pane ? node : NULL;
            continue;
        }
        if (leaf && sscanf(buf, "tab %d %d %d %d", &a, &b, &c, &d) == 4) {
            PaneTab *t = Panes_NewTab(&fresh, leaf, NULL);
            t->hidden = !b;
            t->browsing = c;
            t->color = (u8)d;
            String key = Field(line, 1), title = Field(line, 2), path = Field(line, 3);
            t->key_len = (s32)Min(key.count, (s64)sizeof(t->key));
            memcpy(t->key, key.data, (size_t)t->key_len);
            t->title_len = (s32)Min(title.count, (s64)sizeof(t->title));
            memcpy(t->title, title.data, (size_t)t->title_len);
            if (path.count) t->browse_path = Str_PushCopy(&fresh.arena, path);
            if (a) selected = t;
        }
    }
    if (!fresh.root) return 0;
    // every pane shows one tab
    for (PaneNode *n = fresh.root; n; n = Panes_NextNode(n)) {
        if (n->first_child || !n->first_tab) continue;
        b32 shown = 0;
        for (PaneTab *t = n->first_tab; t; t = t->next) { if (shown) t->hidden = 1; shown |= !t->hidden; }
        if (!shown) n->first_tab->hidden = 0;
    }
    *p = fresh;
    p->selected = selected;
    Panes_ComputeRects(p->root);
    return 1;
}
