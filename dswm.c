#include "dswm.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>

/* macros */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

/* data structures */
typedef struct ManagedWindow ManagedWindow;
struct ManagedWindow {
    Window window;
    int x, y, width, height;
    int is_floating;
    int is_fullscreen;
    int workspace;
    int monitor;
    /* pre-fullscreen geometry */
    int pre_fs_x, pre_fs_y;
    int pre_fs_width, pre_fs_height;
    int pre_fs_floating;
    /* no titlebar, no std::string — simple char buf */
};

typedef struct Monitor Monitor;
struct Monitor {
    int id;
    int x, y;
    int width, height;
    int current_workspace;
    float master_factor;
    int horizontal_mode;
    int scroll_windows_visible;
    /* cached struts */
    int strut_top, strut_bottom, strut_left, strut_right;
    int strut_valid;
};

typedef struct Workspace Workspace;
struct Workspace {
    ManagedWindow *wins;
    int nwin;
    int cap;
    ManagedWindow *focused;
    int scroll_offset;
    /* incremental tiled list */
    ManagedWindow **tiled;
    int ntiled;
    int tiled_cap;
};

/* globals */
static Display *dpy;
static Window root;
static int screen;
static int scrw, scrh;
static int running = 1;
static int cur_ws;
static int retile_pending = 0;

static Monitor mons[8];
static int nmons;
static Workspace spaces[NUM_WORKSPACES];

/* cached atoms — interned once at startup */
static Atom atom_wm_delete;
static Atom atom_wm_protocols;
static Atom atom_net_wm_strut;
static Atom atom_net_wm_state;
static Atom atom_net_wm_state_full;
static Atom atom_net_current_desktop;
static Atom atom_net_supported;
static Atom atom_net_number_of_desktops;
static Atom atom_net_active_window;
static Atom atom_net_wm_name;

/* ---- workspace helpers ---- */

static Workspace *
curws(void)
{
    return &spaces[cur_ws];
}

static Monitor *
curmon(void)
{
    int i;
    for (i = 0; i < nmons; i++)
        if (mons[i].current_workspace == cur_ws)
            return &mons[i];
    return &mons[0];
}

/* ---- tiled list helpers ---- */

static void
tiled_ensure_cap(Workspace *ws)
{
    if (ws->ntiled >= ws->tiled_cap) {
        int newcap = ws->tiled_cap ? ws->tiled_cap * 2 : INITIAL_CAP;
        ManagedWindow **tmp = realloc(ws->tiled, newcap * sizeof(ManagedWindow *));
        if (!tmp) return;
        ws->tiled = tmp;
        ws->tiled_cap = newcap;
    }
}

static void
tiled_add(Workspace *ws, ManagedWindow *mw)
{
    tiled_ensure_cap(ws);
    ws->tiled[ws->ntiled++] = mw;
}

static void
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

/* ---- bar strut support ---- */

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

    for (i = 0; i < ws->nwin; i++) {
        if (XGetWindowProperty(dpy, ws->wins[i].window, atom_net_wm_strut,
                               0, 4, False, XA_CARDINAL, &actual, &format,
                               &nitems, &bytes_after, &data) == Success
            && data && nitems >= 4) {
            long *strut = (long *)data;
            if (strut[0] > 0 && ws->wins[i].x < mon->x + mon->width)
                if (strut[0] > mon->strut_left) mon->strut_left = strut[0];
            if (strut[1] > 0 && ws->wins[i].x + ws->wins[i].width > mon->x)
                if (strut[1] > mon->strut_right) mon->strut_right = strut[1];
            if (strut[2] > 0 && ws->wins[i].y < mon->y + mon->height)
                if (strut[2] > mon->strut_top) mon->strut_top = strut[2];
            if (strut[3] > 0 && ws->wins[i].y + ws->wins[i].height > mon->y)
                if (strut[3] > mon->strut_bottom) mon->strut_bottom = strut[3];
            XFree(data);
            data = NULL;
        }
    }

    mon->strut_valid = 1;
}

/* ---- tiling: compute usable area (shared by all layouts) ---- */

