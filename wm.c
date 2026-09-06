/* wm.c — Window management: map/unmap lifecycle, focus cycling,
   workspace switching, swap/resize, fullscreen/float toggles, and
   X event handlers (MapRequest, DestroyNotify, UnmapNotify, etc.). */

#include "dswm.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <err.h>
#include <limits.h>

#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

/* ---- workspace window list helpers ---- */

static int
wins_ensure_cap(Workspace *ws)
{
    if (ws->nwin >= ws->cap) {
        int newcap = ws->cap ? ws->cap * 2 : INITIAL_CAP;
        ManagedWindow *tmp = realloc(ws->wins, newcap * sizeof(ManagedWindow));
        if (!tmp) return 0;
        ws->wins = tmp;
        ws->cap = newcap;
    }
    if (ws->nwin < ws->cap / 4 && ws->cap > INITIAL_CAP) {
        int newcap = ws->cap / 2;
        if (newcap < INITIAL_CAP) newcap = INITIAL_CAP;
        ManagedWindow *tmp = realloc(ws->wins, newcap * sizeof(ManagedWindow));
        if (!tmp) return 0;
        ws->wins = tmp;
        ws->cap = newcap;
    }
    return 1;
}

static void retile_scratchpad(void);

/* ---- focus ---- */

void
update_border(Window w, int focused)
{
    XSetWindowBorder(dpy, w, focused ? FOCUS_COLOR : BORDER_COLOR);
}

void
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

void
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

/* ---- scroll (camera) ---- */

void
move_horizontal(int forward)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    int idx = -1;
    int i;

    if (!mon->horizontal_mode) return;
    if (ws->ntiled == 0) return;

    for (i = 0; i < ws->ntiled; i++) {
        if (ws->tiled[i] == ws->focused) {
            idx = i;
            break;
        }
    }
    if (idx == -1) idx = 0;

    if (forward) {
        if (idx + 1 >= ws->ntiled) return;
        refocus(ws, ws->tiled[idx + 1]);
    } else {
        if (idx - 1 < 0) return;
        refocus(ws, ws->tiled[idx - 1]);
    }

    update_camera();
}

/* ---- swap ---- */

/* Swap the focused window with its neighbour in the workspace window list.
   A struct copy (tmp) is needed because the two wins[] entries overlap
   in memory and an in-place swap would corrupt data. */
void
swap_impl(int delta)
{
    Workspace *ws = active_ws();
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

    swap_idx = cur_idx + delta;
    if (swap_idx < 0 || swap_idx >= ws->nwin) return;

    tmp = ws->wins[cur_idx];
    ws->wins[cur_idx] = ws->wins[swap_idx];
    ws->wins[swap_idx] = tmp;

    retile_deferred();

    ws->focused = &ws->wins[swap_idx];
    refocus(ws, ws->focused);
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

/* ---- workspace management ---- */

void
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

    /* Keep scratchpad overlay raised above the newly mapped workspace */
    if (visible && scratch_visible) {
        Monitor *mon = curmon();
        Workspace *sp = &spaces[SCRATCHPAD_IDX];
        /* Dim goes above workspace, below scratchpad windows */
        if (mon->dim_win)
            XRaiseWindow(dpy, mon->dim_win);
        for (i = 0; i < sp->nwin; i++)
            XRaiseWindow(dpy, sp->wins[i].window);
    }
}

void
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

void
move_to_workspace(void *arg)
{
    int idx = (int)(long)arg;
    Workspace *ws = curws();
    ManagedWindow win;
    int i, found = 0;

    if (idx < 0 || idx >= NUM_WORKSPACES) return;
    if (idx == cur_ws) return;
    if (!ws->focused) return;

    /* Copy the window to the stack — memmove below invalidates the pointer
       that ws->focused points into, so we need a local snapshot. */
    win = *ws->focused;

    int removed = -1;
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == win.window) {
            removed = i;
            memmove(&ws->wins[i], &ws->wins[i + 1],
                    (ws->nwin - i - 1) * sizeof(ManagedWindow));
            ws->nwin--;
            found = 1;
            break;
        }
    }
    if (!found) return;

    rebuild_tiled(ws);

    Workspace *target = &spaces[idx];
    if (!wins_ensure_cap(target)) err(1, "wins_ensure_cap");
    win.workspace = idx;
    target->wins[target->nwin++] = win;
    target->focused = &target->wins[target->nwin - 1];

    rebuild_tiled(target);

    XUnmapWindow(dpy, win.window);

    if (ws->nwin == 0) {
        ws->focused = NULL;
    } else {
        int ni = removed - 1;
        if (ni < 0) ni = 0;
        if (ni >= ws->nwin) ni = ws->nwin - 1;
        ws->focused = &ws->wins[ni];
        update_border(ws->focused->window, 1);
        XSetInputFocus(dpy, ws->focused->window, RevertToPointerRoot, CurrentTime);
    }
    retile();
}

