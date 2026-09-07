/* events.c — X event handlers.
   Receives raw X events from the main loop and dispatches them to the
   appropriate window management or layout functions.  Extracted from wm.c
   to separate event processing from window management logic. */

#include "dswm.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ---- mouse grab helper ---- */

static void
grab_mouse(ManagedWindow *mw, int resizing, XButtonEvent *e)
{
    mouse.active = 1;
    mouse.resizing = resizing;
    mouse.win = mw;
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

/* EnterNotify: pointer entered a managed window — refocus it.
   _NET_WM_STATE_NOT_FOCUSABLE windows are skipped. */
void
handle_enter_notify(XCrossingEvent *e)
{
    Workspace *ws = active_ws();
    int i;

    if (e->mode != NotifyNormal || e->detail == NotifyInferior) return;

    for (i = 0; i < ws->nwin; i++) {
        if (ws->wins[i].window == e->window) {
            if (ws->wins[i].is_not_focusable) break;
            refocus(ws, &ws->wins[i]);
            /* Update dwindle focus if in dwindle mode */
            {
                Monitor *mon = curmon();
                if (!mon->horizontal_mode && ws->dwindle_root)
                    dwindle_set_focus(ws, e->window);
            }
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

/* ButtonPress: Super+Button1 moves/drag-swaps, Super+Button3 resizes.
   For tiled windows (horizontal mode) Button1 swaps columns; for
   floating windows Button1 moves freely; Button3 on the bottom-right
   corner (RESIZE_HANDLE_PX hotspot) live-resizes the floating window. */
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
            /* RESIZE_HANDLE_PX square at bottom-right for resize handle */
            int rx = mw->x + mw->width - RESIZE_HANDLE_PX;
            int ry = mw->y + mw->height - RESIZE_HANDLE_PX;
            if (e->x_root >= rx && e->x_root < mw->x + mw->width
                && e->y_root >= ry && e->y_root < mw->y + mw->height) {
                grab_mouse(mw, 1, e);
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
                grab_mouse(mw, 1, e);
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
                grab_mouse(mw, 0, e);
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
            grab_mouse(mw, 0, e);
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
        /* Sync dwindle tree: swap the two nodes in the tree by
           removing both and re-inserting in the new order. */
        {
            Monitor *mon = curmon();
            if (!mon->horizontal_mode && ws->dwindle_root) {
                dwindle_remove(ws, a->window);
                dwindle_remove(ws, b->window);
                dwindle_insert(ws, b->window);
                dwindle_insert(ws, a->window);
            }
        }
    }

done:
    mouse.active = 0;
    mouse.resizing = 0;
    mouse.win = NULL;
    XUngrabPointer(dpy, CurrentTime);
    if (ws == &spaces[SCRATCHPAD_IDX])
        retile_ws(&spaces[SCRATCHPAD_IDX]);
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

/* PropertyNotify: a window's property changed.  We care about
   _NET_WM_STATE changes to update above/sticky/not-focusable flags. */
void
handle_property_notify(XPropertyEvent *e)
{
    Workspace *ws;
    ManagedWindow *mw = NULL;
    int i, j;

    if (e->atom != atom_net_wm_state) return;

    /* Find the managed window across all workspaces */
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

    /* Re-read _NET_WM_STATE and update flags */
    Atom actual;
    int fmt;
    unsigned long n, remain;
    unsigned char *data = NULL;
    int was_above = mw->is_above;
    int was_sticky = mw->is_sticky;

    mw->is_above = 0;
    mw->is_sticky = 0;
    mw->is_not_focusable = 0;

    if (XGetWindowProperty(dpy, e->window, atom_net_wm_state, 0, 32, False,
                           XA_ATOM, &actual, &fmt, &n, &remain,
                           &data) == Success && data) {
        if (actual == XA_ATOM && fmt == 32) {
            Atom *states = (Atom *)data;
            unsigned long si;
            for (si = 0; si < n; si++) {
                if (states[si] == atom_net_wm_state_above)
                    mw->is_above = 1;
                else if (states[si] == atom_net_wm_state_sticky)
                    mw->is_sticky = 1;
                else if (states[si] == atom_net_wm_state_not_focusable)
                    mw->is_not_focusable = 1;
            }
        }
        XFree(data);
    }

    /* Force floating for above/sticky */
    if (mw->is_above || mw->is_sticky)
        mw->is_floating = 1;

    /* If above just turned on, raise immediately */
    if (mw->is_above && !was_above)
        XRaiseWindow(dpy, mw->window);

    /* If sticky just turned on, map on all other workspaces */
    if (mw->is_sticky && !was_sticky) {
        for (j = 0; j < NUM_WORKSPACES + 1; j++) {
            if (j == mw->workspace) continue;
            XMapWindow(dpy, mw->window);
        }
    }

    /* If sticky just turned off, unmap from non-current workspaces */
    if (!mw->is_sticky && was_sticky) {
        for (j = 0; j < NUM_WORKSPACES + 1; j++) {
            if (j == cur_ws) continue;
            if (j == mw->workspace) continue;
            XUnmapWindow(dpy, mw->window);
        }
    }

    /* Retile to update stacking/layout if above state changed */
    if (mw->is_above != was_above || mw->is_sticky != was_sticky)
        retile_deferred();
}
