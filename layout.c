/* layout.c — Tiling layout engine.
   Computes usable geometry (accounting for bars/struts), arranges tiled
   windows in horizontal-scroll or master-stack mode, and manages the
   camera (scroll offset) for the infinite-canvas horizontal layout. */

#include "dswm.h"
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

/* globals owned by this file */
int retile_pending = 0;
int center_focused = CENTER_FOCUSED_DEFAULT;

/* ---- workspace helpers ---- */

Workspace *
curws(void)
{
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

/* Read _NET_WM_STRUT from every window on the current workspace.
   Struts are reserved screen edges (e.g. panel bars) that tiled windows
   must avoid.  Results are cached in mon->strut_* and invalidated on
   window add/remove via strut_valid. */
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

/* Compute the rectangular region of the monitor that is usable for tiling,
   after subtracting the built-in bar height and external struts. */
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
update_camera(void)
{
    Workspace *ws = curws();
    Monitor *mon = curmon();
    int i;
    int usable_w, cam_x = 0, centered = 0;

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
                if (col_left < cam_x)
                    cam_x = col_left;
                else if (col_right > cam_x + usable_w)
                    cam_x = col_right - usable_w;
                break;
            }
        }
    } else {
        cam_x = ws->cam_x;
    }

    if (!centered) {
        if (total_w <= usable_w)
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
tile_horizontal(void)
{
    Workspace *ws = curws();
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

    int total_w = cur_x - x_start;
    int centered = 0;
    if (center_focused && ws->focused) {
        for (i = 0; i < ws->ntiled; i++) {
            if (ws->tiled[i] == ws->focused) {
                int outer = ws->tiled[i]->width + 2 * BORDER_WIDTH;
                int cam_x = ws->tiled[i]->x - x_start - (usable_w - outer) / 2;
                ws->cam_x = cam_x;
                centered = 1;
                break;
            }
        }
    } else if (ws->focused) {
        int cam_x = ws->cam_x;
        for (i = 0; i < ws->ntiled; i++) {
            if (ws->tiled[i] == ws->focused) {
                int col = ws->tiled[i]->width + 2 * GAP_OUTER + 2 * BORDER_WIDTH;
                int col_left = ws->tiled[i]->x - x_start - GAP_OUTER;
                int col_right = col_left + col;
                if (col_left < cam_x)
                    cam_x = col_left;
                else if (col_right > cam_x + usable_w)
                    cam_x = col_right - usable_w;
                break;
            }
        }
        ws->cam_x = cam_x;
    }

    if (!centered) {
        int cam_x = ws->cam_x;
        if (total_w <= usable_w)
            cam_x = 0;
        else {
            if (cam_x < 0) cam_x = 0;
            if (cam_x > total_w - usable_w) cam_x = total_w - usable_w;
        }
        ws->cam_x = cam_x;
    }

    update_camera();
}

/* ---- tiling: master-stack layout ---- */
/* Single-window fills the monitor.  Two or more: the first window
   (master) occupies a fraction (master_factor) of the width on the left;
   the remaining windows (stack) are stacked vertically on the right. */

void
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

/* ---- resize ---- */

void
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

void
resize_window(void *arg)
{
    Workspace *ws = curws();
    ManagedWindow *w = ws->focused;
    int dir = (int)(long)arg;
    if (!w) return;

    w->width_factor += dir * RESIZE_FACTOR_STEP;
    if (w->width_factor < MIN_WIDTH_FACTOR) w->width_factor = MIN_WIDTH_FACTOR;
    if (w->width_factor > MAX_WIDTH_FACTOR) w->width_factor = MAX_WIDTH_FACTOR;
    w->is_fit = 0;

    if (w->is_floating || w->is_fullscreen) {
        int new_w = (int)(w->width * (1.0f + dir * RESIZE_FACTOR_STEP));
        int new_h = (int)(w->height * (1.0f + dir * RESIZE_FACTOR_STEP));
        if (new_w < MIN_WIN_DIM) new_w = MIN_WIN_DIM;
        if (new_h < MIN_WIN_DIM) new_h = MIN_WIN_DIM;
        if (new_w > scrw - 2 * GAP_OUTER) new_w = scrw - 2 * GAP_OUTER;
        if (new_h > scrh - 2 * GAP_OUTER) new_h = scrh - 2 * GAP_OUTER;
        w->width = new_w;
        w->height = new_h;
        w->x = scrw / 2 - new_w / 2;
        w->y = scrh / 2 - new_h / 2;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);
    } else {
        tile_horizontal();
    }
}

void
fit_window(void)
{
    Workspace *ws = curws();
    ManagedWindow *w = ws->focused;
    if (!w) return;

    if (w->is_floating || w->is_fullscreen) {
        toggle_fullscreen();
        return;
    }

    if (!w->is_fit) {
        w->saved_factor = w->width_factor;
        w->width_factor = (float)COLUMN_DIVISOR;
        if (w->width_factor > MAX_WIDTH_FACTOR) w->width_factor = MAX_WIDTH_FACTOR;
        w->is_fit = 1;
    } else {
        w->width_factor = w->saved_factor;
        w->is_fit = 0;
    }
    tile_horizontal();
}

/* ---- retile ---- */
/* Deferred retile: multiple state changes within one event-loop iteration
   collapse into a single retile.  retile_deferred() sets a flag;
   flush_retile() (called once per event) performs the actual layout. */

void
retile(void)
{
    Monitor *mon = curmon();
    if (mon->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
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
        retile();
    }
}

/* ---- layout toggles ---- */

void
toggle_center_focus(void)
{
    center_focused = !center_focused;
    retile();
}

void
toggle_layout(void)
{
    Monitor *mon = curmon();

    mon->horizontal_mode = !mon->horizontal_mode;

    if (mon->horizontal_mode) {
        mon->master_factor = 1.0f;
        tile_horizontal();
    } else {
        mon->master_factor = 0.5f;
        tile_windows();
    }
}