/* ---- window management ---- */

/* Manage a new top-level window: query attributes, skip override-redirect
   and special window types (desktop/dock/splash), allocate a ManagedWindow
   in the current workspace, apply class-matching rules, subscribe to
   events, set border, check struts, map, and tile. */
void
manage_window(Window w)
{
    Workspace *ws = scratch_visible ? &spaces[SCRATCHPAD_IDX] : curws();
    XWindowAttributes wa;
    XClassHint ch = { NULL, NULL };
    ManagedWindow mw;
    int i;

    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.override_redirect) return;

    /* skip desktop, dock, splash */
    Atom actual;
    int fmt;
    unsigned long n, remain;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, atom_net_wm_window_type, 0, 1, False,
                           XA_ATOM, &actual, &fmt, &n, &remain,
                           &data) == Success && data) {
        Atom type = *(Atom *)data;
        XFree(data);
        if (type == atom_net_wm_type_desktop ||
            type == atom_net_wm_type_dock ||
            type == atom_net_wm_type_splash) {
            XMapWindow(dpy, w);
            return;
        }
    }

    memset(&mw, 0, sizeof(mw));
    mw.window = w;
    mw.x = wa.x;
    mw.y = wa.y;
    mw.width = wa.width;
    mw.height = wa.height;
    mw.workspace = cur_ws;
    mw.monitor = curmon()->id;
    mw.width_factor = 1.0f;
    mw.saved_factor = 1.0f;

    if (XGetClassHint(dpy, w, &ch)) {
        for (i = 0; i < (int)num_rules; i++) {
            if (ch.res_class && strcmp(ch.res_class, rules[i].wm_class) == 0) {
                mw.is_floating = rules[i].is_floating;
                break;
            }
        }
        if (ch.res_class) XFree(ch.res_class);
        if (ch.res_name) XFree(ch.res_name);
    }

    if (!wins_ensure_cap(ws)) err(1, "wins_ensure_cap");

    int insert_idx = ws->nwin;
    ws->wins[ws->nwin++] = mw;

    rebuild_tiled(ws);

    XSelectInput(dpy, w, EnterWindowMask | StructureNotifyMask | PropertyChangeMask);
    XSetWindowBorderWidth(dpy, w, BORDER_WIDTH);
    refocus(ws, &ws->wins[insert_idx]);

    if (mw.workspace == cur_ws || ws == &spaces[SCRATCHPAD_IDX]) {
        Atom actual;
        int fmt;
        unsigned long n, remain;
        unsigned char *strut_data = NULL;
        if (XGetWindowProperty(dpy, w, atom_net_wm_strut, 0, 1, False,
                               XA_CARDINAL, &actual, &fmt, &n, &remain,
                               &strut_data) == Success && strut_data) {
            XFree(strut_data);
            curmon()->strut_valid = 0;
        }
    }

    if (mw.workspace == cur_ws || ws == &spaces[SCRATCHPAD_IDX])
        XMapWindow(dpy, w);

    if (mw.workspace == cur_ws || ws == &spaces[SCRATCHPAD_IDX]) {
        if (ws == &spaces[SCRATCHPAD_IDX])
            tile_horizontal_ws(ws);
        else if (curmon()->horizontal_mode)
            tile_horizontal();
        else
            tile_windows();
    }
}

/* Unmanage a window (destroyed or unmapped).  When force is true the
   window is removed from every workspace (used on DestroyNotify);
   otherwise only the current workspace is searched (UnmapNotify).
   After removal, refocus the adjacent tiled window if needed. */
