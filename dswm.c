#include "dswm.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
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
    int is_focused;
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
};

typedef struct Workspace Workspace;
struct Workspace {
    ManagedWindow *wins;
    int nwin;
    int cap;
    ManagedWindow *focused;
    int scroll_offset;
    int scroll_maximized;
};

/* globals */
static Display *dpy;
static Window root;
static int screen;
static int scrw, scrh;
static int running = 1;
static int cur_ws;

static Monitor mons[8];
static int nmons;
static Workspace spaces[NUM_WORKSPACES];

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

/* ---- tiling: collect tiled windows from current workspace ---- */

int
collect_tiled(const Workspace *ws, ManagedWindow **out, int maxout, int monid)
{
    int i, n = 0;
    for (i = 0; i < ws->nwin && n < maxout; i++)
        if (!ws->wins[i].is_floating && !ws->wins[i].is_fullscreen
            && ws->wins[i].monitor == monid)
            out[n++] = &ws->wins[i];
    return n;
}

/* ---- bar strut support ---- */

void
compute_struts(const Monitor *mon, int *strut_top, int *strut_bottom,
               int *strut_left, int *strut_right)
{
    Workspace *ws = curws();
    Atom net_wm_strut, actual;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    int i;

    net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);

    *strut_top = 0;
    *strut_bottom = 0;
    *strut_left = 0;
    *strut_right = 0;

    for (i = 0; i < ws->nwin; i++) {
        if (XGetWindowProperty(dpy, ws->wins[i].window, net_wm_strut,
                               0, 4, False, XA_CARDINAL, &actual, &format,
                               &nitems, &bytes_after, &data) == Success
            && data && nitems >= 4) {
            long *strut = (long *)data;
            /* strut format: left, right, top, bottom */
            if (strut[0] > 0 && ws->wins[i].x < mon->x + mon->width)
                if (strut[0] > *strut_left) *strut_left = strut[0];
            if (strut[1] > 0 && ws->wins[i].x + ws->wins[i].width > mon->x)
                if (strut[1] > *strut_right) *strut_right = strut[1];
            if (strut[2] > 0 && ws->wins[i].y < mon->y + mon->height)
                if (strut[2] > *strut_top) *strut_top = strut[2];
            if (strut[3] > 0 && ws->wins[i].y + ws->wins[i].height > mon->y)
                if (strut[3] > *strut_bottom) *strut_bottom = strut[3];
            XFree(data);
            data = NULL;
        }
    }
}

/* ---- tiling: horizontal scroll layout ---- */

static void
tile_horizontal(void)
{
    Workspace *ws = curws();
    ManagedWindow *tiled[256];
    int ntiled, i, k;
    int bar_h, own_bar_top, own_bar_bottom;
    int strut_top, strut_bottom, strut_left, strut_right;
    int usable_h, usable_w, x_start, y_start;
    int scroll_vis, base_ww, ww;
    int all_fit;
    int x_pos, y_pos, win_w, win_h;

    for (k = 0; k < nmons; k++) {
        Monitor *mon = &mons[k];
        ntiled = collect_tiled(ws, tiled, 256, mon->id);
        if (ntiled == 0) continue;

        /* external bar strut (polybar etc.) */
        bar_h = BAR_HEIGHT;
        own_bar_top = 0;
        own_bar_bottom = 0;
        if (BAR_POSITION == 0)
            own_bar_top = bar_h;
        else
            own_bar_bottom = bar_h;

        compute_struts(mon, &strut_top, &strut_bottom, &strut_left, &strut_right);

        usable_h = mon->height - MAX(own_bar_top, strut_top)
                           - MAX(own_bar_bottom, strut_bottom);
        usable_w = mon->width - strut_left - strut_right;
        x_start  = mon->x + strut_left;
        y_start  = mon->y + MAX(own_bar_top, strut_top);

        if (usable_h < 10) usable_h = mon->height;
        if (usable_w < 10) usable_w = mon->width;

        scroll_vis = mon->scroll_windows_visible;
        if (scroll_vis < 1) scroll_vis = 1;

        base_ww = usable_w / scroll_vis;
        ww = ws->scroll_maximized
             ? usable_w
             : (int)(base_ww * mon->master_factor);
        if (ww < 200) ww = 200;
        if (ww > usable_w) ww = usable_w;

        all_fit = (ntiled <= scroll_vis);
        if (all_fit && !ws->scroll_maximized) {
            ws->scroll_offset = 0;
            ww = (int)(base_ww * mon->master_factor);
        }

        for (i = 0; i < ntiled; i++) {
            int scroll_off = all_fit ? 0 : ws->scroll_offset;
            x_pos = x_start + i * ww - scroll_off + GAP_OUTER;
            y_pos = y_start + GAP_OUTER;
            win_w = ww - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
            win_h = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
            if (win_w < 1) win_w = 1;
            if (win_h < 1) win_h = 1;

            tiled[i]->x = x_pos;
            tiled[i]->y = y_pos;
            tiled[i]->width = win_w;
            tiled[i]->height = win_h;

            XMoveResizeWindow(dpy, tiled[i]->window,
                              tiled[i]->x, tiled[i]->y,
                              tiled[i]->width, tiled[i]->height);
        }
    }

    XFlush(dpy);
}

