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
    return 1;
}

/* Find the index of ws->focused in ws->wins[].  Returns -1 if not found. */
static int
find_focused_idx(Workspace *ws)
{
    int i;
    if (!ws->focused) return -1;
    for (i = 0; i < ws->nwin; i++)
        if (&ws->wins[i] == ws->focused)
            return i;
    return -1;
}

/* Refocus the workspace after a window at 'removed' index was memmoved out.
   Handles both cases: focused window was removed, or a different window. */
static void
refocus_after_remove(Workspace *ws, int removed)
{
    if (ws->nwin == 0) {
        ws->focused = NULL;
        return;
    }

    int fi = find_focused_idx(ws);
    int ni;

    if (fi == -1) {
        /* Focused pointer was invalidated by memmove — find the window
           that shifted into the removed slot, or fall back to the end. */
        ni = removed;
        if (ni >= ws->nwin) ni = ws->nwin - 1;
    } else if (fi > removed) {
        /* Focused window shifted left by one due to memmove */
        ni = fi - 1;
    } else {
        /* Focused window was not affected by the removal */
        ni = fi;
    }

    ws->focused = &ws->wins[ni];
    update_border(ws->focused->window, 1);
    XSetInputFocus(dpy, ws->focused->window, RevertToPointerRoot, CurrentTime);
}

/* ---- focus ---- */

