/* layout.c — Tiling layout engine.
   Computes usable geometry (accounting for bars/struts), arranges tiled
   windows in horizontal-scroll or master-stack mode, and manages the
   camera (scroll offset) for the infinite-canvas horizontal layout. */

#include "dswm.h"
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <err.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* globals owned by this file */
int retile_pending = 0;
int center_focused = CENTER_FOCUSED_DEFAULT;

/* ---- workspace helpers ---- */

Workspace *
curws(void)
{
    return &spaces[cur_ws];
}

/* Return the workspace that should receive user input:
   if the scratchpad overlay is visible it takes priority. */
Workspace *
active_ws(void)
{
    if (scratch_visible)
        return &spaces[SCRATCHPAD_IDX];
    return &spaces[cur_ws];
}

Monitor *
curmon(void)
{
    int i;
    for (i = 0; i < nmons; i++)
        if (mons[i].current_workspace == cur_ws)
            return &mons[i];
    return &mons[0];
}

/* ---- tiled list helpers ---- */

int
tiled_ensure_cap(Workspace *ws)
{
    if (ws->ntiled >= ws->tiled_cap) {
        int newcap = ws->tiled_cap ? ws->tiled_cap * 2 : INITIAL_CAP;
        ManagedWindow **tmp = realloc(ws->tiled, newcap * sizeof(ManagedWindow *));
        if (!tmp) return 0;
        ws->tiled = tmp;
        ws->tiled_cap = newcap;
    }
    return 1;
}

void
tiled_add(Workspace *ws, ManagedWindow *mw)
{
    if (!tiled_ensure_cap(ws))
        err(1, "tiled_add: realloc");
    ws->tiled[ws->ntiled++] = mw;
}

void
tiled_remove(Workspace *ws, Window w)
{
    int i;
    for (i = 0; i < ws->ntiled; i++) {
        if (ws->tiled[i]->window == w) {
            memmove(&ws->tiled[i], &ws->tiled[i + 1],
                    (ws->ntiled - i - 1) * sizeof(ManagedWindow *));
            ws->ntiled--;
            return;
        }
    }
}

void
rebuild_tiled(Workspace *ws)
{
    int i;
    ws->ntiled = 0;
    for (i = 0; i < ws->nwin; i++) {
        if (!ws->wins[i].is_floating && !ws->wins[i].is_fullscreen)
            tiled_add(ws, &ws->wins[i]);
    }
}

/* ---- bar strut support ---- */

/* Read _NET_WM_STRUT from windows on the current workspace AND from
   unmanaged root children (e.g. polybar docks).  Struts are reserved
   screen edges that tiled windows must avoid.  Results are cached in
   mon->strut_* and invalidated on window add/remove via strut_valid. */
