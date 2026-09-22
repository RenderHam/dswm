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

/* ---- safety helpers ---- */

/* Check if a window still exists on the server.  Avoids BadWindow crashes. */
int
window_exists(Window w)
{
    XWindowAttributes wa;
    return XGetWindowAttributes(dpy, w, &wa);
}

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

/* Find a managed window by ID.  Returns NULL if not present. */
ManagedWindow *
find_mw(Workspace *ws, Window w)
{
    int i;
    if (w == None) return NULL;
    for (i = 0; i < ws->nwin; i++)
        if (ws->wins[i].window == w)
            return &ws->wins[i];
    return NULL;
}

/* Find a managed window by ID across all workspaces. */
ManagedWindow *
find_mw_any(Window w)
{
    int j;
    if (w == None) return NULL;
    for (j = 0; j < NUM_WORKSPACES + 1; j++) {
        ManagedWindow *mw = find_mw(&spaces[j], w);
        if (mw) return mw;
    }
    return NULL;
}

/* Resolve the workspace's focused ID to a live pointer.
   Returns NULL when nothing is focused or the window is gone. */
ManagedWindow *
focused_mw(Workspace *ws)
{
    return find_mw(ws, ws->focused);
}

/* Whether a window can receive keyboard-cycle focus.  Fullscreen and
   monocle-hidden windows are skipped (they're not visible/interactive). */
int
focus_candidate(ManagedWindow *mw)
{
    return !mw->is_not_focusable && !mw->is_fullscreen && !mw->monocle_hidden;
}

/* Refocus the workspace after a window at 'removed' index was memmoved out.
   Focus is tracked by ID, so removal only matters when the focused window
   itself was removed — otherwise the ID still resolves. */