void
update_border(Window w, int focused)
{
    XSetWindowBorder(dpy, w, focused ? FOCUS_COLOR : BORDER_COLOR);
}
void
refocus(Workspace *ws, ManagedWindow *next)
{
    if (ws->focused && ws->focused != next)
        update_border(ws->focused->window, 0);

    ws->focused = next;
    if (next) {
        /* _NET_WM_STATE_NOT_FOCUSABLE windows don't receive focus */
        if (next->is_not_focusable) {
            update_border(next->window, 0);
            return;
        }
        update_border(next->window, 1);
        XSetInputFocus(dpy, next->window, RevertToPointerRoot, CurrentTime);
        XRaiseWindow(dpy, next->window);
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

    retile_ws(curws());

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

    update_camera_ws(curws());
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

/* ---- workspace management ---- */

void
show_workspace(int idx, int visible)
{
    Workspace *ws = &spaces[idx];
    int i;

    for (i = 0; i < ws->nwin; i++) {
        /* Sticky windows are never unmapped — they stay visible on all
           workspaces.  When showing a workspace, always map them. */
        if (!visible && ws->wins[i].is_sticky)
            continue;
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

    retile_ws(curws());

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

    /* Remove from dwindle tree before memmove (leaf pointers would dangle) */
    if (!curmon()->horizontal_mode)
        dwindle_remove(ws, win.window);

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

    /* Insert into dwindle tree on target workspace if in dwindle mode */
    if (!curmon()->horizontal_mode)
        dwindle_insert(target, win.window);

    /* Sticky windows stay mapped on all workspaces */
    if (!win.is_sticky)
        XUnmapWindow(dpy, win.window);

    refocus_after_remove(ws, removed);
    retile_ws(curws());
}

/* ---- window management ---- */

/* Window type classification results. */
#define WIN_SKIP     0  /* map only, don't manage (desktop/dock/splash/etc.) */
#define WIN_NORMAL   1  /* manage + tile */
#define WIN_DIALOG   2  /* manage + auto-float at requested position */

/* Classify a window by its _NET_WM_WINDOW_TYPE property.
   Returns WIN_SKIP, WIN_NORMAL, or WIN_DIALOG. */
static int
classify_window_type(Window w)
{
    Atom actual;
    int fmt;
    unsigned long n, remain;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, w, atom_net_wm_window_type, 0, 1, False,
                           XA_ATOM, &actual, &fmt, &n, &remain,
                           &data) == Success) {
        if (data && actual == XA_ATOM && fmt == 32 && n >= 1) {
            Atom type = *(Atom *)data;
            XFree(data);
            if (type == atom_net_wm_type_desktop ||
                type == atom_net_wm_type_dock ||
                type == atom_net_wm_type_splash ||
                type == atom_net_wm_type_notification ||
                type == atom_net_wm_type_popup_menu ||
                type == atom_net_wm_type_menu) {
                return WIN_SKIP;
            }
            if (type == atom_net_wm_type_dialog ||
                type == atom_net_wm_type_util ||
                type == atom_net_wm_type_toolbar) {
                return WIN_DIALOG;
            }
            /* NORMAL and anything else → treat as normal */
            return WIN_NORMAL;
        } else if (data) {
            XFree(data);
        }
    }
    /* No type set → treat as normal */
    return WIN_NORMAL;
}

/* Read _NET_WM_STATE and set the corresponding ManagedWindow flags.
   Also sets is_floating=1 for above/sticky windows. */
static void
read_net_wm_state(ManagedWindow *mw, Window w)
{
    Atom actual;
    int fmt;
    unsigned long n, remain;
    unsigned char *data = NULL;
    unsigned long i;

    if (XGetWindowProperty(dpy, w, atom_net_wm_state, 0, 32, False,
                           XA_ATOM, &actual, &fmt, &n, &remain,
                           &data) != Success || !data)
        return;

    if (actual != XA_ATOM || fmt != 32) {
        XFree(data);
        return;
    }

    Atom *states = (Atom *)data;
    for (i = 0; i < n; i++) {
        if (states[i] == atom_net_wm_state_above) {
            mw->is_above = 1;
            mw->is_floating = 1;
        } else if (states[i] == atom_net_wm_state_sticky) {
            mw->is_sticky = 1;
            mw->is_floating = 1;
        } else if (states[i] == atom_net_wm_state_not_focusable) {
            mw->is_not_focusable = 1;
        }
    }
    XFree(data);
}

/* Apply window-class rules to determine initial floating state. */
static void
apply_rules(ManagedWindow *mw, Window w)
{
    XClassHint ch = { NULL, NULL };
    int i;

    if (XGetClassHint(dpy, w, &ch)) {
        for (i = 0; i < (int)num_rules; i++) {
            if (ch.res_class && strcmp(ch.res_class, rules[i].wm_class) == 0) {
                mw->is_floating = rules[i].is_floating;
                break;
            }
        }
        if (ch.res_class) XFree(ch.res_class);
        if (ch.res_name) XFree(ch.res_name);
    }
}

/* Insert a new ManagedWindow into the workspace's wins[] array.
   Places it after the currently focused window so it appears next
   in tiled order.  Falls back to appending if no focused window. */
static int
insert_into_workspace(Workspace *ws, ManagedWindow mw)
{
    int insert_idx = ws->nwin;
    int i;

    if (ws->focused) {
        for (i = 0; i < ws->nwin; i++) {
            if (&ws->wins[i] == ws->focused) {
                insert_idx = i + 1;
                break;
            }
        }
    }
    if (!wins_ensure_cap(ws)) return -1;
    if (insert_idx < ws->nwin)
        memmove(&ws->wins[insert_idx + 1], &ws->wins[insert_idx],
                (ws->nwin - insert_idx) * sizeof(ManagedWindow));
    ws->wins[insert_idx] = mw;
    ws->nwin++;
    return insert_idx;
}

/* Manage a new top-level window: classify by type, read EWMH state,
   allocate a ManagedWindow, apply rules, subscribe to events, set border,
   check struts, map, and tile.  Only _NET_WM_WINDOW_TYPE_NORMAL windows
   are tiled; DIALOG/UTIL/TOOLBAR are auto-floated; all other types are
   mapped but not managed. */
void
manage_window(Window w)
{
    Workspace *ws = scratch_visible ? &spaces[SCRATCHPAD_IDX] : curws();
    XWindowAttributes wa;
    ManagedWindow mw;
    int insert_idx;
    int win_type;

    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.override_redirect) return;

    win_type = classify_window_type(w);
    if (win_type == WIN_SKIP) {
        XMapWindow(dpy, w);
        return;
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

    /* Auto-float dialogs/utils/toolbars at their requested position */
    if (win_type == WIN_DIALOG)
        mw.is_floating = 1;

    apply_rules(&mw, w);
    read_net_wm_state(&mw, w);

    insert_idx = insert_into_workspace(ws, mw);
    if (insert_idx == -1) err(1, "wins_ensure_cap");

    rebuild_tiled(ws);

    /* In dwindle mode, insert tiled windows into dwindle tree */
    if (!mw.is_floating && !mw.is_fullscreen) {
        Monitor *mon = curmon();
        if (!mon->horizontal_mode)
            dwindle_insert(ws, w);
    }

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
        retile_ws(ws);
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
        int removed = -1;

        for (i = 0; i < ws->nwin; i++) {
            if (ws->wins[i].window == w) {
                removed = i;
                /* Remove from dwindle tree before memmove (leaf pointers would dangle) */
                if (!curmon()->horizontal_mode)
                    dwindle_remove(ws, w);
                memmove(&ws->wins[i], &ws->wins[i + 1],
                        (ws->nwin - i - 1) * sizeof(ManagedWindow));
                ws->nwin--;
                break;
            }
        }

        if (removed == -1) continue;

        curmon()->strut_valid = 0;

        rebuild_tiled(ws);

        if (j == cur_ws)
            refocus_after_remove(ws, removed);
    }

    retile_deferred();
}

