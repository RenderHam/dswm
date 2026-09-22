/* main.c — Entry point, X11 init, EWMH setup, atom caching, key grabbing,
   and the main event loop. Globals and command tables live here. */

#include "dswm.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XF86keysym.h>
#include <X11/extensions/Xinerama.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <err.h>

#define WS(n)                                                          \
        { MODKEY,         XK_##n, SWITCH_WORKSPACE,  { .i = n-1 } },   \
        { MODKEY|SHTKEY,  XK_##n, MOVE_TO_WORKSPACE, { .i = n-1 } }

/* ---- globals (owned by this file) ---- */

Display *dpy;
Window root;
int scrw, scrh;
int running;
int cur_ws;
Monitor mons[MAX_MONS];
int nmons;
Workspace spaces[NUM_WORKSPACES + 1];
MouseState mouse;
int scratch_visible;
Window scratch_saved_focus;

/* cached atoms */
Atom atom_wm_delete;
Atom atom_wm_protocols;
Atom atom_wm_take_focus;
Atom atom_net_wm_strut;
Atom atom_net_wm_state;
Atom atom_net_wm_state_full;
Atom atom_net_current_desktop;
Atom atom_net_supported;
Atom atom_net_number_of_desktops;
Atom atom_net_active_window;
Atom atom_net_wm_window_type;
Atom atom_net_wm_type_desktop;
Atom atom_net_wm_type_dock;
Atom atom_net_wm_type_splash;
Atom atom_net_wm_type_dialog;
Atom atom_net_wm_type_util;
Atom atom_net_wm_type_toolbar;
Atom atom_net_wm_type_notification;
Atom atom_net_wm_type_popup_menu;
Atom atom_net_wm_type_menu;
Atom atom_net_wm_state_above;
Atom atom_net_wm_state_sticky;
Atom atom_net_wm_state_not_focusable;
Atom atom_net_wm_state_hidden;
Atom atom_net_close;

/* ---- shell commands ---- */

const char *termcmd[]   = { "alacritty",  NULL };
const char *menucmd[]   = { "sh", "-c", "~/.config/rofi/launcher/launcher.sh", NULL };
const char *browsercmd[] = { "firefox",    NULL };

/* ---- xf86 commands ---- */

const char *vol_up[]      = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "2%+",  NULL };
const char *vol_down[]    = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "2%-",  NULL };
const char *vol_mute[]    = { "wpctl", "set-mute",   "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
const char *bright_up[]   = { "brightnessctl", "s", "2%+",  NULL };
const char *bright_down[] = { "brightnessctl", "s", "2%-",  NULL };
const char *power_off[]   = { "sh", "-c", "~/.config/rofi/power/power.sh", NULL };
const char *dim[]         = { "pkill", "-USR1", "redshift",  NULL };

/* ---- window rules ---- */

Rule rules[] = {
    { "pavucontrol",        1 },
    { "rofi",               1 },
    { "steam",              1 },
    { "steamwebhelper",     1 },
};

const size_t num_rules = sizeof(rules) / sizeof(rules[0]);

/* ---- keybindings ---- */

Key keys[] = {
    { MODKEY,           XK_Return, SPAWN,          { .v = termcmd  } },
    { MODKEY,           XK_r,      SPAWN,          { .v = menucmd  } },
    { MODKEY,           XK_b,      SPAWN,          { .v = browsercmd } },
    { MODKEY,           XK_i,      SPAWN,          { .v = dim } },
    { MODKEY,           XK_w,      CLOSE,          { 0 } },
    { MODKEY|SHTKEY,    XK_q,      QUIT,           { 0 } },
    { MODKEY,           XK_h,      FOCUS_PREV,     { 0 } },
    { MODKEY,           XK_l,      FOCUS_NEXT,     { 0 } },
    { MODKEY|SHTKEY,    XK_h,      SWAP_PREV,      { 0 } },
    { MODKEY|SHTKEY,    XK_l,      SWAP_NEXT,      { 0 } },
    { MODKEY|ControlMask, XK_h,    RESIZE_MASTER,  { .i = -RESIZE_STEP } },
    { MODKEY|ControlMask, XK_l,    RESIZE_MASTER,  { .i = +RESIZE_STEP } },
    { MODKEY|Mod1Mask,  XK_h,      RESIZE_WINDOW,  { .i = -1 } },
    { MODKEY|Mod1Mask,  XK_l,      RESIZE_WINDOW,  { .i = +1 } },
    { MODKEY,           XK_Left,   SCROLL_LEFT,    { 0 } },
    { MODKEY,           XK_Right,  SCROLL_RIGHT,   { 0 } },
    { MODKEY,           XK_t,      TOGGLE_LAYOUT,  { 0 } },
    { MODKEY,           XK_f,      TOGGLE_FULLSCREEN, { 0 } },
    { MODKEY,           XK_m,      FIT_WINDOW,     { 0 } },
    { MODKEY,           XK_c,      TOGGLE_CENTER_FOCUS, { 0 } },
    { MODKEY,           XK_s,      TOGGLE_FLOAT,   { 0 } },
    { MODKEY,           XK_comma,  FOCUS_MONITOR,  { .i = 0 } },
    { MODKEY,           XK_period, FOCUS_MONITOR,  { .i = 1 } },
    { MODKEY,           XK_slash,  FOCUS_MONITOR,  { .i = 2 } },
    { MODKEY,           XK_grave,  TOGGLE_SCRATCHPAD, { 0 } },
    { MODKEY|SHTKEY,    XK_grave,  MOVE_TO_SCRATCHPAD, { 0 } },
    { MODKEY|SHTKEY,    XK_m,      TOGGLE_MONOCLE,   { 0 } },
    WS(1), WS(2), WS(3), WS(4), WS(5), WS(6), WS(7), WS(8), WS(9),
    { 0, XF86XK_AudioRaiseVolume,  SPAWN, { .v = vol_up      } },
    { 0, XF86XK_AudioLowerVolume,  SPAWN, { .v = vol_down    } },
    { 0, XF86XK_AudioMute,         SPAWN, { .v = vol_mute    } },
    { 0, XF86XK_MonBrightnessUp,   SPAWN, { .v = bright_up   } },
    { 0, XF86XK_MonBrightnessDown, SPAWN, { .v = bright_down } },
    { 0, XF86XK_PowerOff, SPAWN, { .v = power_off} },
};

const size_t num_keys = sizeof(keys) / sizeof(keys[0]);

static int
xerror(Display *d, XErrorEvent *ee)
{
    (void)d; (void)ee;
    return 0;
}

/* ---- EWMH support ---- */
/* Advertise supported _NET_WM hints to pagers/taskbars. */

void
update_ewmh_current_desktop(void)
{
    long desktop = cur_ws;
    XChangeProperty(dpy, root, atom_net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);
}

/* Write all supported EWMH atoms and initial desktop count at startup. */
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
                        atom_net_wm_state_full,
                        atom_net_current_desktop,
                        atom_net_number_of_desktops,
                        atom_net_active_window,
                        atom_net_wm_window_type,
                        atom_net_wm_type_desktop,
                        atom_net_wm_type_dock,
                        atom_net_wm_type_splash,
                        atom_net_wm_type_dialog,
                        atom_net_wm_type_util,
                        atom_net_wm_type_toolbar,
                        atom_net_wm_state_above,
                        atom_net_wm_state_sticky,
                        atom_net_wm_state_not_focusable,
                    }, 16);

    update_ewmh_current_desktop();
    XDeleteProperty(dpy, root, atom_net_active_window);
}

