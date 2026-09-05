#include "dswm.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <err.h>

#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

/* ---- globals (owned by this file) ---- */

Display *dpy;
Window root;
int scrw, scrh;
int running;
int cur_ws;
Monitor mons[8];
int nmons;
Workspace spaces[NUM_WORKSPACES];

/* cached atoms */
Atom atom_wm_delete;
Atom atom_wm_protocols;
Atom atom_net_wm_strut;
Atom atom_net_wm_state;
Atom atom_net_wm_state_full;
Atom atom_net_current_desktop;
Atom atom_net_supported;
Atom atom_net_number_of_desktops;
Atom atom_net_active_window;
Atom atom_net_wm_name;
Atom atom_net_wm_window_type;
Atom atom_net_wm_type_desktop;
Atom atom_net_wm_type_dock;
Atom atom_net_wm_type_splash;
Atom atom_motif_wm_hints;

/* ---- X error handler ---- */

static int
xerror(Display *d, XErrorEvent *ee)
{
    (void)d; (void)ee;
    return 0;
}

/* ---- EWMH support ---- */

void
update_ewmh_current_desktop(void)
{
    long desktop = cur_ws;
    XChangeProperty(dpy, root, atom_net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);
    XFlush(dpy);
}

void
setup_ewmh(void)
{
    long num_desktops = NUM_WORKSPACES;

    XChangeProperty(dpy, root, atom_net_number_of_desktops, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&num_desktops, 1);

    XChangeProperty(dpy, root, atom_net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)(Atom[]){
                        atom_net_wm_strut,
                        atom_net_wm_state,
                        atom_net_current_desktop,
                        atom_net_number_of_desktops,
                        atom_net_active_window,
                        atom_net_wm_name,
                        atom_net_wm_window_type,
                        atom_net_wm_type_desktop,
                        atom_net_wm_type_dock,
                        atom_net_wm_type_splash,
                    }, 10);

    update_ewmh_current_desktop();
    XDeleteProperty(dpy, root, atom_net_active_window);
}

/* ---- atom caching ---- */

void
cache_atoms(void)
{
    atom_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    atom_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    atom_net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_net_wm_state_full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    atom_net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    atom_net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    atom_net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_net_wm_type_desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    atom_net_wm_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    atom_net_wm_type_splash = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    atom_motif_wm_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
}

/* ---- key grabbing ---- */

void
grab_keys(void)
{
    unsigned int mod4_variants[] = {0, LockMask, Mod2Mask, LockMask|Mod2Mask};
    unsigned int all_mods[] = {
        0, Mod4Mask, LockMask, Mod2Mask, LockMask|Mod2Mask,
        Mod4Mask|LockMask, Mod4Mask|Mod2Mask, Mod4Mask|LockMask|Mod2Mask,
    };
    size_t i, j;

    XUngrabKey(dpy, AnyKey, AnyModifier, root);

    for (i = 0; i < NELEM(keys); i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].sym);
        if (!code) continue;

        if (keys[i].mod == MODKEY) {
            for (j = 0; j < NELEM(mod4_variants); j++)
                XGrabKey(dpy, code, keys[i].mod | mod4_variants[j],
                         root, True, GrabModeAsync, GrabModeAsync);
        } else if (keys[i].mod == (MODKEY | SHTKEY)) {
            for (j = 0; j < NELEM(mod4_variants); j++)
                XGrabKey(dpy, code, keys[i].mod | mod4_variants[j],
                         root, True, GrabModeAsync, GrabModeAsync);
        } else {
            for (j = 0; j < NELEM(all_mods); j++)
                XGrabKey(dpy, code, keys[i].mod | all_mods[j],
                         root, True, GrabModeAsync, GrabModeAsync);
        }
    }

    XGrabButton(dpy, AnyButton, MODKEY | SHTKEY, root, True,
                ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
}

/* ---- monitors ---- */