static void
compute_usable_area(Monitor *mon, int *usable_w, int *usable_h,
                    int *x_start, int *y_start)
{
    int own_bar_top = 0, own_bar_bottom = 0;

    if (BAR_POSITION == 0)
        own_bar_top = BAR_HEIGHT;
    else
        own_bar_bottom = BAR_HEIGHT;

    compute_struts(mon);

    *usable_h = mon->height - MAX(own_bar_top, mon->strut_top)
                       - MAX(own_bar_bottom, mon->strut_bottom);
    *usable_w = mon->width - mon->strut_left - mon->strut_right;
    *x_start  = mon->x + mon->strut_left;
    *y_start  = mon->y + MAX(own_bar_top, mon->strut_top);

    if (*usable_h < MIN_WIN_DIM) *usable_h = mon->height;
    if (*usable_w < MIN_WIN_DIM) *usable_w = mon->width;
}

/* ---- tiling: horizontal scroll layout ---- */

static void
tile_horizontal(void)
{
    Workspace *ws = curws();
    Monitor *mon = curmon();
    int i;
    int usable_h, usable_w, x_start, y_start;
    int scroll_vis, ww;
    int all_fit;
    int x_pos, y_pos, win_w, win_h;

    if (ws->ntiled == 0) return;

    compute_usable_area(mon, &usable_w, &usable_h, &x_start, &y_start);

    scroll_vis = mon->scroll_windows_visible;
    if (scroll_vis < MIN_SCROLL_VIS) scroll_vis = MIN_SCROLL_VIS;

    all_fit = (ws->ntiled <= scroll_vis);

    if (all_fit) {
        ww = usable_w / ws->ntiled;
    } else {
        ww = usable_w / scroll_vis;
    }
    if (ww < MIN_WIN_W) ww = MIN_WIN_W;
    if (ww > usable_w) ww = usable_w;

    if (!all_fit) {
        int max_off = ws->ntiled * ww - usable_w;
        if (max_off < 0) max_off = 0;
        if (ws->scroll_offset < 0) ws->scroll_offset = 0;
        if (ws->scroll_offset > max_off) ws->scroll_offset = max_off;
    } else {
        ws->scroll_offset = 0;
    }

    for (i = 0; i < ws->ntiled; i++) {
        int scroll_off = all_fit ? 0 : ws->scroll_offset;
        x_pos = x_start + i * ww - scroll_off + GAP_OUTER;
        y_pos = y_start + GAP_OUTER;
        win_w = ww - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        win_h = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
        if (win_w < 1) win_w = 1;
        if (win_h < 1) win_h = 1;

        ws->tiled[i]->x = x_pos;
        ws->tiled[i]->y = y_pos;
        ws->tiled[i]->width = win_w;
        ws->tiled[i]->height = win_h;

        XMoveResizeWindow(dpy, ws->tiled[i]->window,
                          ws->tiled[i]->x, ws->tiled[i]->y,
                          ws->tiled[i]->width, ws->tiled[i]->height);
    }

    XFlush(dpy);
}

/* ---- tiling: master-stack layout ---- */

