#include "dswm.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

/* show/hide workspace windows */

void
show_workspace(int idx)
{
    Workspace *ws = &spaces[idx];
    int i;

    for (i = 0; i < ws->nwin; i++) {
        if (idx == cur_ws)
            XMapWindow(dpy, ws->wins[i].window);
        else
            XUnmapWindow(dpy, ws->wins[i].window);
    }
}

/* switch workspace */

void
switch_workspace(void *arg)
{
    int idx = (int)(long)arg;
    int old_ws = cur_ws;

    if (idx < 0 || idx >= NUM_WORKSPACES) return;
    if (idx == old_ws) return;

    /* hide old workspace */
    show_workspace(old_ws);

    /* show new workspace */
    cur_ws = idx;
    show_workspace(idx);

    /* retile */
    tile();
    update_ewmh_current_desktop();
}

/* move focused window to another workspace */

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

    /* hide the moved window (it's on a non-active workspace now) */
    XUnmapWindow(dpy, win.window);

    ws->focused = NULL;
    tile();
}

/* manage a new window */

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

    /* only map if on the current workspace */
    if (mw.workspace == cur_ws)
        XMapWindow(dpy, w);

    tile();
}

/* unmanage a window */

void
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
    tile();
}

/* focus cycling */

void
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

void
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

/* focus monitor */

void
focus_monitor(void *arg)
{
    int mon_idx = (int)(long)arg;
    int old_ws = cur_ws;

    if (mon_idx < 0 || mon_idx >= nmons) return;

    int new_ws = mons[mon_idx].current_workspace;
    if (new_ws == old_ws) return;

    show_workspace(old_ws);
    cur_ws = new_ws;
    show_workspace(new_ws);
    tile();
    update_ewmh_current_desktop();
}

/* set scroll_visible */

void
set_scroll_visible(void *arg)
{
    int val = (int)(long)arg;
    Monitor *mon = curmon();
    Workspace *ws = curws();
    if (val < MIN_SCROLL_VIS) val = MIN_SCROLL_VIS;
    if (val > MAX_SCROLL_VIS) val = MAX_SCROLL_VIS;
    mon->scroll_windows_visible = val;
    ws->scroll_offset = 0;
    tile();
}

/* close window */

void
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

/* quit wm */

void
quit_wm(void)
{
    running = 0;
}

/* toggle fullscreen */

void
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
        XMoveResizeWindow(dpy, w->window, 0, 0, scrw, scrh);
        XRaiseWindow(dpy, w->window);

        XChangeProperty(dpy, w->window, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&net_wm_state_full, 1);
    } else {
        w->is_floating = w->pre_fs_floating;
        w->x = w->pre_fs_x;
        w->y = w->pre_fs_y;
        w->width = w->pre_fs_width;
        w->height = w->pre_fs_height;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);

        XChangeProperty(dpy, w->window, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)0, 0);

        tile();
    }

    XFlush(dpy);
}

/* toggle float */

void
toggle_float(void)
{
    Workspace *ws = curws();
    ManagedWindow *w = ws->focused;
    if (!w) return;

    w->is_floating = !w->is_floating;
    if (w->is_floating) {
        w->x = scrw / 2 - w->width / 2;
        w->y = scrh / 2 - w->height / 2;
        XMoveResizeWindow(dpy, w->window, w->x, w->y, w->width, w->height);
        XRaiseWindow(dpy, w->window);
    } else {
        tile();
    }

    XFlush(dpy);
}