void
unmanage_window(Window w, int force)
{
    int i, j;
    int lo = force ? 0 : cur_ws;
    int hi = force ? NUM_WORKSPACES + 1 : cur_ws + 1;

    for (j = lo; j < hi; j++) {
        Workspace *ws = &spaces[j];
        int removed = -1, focused_idx = -1;

        for (i = 0; i < ws->nwin; i++) {
            if (ws->focused && &ws->wins[i] == ws->focused)
                focused_idx = i;
        }

        for (i = 0; i < ws->nwin; i++) {
            if (ws->wins[i].window == w) {
                removed = i;
                memmove(&ws->wins[i], &ws->wins[i + 1],
                        (ws->nwin - i - 1) * sizeof(ManagedWindow));
                ws->nwin--;
                break;
            }
        }

        if (removed == -1) continue;

        curmon()->strut_valid = 0;

        rebuild_tiled(ws);

        if (j == cur_ws) {
            if (ws->nwin == 0) {
                ws->focused = NULL;
            } else if (focused_idx == removed) {
                int ni = removed - 1;
                if (ni < 0) ni = 0;
                if (ni >= ws->nwin) ni = ws->nwin - 1;
                ws->focused = &ws->wins[ni];
                update_border(ws->focused->window, 1);
                XSetInputFocus(dpy, ws->focused->window, RevertToPointerRoot, CurrentTime);
            } else if (focused_idx > removed) {
                ws->focused = &ws->wins[focused_idx - 1];
            } else if (focused_idx >= 0) {
                ws->focused = &ws->wins[focused_idx];
            } else {
                ws->focused = NULL;
            }
        }
    }

    retile_deferred();
}

/* ---- focus cycling ---- */

/* Move focus to the next/previous tiled window.  Both iterate the tiled[]
   pointer array to find the current index, then shift by ±1.  In
   horizontal mode the camera is updated to keep the focused column visible. */
void
focus_next(void)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    int idx = -1, i;

    if (ws->ntiled == 0) return;

    for (i = 0; i < ws->ntiled; i++) {
        if (ws->tiled[i] == ws->focused) {
            idx = i;
            break;
        }
    }
    if (idx == -1 || idx + 1 >= ws->ntiled) return;

    refocus(ws, ws->tiled[idx + 1]);

    if (mon->horizontal_mode)
        update_camera();
}

void
focus_prev(void)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    int idx = -1, i;

    if (ws->ntiled == 0) return;

    for (i = 0; i < ws->ntiled; i++) {
        if (ws->tiled[i] == ws->focused) {
            idx = i;
            break;
        }
    }
    if (idx <= 0) return;

    refocus(ws, ws->tiled[idx - 1]);

    if (mon->horizontal_mode)
        update_camera();
}

/* ---- close/quit ---- */

void
close_window(void)
{
    Workspace *ws = active_ws();
    XEvent ev;

    if (!ws->focused) return;

    ev.xclient.type = ClientMessage;
    ev.xclient.window = ws->focused->window;
    ev.xclient.message_type = atom_wm_protocols;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = atom_wm_delete;
    XSendEvent(dpy, ws->focused->window, False, NoEventMask, &ev);
}

void
quit_wm(void)
{
    running = 0;
}

/* ---- toggle states ---- */

void
toggle_fullscreen(void)
{
    Workspace *ws = active_ws();
    ManagedWindow *w = ws->focused;

    if (!w) return;

    w->is_fullscreen = !w->is_fullscreen;

    if (w->is_fullscreen)
        tiled_remove(ws, w->window);
    else if (!w->is_floating)
        tiled_add(ws, w);

    if (w->is_fullscreen) {
        w->pre_fs_x = w->x;
        w->pre_fs_y = w->y;
        w->pre_fs_width = w->width;
        w->pre_fs_height = w->height;
        w->pre_fs_floating = w->is_floating;
        w->is_floating = 1;

        w->x = 0;
        w->y = 0;
        w->width = scrw;
        w->height = scrh;
        XSetWindowBorderWidth(dpy, w->window, 0);
        XMoveResizeWindow(dpy, w->window, 0, 0, scrw, scrh);
        XRaiseWindow(dpy, w->window);

        XChangeProperty(dpy, w->window, atom_net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&atom_net_wm_state_full, 1);
    } else {
        w->is_floating = w->pre_fs_floating;
        w->x = w->pre_fs_x;
        w->y = w->pre_fs_y;
        w->width = w->pre_fs_width;
        w->height = w->pre_fs_height;
        XSetWindowBorderWidth(dpy, w->window, BORDER_WIDTH);
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);

        XChangeProperty(dpy, w->window, atom_net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)0, 0);

        retile_deferred();
    }
}