static void
tile_windows(void)
{
    Workspace *ws = curws();
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

/* ---- tiling: resize master factor ---- */

static void
resize_master(void *arg)
{
    Workspace *ws = curws();
    Monitor *mon = curmon();
    int delta = (int)(long)arg;
    float delta_f;

    if (mon->horizontal_mode) return;

    if (ws->nwin < 2) return;

    delta_f = (float)delta / mon->width;
    mon->master_factor += delta_f;
    if (mon->master_factor < MIN_MASTER_VERT) mon->master_factor = MIN_MASTER_VERT;
    if (mon->master_factor > MAX_MASTER_VERT) mon->master_factor = MAX_MASTER_VERT;

    tile_windows();
}

static void
retile(void)
{
    Monitor *mon = curmon();
    if (mon->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
}

static void
retile_deferred(void)
{
    retile_pending = 1;
}

static void
flush_retile(void)
{
    if (retile_pending) {
        retile_pending = 0;
        retile();
    }
}

/* ---- tiling: monitor focus ---- */

static void show_workspace(int idx, int visible);
static void update_ewmh_current_desktop(void);

static void
update_border(Window w, int focused)
{
    XSetWindowBorder(dpy, w, focused ? FOCUS_COLOR : BORDER_COLOR);
}

static void
refocus(Workspace *ws, ManagedWindow *new)
{
    if (ws->focused && ws->focused != new)
        update_border(ws->focused->window, 0);
    ws->focused = new;
    if (new) {
        update_border(new->window, 1);
        XSetInputFocus(dpy, new->window, RevertToPointerRoot, CurrentTime);
        XRaiseWindow(dpy, new->window);
    }
}

static void
focus_monitor(void *arg)
{
    int mon_idx = (int)(long)arg;
    int old_ws;
    if (mon_idx < 0 || mon_idx >= nmons) return;

    old_ws = cur_ws;
    if (mons[mon_idx].current_workspace == old_ws) return;

    show_workspace(old_ws, 0);
    cur_ws = mons[mon_idx].current_workspace;
    show_workspace(cur_ws, 1);

    retile();

    update_ewmh_current_desktop();
}

/* ---- tiling: set scroll_visible ---- */

static void
set_scroll_visible(void *arg)
{
    int val = (int)(long)arg;
    Monitor *mon = curmon();
    Workspace *ws = curws();

    /* positive = absolute set, negative = relative adjust */
    if (val >= 0) {
        if (val < MIN_SCROLL_VIS) val = MIN_SCROLL_VIS;
        if (val > MAX_SCROLL_VIS) val = MAX_SCROLL_VIS;
        mon->scroll_windows_visible = val;
    } else {
        mon->scroll_windows_visible += val;
        if (mon->scroll_windows_visible < MIN_SCROLL_VIS)
            mon->scroll_windows_visible = MIN_SCROLL_VIS;
        if (mon->scroll_windows_visible > MAX_SCROLL_VIS)
            mon->scroll_windows_visible = MAX_SCROLL_VIS;
    }

    ws->scroll_offset = 0;
    if (mon->horizontal_mode)
        tile_horizontal();
}

/* ---- tiling: scroll left/right ---- */

static void
move_horizontal(int forward)
{
    Monitor *mon = curmon();
    Workspace *ws = curws();
    int scroll_vis, ww, total_w, max_scroll;

    if (!mon->horizontal_mode) return;
    if (ws->nwin == 0) return;

    scroll_vis = mon->scroll_windows_visible;
    if (scroll_vis < 1) scroll_vis = 1;

    ww = mon->width / scroll_vis;

    if (forward) {
        total_w = ws->nwin * ww;
        max_scroll = total_w - mon->width;
        if (max_scroll < 0) max_scroll = 0;
        ws->scroll_offset += ww;
        if (ws->scroll_offset > max_scroll)
            ws->scroll_offset = max_scroll;
    } else {
        ws->scroll_offset -= ww;
        if (ws->scroll_offset < 0)
            ws->scroll_offset = 0;
    }

    tile_horizontal();
}

/* ---- tiling: toggle layout mode ---- */

static void
toggle_layout(void)
{
    Monitor *mon = curmon();
    Workspace *ws = curws();

    mon->horizontal_mode = !mon->horizontal_mode;
    ws->scroll_offset = 0;

    if (mon->horizontal_mode) {
        mon->master_factor = 1.0f;
        tile_horizontal();
    } else {
        mon->master_factor = 0.5f;
        tile_windows();
    }
}

/* ---- tiling: swap windows ---- */

static void
swap_impl(int delta)
{
    Workspace *ws = curws();
    ManagedWindow tmp;
    int cur_idx = -1, swap_idx, i;

    if (ws->nwin < 2) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->focused && ws->wins[i].window == ws->focused->window) {
            cur_idx = i;
            break;
        }
    }

    if (cur_idx == -1 || ws->wins[cur_idx].is_floating) return;

    swap_idx = (cur_idx + delta + ws->nwin) % ws->nwin;

    /* swap */
    tmp = ws->wins[cur_idx];
    ws->wins[cur_idx] = ws->wins[swap_idx];
    ws->wins[swap_idx] = tmp;

    retile_deferred();

    ws->focused = &ws->wins[swap_idx];
}

static void
swap_next(void)
{
    swap_impl(1);
}

static void
swap_prev(void)
{
    swap_impl(-1);
}

/* ---- workspace management ---- */

