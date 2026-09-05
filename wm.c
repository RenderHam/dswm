#include "dswm.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <err.h>

#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

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
    Workspace *ws = curws();
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

    tile_horizontal();
}

/* ---- swap ---- */

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
    XFlush(dpy);
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
    if (target->nwin >= target->cap) {
        int newcap = target->cap ? target->cap * 2 : INITIAL_CAP;
        ManagedWindow *tmp = realloc(target->wins, newcap * sizeof(ManagedWindow));
        if (!tmp) err(1, "realloc");
        target->wins = tmp;
        target->cap = newcap;
    }
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

void
manage_window(Window w)
{
    Workspace *ws = curws();
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
        if (!tmp) err(1, "realloc");
        ws->wins = tmp;
        ws->cap = newcap;
    }

    int insert_idx = ws->nwin;
    ws->wins[ws->nwin++] = mw;

    rebuild_tiled(ws);

    XSelectInput(dpy, w, EnterWindowMask | StructureNotifyMask | PropertyChangeMask);
    XSetWindowBorderWidth(dpy, w, BORDER_WIDTH);
    refocus(ws, &ws->wins[insert_idx]);

    if (mw.workspace == cur_ws)
        XMapWindow(dpy, w);

    if (curmon()->horizontal_mode)
        tile_horizontal();
    else
        tile_windows();
}

void
unmanage_window(Window w, int force)
{
    int i, j;
    int lo = force ? 0 : cur_ws;
    int hi = force ? NUM_WORKSPACES : cur_ws + 1;

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

void
focus_next(void)
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

    target = (cur_tiled == -1) ? 0 : cur_tiled + 1;
    if (target >= ntiled) return;

    int count = 0;
    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].monitor != mon->id || ws->wins[i].is_floating) continue;
        if (count == target) {
            refocus(ws, &ws->wins[i]);
            break;
        }
        count++;
    }

    if (mon->horizontal_mode)
        tile_horizontal();
}

void
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

    if (mon->horizontal_mode)
        tile_horizontal();
}

/* ---- close/quit ---- */

void
close_window(void)
{
    Workspace *ws = curws();
    XEvent ev;

    if (!ws->focused) return;

    memset(&ev, 0, sizeof(ev));
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
    Workspace *ws = curws();
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

    XFlush(dpy);
}

void
toggle_float(void)
{
    Workspace *ws = curws();
    ManagedWindow *w = ws->focused;
    if (!w) return;

    w->is_floating = !w->is_floating;

    if (w->is_floating)
        tiled_remove(ws, w->window);
    else
        tiled_add(ws, w);

    if (w->is_floating) {
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
    unmanage_window(e->window, 0);
}

void
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

void
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
            case RESIZE_WINDOW:      resize_window((void *)(long)keys[i].arg.i); break;
            case SCROLL_LEFT:        move_horizontal(0); break;
            case SCROLL_RIGHT:       move_horizontal(1); break;
            case TOGGLE_LAYOUT:      toggle_layout(); break;
            case TOGGLE_FULLSCREEN:  toggle_fullscreen(); break;
            case TOGGLE_FLOAT:       toggle_float(); break;
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

void
handle_button_press(XButtonEvent *e)
{
    if ((e->state & (Mod4Mask | ShiftMask)) == (Mod4Mask | ShiftMask)) {
        if (e->button == Button4)
            move_horizontal(0);
        else if (e->button == Button5)
            move_horizontal(1);
    }
}