void
toggle_float(void)
{
    Workspace *ws = active_ws();
    ManagedWindow *w = ws->focused;
    Monitor *mon = curmon();
    int i;

    if (!w) return;
    if (w->is_fullscreen) return;

    if (!w->is_floating) {
        /* Save tiled geometry and position for snap-back */
        int idx = -1;
        for (i = 0; i < ws->ntiled; i++) {
            if (ws->tiled[i] == w) { idx = i; break; }
        }
        w->pre_float_x = w->x;
        w->pre_float_y = w->y;
        w->pre_float_w = w->width;
        w->pre_float_h = w->height;
        w->pre_float_idx = idx;
        w->pre_float_cam_x = ws->cam_x;

        tiled_remove(ws, w->window);
        w->is_floating = 1;

        /* Center on the current monitor (not global scrw/scrh) */
        w->x = mon->x + (mon->width - w->width) / 2;
        w->y = mon->y + (mon->height - w->height) / 2;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);
        XRaiseWindow(dpy, w->window);

        /* Retile the remaining tiled windows */
        if (ws == &spaces[SCRATCHPAD_IDX])
            retile_scratchpad();
        else
            retile_deferred();
    } else {
        /* Restore to tiling at original index */
        int idx = w->pre_float_idx;
        w->is_floating = 0;

        if (!tiled_ensure_cap(ws)) err(1, "toggle_float: tiled_ensure_cap");
        if (idx < 0 || idx > ws->ntiled) idx = ws->ntiled;
        memmove(&ws->tiled[idx + 1], &ws->tiled[idx],
                (ws->ntiled - idx) * sizeof(ManagedWindow *));
        ws->tiled[idx] = w;
        ws->ntiled++;

        /* Retile in the correct layout mode so snap-back works in both
           scrolling and stacking layouts. */
        if (ws == &spaces[SCRATCHPAD_IDX]) {
            retile_scratchpad();
        } else if (mon->horizontal_mode) {
            tile_horizontal_ws(ws);
        } else {
            tile_windows_ws(ws);
        }
    }
}

/* ---- scratchpad ---- */

/* Find an ARGB (depth-32) visual for semi-transparent windows.
   Returns 1 on success, 0 if no ARGB visual is available. */
static int
find_argb_visual(Display *d, Visual **out_vis, int *out_depth, Colormap *out_cm)
{
    XVisualInfo vi;

    if (XMatchVisualInfo(d, DefaultScreen(d), 32, TrueColor, &vi)) {
        *out_vis = vi.visual;
        *out_depth = 32;
        *out_cm = XCreateColormap(d, RootWindow(d, DefaultScreen(d)),
                                  vi.visual, AllocNone);
        return 1;
    }

    /* Fallback: no ARGB — caller will use opaque window */
    *out_vis = DefaultVisual(d, DefaultScreen(d));
    *out_depth = DefaultDepth(d, DefaultScreen(d));
    *out_cm = 0;
    return 0;
}

/* Extract R, G, B, A from a 0xRRGGBBAA hex value. */
static void
parse_dim_color(unsigned int hex, unsigned char *r, unsigned char *g,
                unsigned char *b, unsigned char *a)
{
    *r = (hex >> 24) & 0xFF;
    *g = (hex >> 16) & 0xFF;
    *b = (hex >> 8)  & 0xFF;
    *a =  hex        & 0xFF;
}

/* Scratchpad tile: runs tile_horizontal or tile_windows on the scratchpad
   workspace, same as retile() but on spaces[SCRATCHPAD_IDX] instead of
   the current workspace. */
static void
retile_scratchpad(void)
{
    Workspace *ws = &spaces[SCRATCHPAD_IDX];
    Monitor *mon = curmon();

    if (mon->horizontal_mode)
        tile_horizontal_ws(ws);
    else
        tile_windows_ws(ws);
}

/* Scratchpad overlay raise: map + raise every window in the scratchpad
   so they stack above the dimmed underlying workspace. */