static void
show_workspace(int idx, int visible)
{
    Workspace *ws = &spaces[idx];
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (visible)
            XMapWindow(dpy, ws->wins[i].window);
        else
            XUnmapWindow(dpy, ws->wins[i].window);
    }
    XFlush(dpy);
}

static void
switch_workspace(void *arg)
{
    int idx = (int)(long)arg;
    if (idx < 0 || idx >= NUM_WORKSPACES) return;
    if (idx == cur_ws) return;

    show_workspace(cur_ws, 0);
    cur_ws = idx;
    show_workspace(cur_ws, 1);

    retile();

    update_ewmh_current_desktop();
}

static void
move_to_workspace(void *arg)
{
    int idx = (int)(long)arg;
    Workspace *ws = curws();
    ManagedWindow win;
    int i, found = 0;

    if (idx < 0 || idx >= NUM_WORKSPACES) return;
    if (idx == cur_ws) return;
    if (!ws->focused) return;

    /* save data before memmove invalidates pointer */
    win = *ws->focused;

    /* remove from current workspace */
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == win.window) {
            tiled_remove(ws, win.window);
            memmove(&ws->wins[i], &ws->wins[i + 1],
                    (ws->nwin - i - 1) * sizeof(ManagedWindow));
            ws->nwin--;
            found = 1;
            break;
        }
    }
    if (!found) return;

    /* add to target workspace */
    Workspace *target = &spaces[idx];
    if (target->nwin >= target->cap) {
        int newcap = target->cap ? target->cap * 2 : INITIAL_CAP;
        ManagedWindow *tmp = realloc(target->wins, newcap * sizeof(ManagedWindow));
        if (!tmp) { free(target->wins); target->wins = NULL; target->cap = 0; err(1, "realloc"); }
        target->wins = tmp;
        target->cap = newcap;
    }
    win.workspace = idx;
    target->wins[target->nwin++] = win;
    target->focused = &target->wins[target->nwin - 1];

    /* update tiled list for target workspace */
    if (!win.is_floating && !win.is_fullscreen)
        tiled_add(target, &target->wins[target->nwin - 1]);

    /* hide the moved window (it's on a non-active workspace now) */
    XUnmapWindow(dpy, win.window);

    refocus(ws, NULL);
    retile();
}

/* ---- window management ---- */

static void
manage_window(Window w)
{
    Workspace *ws = curws();
    XWindowAttributes wa;
    XClassHint ch = { NULL, NULL };
    ManagedWindow mw;
    int i;

    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.override_redirect) return;

    memset(&mw, 0, sizeof(mw));
    mw.window = w;
    mw.x = wa.x;
    mw.y = wa.y;
    mw.width = wa.width;
    mw.height = wa.height;
    mw.workspace = cur_ws;
    mw.monitor = curmon()->id;

    /* apply window rules */
    if (XGetClassHint(dpy, w, &ch)) {
        for (i = 0; i < (int)NELEM(rules); i++) {
            if (ch.res_class && strcmp(ch.res_class, rules[i].wm_class) == 0) {
                mw.is_floating = rules[i].is_floating;
                break;
            }
        }
        if (ch.res_class) XFree(ch.res_class);
        if (ch.res_name) XFree(ch.res_name);
    }

    if (ws->nwin >= ws->cap) {
        int newcap = ws->cap ? ws->cap * 2 : INITIAL_CAP;
        ManagedWindow *tmp = realloc(ws->wins, newcap * sizeof(ManagedWindow));
        if (!tmp) { free(ws->wins); ws->wins = NULL; ws->cap = 0; err(1, "realloc"); }
        ws->wins = tmp;
        ws->cap = newcap;
    }
    ws->wins[ws->nwin++] = mw;

    /* update incremental tiled list */
    if (!mw.is_floating && !mw.is_fullscreen)
        tiled_add(ws, &ws->wins[ws->nwin - 1]);

    XSelectInput(dpy, w, EnterWindowMask | StructureNotifyMask);
    XSetWindowBorderWidth(dpy, w, BORDER_WIDTH);
    refocus(ws, &ws->wins[ws->nwin - 1]);

    /* only map if on the current workspace */
    if (mw.workspace == cur_ws)
        XMapWindow(dpy, w);

    if (curmon()->horizontal_mode) {
        Monitor *mon = curmon();
        int scroll_vis = mon->scroll_windows_visible;
        if (scroll_vis < MIN_SCROLL_VIS) scroll_vis = MIN_SCROLL_VIS;
        if (ws->nwin > scroll_vis) {
            int ww = mon->width / scroll_vis;
            int new_off = (ws->nwin - scroll_vis) * ww;
            if (new_off > ws->scroll_offset)
                ws->scroll_offset = new_off;
        }
        tile_horizontal();
    } else {
        tile_windows();
    }
}