static void
compute_struts(Monitor *mon)
{
    Workspace *ws = curws();
    Atom actual;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    int i;

    if (mon->strut_valid) return;

    mon->strut_top = 0;
    mon->strut_bottom = 0;
    mon->strut_left = 0;
    mon->strut_right = 0;

    /* Check managed windows on current workspace */
    for (i = 0; i < ws->nwin; i++) {
        if (XGetWindowProperty(dpy, ws->wins[i].window, atom_net_wm_strut,
                               0, 4, False, XA_CARDINAL, &actual, &format,
                               &nitems, &bytes_after, &data) == Success) {
            if (data && actual == XA_CARDINAL && format == 32 && nitems >= 4) {
                long *strut = (long *)data;
                if (strut[0] > 0 && ws->wins[i].x + ws->wins[i].width > mon->x)
                    if (strut[0] > mon->strut_left) mon->strut_left = strut[0];
                if (strut[1] > 0 && ws->wins[i].x < mon->x + mon->width)
                    if (strut[1] > mon->strut_right) mon->strut_right = strut[1];
                if (strut[2] > 0 && ws->wins[i].y + ws->wins[i].height > mon->y)
                    if (strut[2] > mon->strut_top) mon->strut_top = strut[2];
                if (strut[3] > 0 && ws->wins[i].y < mon->y + mon->height)
                    if (strut[3] > mon->strut_bottom) mon->strut_bottom = strut[3];
            }
            if (data) XFree(data);
            data = NULL;
        }
    }

    /* Also check unmanaged root children (WIN_SKIP docks like polybar) */
    {
        Window root_ret, parent_ret, *children = NULL;
        unsigned int nchildren = 0;
        if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
            for (i = 0; i < (int)nchildren; i++) {
                XWindowAttributes wa;
                if (!XGetWindowAttributes(dpy, children[i], &wa)) continue;
                if (wa.map_state != IsViewable) continue;

                if (XGetWindowProperty(dpy, children[i], atom_net_wm_strut,
                                       0, 4, False, XA_CARDINAL, &actual,
                                       &format, &nitems, &bytes_after,
                                       &data) == Success) {
                    if (data && actual == XA_CARDINAL && format == 32 && nitems >= 4) {
                        long *strut = (long *)data;
                        if (strut[0] > 0 && wa.x + wa.width > mon->x)
                            if (strut[0] > mon->strut_left) mon->strut_left = strut[0];
                        if (strut[1] > 0 && wa.x < mon->x + mon->width)
                            if (strut[1] > mon->strut_right) mon->strut_right = strut[1];
                        if (strut[2] > 0 && wa.y + wa.height > mon->y)
                            if (strut[2] > mon->strut_top) mon->strut_top = strut[2];
                        if (strut[3] > 0 && wa.y < mon->y + mon->height)
                            if (strut[3] > mon->strut_bottom) mon->strut_bottom = strut[3];
                    }
                    if (data) XFree(data);
                    data = NULL;
                }
            }
            if (children) XFree(children);
        }
    }

    mon->strut_valid = 1;
}

/* Compute the rectangular region of the monitor that is usable for tiling,
   after subtracting external struts. */
static void
compute_usable_area(Monitor *mon, int *usable_w, int *usable_h,
                    int *x_start, int *y_start)
{
    compute_struts(mon);

    *usable_h = mon->height - mon->strut_top - mon->strut_bottom;
    *usable_w = mon->width - mon->strut_left - mon->strut_right;
    *x_start  = mon->x + mon->strut_left;
    *y_start  = mon->y + mon->strut_top;

    if (*usable_h < MIN_WIN_DIM) *usable_h = mon->height;
    if (*usable_w < MIN_WIN_DIM) *usable_w = mon->width;
}

/* ---- tiling: horizontal scroll layout (infinite canvas) ---- */
/* Each tiled window is a column whose width is scaled by width_factor.
   A virtual camera (cam_x) scrolls the strip left/right so that the
   focused column is visible — either centered or edge-snapped. */

static int
compute_usable_w(Monitor *mon)
{
    int usable_w = mon->width - mon->strut_left - mon->strut_right;
    if (usable_w < MIN_WIN_DIM) usable_w = mon->width;
    return usable_w;
}