/* ---- atom caching ---- */
/* Intern frequently-used X atoms once at startup to avoid repeated round-trips. */

void
cache_atoms(void)
{
    atom_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    atom_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    atom_wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    atom_net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_net_wm_state_full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    atom_net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    atom_net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    atom_net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_net_wm_type_desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    atom_net_wm_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    atom_net_wm_type_splash = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    atom_net_wm_type_dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    atom_net_wm_type_util = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTIL", False);
    atom_net_wm_type_toolbar = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    atom_net_wm_type_notification = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    atom_net_wm_type_popup_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    atom_net_wm_type_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    atom_net_wm_state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    atom_net_wm_state_sticky = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    atom_net_wm_state_not_focusable = XInternAtom(dpy, "_NET_WM_STATE_NOT_FOCUSABLE", False);
    atom_net_wm_state_hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    atom_net_close = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
}

/* ---- key grabbing ---- */
/* Grab all key bindings on the root window.  We iterate modifier variants
   so that CapsLock/NumLock (LockMask, Mod2Mask) don't break bindings. */

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

    for (i = 0; i < num_keys; i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].sym);
        if (!code) continue;

        if (keys[i].mod == MODKEY || keys[i].mod == (MODKEY | SHTKEY)) {
            for (j = 0; j < NELEM(mod4_variants); j++)
                XGrabKey(dpy, code, keys[i].mod | mod4_variants[j],
                         root, True, GrabModeAsync, GrabModeAsync);
        } else {
            for (j = 0; j < NELEM(all_mods); j++)
                XGrabKey(dpy, code, keys[i].mod | all_mods[j],
                         root, True, GrabModeAsync, GrabModeAsync);
        }
    }

    {
        unsigned int mv[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
        for (size_t k = 0; k < NELEM(mv); k++) {
            XGrabButton(dpy, Button1, MODKEY | mv[k], root, True,
                        ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
            XGrabButton(dpy, Button3, MODKEY | mv[k], root, True,
                        ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
        }
    }
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
            if (nmons > MAX_MONS) nmons = MAX_MONS;
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
                mons[i].dim_win = 0;
                mons[i].dim_colormap = 0;
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
        mons[0].dim_win = 0;
        mons[0].dim_colormap = 0;
    }
}

/* ---- init / cleanup / run ---- */

/* Initialise the display, root window, atoms, monitors, workspaces,
   key grabs, and manage any pre-existing windows. */
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

    for (i = 0; i < NUM_WORKSPACES + 1; i++) {
        ws = &spaces[i];
        ws->wins = NULL;
        ws->nwin = 0;
        ws->cap = 0;
        ws->focused = None;
        ws->cam_x = 0;
        ws->tiled = NULL;
        ws->ntiled = 0;
        ws->tiled_cap = 0;
    }

    grab_keys();
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask
                           | KeyPressMask | ButtonPressMask | ButtonReleaseMask
                           | PointerMotionMask | PropertyChangeMask);

    signal(SIGCHLD, SIG_IGN);

    scratch_visible = 0;
    scratch_saved_focus = None;
    mouse.win = None;

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
    for (i = 0; i < NUM_WORKSPACES + 1; i++) {
        dwindle_cleanup(&spaces[i]);
        free(spaces[i].wins);
        free(spaces[i].tiled);
        spaces[i].wins = NULL;
        spaces[i].tiled = NULL;
    }
    /* Destroy any lingering dim overlay */
    for (i = 0; i < nmons; i++) {
        if (mons[i].dim_win) {
            XDestroyWindow(dpy, mons[i].dim_win);
            mons[i].dim_win = 0;
        }
        if (mons[i].dim_colormap) {
            XFreeColormap(dpy, mons[i].dim_colormap);
            mons[i].dim_colormap = 0;
        }
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
        case ButtonRelease:    handle_button_release(&ev.xbutton); break;
        case MotionNotify:     handle_motion_notify(&ev.xmotion); break;
        case MapRequest:       handle_map_request(&ev.xmaprequest); break;
        case DestroyNotify:    handle_destroy_notify(&ev.xdestroywindow); break;
        case UnmapNotify:      handle_unmap_notify(&ev.xunmap); break;
        case ConfigureRequest: handle_configure_request(&ev.xconfigurerequest); break;
        case EnterNotify:      handle_enter_notify(&ev.xcrossing); break;
        case FocusIn:          handle_focus_in(&ev.xfocus); break;
        case PropertyNotify:   handle_property_notify(&ev.xproperty); break;
        case ClientMessage:    handle_client_message(&ev.xclient); break;
        default: continue;
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