void
monitors_init(void)
{
    nmons = 0;

#if USE_XINERAMA
    if (XineramaIsActive(dpy)) {
        XineramaScreenInfo *info = XineramaQueryScreens(dpy, &nmons);
        int i;
        if (info) {
            if (nmons > 8) nmons = 8;
            for (i = 0; i < nmons; i++) {
                mons[i].id = i;
                mons[i].x = info[i].x_org;
                mons[i].y = info[i].y_org;
                mons[i].width = info[i].width;
                mons[i].height = info[i].height;
                mons[i].current_workspace = i < NUM_WORKSPACES ? i : 0;
                mons[i].master_factor = 0.5f;
                mons[i].horizontal_mode = 1;
                mons[i].strut_valid = 0;
            }
            XFree(info);
        }
    }
#endif

    if (nmons == 0) {
        nmons = 1;
        mons[0].id = 0;
        mons[0].x = 0;
        mons[0].y = 0;
        mons[0].width = scrw;
        mons[0].height = scrh;
        mons[0].current_workspace = 0;
        mons[0].master_factor = 0.5f;
        mons[0].horizontal_mode = 1;
        mons[0].strut_valid = 0;
    }
}

/* ---- init / cleanup / run ---- */

static void
init(void)
{
    Workspace *ws;
    int i;

    dpy = XOpenDisplay(NULL);
    if (!dpy) errx(1, "cannot open display");

    XSetErrorHandler(xerror);

    root = DefaultRootWindow(dpy);
    scrw = DisplayWidth(dpy, DefaultScreen(dpy));
    scrh = DisplayHeight(dpy, DefaultScreen(dpy));
    cur_ws = 0;
    running = 1;

    cache_atoms();
    monitors_init();
    setup_ewmh();

    for (i = 0; i < NUM_WORKSPACES; i++) {
        ws = &spaces[i];
        ws->wins = NULL;
        ws->nwin = 0;
        ws->cap = 0;
        ws->focused = NULL;
        ws->cam_x = 0;
        ws->tiled = NULL;
        ws->ntiled = 0;
        ws->tiled_cap = 0;
    }

    grab_keys();
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask
                           | KeyPressMask | ButtonPressMask | PropertyChangeMask);

    signal(SIGCHLD, SIG_IGN);

    /* map existing windows */
    {
        Window rr, parent, *children = NULL;
        unsigned int nchildren = 0;
        if (XQueryTree(dpy, root, &rr, &parent, &children, &nchildren)) {
            for (i = (int)nchildren - 1; i >= 0; i--) {
                XWindowAttributes wa;
                if (XGetWindowAttributes(dpy, children[i], &wa)
                    && wa.map_state == IsViewable && !wa.override_redirect) {
                    manage_window(children[i]);
                }
            }
            if (children) XFree(children);
        }
    }

    XSync(dpy, False);
}

static void
cleanup(void)
{
    int i;
    for (i = 0; i < NUM_WORKSPACES; i++) {
        free(spaces[i].wins);
        free(spaces[i].tiled);
        spaces[i].wins = NULL;
        spaces[i].tiled = NULL;
    }
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);
    XCloseDisplay(dpy);
}

static void
run(void)
{
    XEvent ev;

    while (running && !XNextEvent(dpy, &ev)) {
        switch (ev.type) {
        case KeyPress:         handle_key_press(&ev.xkey); break;
        case ButtonPress:      handle_button_press(&ev.xbutton); break;
        case MapRequest:       handle_map_request(&ev.xmaprequest); break;
        case DestroyNotify:    handle_destroy_notify(&ev.xdestroywindow); break;
        case UnmapNotify:      handle_unmap_notify(&ev.xunmap); break;
        case ConfigureRequest: handle_configure_request(&ev.xconfigurerequest); break;
        case EnterNotify:      handle_enter_notify(&ev.xcrossing); break;
        }
        flush_retile();
    }
}

/* ---- main ---- */

int
main(void)
{
    init();
    run();
    cleanup();
    return 0;
}