void
update_camera_ws(Workspace *ws)
{
    Monitor *mon = curmon();
    int i;
    int usable_w, cam_x = 0, centered = 0, scrolled = 0;

    if (ws->ntiled == 0) return;

    usable_w = compute_usable_w(mon);

    int x_start = ws->tiled[0]->x - GAP_OUTER;
    int last = ws->ntiled - 1;
    int total_w = ws->tiled[last]->x + ws->tiled[last]->width
                + GAP_OUTER + 2 * BORDER_WIDTH - x_start;

    if (center_focused && ws->focused) {
        for (i = 0; i < ws->ntiled; i++) {
            if (ws->tiled[i] == ws->focused) {
                int outer = ws->tiled[i]->width + 2 * BORDER_WIDTH;
                cam_x = ws->tiled[i]->x - x_start - (usable_w - outer) / 2;
                centered = 1;
                break;
            }
        }
    } else if (ws->focused) {
        cam_x = ws->cam_x;
        for (i = 0; i < ws->ntiled; i++) {
            if (ws->tiled[i] == ws->focused) {
                int col = ws->tiled[i]->width + 2 * GAP_OUTER + 2 * BORDER_WIDTH;
                int col_left = ws->tiled[i]->x - x_start - GAP_OUTER;
                int col_right = col_left + col;
                if (col_left < cam_x) {
                    cam_x = col_left;
                    scrolled = 1;
                } else if (col_right > cam_x + usable_w) {
                    cam_x = col_right - usable_w;
                    scrolled = 1;
                }
                break;
            }
        }
    } else {
        cam_x = ws->cam_x;
    }

    if (!centered) {
        if (!scrolled && total_w <= usable_w)
            cam_x = 0;
        else {
            if (cam_x < 0) cam_x = 0;
            if (cam_x > total_w - usable_w) cam_x = total_w - usable_w;
        }
    }
    ws->cam_x = cam_x;

    for (i = 0; i < ws->ntiled; i++) {
        int screen_x = ws->tiled[i]->x - cam_x;
        XMoveResizeWindow(dpy, ws->tiled[i]->window,
                          screen_x, ws->tiled[i]->y,
                          ws->tiled[i]->width, ws->tiled[i]->height);
    }

    XFlush(dpy);
}

void
tile_horizontal_ws(Workspace *ws)
{
    Monitor *mon = curmon();
    int i;
    int usable_h, usable_w, x_start, y_start;
    int win_h, col_w, win_w;
    int cur_x;

    if (ws->ntiled == 0) return;

    compute_usable_area(mon, &usable_w, &usable_h, &x_start, &y_start);

    win_h = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
    if (win_h < 1) win_h = 1;

    cur_x = x_start;
    for (i = 0; i < ws->ntiled; i++) {
        float f = ws->tiled[i]->width_factor;
        if (f < MIN_WIDTH_FACTOR) f = MIN_WIDTH_FACTOR;
        if (f > MAX_WIDTH_FACTOR) f = MAX_WIDTH_FACTOR;
        if (ws->ntiled == 1)
            col_w = usable_w;
        else
            col_w = (int)((usable_w / (float)COLUMN_DIVISOR) * f);
        if (col_w < MIN_WIN_DIM + 2 * GAP_OUTER + 2 * BORDER_WIDTH)
            col_w = MIN_WIN_DIM + 2 * GAP_OUTER + 2 * BORDER_WIDTH;
        if (col_w > usable_w) col_w = usable_w;
        win_w = col_w - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        if (win_w < 1) win_w = 1;

        ws->tiled[i]->x = cur_x + GAP_OUTER;
        ws->tiled[i]->y = y_start + GAP_OUTER;
        ws->tiled[i]->width = win_w;
        ws->tiled[i]->height = win_h;
        cur_x += col_w;
    }

    update_camera_ws(ws);
}

/* ---- tiling: master-stack layout ---- */
/* Single-window fills the monitor.  Two or more: the first window
   (master) occupies a fraction (master_factor) of the width on the left;
   the remaining windows (stack) are stacked vertically on the right. */