static void
refocus_after_remove(Workspace *ws, int removed)
{
    ManagedWindow *mw;

    if (ws->nwin == 0) {
        ws->focused = None;
        return;
    }

    mw = focused_mw(ws);
    if (!mw) {
        /* Focused window was removed — fall back to the window that
           shifted into the removed slot, or the end of the list. */
        int ni = removed;
        if (ni >= ws->nwin) ni = ws->nwin - 1;
        mw = &ws->wins[ni];
    }

    ws->focused = mw->window;
    if (mw->is_not_focusable) {
        update_border(mw->window, 0);
        return;
    }
    update_border(mw->window, 1);
    if (window_exists(mw->window))
        XSetInputFocus(dpy, mw->window, RevertToPointerRoot, CurrentTime);
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
    if (ws->focused != None && (!next || ws->focused != next->window))
        update_border(ws->focused, 0);

    ws->focused = next ? next->window : None;
    if (next) {
        /* _NET_WM_STATE_NOT_FOCUSABLE windows don't receive focus */
        if (next->is_not_focusable) {
            update_border(next->window, 0);
            return;
        }
        update_border(next->window, 1);
        /* No raise here: stacking changes only on explicit focus
           (keyboard cycle, click, drag, manage) — never on hover. */
        if (window_exists(next->window)) {
            if (next->input_hint)
                XSetInputFocus(dpy, next->window, RevertToPointerRoot, CurrentTime);
            else {
                /* Window doesn't want input focus — send WM_TAKE_FOCUS instead */
                XEvent ev = {0};
                ev.xclient.type = ClientMessage;
                ev.xclient.window = next->window;
                ev.xclient.message_type = atom_wm_protocols;
                ev.xclient.format = 32;
                ev.xclient.data.l[0] = atom_wm_take_focus;
                ev.xclient.data.l[1] = CurrentTime;
                XSendEvent(dpy, next->window, False, NoEventMask, &ev);
            }
        }
        /* Update _NET_ACTIVE_WINDOW for pagers/taskbars */
        XChangeProperty(dpy, root, atom_net_active_window, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&next->window, 1);
    } else {
        XDeleteProperty(dpy, root, atom_net_active_window);
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
        if (ws->tiled[i]->window == ws->focused) {
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

/* Swap the focused tiled window with its neighbour in the tiled list.
   Only operates on tiled windows — floating neighbours are skipped. */
void
swap_impl(int delta)
{
    Workspace *ws = active_ws();
    ManagedWindow tmp;
    int ti, si, wi, wj;

    ManagedWindow *cur;

    if (ws->ntiled < 2 || ws->focused == None) return;
    cur = focused_mw(ws);
    if (!cur || cur->is_floating) return;

    /* Find focused window in tiled[] */
    for (ti = 0; ti < ws->ntiled; ti++)
        if (ws->tiled[ti]->window == ws->focused)
            break;
    if (ti == ws->ntiled) return;

    si = ti + delta;
    if (si < 0 || si >= ws->ntiled) return;

    /* Swap in wins[] to reorder the master list */
    wi = ws->tiled[ti] - ws->wins;
    wj = ws->tiled[si] - ws->wins;

    tmp = ws->wins[wi];
    ws->wins[wi] = ws->wins[wj];
    ws->wins[wj] = tmp;

    retile_deferred();

    ws->focused = ws->wins[wj].window;
    refocus(ws, &ws->wins[wj]);
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
        /* Monocle-hidden windows stay hidden until monocle exits */
        if (visible && ws->wins[i].monocle_hidden)
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

    ManagedWindow *cur;

    if (idx < 0 || idx >= NUM_WORKSPACES) return;
    if (idx == cur_ws) return;
    cur = focused_mw(ws);
    if (!cur) return;

    /* Copy the window to the stack — memmove below would invalidate any
       pointer into wins[], so we work from a local snapshot. */
    win = *cur;

    /* Remove from dwindle tree before memmove (leaf pointers would dangle) */
    layout_remove(ws, win.window);

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

    /* Clamp floating windows to virtual screen bounds */
    if (win.is_floating) {
        if (win.x < 0) win.x = 0;
        if (win.y < 0) win.y = 0;
        if (win.x + win.width > scrw) win.x = scrw - win.width;
        if (win.y + win.height > scrh) win.y = scrh - win.height;
    }

    target->wins[target->nwin++] = win;
    target->focused = win.window;

    rebuild_tiled(target);

    /* Insert into dwindle tree on target workspace if in dwindle mode */
    layout_insert(target, win.window);

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
        if (states[i] == atom_net_wm_state_full) {
            mw->is_fullscreen = 1;
        } else if (states[i] == atom_net_wm_state_above) {
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

    if (ws->focused != None) {
        for (i = 0; i < ws->nwin; i++) {
            if (ws->wins[i].window == ws->focused) {
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

    /* Read ICCCM WM_HINTS — default input_hint to 1 (accept focus) */
    mw.input_hint = 1;
    {
        XWMHints *hints = XGetWMHints(dpy, w);
        if (hints) {
            if (hints->flags & InputHint)
                mw.input_hint = hints->input;
            XFree(hints);
        }
    }

    /* Center floating windows on current monitor */
    if (mw.is_floating && !mw.is_fullscreen) {
        Monitor *mon = curmon();
        mw.width = mw.width > mon->width ? mon->width : mw.width;
        mw.height = mw.height > mon->height ? mon->height : mw.height;
        mw.x = mon->x + (mon->width - mw.width) / 2;
        mw.y = mon->y + (mon->height - mw.height) / 2;
    }

    insert_idx = insert_into_workspace(ws, mw);
    if (insert_idx == -1) err(1, "wins_ensure_cap");

    rebuild_tiled(ws);

    /* In dwindle mode, insert tiled windows into dwindle tree */
    if (!mw.is_floating && !mw.is_fullscreen) {
        layout_insert(ws, w);
    }

    XSelectInput(dpy, w, EnterWindowMask | StructureNotifyMask | PropertyChangeMask);
    XSetWindowBorderWidth(dpy, w, BORDER_WIDTH);

    /* Position floating windows before mapping to avoid one-frame jump */
    if (mw.is_floating && !mw.is_fullscreen) {
        XMoveResizeWindow(dpy, w, mw.x, mw.y, mw.width, mw.height);
        XRaiseWindow(dpy, w);
    }

    refocus(ws, &ws->wins[insert_idx]);

    /* Spawning is explicit: keep a born-fullscreen window on top
       (floating is already raised above and ordered by retile) */
    if (mw.is_fullscreen)
        XRaiseWindow(dpy, w);

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
                layout_remove(ws, w);
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

/* Move focus to the next/previous window in wins[] order, wrapping at
   edges.  Tiled and floating windows are both cycled (floating was
   previously keyboard-unreachable).  In horizontal mode the camera is
   updated to keep the focused column visible. */
void
focus_cycle(int delta)
{
    Workspace *ws = active_ws();
    Monitor *mon = curmon();
    int idx = -1, i, steps;

    /* In dwindle mode, cycle in tree-visual order (see layout.c) */
    if (!mon->horizontal_mode && ws->dwindle_root) {
        dwindle_focus_cycle(ws, delta);
        return;
    }

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == ws->focused) {
            idx = i;
            break;
        }
    }

    /* Walk in the given direction, skipping non-candidates */
    i = idx;
    for (steps = 0; steps < ws->nwin; steps++) {
        i += delta;
        if (i < 0) i = ws->nwin - 1;
        else if (i >= ws->nwin) i = 0;
        if (focus_candidate(&ws->wins[i]))
            break;
    }
    if (steps >= ws->nwin || ws->nwin == 0) return;

    refocus(ws, &ws->wins[i]);
    /* Keyboard focus is explicit: raise floating/fullscreen targets */
    if ((ws->wins[i].is_floating || ws->wins[i].is_fullscreen)
        && window_exists(ws->wins[i].window))
        XRaiseWindow(dpy, ws->wins[i].window);

    if (mon->horizontal_mode)
        update_camera_ws(curws());
}

/* ---- client messages ---- */

void
handle_client_message(XClientMessageEvent *e)
{
    Workspace *ws = active_ws();
    ManagedWindow *mw = NULL;
    int i, j;

    if (e->message_type == atom_net_wm_state) {
        /* _NET_WM_STATE: data.l[0] = action, data.l[1]/[2] = property atoms */
        int action = e->data.l[0];
        Atom prop1 = e->data.l[1];
        Atom prop2 = e->data.l[2];

        /* Find managed window */
        for (j = 0; j < NUM_WORKSPACES + 1; j++) {
            for (i = 0; i < spaces[j].nwin; i++) {
                if (spaces[j].wins[i].window == e->window) {
                    mw = &spaces[j].wins[i];
                    break;
                }
            }
            if (mw) break;
        }
        if (!mw) return;

        /* Handle fullscreen toggle */
        if (prop1 == atom_net_wm_state_full ||
            prop2 == atom_net_wm_state_full) {
            int want_fs = 0;
            if (action == 2)       /* TOGGLE */
                want_fs = !mw->is_fullscreen;
            else if (action == 1)  /* ADD */
                want_fs = 1;
            else if (action == 0)  /* REMOVE */
                want_fs = 0;
            else
                return;

            if (want_fs == mw->is_fullscreen)
                return;

            if (ws->focused != mw->window)
                refocus(ws, mw);
            toggle_fullscreen();
            return;
        }
    }

    /* _NET_ACTIVE_WINDOW: focus request */
    if (e->message_type == atom_net_active_window) {
        for (j = 0; j < NUM_WORKSPACES + 1; j++) {
            for (i = 0; i < spaces[j].nwin; i++) {
                if (spaces[j].wins[i].window == e->window) {
                    mw = &spaces[j].wins[i];
                    break;
                }
            }
            if (mw) break;
        }
        if (!mw) return;

        if (mw->workspace != cur_ws && !mw->is_sticky)
            switch_workspace((void *)(long)mw->workspace);
        if (ws->focused != mw->window)
            refocus(ws, mw);
        /* Pager/taskbar focus requests are explicit: raise floaters */
        if ((mw->is_floating || mw->is_fullscreen) && window_exists(mw->window))
            XRaiseWindow(dpy, mw->window);
        return;
    }

    /* _NET_CLOSE_WINDOW */
    if (e->message_type == atom_net_close) {
        unmanage_window(e->window, 0);
        return;
    }
}

/* ---- close/quit ---- */

void
close_window(void)
{
    Workspace *ws = active_ws();
    ManagedWindow *w = focused_mw(ws);
    XEvent ev;
    Atom *protocols = NULL;
    int nprotocols = 0;
    int supports_delete = 0;
    int i;

    if (!w) return;

    /* Check if window supports WM_DELETE_WINDOW */
    if (XGetWMProtocols(dpy, w->window, &protocols, &nprotocols)) {
        for (i = 0; i < nprotocols; i++) {
            if (protocols[i] == atom_wm_delete) {
                supports_delete = 1;
                break;
            }
        }
        XFree(protocols);
    }

    if (supports_delete) {
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w->window;
        ev.xclient.message_type = atom_wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = atom_wm_delete;
        XSendEvent(dpy, w->window, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, w->window);
    }
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
    ManagedWindow *w = focused_mw(ws);

    if (!w) return;

    w->is_fullscreen = !w->is_fullscreen;

    if (w->is_fullscreen) {
        tiled_remove(ws, w->window);
        layout_remove(ws, w->window);
    } else if (!w->pre_fs_floating) {
        /* Was tiled before fullscreen: re-insert into layout.
           Previously-floating windows must NOT be added to tiled[]. */
        tiled_add(ws, w);
        layout_insert(ws, w->window);
    }

    if (w->is_fullscreen) {
        Monitor *mon = curmon();
        w->pre_fs_floating = w->is_floating;

        if (w->is_floating) {
            w->pre_fs_x = w->x;
            w->pre_fs_y = w->y;
            w->pre_fs_w = w->width;
            w->pre_fs_h = w->height;
        }

        w->is_floating = 1;
        w->x = mon->x;
        w->y = mon->y;
        w->width = mon->width;
        w->height = mon->height;
        XSetWindowBorderWidth(dpy, w->window, 0);
        XMoveResizeWindow(dpy, w->window, mon->x, mon->y,
                          mon->width, mon->height);
        XRaiseWindow(dpy, w->window);

        /* Build _NET_WM_STATE with fullscreen + existing flags */
        {
            Atom states[4];
            int nstates = 0;
            states[nstates++] = atom_net_wm_state_full;
            if (w->is_above)    states[nstates++] = atom_net_wm_state_above;
            if (w->is_sticky)   states[nstates++] = atom_net_wm_state_sticky;
            if (w->is_not_focusable) states[nstates++] = atom_net_wm_state_not_focusable;
            XChangeProperty(dpy, w->window, atom_net_wm_state, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)states, nstates);
        }
    } else {
        w->is_floating = w->pre_fs_floating;
        XSetWindowBorderWidth(dpy, w->window, BORDER_WIDTH);

        /* Restore floating geometry or let retile recompute for tiled */
        if (w->pre_fs_floating) {
            w->x = w->pre_fs_x;
            w->y = w->pre_fs_y;
            w->width = w->pre_fs_w;
            w->height = w->pre_fs_h;
            XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);
        }

        /* Rebuild _NET_WM_STATE, preserving above/sticky/not_focusable */
        {
            Atom states[4];
            int nstates = 0;
            if (w->is_above)    states[nstates++] = atom_net_wm_state_above;
            if (w->is_sticky)   states[nstates++] = atom_net_wm_state_sticky;
            if (w->is_not_focusable) states[nstates++] = atom_net_wm_state_not_focusable;
            if (nstates)
                XChangeProperty(dpy, w->window, atom_net_wm_state, XA_ATOM, 32,
                                PropModeReplace, (unsigned char *)states, nstates);
            else
                XDeleteProperty(dpy, w->window, atom_net_wm_state);
        }

        retile_deferred();
    }
}

void
toggle_float(void)
{
    Workspace *ws = active_ws();
    ManagedWindow *w = focused_mw(ws);
    Monitor *mon = curmon();
    int i;

    if (!w) return;
    if (w->is_fullscreen) return;
    if (w->is_above || w->is_sticky) return;  /* always floating */

    if (!w->is_floating) {
        /* Save tiled geometry and position for snap-back */
        int idx = -1;
        for (i = 0; i < ws->ntiled; i++) {
            if (ws->tiled[i]->window == w->window) { idx = i; break; }
        }
        w->pre_float_idx = idx;

        tiled_remove(ws, w->window);
        layout_remove(ws, w->window);
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
        layout_insert(ws, w->window);

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
    ManagedWindow *cur;
    int i, found = 0;

    if (src == dst) return;
    cur = focused_mw(src);
    if (!cur) return;

    win = *cur;

    /* Remove from dwindle tree before memmove (leaf pointers would dangle) */
    layout_remove(src, win.window);

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

    {
        ManagedWindow *cur = focused_mw(ws);
        if (cur) {
            refocus(ws, cur);
        } else if (ws->nwin > 0) {
            ws->focused = ws->wins[0].window;
            refocus(ws, &ws->wins[0]);
        }
    }
}

/* Hide the scratchpad overlay: unmap windows, destroy dim, restore focus. */
static void
scratchpad_hide(void)
{
    Workspace *under = &spaces[cur_ws];
    Monitor *mon = curmon();

    scratch_unmap_all();
    scratch_visible = 0;

    scratchpad_destroy_dim(mon);

    /* Restore focus to the underlying workspace */
    if (scratch_saved_focus != None) {
        ManagedWindow *saved = find_mw(under, scratch_saved_focus);
        if (saved)
            refocus(under, saved);
        else if (under->nwin > 0)
            refocus(under, &under->wins[0]);
        else
            under->focused = None;
        scratch_saved_focus = None;
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

/* ---- X event handlers ---- */

static void
grab_mouse(ManagedWindow *mw, int resizing, XButtonEvent *e)
{
    mouse.active = 1;
    mouse.resizing = resizing;
    mouse.win = mw->window;
    mouse.start_x = e->x_root;
    mouse.start_y = e->y_root;
    if (resizing) {
        mouse.orig_w = mw->width;
        mouse.orig_h = mw->height;
    } else {
        mouse.orig_x = mw->x;
        mouse.orig_y = mw->y;
    }
    XGrabPointer(dpy, root, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, e->time);
}

void
handle_map_request(XMapRequestEvent *e)
{
    manage_window(e->window);
}

void
handle_destroy_notify(XDestroyWindowEvent *e)
{
    unmanage_window(e->window, 1);
}

void
handle_unmap_notify(XUnmapEvent *e)
{
    ManagedWindow *mw = find_mw_any(e->window);

    /* Windows hidden by monocle are still managed — ignore their unmaps */
    if (mw && mw->monocle_hidden)
        return;
    unmanage_window(e->window, 0);
}

void
handle_configure_request(XConfigureRequestEvent *e)
{
    ManagedWindow *mw = find_mw_any(e->window);

    if (mw && !mw->is_floating && !mw->is_fullscreen) {
        /* Tiled: honor geometry changes but block stacking.
           Send synthetic ConfigureNotify so client knows its real geometry. */
        int mask = e->value_mask & ~(CWSibling | CWStackMode);
        if (mask) {
            XWindowChanges wc;
            wc.x = e->x;
            wc.y = e->y;
            wc.width = e->width;
            wc.height = e->height;
            wc.border_width = e->border_width;
            XConfigureWindow(dpy, e->window, mask, &wc);
        }

        XConfigureEvent ce;
        memset(&ce, 0, sizeof(ce));
        ce.type = ConfigureNotify;
        ce.event = e->window;
        ce.window = e->window;
        ce.above = None;
        ce.x = mw->x;
        ce.y = mw->y;
        ce.width = mw->width;
        ce.height = mw->height;
        ce.border_width = BORDER_WIDTH;
        ce.override_redirect = False;
        XSendEvent(dpy, e->window, False, StructureNotifyMask, (XEvent *)&ce);
        return;
    }

    /* Floating/unmanaged: honor all fields including stacking */
    XWindowChanges wc;
    wc.x = e->x;
    wc.y = e->y;
    wc.width = e->width;
    wc.height = e->height;
    wc.border_width = e->border_width;
    wc.sibling = e->above;
    wc.stack_mode = e->detail;
    XConfigureWindow(dpy, e->window, e->value_mask, &wc);

    /* Sync managed state after proxying floating configure */
    if (mw) {
        if (e->value_mask & CWX)      mw->x = e->x;
        if (e->value_mask & CWY)      mw->y = e->y;
        if (e->value_mask & CWWidth)  mw->width = e->width;
        if (e->value_mask & CWHeight) mw->height = e->height;
    }
}

void
handle_enter_notify(XCrossingEvent *e)
{
    Workspace *ws = active_ws();
    int i;

    /* Never steal focus mid-drag — the drag target keeps focus */
    if (mouse.active) return;

    if (e->mode != NotifyNormal || e->detail == NotifyInferior) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            if (ws->wins[i].is_not_focusable) break;
            refocus(ws, &ws->wins[i]);
            {
                Monitor *mon = curmon();
                if (!mon->horizontal_mode && ws->dwindle_root)
                    dwindle_set_focus(ws, e->window);
            }
            break;
        }
    }
}

/* Re-focus the tracked window if focus drifts to root or an unmanaged window.
   Prevents focus stealing and handles clients that expect the WM to maintain
   focus on the active window. */
void
handle_focus_in(XFocusInEvent *e)
{
    Workspace *ws = active_ws();

    if (e->detail == NotifyInferior || e->detail == NotifyPointer
        || e->detail == NotifyPointerRoot)
        return;

    /* If focus went to root, re-focus the active window */
    if (e->window == root && ws->focused != None && ws->focused != root) {
        ManagedWindow *cur = focused_mw(ws);
        if (cur)
            refocus(ws, cur);
        else
            ws->focused = None;
    }
}

/* Keyboard mapping changed (layout added/removed) — re-grab keys and
   buttons so bindings keep working without a restart. */
void
handle_mapping_notify(XMappingEvent *e)
{
    (void)e;
    grab_keys();
}

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
            case FOCUS_NEXT:         focus_cycle(1); break;
            case FOCUS_PREV:         focus_cycle(-1); break;
            case SWAP_NEXT:          swap_impl(1); break;
            case SWAP_PREV:          swap_impl(-1); break;
            case RESIZE_MASTER:      resize_master((void *)(long)keys[i].arg.i); break;
            case RESIZE_WINDOW:      resize_window((void *)(long)keys[i].arg.i); break;
            case SCROLL_LEFT:        move_horizontal(0); break;
            case SCROLL_RIGHT:       move_horizontal(1); break;
            case TOGGLE_LAYOUT:      toggle_layout(); break;
            case TOGGLE_FULLSCREEN:  toggle_fullscreen(); break;
            case TOGGLE_FLOAT:       toggle_float(); break;
            case TOGGLE_SCRATCHPAD:  toggle_scratchpad(); break;
            case MOVE_TO_SCRATCHPAD: move_to_scratchpad(); break;
            case FIT_WINDOW:         if (fit_window()) toggle_fullscreen(); break;
            case TOGGLE_CENTER_FOCUS: toggle_center_focus(); break;
            case TOGGLE_MONOCLE:      toggle_monocle(); break;
            case SWITCH_WORKSPACE:   switch_workspace((void *)(long)keys[i].arg.i); break;
            case MOVE_TO_WORKSPACE:  move_to_workspace((void *)(long)keys[i].arg.i); break;
            case FOCUS_MONITOR:      focus_monitor((void *)(long)keys[i].arg.i); break;
            }
            break;
        }
    }
}

void
handle_button_press(XButtonEvent *e)
{
    Workspace *ws = active_ws();
    int i;

    if (!(e->state & Mod4Mask)) return;

    if (e->button == Button3) {
        for (i = ws->nwin - 1; i >= 0; i--) {
            ManagedWindow *mw = &ws->wins[i];
            if (!mw->is_floating || mw->is_fullscreen) continue;
            if (e->x_root >= mw->x && e->x_root < mw->x + mw->width
                && e->y_root >= mw->y && e->y_root < mw->y + mw->height) {
                if (ws->focused != mw->window) refocus(ws, mw);
                XRaiseWindow(dpy, mw->window);
                grab_mouse(mw, 1, e);
                return;
            }
        }
        return;
    }

    if (e->button != Button1) return;

    for (i = ws->nwin - 1; i >= 0; i--) {
        ManagedWindow *mw = &ws->wins[i];
        if (!mw->is_floating || mw->is_fullscreen) continue;
        if (e->x_root >= mw->x && e->x_root < mw->x + mw->width
            && e->y_root >= mw->y && e->y_root < mw->y + mw->height) {
            if (ws->focused != mw->window) refocus(ws, mw);
            XRaiseWindow(dpy, mw->window);
            grab_mouse(mw, 0, e);
            return;
        }
    }
}

void
handle_button_release(XButtonEvent *e)
{
    (void)e;

    mouse.active = 0;
    mouse.resizing = 0;
    mouse.win = None;
    XUngrabPointer(dpy, CurrentTime);
}

void
handle_motion_notify(XMotionEvent *e)
{
    ManagedWindow *mw;
    int dx, dy;

    if (!mouse.active || mouse.win == None) return;

    /* Resolve the drag target by ID — wins[] may have moved since grab */
    mw = find_mw_any(mouse.win);
    if (!mw) {
        mouse.active = 0;
        mouse.resizing = 0;
        mouse.win = None;
        XUngrabPointer(dpy, CurrentTime);
        return;
    }

    dx = e->x_root - mouse.start_x;
    dy = e->y_root - mouse.start_y;

    if (mouse.resizing) {
        XEvent ev;
        while (XCheckMaskEvent(dpy, PointerMotionMask, &ev)) {
            if (ev.type == MotionNotify) {
                dx = ev.xmotion.x_root - mouse.start_x;
                dy = ev.xmotion.y_root - mouse.start_y;
            }
        }
        int nw = mouse.orig_w + dx;
        int nh = mouse.orig_h + dy;
        if (nw < MIN_WIN_DIM) nw = MIN_WIN_DIM;
        if (nh < MIN_WIN_DIM) nh = MIN_WIN_DIM;
        mw->width = nw;
        mw->height = nh;
        XMoveResizeWindow(dpy, mw->window, mw->x, mw->y, nw, nh);
        return;
    }

    mw->x = mouse.orig_x + dx;
    mw->y = mouse.orig_y + dy;
    XMoveResizeWindow(dpy, mw->window, mw->x, mw->y, mw->width, mw->height);
}

void
handle_property_notify(XPropertyEvent *e)
{
    Workspace *ws;
    ManagedWindow *mw = NULL;
    int i, j;

    if (e->atom != atom_net_wm_state) return;

    for (j = 0; j < NUM_WORKSPACES + 1; j++) {
        ws = &spaces[j];
        for (i = 0; i < ws->nwin; i++) {
            if (ws->wins[i].window == e->window) {
                mw = &ws->wins[i];
                break;
            }
        }
        if (mw) break;
    }
    if (!mw) return;

    Atom actual;
    int fmt;
    unsigned long n, remain;
    unsigned char *data = NULL;
    int was_above = mw->is_above;
    int was_sticky = mw->is_sticky;
    int was_fullscreen = mw->is_fullscreen;

    mw->is_above = 0;
    mw->is_sticky = 0;
    mw->is_not_focusable = 0;
    mw->is_fullscreen = 0;

    if (XGetWindowProperty(dpy, e->window, atom_net_wm_state, 0, 32, False,
                           XA_ATOM, &actual, &fmt, &n, &remain,
                           &data) == Success && data) {
        if (actual == XA_ATOM && fmt == 32) {
            Atom *states = (Atom *)data;
            unsigned long si;
            for (si = 0; si < n; si++) {
                if (states[si] == atom_net_wm_state_full)
                    mw->is_fullscreen = 1;
                else if (states[si] == atom_net_wm_state_above)
                    mw->is_above = 1;
                else if (states[si] == atom_net_wm_state_sticky)
                    mw->is_sticky = 1;
                else if (states[si] == atom_net_wm_state_not_focusable)
                    mw->is_not_focusable = 1;
            }
        }
        XFree(data);
    }

    if (mw->is_above || mw->is_sticky)
        mw->is_floating = 1;

    if (mw->is_above && !was_above)
        XRaiseWindow(dpy, mw->window);

    if (mw->is_sticky && !was_sticky) {
        for (j = 0; j < NUM_WORKSPACES + 1; j++) {
            if (j == mw->workspace) continue;
            XMapWindow(dpy, mw->window);
        }
    }

    if (!mw->is_sticky && was_sticky) {
        for (j = 0; j < NUM_WORKSPACES + 1; j++) {
            if (j == cur_ws) continue;
            if (j == mw->workspace) continue;
            XUnmapWindow(dpy, mw->window);
        }
    }

    /* Handle fullscreen state change from client */
    if (mw->is_fullscreen != was_fullscreen) {
        Workspace *aws = active_ws();
        if (aws->focused != mw->window)
            refocus(aws, mw);
        toggle_fullscreen();
        return;
    }

    if (mw->is_above != was_above || mw->is_sticky != was_sticky)
        retile_deferred();
}