static void
unmanage_window(Window w)
{
    Workspace *ws = curws();
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == w) {
            tiled_remove(ws, w);
            memmove(&ws->wins[i], &ws->wins[i + 1],
                    (ws->nwin - i - 1) * sizeof(ManagedWindow));
            ws->nwin--;
            break;
        }
    }

    ws->focused = (ws->nwin > 0) ? &ws->wins[ws->nwin - 1] : NULL;
    if (ws->focused)
        update_border(ws->focused->window, 1);

    retile_deferred();
}

/* ---- focus ---- */

static void
focus_next(void)
{
    Workspace *ws = curws();
    Monitor *mon = curmon();
    int ntiled = 0, cur_tiled = -1, target = -1, i;

    if (ws->nwin == 0) return;

    /* single pass: count tiled and find current tiled index */
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].monitor != mon->id || ws->wins[i].is_floating) continue;
        if (ws->focused && ws->wins[i].window == ws->focused->window)
            cur_tiled = ntiled;
        ntiled++;
    }
    if (ntiled == 0) return;

    /* find target tiled index */
    target = (cur_tiled == -1) ? 0 : cur_tiled + 1;
    if (target >= ntiled) return;

    /* second pass: find window at target tiled index */
    int count = 0;
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].monitor != mon->id || ws->wins[i].is_floating) continue;
        if (count == target) {
            refocus(ws, &ws->wins[i]);
            break;
        }
        count++;
    }

    /* auto-scroll if the new focus is off-screen */
    if (mon->horizontal_mode && ntiled > mon->scroll_windows_visible) {
        int scroll_vis = mon->scroll_windows_visible;
        if (scroll_vis < MIN_SCROLL_VIS) scroll_vis = MIN_SCROLL_VIS;
        int ww = mon->width / scroll_vis;
        int first_visible = ws->scroll_offset / ww;
        int last_visible = first_visible + scroll_vis - 1;
        int old_offset = ws->scroll_offset;

        if (target > last_visible) {
            ws->scroll_offset += ww;
        } else if (target < first_visible) {
            ws->scroll_offset -= ww;
            if (ws->scroll_offset < 0) ws->scroll_offset = 0;
        }

        if (ws->scroll_offset != old_offset)
            tile_horizontal();
    }
}

static void
focus_prev(void)
{
    Workspace *ws = curws();
    Monitor *mon = curmon();
    int ntiled = 0, cur_tiled = -1, target = -1, i;

    if (ws->nwin == 0) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].monitor != mon->id || ws->wins[i].is_floating) continue;
        if (ws->focused && ws->wins[i].window == ws->focused->window)
            cur_tiled = ntiled;
        ntiled++;
    }
    if (ntiled == 0) return;

    target = (cur_tiled == -1) ? ntiled - 1 : cur_tiled - 1;
    if (target < 0) return;

    int count = 0;
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].monitor != mon->id || ws->wins[i].is_floating) continue;
        if (count == target) {
            refocus(ws, &ws->wins[i]);
            break;
        }
        count++;
    }

    if (mon->horizontal_mode && ntiled > mon->scroll_windows_visible) {
        int scroll_vis = mon->scroll_windows_visible;
        if (scroll_vis < MIN_SCROLL_VIS) scroll_vis = MIN_SCROLL_VIS;
        int ww = mon->width / scroll_vis;
        int first_visible = ws->scroll_offset / ww;
        int last_visible = first_visible + scroll_vis - 1;
        int old_offset = ws->scroll_offset;

        if (target > last_visible) {
            ws->scroll_offset += ww;
        } else if (target < first_visible) {
            ws->scroll_offset -= ww;
            if (ws->scroll_offset < 0) ws->scroll_offset = 0;
        }

        if (ws->scroll_offset != old_offset)
            tile_horizontal();
    }
}