void
tile_windows_ws(Workspace *ws)
{
    Monitor *mon = curmon();
    int i;
    int usable_h, usable_w, x_start, y_start;
    int master_w, stack_x, stack_w, stack_h;

    if (ws->ntiled == 0) return;

    compute_usable_area(mon, &usable_w, &usable_h, &x_start, &y_start);

    if (ws->ntiled == 1) {
        ws->tiled[0]->x = x_start + GAP_OUTER;
        ws->tiled[0]->y = y_start + GAP_OUTER;
        ws->tiled[0]->width = usable_w - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        ws->tiled[0]->height = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        if (ws->tiled[0]->width < 1) ws->tiled[0]->width = 1;
        if (ws->tiled[0]->height < 1) ws->tiled[0]->height = 1;

        XMoveResizeWindow(dpy, ws->tiled[0]->window,
                          ws->tiled[0]->x, ws->tiled[0]->y,
                          ws->tiled[0]->width, ws->tiled[0]->height);
    } else {
        master_w = (int)(usable_w * mon->master_factor)
                   - GAP_OUTER - GAP_INNER - 2 * BORDER_WIDTH;
        stack_x  = x_start + (int)(usable_w * mon->master_factor) + GAP_INNER;
        stack_w  = usable_w - (int)(usable_w * mon->master_factor)
                   - GAP_OUTER - GAP_INNER - 2 * BORDER_WIDTH;
        if (master_w < 1) master_w = 1;
        if (stack_w < 1) stack_w = 1;

        stack_h = (usable_h - GAP_OUTER * 2 - GAP_INNER * (ws->ntiled - 1)) / (ws->ntiled - 1)
                  - 2 * BORDER_WIDTH;
        if (stack_h < 1) stack_h = 1;

        ws->tiled[0]->x = x_start + GAP_OUTER;
        ws->tiled[0]->y = y_start + GAP_OUTER;
        ws->tiled[0]->width = master_w;
        ws->tiled[0]->height = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        if (ws->tiled[0]->height < 1) ws->tiled[0]->height = 1;

        for (i = 1; i < ws->ntiled; i++) {
            ws->tiled[i]->x = stack_x;
            ws->tiled[i]->y = y_start + GAP_OUTER
                          + (i - 1) * (stack_h + GAP_INNER + 2 * BORDER_WIDTH);
            ws->tiled[i]->width = stack_w;
            ws->tiled[i]->height = stack_h;
        }

        for (i = 0; i < ws->ntiled; i++) {
            XMoveResizeWindow(dpy, ws->tiled[i]->window,
                              ws->tiled[i]->x, ws->tiled[i]->y,
                              ws->tiled[i]->width, ws->tiled[i]->height);
        }
    }

    XFlush(dpy);
}

/* ---- resize ---- */

void
resize_master(void *arg)
{
    Workspace *ws = curws();
    Monitor *mon = NULL;
    int delta = (int)(long)arg;
    float delta_f;
    int i;

    if (ws->nwin < 2) return;

    /* Find the monitor containing the focused window */
    if (ws->focused) {
        for (i = 0; i < nmons; i++) {
            if (mons[i].x <= ws->focused->x
                && ws->focused->x < mons[i].x + mons[i].width) {
                mon = &mons[i];
                break;
            }
        }
    }
    if (!mon) mon = curmon();

    if (mon->horizontal_mode) return;

    delta_f = (float)delta / mon->width;
    mon->master_factor += delta_f;
    if (mon->master_factor < MIN_MASTER_VERT) mon->master_factor = MIN_MASTER_VERT;
    if (mon->master_factor > MAX_MASTER_VERT) mon->master_factor = MAX_MASTER_VERT;

    tile_windows_ws(ws);
    raise_above_windows(ws);
}

void
resize_window(void *arg)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    ManagedWindow *w = ws->focused;
    int dir = (int)(long)arg;

    if (!w) return;
    if (w->is_floating || w->is_fullscreen) return;

    /* In dwindle mode, use dwindle resize */
    if (!mon->horizontal_mode && ws->dwindle_root) {
        dwindle_resize(ws, dir, RESIZE_STEP);
        dwindle_arrange(ws, mon);
        raise_above_windows(ws);
        return;
    }

    w->width_factor += dir * RESIZE_FACTOR_STEP;
    if (w->width_factor < MIN_WIDTH_FACTOR) w->width_factor = MIN_WIDTH_FACTOR;
    if (w->width_factor > MAX_WIDTH_FACTOR) w->width_factor = MAX_WIDTH_FACTOR;
    w->is_fit = 0;

    if (mon->horizontal_mode)
        tile_horizontal_ws(ws);
    else
        tile_windows_ws(ws);
    raise_above_windows(ws);
}

