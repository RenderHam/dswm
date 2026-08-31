#include "dswm.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
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

/* globals */
Display *dpy;
Window root;
int screen;
int scrw, scrh;
int running = 1;
int cur_ws;

Monitor mons[MAX_MONS];
int nmons;
Workspace spaces[NUM_WORKSPACES];

/* workspace helpers */

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

/* monitor init */

static void
monitors_init(void)
{
#ifdef __OpenBSD__
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
            if (nmons > MAX_MONS) nmons = MAX_MONS;
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

/* init / run / cleanup */

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

    for (int i = 0; i < NUM_WORKSPACES; i++) {
        spaces[i].wins = NULL;
        spaces[i].nwin = 0;
        spaces[i].cap = 0;
        spaces[i].focused = NULL;
        spaces[i].scroll_offset = 0;
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

/* main */

int
main(void)
{
    init();
    run();
    cleanup();
    return 0;
}