/* ---- focus cycling ---- */

/* Move focus to the next/previous tiled window.  Both iterate the tiled[]
   pointer array to find the current index, then shift by ±1.  In
   horizontal mode the camera is updated to keep the focused column visible. */
void
focus_cycle(int delta)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    int idx = -1, i;

    /* In dwindle mode, use tree-based focus */
    if (!mon->horizontal_mode && ws->dwindle_root) {
        dwindle_focus_prevnext(ws, delta);
        return;
    }

    if (ws->ntiled == 0) return;

    for (i = 0; i < ws->ntiled; i++) {
        if (ws->tiled[i] == ws->focused) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return;

    /* Walk in the given direction, skipping not-focusable windows */
    int new_idx = idx + delta;
    while (new_idx >= 0 && new_idx < ws->ntiled) {
        if (!ws->tiled[new_idx]->is_not_focusable)
            break;
        new_idx += delta;
    }
    if (new_idx < 0 || new_idx >= ws->ntiled) return;

    refocus(ws, ws->tiled[new_idx]);

    if (mon->horizontal_mode)
        update_camera_ws(curws());
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

    if (w->is_fullscreen) {
        tiled_remove(ws, w->window);
        if (!curmon()->horizontal_mode)
            dwindle_remove(ws, w->window);
    } else {
        /* Re-insert into layout BEFORE restoring is_floating */
        tiled_add(ws, w);
        if (!curmon()->horizontal_mode)
            dwindle_insert(ws, w->window);
    }

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

        /* Rebuild _NET_WM_STATE, preserving above/sticky/not_focusable */
        {
            Atom states[4];
            int nstates = 0;
            if (w->is_above)    states[nstates++] = atom_net_wm_state_above;
            if (w->is_sticky)   states[nstates++] = atom_net_wm_state_sticky;
            if (w->is_not_focusable) states[nstates++] = atom_net_wm_state_not_focusable;
            XChangeProperty(dpy, w->window, atom_net_wm_state, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)states, nstates);
        }

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
    if (w->is_above || w->is_sticky) return;  /* always floating */

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
        if (!mon->horizontal_mode)
            dwindle_remove(ws, w->window);
        w->is_floating = 1;

        /* Center on the current monitor (not global scrw/scrh) */
        w->x = mon->x + (mon->width - w->width) / 2;
        w->y = mon->y + (mon->height - w->height) / 2;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);
        XRaiseWindow(dpy, w->window);

        /* Retile the remaining tiled windows */
        if (ws == &spaces[SCRATCHPAD_IDX])
            retile_ws(&spaces[SCRATCHPAD_IDX]);
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

        /* Re-insert into dwindle tree if in dwindle mode */
        if (!mon->horizontal_mode)
            dwindle_insert(ws, w->window);

        /* Retile in the correct layout mode so snap-back works in both
           scrolling and stacking layouts. */
        if (ws == &spaces[SCRATCHPAD_IDX]) {
            retile_ws(&spaces[SCRATCHPAD_IDX]);
        } else {
            retile_deferred();
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

/* Create the dim overlay window for the scratchpad. */
static void
scratchpad_create_dim(Monitor *mon)
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
    XRaiseWindow(dpy, mon->dim_win);
    mon->dim_colormap = has_argb ? cm : 0;
}

