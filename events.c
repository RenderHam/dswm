#include "dswm.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

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
            case SCROLL_LEFT:        move_horizontal(0); break;
            case SCROLL_RIGHT:       move_horizontal(1); break;
            case TOGGLE_LAYOUT:      toggle_layout(); break;
            case TOGGLE_FULLSCREEN:  toggle_fullscreen(); break;
            case TOGGLE_FLOAT:       toggle_float(); break;
            case SWITCH_WORKSPACE:   switch_workspace((void *)(long)keys[i].arg.i); break;
            case MOVE_TO_WORKSPACE:  move_to_workspace((void *)(long)keys[i].arg.i); break;
            case FOCUS_MONITOR:      focus_monitor((void *)(long)keys[i].arg.i); break;
            case SET_SCROLL_VISIBLE: set_scroll_visible((void *)(long)keys[i].arg.i); break;
            case INCR_SCROLL_VISIBLE: adjust_scroll_visible(1); break;
            case DECR_SCROLL_VISIBLE: adjust_scroll_visible(-1); break;
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