/* ---- close/quit ---- */

static void
close_window(void)
{
    Workspace *ws = curws();
    XEvent ev;
    Atom wm_delete;

    if (!ws->focused) return;

    wm_delete = atom_wm_delete;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = ws->focused->window;
    ev.xclient.message_type = atom_wm_protocols;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = wm_delete;
    XSendEvent(dpy, ws->focused->window, False, NoEventMask, &ev);
}

static void
quit_wm(void)
{
    running = 0;
}

/* ---- toggle states ---- */

static void
toggle_fullscreen(void)
{
    Workspace *ws = curws();
    ManagedWindow *w = ws->focused;

    if (!w) return;

    w->is_fullscreen = !w->is_fullscreen;

    /* update tiled list */
    if (w->is_fullscreen)
        tiled_remove(ws, w->window);
    else if (!w->is_floating)
        tiled_add(ws, w);

    if (w->is_fullscreen) {
        /* save pre-fullscreen geometry */
        w->pre_fs_x = w->x;
        w->pre_fs_y = w->y;
        w->pre_fs_width = w->width;
        w->pre_fs_height = w->height;
        w->pre_fs_floating = w->is_floating;
        w->is_floating = 1;

        /* maximize to screen */
        w->x = 0;
        w->y = 0;
        w->width = scrw;
        w->height = scrh;
        XSetWindowBorderWidth(dpy, w->window, 0);
        XMoveResizeWindow(dpy, w->window, 0, 0, scrw, scrh);
        XRaiseWindow(dpy, w->window);

        /* set EWMH state */
        XChangeProperty(dpy, w->window, atom_net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&atom_net_wm_state_full, 1);
    } else {
        /* restore */
        w->is_floating = w->pre_fs_floating;
        w->x = w->pre_fs_x;
        w->y = w->pre_fs_y;
        w->width = w->pre_fs_width;
        w->height = w->pre_fs_height;
        XSetWindowBorderWidth(dpy, w->window, BORDER_WIDTH);
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);

        /* clear EWMH state */
        XChangeProperty(dpy, w->window, atom_net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)0, 0);

        retile_deferred();
    }

    XFlush(dpy);
}

static void
toggle_float(void)
{
    Workspace *ws = curws();
    ManagedWindow *w = ws->focused;
    if (!w) return;

    w->is_floating = !w->is_floating;

    /* update tiled list */
    if (w->is_floating)
        tiled_remove(ws, w->window);
    else
        tiled_add(ws, w);

    if (w->is_floating) {
        /* center on screen */
        w->x = scrw / 2 - w->width / 2;
        w->y = scrh / 2 - w->height / 2;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);
        XRaiseWindow(dpy, w->window);
    } else {
        retile_deferred();
    }

    XFlush(dpy);
}

/* ---- spawn ---- */

static void
spawn(void *arg)
{
    const char **cmd = (const char **)arg;
    if (fork() == 0) {
        close(ConnectionNumber(dpy));
        setsid();
        execvp(cmd[0], (char *const *)cmd);
        fprintf(stderr, "dswm: execvp %s failed\n", cmd[0]);
        _exit(1);
    }
}

/* ---- EWMH ---- */

static void
setup_ewmh(void)
{
    XChangeProperty(dpy, root, atom_net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char[]){
                        atom_net_number_of_desktops,
                        atom_net_current_desktop,
                        atom_net_active_window,
                        atom_net_wm_name,
                    }, 4);

    long ndesk = NUM_WORKSPACES;
    XChangeProperty(dpy, root, atom_net_number_of_desktops, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&ndesk, 1);

    long cdesk = cur_ws;
    XChangeProperty(dpy, root, atom_net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&cdesk, 1);
}

static void
update_ewmh_current_desktop(void)
{
    long cdesk = cur_ws;
    XChangeProperty(dpy, root, atom_net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&cdesk, 1);
    XFlush(dpy);
}

/* ---- key grabbing ---- */