static void
scratch_raise_all(void)
{
    Workspace *ws = &spaces[SCRATCHPAD_IDX];
    Monitor *mon = curmon();
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].is_fullscreen) {
            ws->wins[i].x = mon->x;
            ws->wins[i].y = mon->y;
            ws->wins[i].width = mon->width;
            ws->wins[i].height = mon->height;
            XSetWindowBorderWidth(dpy, ws->wins[i].window, 0);
            XMoveResizeWindow(dpy, ws->wins[i].window,
                              mon->x, mon->y, mon->width, mon->height);
        }
        XMapWindow(dpy, ws->wins[i].window);
        XRaiseWindow(dpy, ws->wins[i].window);
    }
}

/* Scratchpad overlay unmap: hide every window in the scratchpad. */
static void
scratch_unmap_all(void)
{
    Workspace *ws = &spaces[SCRATCHPAD_IDX];
    int i;

    for (i = 0; i < ws->nwin; i++)
        XUnmapWindow(dpy, ws->wins[i].window);
}

/* Move the focused window into the scratchpad workspace.
   Preserves tiling state; if scratchpad is visible the window appears
   immediately, otherwise it is hidden until the next toggle. */
void
move_to_scratchpad(void)
{
    Workspace *src = curws();
    Workspace *dst = &spaces[SCRATCHPAD_IDX];
    ManagedWindow win;
    int i, found = 0;

    if (!src->focused) return;
    if (src == dst) return;

    win = *src->focused;

    int removed = -1;
    for (i = 0; i < src->nwin; i++) {
        if (src->wins[i].window == win.window) {
            removed = i;
            memmove(&src->wins[i], &src->wins[i + 1],
                    (src->nwin - i - 1) * sizeof(ManagedWindow));
            src->nwin--;
            found = 1;
            break;
        }
    }
    if (!found) return;

    rebuild_tiled(src);

    if (!wins_ensure_cap(dst)) err(1, "wins_ensure_cap");
    win.workspace = SCRATCHPAD_IDX;
    dst->wins[dst->nwin++] = win;

    rebuild_tiled(dst);

    if (scratch_visible) {
        /* Window appears immediately in the overlay */
        ManagedWindow *nw = &dst->wins[dst->nwin - 1];
        XMapWindow(dpy, nw->window);
        XRaiseWindow(dpy, nw->window);
        retile_scratchpad();
    } else {
        /* Hidden until next toggle */
        XUnmapWindow(dpy, win.window);
    }

    /* Refocus source workspace */
    if (src->nwin == 0) {
        src->focused = NULL;
    } else {
        int ni = removed - 1;
        if (ni < 0) ni = 0;
        if (ni >= src->nwin) ni = src->nwin - 1;
        src->focused = &src->wins[ni];
        update_border(src->focused->window, 1);
        XSetInputFocus(dpy, src->focused->window, RevertToPointerRoot, CurrentTime);
    }
    retile_deferred();
}

/* Toggle the scratchpad overlay on/off.  When showing, all scratchpad
   windows are mapped and raised above the dimmed underlying workspace;
   when hiding they are all unmapped and focus returns to where we were. */