/* Returns 1 if the caller should toggle fullscreen (window was floating
   or fullscreen), 0 otherwise.  This avoids a circular dependency where
   layout.c would need to call toggle_fullscreen() in wm.c. */
int
fit_window(void)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    ManagedWindow *w = ws->focused;
    if (!w) return 0;

    if (w->is_floating || w->is_fullscreen)
        return 1;

    /* Only works in horizontal scroll mode */
    if (!mon->horizontal_mode) return 0;

    if (!w->is_fit) {
        w->saved_factor = w->width_factor;
        w->width_factor = (float)COLUMN_DIVISOR;
        if (w->width_factor > MAX_WIDTH_FACTOR) w->width_factor = MAX_WIDTH_FACTOR;
        w->is_fit = 1;
    } else {
        w->width_factor = w->saved_factor;
        w->is_fit = 0;
    }
    tile_horizontal_ws(ws);
    return 0;
}

/* ---- retile ---- */
/* Deferred retile: multiple state changes within one event-loop iteration
   collapse into a single retile.  retile_deferred() sets a flag;
   flush_retile() (called once per event) performs the actual layout. */

/* Raise all floating windows above tiled.  Also called from focus paths
   and resize/move handlers to keep floating above tiled at all times.
   Order: regular floating first, then above/sticky on top. */
void
raise_above_windows(Workspace *ws)
{
    int i;
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].is_floating && !ws->wins[i].is_fullscreen &&
            !ws->wins[i].is_above && !ws->wins[i].is_sticky)
            XRaiseWindow(dpy, ws->wins[i].window);
    }
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].is_above || ws->wins[i].is_sticky)
            XRaiseWindow(dpy, ws->wins[i].window);
    }
}

void
retile_ws(Workspace *ws)
{
    Monitor *mon = curmon();
    if (mon->horizontal_mode)
        tile_horizontal_ws(ws);
    else
        dwindle_arrange(ws, mon);
    raise_above_windows(ws);
}

void
retile_deferred(void)
{
    retile_pending = 1;
}

void
flush_retile(void)
{
    if (retile_pending) {
        retile_pending = 0;
        retile_ws(curws());
    }
}

/* ---- layout toggles ---- */

void
toggle_center_focus(void)
{
    center_focused = !center_focused;
    retile_ws(curws());
}

void
toggle_layout(void)
{
    Workspace *ws = curws();
    Monitor *mon = curmon();
    int i;

    mon->horizontal_mode = !mon->horizontal_mode;

    if (mon->horizontal_mode) {
        mon->master_factor = 1.0f;
        /* Leaving dwindle: cleanup dwindle tree */
        dwindle_cleanup(ws);
        rebuild_tiled(ws);
        tile_horizontal_ws(ws);
    } else {
        mon->master_factor = 0.5f;
        /* Entering dwindle: build dwindle tree from all tiled windows */
        rebuild_tiled(ws);
        ws->dwindle_monocle = 0;
        for (i = 0; i < ws->ntiled; i++)
            layout_insert(ws, ws->tiled[i]->window);
        dwindle_arrange(ws, mon);
    }
    raise_above_windows(ws);
}

/* ---- dwindle tree ---- */

/* Collect all leaf nodes from the dwindle tree into the provided array.
   Returns the number of leaves collected (capped at MAX_LEAVES). */
static int
dwindle_collect_leaves(Workspace *ws, DwindleNode **out, int max)
{
    DwindleNode *stack[MAX_TREE_STACK];
    int top = 0;
    DwindleNode *cur = ws->dwindle_root;
    int n = 0;

    while (cur || top > 0) {
        while (cur) {
            if (top >= MAX_TREE_STACK) break;
            stack[top++] = cur;
            cur = cur->first;
        }
        if (top >= MAX_TREE_STACK) break;
        cur = stack[--top];
        if (cur->win && n < max)
            out[n++] = cur;
        cur = cur->second;
    }
    return n;
}