/* ---- tiling: master-stack layout ---- */

static void
tile_windows(void)
{
    Workspace *ws = curws();
    ManagedWindow *tiled[256];
    int ntiled, i, k;
    int bar_h, own_bar_top, own_bar_bottom;
    int strut_top, strut_bottom, strut_left, strut_right;
    int usable_h, usable_w, x_start, y_start;
    int master_w, stack_x, stack_w, stack_h;

    for (k = 0; k < nmons; k++) {
        Monitor *mon = &mons[k];
        ntiled = collect_tiled(ws, tiled, 256, mon->id);
        if (ntiled == 0) continue;

        bar_h = BAR_HEIGHT;
        own_bar_top = 0;
        own_bar_bottom = 0;
        if (BAR_POSITION == 0)
            own_bar_top = bar_h;
        else
            own_bar_bottom = bar_h;

        compute_struts(mon, &strut_top, &strut_bottom, &strut_left, &strut_right);

        usable_h = mon->height - MAX(own_bar_top, strut_top)
                           - MAX(own_bar_bottom, strut_bottom);
        usable_w = mon->width - strut_left - strut_right;
        x_start  = mon->x + strut_left;
        y_start  = mon->y + MAX(own_bar_top, strut_top);

        if (usable_h < 10) usable_h = mon->height;
        if (usable_w < 10) usable_w = mon->width;

        if (ntiled == 1) {
            /* single window: fill entire space */
            tiled[0]->x = x_start + GAP_OUTER;
            tiled[0]->y = y_start + GAP_OUTER;
            tiled[0]->width = usable_w - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
            tiled[0]->height = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
            if (tiled[0]->width < 1) tiled[0]->width = 1;
            if (tiled[0]->height < 1) tiled[0]->height = 1;

            XMoveResizeWindow(dpy, tiled[0]->window,
                              tiled[0]->x, tiled[0]->y,
                              tiled[0]->width, tiled[0]->height);
        } else {
            /* master + stack */
            master_w = (int)(usable_w * mon->master_factor)
                       - GAP_OUTER - GAP_OUTER / 2 - 2 * BORDER_WIDTH;
            stack_x  = x_start + (int)(usable_w * mon->master_factor) + GAP_OUTER / 2;
            stack_w  = usable_w - (int)(usable_w * mon->master_factor)
                       - GAP_OUTER - GAP_OUTER / 2 - 2 * BORDER_WIDTH;
            if (master_w < 1) master_w = 1;
            if (stack_w < 1) stack_w = 1;

            stack_h = (usable_h - GAP_OUTER * ntiled) / (ntiled - 1)
                      - 2 * BORDER_WIDTH;
            if (stack_h < 1) stack_h = 1;

            /* master window */
            tiled[0]->x = x_start + GAP_OUTER;
            tiled[0]->y = y_start + GAP_OUTER;
            tiled[0]->width = master_w;
            tiled[0]->height = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
            if (tiled[0]->height < 1) tiled[0]->height = 1;

            /* stack windows */
            for (i = 1; i < ntiled; i++) {
                tiled[i]->x = stack_x;
                tiled[i]->y = y_start + GAP_OUTER
                              + (i - 1) * (stack_h + GAP_OUTER + 2 * BORDER_WIDTH);
                tiled[i]->width = stack_w;
                tiled[i]->height = stack_h;
            }

            /* apply all */
            for (i = 0; i < ntiled; i++) {
                XMoveResizeWindow(dpy, tiled[i]->window,
                                  tiled[i]->x, tiled[i]->y,
                                  tiled[i]->width, tiled[i]->height);
            }
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

    if (mon->horizontal_mode) {
        if (ws->nwin == 0) return;

        int scroll_vis = mon->scroll_windows_visible;
        if (scroll_vis < 1) scroll_vis = 1;

        delta_f = (float)delta / (mon->width / scroll_vis);
        mon->master_factor += delta_f;
        if (mon->master_factor < 0.3f) mon->master_factor = 0.3f;
        if (mon->master_factor > 3.0f) mon->master_factor = 3.0f;

        tile_horizontal();
    } else {
        if (ws->nwin < 2) return;

        delta_f = (float)delta / mon->width;
        mon->master_factor += delta_f;
        if (mon->master_factor < 0.1f) mon->master_factor = 0.1f;
        if (mon->master_factor > 0.9f) mon->master_factor = 0.9f;

        tile_windows();
    }
}

/* ---- tiling: monitor focus ---- */

static void
focus_monitor(void *arg)
{
    int mon_idx = *(int *)arg;
    if (mon_idx < 0 || mon_idx >= nmons) return;
    cur_ws = mons[mon_idx].current_workspace;
    if (curmon()->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
}

/* ---- tiling: set scroll_visible ---- */

static void
set_scroll_visible(void *arg)
{
    int val = *(int *)arg;
    Monitor *mon = curmon();
    Workspace *ws = curws();
    if (val < 1) val = 1;
    if (val > 10) val = 10;
    mon->scroll_windows_visible = val;
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
    int scroll_vis, base_ww, ww, total_w, max_scroll;
    int scroll_amount;

    if (!mon->horizontal_mode) return;
    if (ws->nwin == 0) return;

    scroll_vis = mon->scroll_windows_visible;
    if (scroll_vis < 1) scroll_vis = 1;

    base_ww = mon->width / scroll_vis;
    ww = ws->scroll_maximized
         ? mon->width
         : (int)(base_ww * mon->master_factor);

    if (forward) {
        total_w = ws->nwin * ww;
        max_scroll = total_w - mon->width;
        if (max_scroll < 0) max_scroll = 0;
        scroll_amount = (int)(base_ww * mon->master_factor);
        ws->scroll_offset += scroll_amount;
        if (ws->scroll_offset > max_scroll)
            ws->scroll_offset = max_scroll;
    } else {
        scroll_amount = (int)(base_ww * mon->master_factor);
        ws->scroll_offset -= scroll_amount;
        if (ws->scroll_offset < 0)
            ws->scroll_offset = 0;
    }

    tile_horizontal();
}

static void
scroll_left(void)
{
    move_horizontal(0);
}

static void
scroll_right(void)
{
    move_horizontal(1);
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

void
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

    if (curmon()->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();

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

/* ---- tiling: increment/decrement scroll_visible ---- */

static void
increment_scroll_visible(void)
{
    Monitor *mon = curmon();
    Workspace *ws = curws();

    mon->scroll_windows_visible++;
    if (mon->scroll_windows_visible > 10) mon->scroll_windows_visible = 10;
    ws->scroll_offset = 0;

    if (mon->horizontal_mode)
        tile_horizontal();
}

static void
decrement_scroll_visible(void)
{
    Monitor *mon = curmon();
    Workspace *ws = curws();

    mon->scroll_windows_visible--;
    if (mon->scroll_windows_visible < 1) mon->scroll_windows_visible = 1;
    ws->scroll_offset = 0;

    if (mon->horizontal_mode)
        tile_horizontal();
}

/* ---- workspace management ---- */

static void
switch_workspace(void *arg)
{
    int idx = (int)(long)arg;
    if (idx < 0 || idx >= NUM_WORKSPACES) return;
    cur_ws = idx;
    if (curmon()->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
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

    ws->focused = NULL;
    if (curmon()->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
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
    mw.is_floating = 0;
    mw.is_fullscreen = 0;

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
    ws->focused = &ws->wins[ws->nwin - 1];

    XSelectInput(dpy, w, EnterWindowMask | StructureNotifyMask);
    XMapWindow(dpy, w);

    if (curmon()->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
}

static void
unmanage_window(Window w)
{
    Workspace *ws = curws();
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == w) {
            memmove(&ws->wins[i], &ws->wins[i + 1],
                    (ws->nwin - i - 1) * sizeof(ManagedWindow));
            ws->nwin--;
            break;
        }
    }

    ws->focused = (ws->nwin > 0) ? &ws->wins[ws->nwin - 1] : NULL;

    if (curmon()->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
}

/* ---- focus ---- */

static void
focus_next(void)
{
    Workspace *ws = curws();
    int i, cur = -1;

    if (ws->nwin == 0) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->focused && ws->wins[i].window == ws->focused->window) {
            cur = i;
            break;
        }
    }

    i = (cur + 1) % ws->nwin;
    ws->focused = &ws->wins[i];
    XSetInputFocus(dpy, ws->focused->window, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, ws->focused->window);
}

static void
focus_prev(void)
{
    Workspace *ws = curws();
    int i, cur = -1;

    if (ws->nwin == 0) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->focused && ws->wins[i].window == ws->focused->window) {
            cur = i;
            break;
        }
    }

    i = (cur - 1 + ws->nwin) % ws->nwin;
    ws->focused = &ws->wins[i];
    XSetInputFocus(dpy, ws->focused->window, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, ws->focused->window);
}

/* ---- close/quit ---- */

static void
close_window(void)
{
    Workspace *ws = curws();
    XEvent ev;
    Atom wm_delete;

    if (!ws->focused) return;

    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = ws->focused->window;
    ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
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
toggle_gap(void)
{
    /* GAP_OUTER/GAP_INNER are compile-time; toggle needs runtime state.
       For now this is a no-op placeholder. A runtime flag would be needed
       to make gaps toggleable without recompiling. */
}

static void
toggle_fullscreen(void)
{
    Workspace *ws = curws();
    ManagedWindow *w = ws->focused;
    Atom net_wm_state, net_wm_state_full;

    if (!w) return;

    w->is_fullscreen = !w->is_fullscreen;

    net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_wm_state_full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

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
        XMoveResizeWindow(dpy, w->window, 0, 0, scrw, scrh);
        XRaiseWindow(dpy, w->window);

        /* set EWMH state */
        XChangeProperty(dpy, w->window, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&net_wm_state_full, 1);
    } else {
        /* restore */
        w->is_floating = w->pre_fs_floating;
        w->x = w->pre_fs_x;
        w->y = w->pre_fs_y;
        w->width = w->pre_fs_width;
        w->height = w->pre_fs_height;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);

        /* clear EWMH state */
        XChangeProperty(dpy, w->window, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)0, 0);

        if (curmon()->horizontal_mode)
            tile_horizontal();
        else
            tile_windows();
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
    if (w->is_floating) {
        /* center on screen */
        w->x = scrw / 2 - w->width / 2;
        w->y = scrh / 2 - w->height / 2;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);
        XRaiseWindow(dpy, w->window);
    } else {
        if (curmon()->horizontal_mode)
            tile_horizontal();
        else
            tile_windows();
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

void
setup_ewmh(void)
{
    Atom net_supported, net_number_of_desktops, net_current_desktop,
         net_active_window, net_wm_name;

    net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);

    XChangeProperty(dpy, root, net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char[]){
                        net_number_of_desktops,
                        net_current_desktop,
                        net_active_window,
                        net_wm_name,
                    }, 4);

    long ndesk = NUM_WORKSPACES;
    XChangeProperty(dpy, root, net_number_of_desktops, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&ndesk, 1);

    long cdesk = cur_ws;
    XChangeProperty(dpy, root, net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&cdesk, 1);
}

/* ---- key grabbing ---- */

void
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

int
xerror(Display *d, XErrorEvent *ee)
{
    (void)d;
    (void)ee;
    return 0;
}

/* ---- event handlers ---- */

void
handle_map_request(XMapRequestEvent *e)
{
    manage_window(e->window);
}

void
handle_destroy_notify(XDestroyWindowEvent *e)
{
    unmanage_window(e->window);
}

void
handle_unmap_notify(XUnmapEvent *e)
{
    unmanage_window(e->window);
}

void
handle_configure_request(XConfigureRequestEvent *e)
{
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

void
handle_enter_notify(XCrossingEvent *e)
{
    Workspace *ws = curws();
    int i;

    if (e->mode != NotifyNormal || e->detail == NotifyInferior) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            ws->focused = &ws->wins[i];
            XSetInputFocus(dpy, e->window, RevertToPointerRoot, CurrentTime);
            XRaiseWindow(dpy, e->window);
            break;
        }
    }
}