void
toggle_scratchpad(void)
{
    Workspace *ws = &spaces[SCRATCHPAD_IDX];
    Workspace *under = &spaces[cur_ws];
    Monitor *mon = curmon();
    int i;

    if (!scratch_visible) {
        /* Show scratchpad overlay */
        scratch_saved_focus = under->focused;
        scratch_visible = 1;

        /* Create dim overlay: ARGB fullscreen window behind scratchpad */
        {
            Visual *vis;
            int depth;
            Colormap cm;
            int has_argb = find_argb_visual(dpy, &vis, &depth, &cm);
            unsigned char r, g, b, a;
            XSetWindowAttributes swa;

            parse_dim_color(DIM_COLOR, &r, &g, &b, &a);

            swa.override_redirect = True;
            swa.colormap = has_argb ? cm : CopyFromParent;
            /* Pack pixel as 0xAARRGGBB for depth-32, or plain RGB */
            if (has_argb)
                swa.background_pixel = (a << 24) | (r << 16) | (g << 8) | b;
            else
                swa.background_pixel = (r << 16) | (g << 8) | b;
            swa.border_pixel = 0;

            mon->dim_win = XCreateWindow(dpy, root,
                mon->x, mon->y, mon->width, mon->height, 0,
                depth, InputOutput, vis,
                CWOverrideRedirect | CWBackPixel | CWBorderPixel
                    | (has_argb ? CWColormap : 0),
                &swa);
            XMapWindow(dpy, mon->dim_win);
            /* Dim goes above workspace, below scratchpad windows */
            XRaiseWindow(dpy, mon->dim_win);
            /* Free colormap on cleanup (stored in dim_win is enough) */
            if (has_argb)
                mon->dim_colormap = cm;
            else
                mon->dim_colormap = 0;
        }

        /* Compute usable area for scratchpad layout */
        compute_usable_area_ws(mon, ws);

        retile_scratchpad();
        scratch_raise_all();

        /* Focus the last-focused scratchpad window, or first */
        if (ws->focused) {
            refocus(ws, ws->focused);
        } else if (ws->nwin > 0) {
            ws->focused = &ws->wins[0];
            refocus(ws, ws->focused);
        }
    } else {
        /* Hide scratchpad overlay */
        scratch_unmap_all();
        scratch_visible = 0;

        /* Destroy dim overlay */
        if (mon->dim_win) {
            XDestroyWindow(dpy, mon->dim_win);
            mon->dim_win = 0;
        }
        if (mon->dim_colormap) {
            XFreeColormap(dpy, mon->dim_colormap);
            mon->dim_colormap = 0;
        }

        /* Restore focus to the underlying workspace */
        if (scratch_saved_focus) {
            /* Validate pointer is still in wins[] */
            int valid = 0;
            for (i = 0; i < under->nwin; i++) {
                if (&under->wins[i] == scratch_saved_focus) {
                    valid = 1;
                    break;
                }
            }
            if (valid)
                refocus(under, scratch_saved_focus);
            else if (under->nwin > 0)
                refocus(under, &under->wins[0]);
            else
                under->focused = NULL;
        } else if (under->nwin > 0) {
            refocus(under, &under->wins[0]);
        }
    }
}

/* ---- spawn ---- */

void
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

/* ---- event handlers ---- */

/* MapRequest: a new window wants to be shown — manage it. */
void
handle_map_request(XMapRequestEvent *e)
{
    manage_window(e->window);
}

/* DestroyNotify: window was destroyed — remove from all workspaces. */
void
handle_destroy_notify(XDestroyWindowEvent *e)
{
    unmanage_window(e->window, 1);
}

/* UnmapNotify: window was unmapped — remove from current workspace. */
void
handle_unmap_notify(XUnmapEvent *e)
{
    unmanage_window(e->window, 0);
}

/* ConfigureRequest: honour stacking/resize requests from managed windows.
   Tiled windows only get stacking updates; floating/fullscreen windows
   are allowed full geometry changes. */
void
handle_configure_request(XConfigureRequestEvent *e)
{
    Workspace *ws = active_ws();
    ManagedWindow *mw = NULL;
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            mw = &ws->wins[i];
            break;
        }
    }

    if (mw && !mw->is_floating && !mw->is_fullscreen) {
        XWindowChanges wc;
        wc.sibling = e->above;
        wc.stack_mode = e->detail;
        XConfigureWindow(dpy, e->window, CWSibling | CWStackMode, &wc);
        return;
    }

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

/* EnterNotify: pointer entered a managed window — refocus it. */
void
handle_enter_notify(XCrossingEvent *e)
{
    Workspace *ws = active_ws();
    int i;

    if (e->mode != NotifyNormal || e->detail == NotifyInferior) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            refocus(ws, &ws->wins[i]);
            break;
        }
    }
}

/* KeyPress: look up the key binding and dispatch the action. */
void
handle_key_press(XKeyEvent *e)
{
    KeySym keysym = XLookupKeysym(e, 0);
    unsigned int mod = e->state & (Mod1Mask | Mod4Mask | ShiftMask | ControlMask);
    int i;

    for (i = 0; i < (int)num_keys; i++) {
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
            case RESIZE_WINDOW:      resize_window((void *)(long)keys[i].arg.i); break;
            case SCROLL_LEFT:        move_horizontal(0); break;
            case SCROLL_RIGHT:       move_horizontal(1); break;
            case TOGGLE_LAYOUT:      toggle_layout(); break;
            case TOGGLE_FULLSCREEN:  toggle_fullscreen(); break;
            case TOGGLE_FLOAT:       toggle_float(); break;
            case TOGGLE_SCRATCHPAD:  toggle_scratchpad(); break;
            case MOVE_TO_SCRATCHPAD: move_to_scratchpad(); break;
            case FIT_WINDOW:         fit_window(); break;
            case TOGGLE_CENTER_FOCUS: toggle_center_focus(); break;
            case SWITCH_WORKSPACE:   switch_workspace((void *)(long)keys[i].arg.i); break;
            case MOVE_TO_WORKSPACE:  move_to_workspace((void *)(long)keys[i].arg.i); break;
            case FOCUS_MONITOR:      focus_monitor((void *)(long)keys[i].arg.i); break;
            }
            break;
        }
    }
}