/* Look up a ManagedWindow by its X11 window ID. */
ManagedWindow *
dwindle_find_mw(Workspace *ws, Window w)
{
    int i;
    for (i = 0; i < ws->nwin; i++)
        if (ws->wins[i].window == w)
            return &ws->wins[i];
    return NULL;
}

DwindleNode *
dwindle_node_new(Window w)
{
    DwindleNode *n = calloc(1, sizeof(DwindleNode));
    if (!n) err(1, "dwindle_node_new");
    n->win = w;
    n->split_ratio = DWINDLE_SPLIT_RATIO;
    n->split_type = DWINDLE_SPLIT_V;
    return n;
}

/* Find the leaf node that stores this Window ID. */
static DwindleNode *
dwindle_find_leaf(DwindleNode *n, Window w)
{
    if (!n) return NULL;
    if (n->win == w) return n;
    DwindleNode *f = dwindle_find_leaf(n->first, w);
    if (f) return f;
    return dwindle_find_leaf(n->second, w);
}

/* Find the deepest focused leaf in the tree. */
static DwindleNode *
dwindle_find_focused_leaf(DwindleNode *n)
{
    if (!n) return NULL;
    if (n->win) return n;
    DwindleNode *f = dwindle_find_focused_leaf(n->second);
    if (f) return f;
    return dwindle_find_focused_leaf(n->first);
}

/* Insert a new window into the dwindle tree. */
void
dwindle_insert(Workspace *ws, Window w)
{
    DwindleNode *node = dwindle_node_new(w);
    DwindleNode *anchor;
    DwindleNode *parent;
    DwindleNode *grandparent;

    if (!ws->dwindle_root) {
        ws->dwindle_root = node;
        ws->dwindle_focus = node;
        return;
    }

    anchor = ws->dwindle_focus;
    if (!anchor || !anchor->win)
        anchor = dwindle_find_focused_leaf(ws->dwindle_root);
    if (!anchor) anchor = dwindle_find_leaf(ws->dwindle_root, ws->focused ? ws->focused->window : 0);
    if (!anchor) anchor = ws->dwindle_root;

    grandparent = anchor->parent;

    parent = calloc(1, sizeof(DwindleNode));
    if (!parent) err(1, "dwindle_insert");
    parent->split_ratio = DWINDLE_SPLIT_RATIO;

    /* Split type: longest side of anchor's rectangle */
    parent->split_type = (anchor->w > anchor->h)
                        ? DWINDLE_SPLIT_V : DWINDLE_SPLIT_H;

    /* New window always goes to second child (right/bottom) */
    parent->first = anchor;
    parent->second = node;
    node->parent = parent;
    anchor->parent = parent;
    parent->parent = grandparent;

    if (grandparent) {
        if (grandparent->first == anchor)
            grandparent->first = parent;
        else
            grandparent->second = parent;
    } else {
        ws->dwindle_root = parent;
    }

    ws->dwindle_focus = node;
}

/* Remove a window from the dwindle tree. Brother takes parent's place. */
void
dwindle_remove(Workspace *ws, Window w)
{
    DwindleNode *leaf = dwindle_find_leaf(ws->dwindle_root, w);
    if (!leaf) return;

    DwindleNode *parent = leaf->parent;
    if (!parent) {
        ws->dwindle_root = NULL;
        ws->dwindle_focus = NULL;
        free(leaf);
        return;
    }

    DwindleNode *brother = (parent->first == leaf) ? parent->second : parent->first;
    brother->parent = parent->parent;

    if (parent->parent) {
        if (parent->parent->first == parent)
            parent->parent->first = brother;
        else
            parent->parent->second = brother;
    } else {
        ws->dwindle_root = brother;
    }

    /* Adjust split type by longest side of parent's rectangle */
    if (brother->w < brother->h)
        brother->split_type = DWINDLE_SPLIT_H;
    else
        brother->split_type = DWINDLE_SPLIT_V;

    if (ws->dwindle_focus == leaf || ws->dwindle_focus == parent)
        ws->dwindle_focus = brother;

    free(parent);
    free(leaf);
}