static void
grab_keys(void)
{
    unsigned int i, j;
    KeyCode code;
    unsigned int mods[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };

    XUngrabKey(dpy, AnyKey, AnyModifier, root);

    for (i = 0; i < NELEM(keys); i++) {
        code = XKeysymToKeycode(dpy, keys[i].sym);
        if (!code) continue;
        for (j = 0; j < NELEM(mods); j++)
            XGrabKey(dpy, code, keys[i].mod | mods[j], root,
                     True, GrabModeAsync, GrabModeAsync);
    }
}

/* ---- error handler (ignore X errors) ---- */

static int
xerror(Display *d, XErrorEvent *ee)
{
    (void)d;
    (void)ee;
    return 0;
}

/* ---- event handlers ---- */

static void
handle_map_request(XMapRequestEvent *e)
{
    manage_window(e->window);
}

static void
handle_destroy_notify(XDestroyWindowEvent *e)
{
    unmanage_window(e->window);
}

static void
handle_unmap_notify(XUnmapEvent *e)
{
    unmanage_window(e->window);
}

static void
handle_configure_request(XConfigureRequestEvent *e)
{
    Workspace *ws = curws();
    ManagedWindow *mw = NULL;
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            mw = &ws->wins[i];
            break;
        }
    }

    /* tiled windows: WM controls geometry, only honor stacking */
    if (mw && !mw->is_floating && !mw->is_fullscreen) {
        XWindowChanges wc;
        wc.sibling = e->above;
        wc.stack_mode = e->detail;
        XConfigureWindow(dpy, e->window, CWSibling | CWStackMode, &wc);
        return;
    }

    /* floating/unknown: honor everything */
    XWindowChanges wc;
    wc.x = e->x;
    wc.y = e->y;
    wc.width = e->width;
    wc.height = e->height;
    wc.border_width = e->border_width;
    wc.sibling = e->above;
    wc.stack_mode = e->detail;
    XConfigureWindow(dpy, e->window, e->value_mask, &wc);
}

static void
handle_enter_notify(XCrossingEvent *e)
{
    Workspace *ws = curws();
    int i;

    if (e->mode != NotifyNormal || e->detail == NotifyInferior) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            refocus(ws, &ws->wins[i]);
            break;
        }
    }
}

static void
handle_key_press(XKeyEvent *e)
{
    KeySym keysym = XLookupKeysym(e, 0);
    unsigned int mod = e->state & (Mod1Mask | Mod4Mask | ShiftMask | ControlMask);
    int i;

    for (i = 0; i < (int)NELEM(keys); i++) {
        if (keys[i].sym == keysym && keys[i].mod == mod) {
            switch (keys[i].act) {
            case SPAWN:              spawn(keys[i].arg.v); break;
            case CLOSE:              close_window(); break;
            case QUIT:               quit_wm(); break;
            case FOCUS_NEXT:         focus_next(); break;
            case FOCUS_PREV:         focus_prev(); break;
            case SWAP_NEXT:          swap_next(); break;
            case SWAP_PREV:          swap_prev(); break;
            case RESIZE_MASTER:      resize_master((void *)(long)keys[i].arg.i); break;
            case SCROLL_LEFT:        move_horizontal(0); break;
            case SCROLL_RIGHT:       move_horizontal(1); break;
            case TOGGLE_LAYOUT:      toggle_layout(); break;
            case TOGGLE_FULLSCREEN:  toggle_fullscreen(); break;
            case TOGGLE_FLOAT:       toggle_float(); break;
            case SWITCH_WORKSPACE:   switch_workspace((void *)(long)keys[i].arg.i); break;
            case MOVE_TO_WORKSPACE:  move_to_workspace((void *)(long)keys[i].arg.i); break;
            case FOCUS_MONITOR:      focus_monitor((void *)(long)keys[i].arg.i); break;
            case SET_SCROLL_VISIBLE: set_scroll_visible((void *)(long)keys[i].arg.i); break;
            case INCR_SCROLL_VISIBLE: set_scroll_visible((void *)(long)1); break;
            case DECR_SCROLL_VISIBLE: set_scroll_visible((void *)(long)-1); break;
            }
            break;
        }
    }
}

static void
handle_button_press(XButtonEvent *e)
{
    Workspace *ws = curws();
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            refocus(ws, &ws->wins[i]);
            break;
        }
    }
}