void
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
            case SCROLL_LEFT:        scroll_left(); break;
            case SCROLL_RIGHT:       scroll_right(); break;
            case TOGGLE_LAYOUT:      toggle_layout(); break;
            case TOGGLE_GAP:         toggle_gap(); break;
            case TOGGLE_FULLSCREEN:  toggle_fullscreen(); break;
            case TOGGLE_FLOAT:       toggle_float(); break;
            case SWITCH_WORKSPACE:   switch_workspace((void *)(long)keys[i].arg.i); break;
            case MOVE_TO_WORKSPACE:  move_to_workspace((void *)(long)keys[i].arg.i); break;
            case FOCUS_MONITOR:      focus_monitor(keys[i].arg.v); break;
            case SET_SCROLL_VISIBLE: set_scroll_visible(keys[i].arg.v); break;
            case INCR_SCROLL_VISIBLE: increment_scroll_visible(); break;
            case DECR_SCROLL_VISIBLE: decrement_scroll_visible(); break;
            }
            break;
        }
    }
}

void
handle_button_press(XButtonEvent *e)
{
    Workspace *ws = curws();
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            ws->focused = &ws->wins[i];
            XSetInputFocus(dpy, e->window, RevertToPointerRoot, CurrentTime);
            XRaiseWindow(dpy, e->window);
            break;
        }
    }
}