/* ---- layout-mode wrappers ---- */

void
layout_insert(Workspace *ws, Window w)
{
    if (!curmon()->horizontal_mode)
        dwindle_insert(ws, w);
}

void
layout_remove(Workspace *ws, Window w)
{
    if (!curmon()->horizontal_mode)
        dwindle_remove(ws, w);
}

/* Recursive rectangle layout. */
static void
dwindle_apply_layout(Workspace *ws, DwindleNode *n, int x, int y, int w, int h)
{
    if (!n) return;

    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;

    if (n->win) {
        ManagedWindow *mw = dwindle_find_mw(ws, n->win);
        if (!mw) return;
        int win_x = x + GAP_OUTER;
        int win_y = y + GAP_OUTER;
        int win_w = w - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        int win_h = h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        if (win_w < 1) win_w = 1;
        if (win_h < 1) win_h = 1;

        mw->x = win_x;
        mw->y = win_y;
        mw->width = win_w;
        mw->height = win_h;

        XMoveResizeWindow(dpy, mw->window, mw->x, mw->y, mw->width, mw->height);
        return;
    }

    int fence;
    if (n->split_type == DWINDLE_SPLIT_V) {
        fence = (int)(w * n->split_ratio);
        if (fence < DWINDLE_MIN_NODE) fence = DWINDLE_MIN_NODE;
        if (fence > w - DWINDLE_MIN_NODE) fence = w - DWINDLE_MIN_NODE;
        dwindle_apply_layout(ws, n->first, x, y, fence, h);
        dwindle_apply_layout(ws, n->second, x + fence, y, w - fence, h);
    } else {
        fence = (int)(h * n->split_ratio);
        if (fence < DWINDLE_MIN_NODE) fence = DWINDLE_MIN_NODE;
        if (fence > h - DWINDLE_MIN_NODE) fence = h - DWINDLE_MIN_NODE;
        dwindle_apply_layout(ws, n->first, x, y, w, fence);
        dwindle_apply_layout(ws, n->second, x, y + fence, w, h - fence);
    }
}

/* Arrange dwindle tree on monitor. */
void
dwindle_arrange(Workspace *ws, Monitor *mon)
{
    int usable_h, usable_w, x_start, y_start;

    compute_usable_area(mon, &usable_w, &usable_h, &x_start, &y_start);

    if (ws->dwindle_monocle) {
        /* Monocle: focused window fullscreen, others hidden */
        if (ws->dwindle_focus && ws->dwindle_focus->win) {
            ManagedWindow *mw = dwindle_find_mw(ws, ws->dwindle_focus->win);
            if (mw) {
                mw->x = x_start + GAP_OUTER;
                mw->y = y_start + GAP_OUTER;
                mw->width = usable_w - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
                mw->height = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
                if (mw->width < 1) mw->width = 1;
                if (mw->height < 1) mw->height = 1;
                XMoveResizeWindow(dpy, mw->window, mw->x, mw->y, mw->width, mw->height);
                XRaiseWindow(dpy, mw->window);
            }
        }
        /* Hide all other tiled leaves */
        {
            DwindleNode *leaves[MAX_LEAVES];
            int nleaves = dwindle_collect_leaves(ws, leaves, MAX_LEAVES);
            int li;
            for (li = 0; li < nleaves; li++) {
                if (leaves[li] != ws->dwindle_focus) {
                    ManagedWindow *mw = dwindle_find_mw(ws, leaves[li]->win);
                    if (mw) XUnmapWindow(dpy, mw->window);
                }
            }
        }
        XFlush(dpy);
        return;
    }

    dwindle_apply_layout(ws, ws->dwindle_root, x_start, y_start, usable_w, usable_h);
    XFlush(dpy);
}

