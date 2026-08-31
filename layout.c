#include "dswm.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

/* collect tiled windows from a workspace */

static int
collect_tiled(const Workspace *ws, ManagedWindow **out, int maxout, int monid)
{
    int i, n = 0;
    for (i = 0; i < ws->nwin && n < maxout; i++)
        if (!ws->wins[i].is_floating && !ws->wins[i].is_fullscreen
            && ws->wins[i].monitor == monid)
            out[n++] = &ws->wins[i];
    return n;
}

/* bar strut support */

static void
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

/* compute usable area (shared by all layouts) */

static void
compute_usable_area(const Monitor *mon, int *usable_w, int *usable_h,
                    int *x_start, int *y_start)
{
    int own_bar_top = 0, own_bar_bottom = 0;
    int strut_top, strut_bottom, strut_left, strut_right;

    if (BAR_POSITION == 0)
        own_bar_top = BAR_HEIGHT;
    else
        own_bar_bottom = BAR_HEIGHT;

    compute_struts(mon, &strut_top, &strut_bottom, &strut_left, &strut_right);

    *usable_h = mon->height - MAX(own_bar_top, strut_top)
                       - MAX(own_bar_bottom, strut_bottom);
    *usable_w = mon->width - strut_left - strut_right;
    *x_start  = mon->x + strut_left;
    *y_start  = mon->y + MAX(own_bar_top, strut_top);

    if (*usable_h < MIN_WIN_DIM) *usable_h = mon->height;
    if (*usable_w < MIN_WIN_DIM) *usable_w = mon->width;
}

/* horizontal scroll layout */

static void
tile_horizontal(void)
{
    Workspace *ws = curws();
    ManagedWindow *tiled[MAX_TILED];
    int ntiled, i, k;
    int usable_h, usable_w, x_start, y_start;
    int scroll_vis, base_ww, ww;
    int all_fit;
    int x_pos, y_pos, win_w, win_h;

    for (k = 0; k < nmons; k++) {
        Monitor *mon = &mons[k];
        ntiled = collect_tiled(ws, tiled, MAX_TILED, mon->id);
        if (ntiled == 0) continue;

        compute_usable_area(mon, &usable_w, &usable_h, &x_start, &y_start);

        scroll_vis = mon->scroll_windows_visible;
        if (scroll_vis < MIN_SCROLL_VIS) scroll_vis = MIN_SCROLL_VIS;

        base_ww = usable_w / scroll_vis;
        ww = (int)(base_ww * mon->master_factor);
        if (ww < MIN_WIN_W) ww = MIN_WIN_W;
        if (ww > usable_w) ww = usable_w;

        all_fit = (ntiled <= scroll_vis);
        if (all_fit) {
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

/* master-stack layout */

static void
tile_master_stack(void)
{
    Workspace *ws = curws();
    ManagedWindow *tiled[MAX_TILED];
    int ntiled, i, k;
    int usable_h, usable_w, x_start, y_start;
    int master_w, stack_x, stack_w, stack_h;

    for (k = 0; k < nmons; k++) {
        Monitor *mon = &mons[k];
        ntiled = collect_tiled(ws, tiled, MAX_TILED, mon->id);
        if (ntiled == 0) continue;

        compute_usable_area(mon, &usable_w, &usable_h, &x_start, &y_start);

        if (ntiled == 1) {
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

            tiled[0]->x = x_start + GAP_OUTER;
            tiled[0]->y = y_start + GAP_OUTER;
            tiled[0]->width = master_w;
            tiled[0]->height = usable_h - 2 * GAP_OUTER - 2 * BORDER_WIDTH;
            if (tiled[0]->height < 1) tiled[0]->height = 1;

            for (i = 1; i < ntiled; i++) {
                tiled[i]->x = stack_x;
                tiled[i]->y = y_start + GAP_OUTER
                              + (i - 1) * (stack_h + GAP_OUTER + 2 * BORDER_WIDTH);
                tiled[i]->width = stack_w;
                tiled[i]->height = stack_h;
            }

            for (i = 0; i < ntiled; i++) {
                XMoveResizeWindow(dpy, tiled[i]->window,
                                  tiled[i]->x, tiled[i]->y,
                                  tiled[i]->width, tiled[i]->height);
            }
        }
    }

    XFlush(dpy);
}

/* tile current workspace */

void
tile(void)
{
    Monitor *mon = curmon();
    if (mon->horizontal_mode)
        tile_horizontal();
    else
        tile_master_stack();
}

/* resize master factor */

void
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
        if (mon->master_factor < MIN_MASTER_HORIZ) mon->master_factor = MIN_MASTER_HORIZ;
        if (mon->master_factor > MAX_MASTER_HORIZ) mon->master_factor = MAX_MASTER_HORIZ;

        tile_horizontal();
    } else {
        if (ws->nwin < 2) return;

        delta_f = (float)delta / mon->width;
        mon->master_factor += delta_f;
        if (mon->master_factor < MIN_MASTER_VERT) mon->master_factor = MIN_MASTER_VERT;
        if (mon->master_factor > MAX_MASTER_VERT) mon->master_factor = MAX_MASTER_VERT;

        tile_master_stack();
    }
}

/* scroll left/right */

void
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
    ww = (int)(base_ww * mon->master_factor);

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

/* toggle layout mode */

void
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
        tile_master_stack();
    }
}

/* swap windows */

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

    tmp = ws->wins[cur_idx];
    ws->wins[cur_idx] = ws->wins[swap_idx];
    ws->wins[swap_idx] = tmp;

    tile();

    ws->focused = &ws->wins[swap_idx];
}

void
swap_next(void)
{
    swap_impl(1);
}

void
swap_prev(void)
{
    swap_impl(-1);
}

/* adjust scroll_visible */

void
adjust_scroll_visible(int delta)
{
    Monitor *mon = curmon();
    Workspace *ws = curws();

    mon->scroll_windows_visible += delta;
    if (mon->scroll_windows_visible < MIN_SCROLL_VIS)
        mon->scroll_windows_visible = MIN_SCROLL_VIS;
    if (mon->scroll_windows_visible > MAX_SCROLL_VIS)
        mon->scroll_windows_visible = MAX_SCROLL_VIS;
    ws->scroll_offset = 0;

    if (mon->horizontal_mode)
        tile_horizontal();
}