/* ---- monitor init ---- */

static void
monitors_init(void)
{
    /* try Xinerama first */
#ifdef __OpenBSD__
    /* OpenBSD doesn't have XineramaQueryScreens reliably */
    nmons = 1;
    mons[0].id = 0;
    mons[0].x = 0;
    mons[0].y = 0;
    mons[0].width = scrw;
    mons[0].height = scrh;
    mons[0].current_workspace = 0;
    mons[0].master_factor = 0.5f;
    mons[0].horizontal_mode = 1;
    mons[0].scroll_windows_visible = SCROLL_WINDOWS_VISIBLE;
#else
    if (USE_XINERAMA) {
        int nscreens;
        XineramaScreenInfo *screens;

        screens = XineramaQueryScreens(dpy, &nscreens);
        if (screens && nscreens > 0) {
            nmons = nscreens;
            if (nmons > 8) nmons = 8;
            for (int i = 0; i < nmons; i++) {
                mons[i].id = i;
                mons[i].x = screens[i].x_org;
                mons[i].y = screens[i].y_org;
                mons[i].width = screens[i].width;
                mons[i].height = screens[i].height;
                mons[i].current_workspace = i % NUM_WORKSPACES;
                mons[i].master_factor = 0.5f;
                mons[i].horizontal_mode = 1;
                mons[i].scroll_windows_visible = SCROLL_WINDOWS_VISIBLE;
            }
            XFree(screens);
            return;
        }
        if (screens) XFree(screens);
    }
    /* fallback: single monitor */
    nmons = 1;
    mons[0].id = 0;
    mons[0].x = 0;
    mons[0].y = 0;
    mons[0].width = scrw;
    mons[0].height = scrh;
    mons[0].current_workspace = 0;
    mons[0].master_factor = 0.5f;
    mons[0].horizontal_mode = 1;
    mons[0].scroll_windows_visible = SCROLL_WINDOWS_VISIBLE;
#endif
}

/* ---- init / run / cleanup ---- */

static void
cache_atoms(void)
{
    atom_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    atom_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    atom_net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_net_wm_state_full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    atom_net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    atom_net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    atom_net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
}

static void
init(void)
{
    XSetErrorHandler(xerror);

    dpy = XOpenDisplay(NULL);
    if (!dpy) errx(1, "cannot open display");
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    scrw = DisplayWidth(dpy, screen);
    scrh = DisplayHeight(dpy, screen) - BAR_HEIGHT;

    cache_atoms();

    /* init workspaces */
    for (int i = 0; i < NUM_WORKSPACES; i++) {
        spaces[i].wins = NULL;
        spaces[i].nwin = 0;
        spaces[i].cap = 0;
        spaces[i].focused = NULL;
        spaces[i].scroll_offset = 0;
        spaces[i].tiled = NULL;
        spaces[i].ntiled = 0;
        spaces[i].tiled_cap = 0;
    }

    monitors_init();
    setup_ewmh();
    grab_keys();

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask
                            | KeyPressMask | ButtonPressMask);

    signal(SIGCHLD, SIG_IGN);
}

static void
run(void)
{
    XEvent ev;

    while (running && !XNextEvent(dpy, &ev)) {
        switch (ev.type) {
        case MapRequest:       handle_map_request(&ev.xmaprequest); break;
        case DestroyNotify:    handle_destroy_notify(&ev.xdestroywindow); break;
        case UnmapNotify:      handle_unmap_notify(&ev.xunmap); break;
        case ConfigureRequest: handle_configure_request(&ev.xconfigurerequest); break;
        case EnterNotify:      handle_enter_notify(&ev.xcrossing); break;
        case KeyPress:         handle_key_press(&ev.xkey); break;
        case ButtonPress:      handle_button_press(&ev.xbutton); break;
        default: break;
        }
        flush_retile();
    }
}

static void
cleanup(void)
{
    int i;
    for (i = 0; i < NUM_WORKSPACES; i++) {
        free(spaces[i].wins);
        free(spaces[i].tiled);
    }
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XCloseDisplay(dpy);
}

/* ---- main ---- */

int
main(void)
{
    init();
    run();
    cleanup();
    return 0;
}