/* Free all dwindle nodes. */
static void
dwindle_cleanup_recursive(DwindleNode *n)
{
    if (!n) return;
    dwindle_cleanup_recursive(n->first);
    dwindle_cleanup_recursive(n->second);
    free(n);
}

void
dwindle_cleanup(Workspace *ws)
{
    dwindle_cleanup_recursive(ws->dwindle_root);
    ws->dwindle_root = NULL;
    ws->dwindle_focus = NULL;
}

/* Focus next/prev leaf via in-order traversal. */
void
dwindle_focus_prevnext(Workspace *ws, int delta)
{
    if (!ws->dwindle_root || !ws->dwindle_focus) return;

    DwindleNode *leaves[MAX_LEAVES];
    int nleaves = dwindle_collect_leaves(ws, leaves, MAX_LEAVES);

    if (nleaves == 0) return;

    int idx = 0;
    for (int i = 0; i < nleaves; i++) {
        if (leaves[i] == ws->dwindle_focus) {
            idx = i;
            break;
        }
    }

    idx += delta;
    if (idx < 0) idx = nleaves - 1;
    if (idx >= nleaves) idx = 0;

    ws->dwindle_focus = leaves[idx];
    ManagedWindow *mw = dwindle_find_mw(ws, leaves[idx]->win);
    if (mw) refocus(ws, mw);
    raise_above_windows(ws);
}

/* Find the fence ancestor perpendicular to resize direction. */
static DwindleNode *
dwindle_find_fence(DwindleNode *leaf, int dir)
{
    DwindleNode *n = leaf;
    while (n->parent) {
        DwindleNode *p = n->parent;
        if (dir == DWINDLE_DIR_WEST || dir == DWINDLE_DIR_EAST) {
            if (p->split_type == DWINDLE_SPLIT_V) return p;
        } else {
            if (p->split_type == DWINDLE_SPLIT_H) return p;
        }
        n = p;
    }
    return NULL;
}

/* Resize: adjust split ratio of the fence ancestor. */
void
dwindle_resize(Workspace *ws, int dir, int delta)
{
    if (!ws->dwindle_focus || !ws->dwindle_focus->win) return;

    /* Map keybind delta (-1/+1) to DWINDLE_DIR_* constants */
    int d_dir;
    if (delta < 0)
        d_dir = DWINDLE_DIR_WEST;
    else
        d_dir = DWINDLE_DIR_EAST;

    (void)dir;

    DwindleNode *fence = dwindle_find_fence(ws->dwindle_focus, d_dir);
    if (!fence) return;

    float step = (float)abs(delta) / DWINDLE_SPLIT_STEP;
    if (d_dir == DWINDLE_DIR_WEST || d_dir == DWINDLE_DIR_NORTH)
        step = -step;

    fence->split_ratio += step;
    if (fence->split_ratio < DWINDLE_RATIO_MIN) fence->split_ratio = DWINDLE_RATIO_MIN;
    if (fence->split_ratio > DWINDLE_RATIO_MAX) fence->split_ratio = DWINDLE_RATIO_MAX;
}

/* Get the ManagedWindow of the focused dwindle leaf. */
ManagedWindow *
dwindle_focused_mw(Workspace *ws)
{
    if (ws->dwindle_focus && ws->dwindle_focus->win)
        return dwindle_find_mw(ws, ws->dwindle_focus->win);
    return NULL;
}

/* Set dwindle focus to the leaf containing the given window. */
void
dwindle_set_focus(Workspace *ws, Window w)
{
    if (!ws->dwindle_root) return;
    DwindleNode *leaf = dwindle_find_leaf(ws->dwindle_root, w);
    if (leaf) ws->dwindle_focus = leaf;
}

/* Toggle monocle sub-mode within dwindle. */
void
toggle_monocle(void)
{
    Workspace *ws = curws();
    Monitor *mon = curmon();
    if (mon->horizontal_mode) return;

    ws->dwindle_monocle = !ws->dwindle_monocle;

    dwindle_arrange(ws, mon);
}