/* Destroy the dim overlay window for the scratchpad. */
static void
scratchpad_destroy_dim(Monitor *mon)
{
    if (mon->dim_win) {
        XDestroyWindow(dpy, mon->dim_win);
        mon->dim_win = 0;
    }
    if (mon->dim_colormap) {
        XFreeColormap(dpy, mon->dim_colormap);
        mon->dim_colormap = 0;
    }
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

    /* Remove from dwindle tree before memmove (leaf pointers would dangle) */
    if (!curmon()->horizontal_mode)
        dwindle_remove(src, win.window);

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

    /* Skip dwindle for scratchpad — it has its own workspace layout */

    if (scratch_visible) {
        /* Window appears immediately in the overlay */
        ManagedWindow *nw = &dst->wins[dst->nwin - 1];
        XMapWindow(dpy, nw->window);
        XRaiseWindow(dpy, nw->window);
        retile_ws(&spaces[SCRATCHPAD_IDX]);
    } else {
        /* Hidden until next toggle — but sticky windows stay mapped */
        if (!win.is_sticky)
            XUnmapWindow(dpy, win.window);
    }

    /* Refocus source workspace */
    refocus_after_remove(src, removed);
    retile_deferred();
}

/* Show the scratchpad overlay: create dim, map windows, refocus. */
static void
scratchpad_show(void)
{
    Workspace *ws = &spaces[SCRATCHPAD_IDX];
    Workspace *under = &spaces[cur_ws];
    Monitor *mon = curmon();

    scratch_saved_focus = under->focused;
    scratch_visible = 1;

    scratchpad_create_dim(mon);
    retile_ws(&spaces[SCRATCHPAD_IDX]);
    scratch_raise_all();

    if (ws->focused) {
        refocus(ws, ws->focused);
    } else if (ws->nwin > 0) {
        ws->focused = &ws->wins[0];
        refocus(ws, ws->focused);
    }
}

/* Hide the scratchpad overlay: unmap windows, destroy dim, restore focus. */
static void
scratchpad_hide(void)
{
    Workspace *under = &spaces[cur_ws];
    Monitor *mon = curmon();
    int i;

    scratch_unmap_all();
    scratch_visible = 0;

    scratchpad_destroy_dim(mon);

    /* Restore focus to the underlying workspace */
    if (scratch_saved_focus) {
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
void
toggle_scratchpad(void)
{
    if (!scratch_visible)
        scratchpad_show();
    else
        scratchpad_hide();
}

/* ---- spawn ---- */

void
spawn(void *arg)
{
    const char **cmd = (const char **)arg;
    pid_t pid = fork();
    if (pid == -1) {
        perror("dswm: fork");
        return;
    }
    if (pid == 0) {
        close(ConnectionNumber(dpy));
        setsid();
        execvp(cmd[0], (char *const *)cmd);
        fprintf(stderr, "dswm: execvp %s failed\n", cmd[0]);
        _exit(1);
    }
}