/* ---- monitor init ---- */

void
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
    mons[0].horizontal_mode = 0;
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
                mons[i].horizontal_mode = 0;
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
    mons[0].horizontal_mode = 0;
    mons[0].scroll_windows_visible = SCROLL_WINDOWS_VISIBLE;
#endif
}

/* ---- init / run / cleanup ---- */

void
init(void)
{
    XSetErrorHandler(xerror);

    dpy = XOpenDisplay(NULL);
    if (!dpy) errx(1, "cannot open display");
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    scrw = DisplayWidth(dpy, screen);
    scrh = DisplayHeight(dpy, screen) - BAR_HEIGHT;

    /* init workspaces */
    for (int i = 0; i < NUM_WORKSPACES; i++) {
        spaces[i].wins = NULL;
        spaces[i].nwin = 0;
        spaces[i].cap = 0;
        spaces[i].focused = NULL;
        spaces[i].scroll_offset = 0;
        spaces[i].scroll_maximized = 0;
    }

    monitors_init();
    setup_ewmh();
    grab_keys();

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask
                            | KeyPressMask | ButtonPressMask);

    signal(SIGCHLD, SIG_IGN);
}

void
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
    }
}

void
cleanup(void)
{
    int i;
    for (i = 0; i < NUM_WORKSPACES; i++)
        free(spaces[i].wins);
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