/* ButtonPress: Super+Button1 moves/drag-swaps, Super+Button3 resizes.
   For tiled windows (horizontal mode) Button1 swaps columns; for
   floating windows Button1 moves freely; Button3 on the bottom-right
   corner (16px hotspot) live-resizes the floating window. */
void
handle_button_press(XButtonEvent *e)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    int i;

    if (!(e->state & Mod4Mask)) return;

    if (e->button == Button3) {
        /* Floating resize: bottom-right corner hotspot */
        for (i = 0; i < ws->nwin; i++) {
            ManagedWindow *mw = &ws->wins[i];
            if (!mw->is_floating || mw->is_fullscreen) continue;
            /* 16px square at bottom-right for resize handle */
            int rx = mw->x + mw->width - 16;
            int ry = mw->y + mw->height - 16;
            if (e->x_root >= rx && e->x_root < mw->x + mw->width
                && e->y_root >= ry && e->y_root < mw->y + mw->height) {
                mouse.active = 1;
                mouse.resizing = 1;
                mouse.win = mw;
                mouse.start_x = e->x_root;
                mouse.start_y = e->y_root;
                mouse.orig_w = mw->width;
                mouse.orig_h = mw->height;
                XGrabPointer(dpy, root, True,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                return;
            }
        }
        /* Also allow resize from anywhere on floating window via Button3
           (fallback if corner not hit) */
        for (i = 0; i < ws->nwin; i++) {
            ManagedWindow *mw = &ws->wins[i];
            if (!mw->is_floating || mw->is_fullscreen) continue;
            if (e->x_root >= mw->x && e->x_root < mw->x + mw->width
                && e->y_root >= mw->y && e->y_root < mw->y + mw->height) {
                mouse.active = 1;
                mouse.resizing = 1;
                mouse.win = mw;
                mouse.start_x = e->x_root;
                mouse.start_y = e->y_root;
                mouse.orig_w = mw->width;
                mouse.orig_h = mw->height;
                XGrabPointer(dpy, root, True,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                return;
            }
        }
        return;
    }

    if (e->button != Button1) return;

    /* Try tiled window under cursor (only in horizontal mode) */
    if (mon->horizontal_mode) {
        for (i = 0; i < ws->ntiled; i++) {
            ManagedWindow *mw = ws->tiled[i];
            int screen_x = mw->x - ws->cam_x;
            if (e->x_root >= screen_x && e->x_root < screen_x + mw->width
                && e->y_root >= mw->y && e->y_root < mw->y + mw->height) {
                mouse.active = 1;
                mouse.resizing = 0;
                mouse.win = mw;
                mouse.start_x = e->x_root;
                mouse.start_y = e->y_root;
                mouse.orig_x = mw->x;
                mouse.orig_y = mw->y;
                XGrabPointer(dpy, root, True,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                return;
            }
        }
    }

    /* Try floating window under cursor (any layout mode) */
    for (i = 0; i < ws->nwin; i++) {
        ManagedWindow *mw = &ws->wins[i];
        if (!mw->is_floating || mw->is_fullscreen) continue;
        if (e->x_root >= mw->x && e->x_root < mw->x + mw->width
            && e->y_root >= mw->y && e->y_root < mw->y + mw->height) {
            mouse.active = 1;
            mouse.resizing = 0;
            mouse.win = mw;
            mouse.start_x = e->x_root;
            mouse.start_y = e->y_root;
            mouse.orig_x = mw->x;
            mouse.orig_y = mw->y;
            XGrabPointer(dpy, root, True,
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            return;
        }
    }
}

/* ButtonRelease: finalize drag — for tiled windows swap with the closest
   tiled window under the cursor, then retile.  For floating windows the
   position set during motion is kept as-is. */
void
handle_button_release(XButtonEvent *e)
{
    Workspace *ws = active_ws();
    int i;
    int best_idx = -1;
    int best_dist = INT_MAX;

    (void)e;

    if (!mouse.active || !mouse.win) goto done;

    if (mouse.resizing) goto done_floating;
    {
        int was_floating = mouse.win->is_floating;
        if (was_floating) goto done_floating;
    }

    /* Find the closest non-dragged tiled window to the cursor */
    for (i = 0; i < ws->ntiled; i++) {
        ManagedWindow *mw = ws->tiled[i];
        if (mw == mouse.win) continue;
        int screen_x = mw->x - ws->cam_x;
        int cx = screen_x + mw->width / 2;
        int cy = mw->y + mw->height / 2;
        int dx = e->x_root - cx;
        int dy = e->y_root - cy;
        int dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        /* Swap the dragged window with the closest window in tiled[] */
        ManagedWindow *a = mouse.win;
        ManagedWindow *b = ws->tiled[best_idx];
        int idx_a = -1, j;
        for (j = 0; j < ws->ntiled; j++) {
            if (ws->tiled[j] == a) { idx_a = j; break; }
        }
        if (idx_a != -1) {
            ws->tiled[idx_a] = b;
            ws->tiled[best_idx] = a;
        }
    }

done:
    mouse.active = 0;
    mouse.resizing = 0;
    mouse.win = NULL;
    XUngrabPointer(dpy, CurrentTime);
    if (ws == &spaces[SCRATCHPAD_IDX])
        retile_scratchpad();
    else
        retile_deferred();
    return;

done_floating:
    mouse.active = 0;
    mouse.resizing = 0;
    mouse.win = NULL;
    XUngrabPointer(dpy, CurrentTime);
}

/* MotionNotify: during drag, move or live-resize the dragged window.
   For resizing (Super+Button3 on floating) the bottom-right corner is
   dragged.  For moving, floating windows are clamped to the monitor. */
void
handle_motion_notify(XMotionEvent *e)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    int dx, dy, screen_x;

    if (!mouse.active || !mouse.win) return;

    dx = e->x_root - mouse.start_x;
    dy = e->y_root - mouse.start_y;

    if (mouse.resizing) {
        /* Drain coalesced motions, keep only the last for live content */
        XEvent ev;
        while (XCheckMaskEvent(dpy, PointerMotionMask, &ev)) {
            if (ev.type == MotionNotify) {
                dx = ev.xmotion.x_root - mouse.start_x;
                dy = ev.xmotion.y_root - mouse.start_y;
            }
        }
        int nw = mouse.orig_w + dx;
        int nh = mouse.orig_h + dy;
        nw = nw < MIN_WIN_DIM ? MIN_WIN_DIM : nw;
        nh = nh < MIN_WIN_DIM ? MIN_WIN_DIM : nh;
        if (nw > mon->width - 2 * BORDER_WIDTH)  nw = mon->width  - 2 * BORDER_WIDTH;
        if (nh > mon->height - 2 * BORDER_WIDTH) nh = mon->height - 2 * BORDER_WIDTH;
        mouse.win->width = nw;
        mouse.win->height = nh;
        XMoveResizeWindow(dpy, mouse.win->window,
                          mouse.win->x, mouse.win->y, nw, nh);
        return;
    }

    mouse.win->x = mouse.orig_x + dx;
    mouse.win->y = mouse.orig_y + dy;

    if (mouse.win->is_floating) {
        /* Clamp floating window inside monitor */
        if (mouse.win->x < mon->x) mouse.win->x = mon->x;
        if (mouse.win->y < mon->y) mouse.win->y = mon->y;
        if (mouse.win->x + mouse.win->width + 2 * BORDER_WIDTH > mon->x + mon->width)
            mouse.win->x = mon->x + mon->width - mouse.win->width - 2 * BORDER_WIDTH;
        if (mouse.win->y + mouse.win->height + 2 * BORDER_WIDTH > mon->y + mon->height)
            mouse.win->y = mon->y + mon->height - mouse.win->height - 2 * BORDER_WIDTH;
        XMoveResizeWindow(dpy, mouse.win->window,
                          mouse.win->x, mouse.win->y,
                          mouse.win->width, mouse.win->height);
    } else {
        screen_x = mouse.win->x - ws->cam_x;
        XMoveResizeWindow(dpy, mouse.win->window,
                          screen_x, mouse.win->y,
                          mouse.win->width, mouse.win->height);
    }
}
