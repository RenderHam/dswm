#ifndef DSWM_H
#define DSWM_H

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define PATCH_VERSION 0

#define NUM_WORKSPACES     9

#define BORDER_WIDTH       3
#define BORDER_COLOR       0x181818
#define FOCUS_COLOR        0x005577

#define GAP_OUTER          10
#define GAP_INNER          10

#define COLUMN_DIVISOR     2
#define RESIZE_STEP        50
#define RESIZE_FACTOR_STEP 0.1f
#define MIN_WIDTH_FACTOR   0.3f
#define MAX_WIDTH_FACTOR   3.0f

#define BAR_POSITION       0
#define BAR_HEIGHT         0

#define CENTER_FOCUSED_DEFAULT 0

#define USE_XINERAMA       1

#define INITIAL_CAP        16
#define MIN_WIN_DIM        10
#define MIN_MASTER_VERT    0.1f
#define MAX_MASTER_VERT    0.9f

#define MODKEY             Mod4Mask
#define SHTKEY             ShiftMask

/* ---- data structures ---- */

typedef struct ManagedWindow ManagedWindow;
struct ManagedWindow {
    Window window;
    int x, y, width, height;
    int is_floating;
    int is_fullscreen;
    int workspace;
    int monitor;
    float width_factor;
    int is_fit;
    float saved_factor;
    int pre_fs_x, pre_fs_y;
    int pre_fs_width, pre_fs_height;
    int pre_fs_floating;
};

typedef struct Monitor Monitor;
struct Monitor {
    int id;
    int x, y;
    int width, height;
    int current_workspace;
    float master_factor;
    int horizontal_mode;
    int strut_top, strut_bottom, strut_left, strut_right;
    int strut_valid;
};

typedef struct Workspace Workspace;
struct Workspace {
    ManagedWindow *wins;
    int nwin;
    int cap;
    ManagedWindow *focused;
    int cam_x;
    ManagedWindow **tiled;
    int ntiled;
    int tiled_cap;
};

/* ---- window rules ---- */

typedef struct {
    const char *wm_class;
    int is_floating;
} Rule;

static Rule rules[] __attribute__((unused)) = {
    { "pavucontrol",        1 },
    { "rofi",               1 },
    { "steam",              1 },
    { "steamwebhelper",     1 },
};

/* ---- actions ---- */

enum {
    SPAWN, CLOSE, QUIT,
    FOCUS_NEXT, FOCUS_PREV,
    SWAP_PREV, SWAP_NEXT,
    RESIZE_MASTER, RESIZE_WINDOW,
    SCROLL_LEFT, SCROLL_RIGHT,
    TOGGLE_LAYOUT, TOGGLE_FULLSCREEN, TOGGLE_FLOAT, FIT_WINDOW, TOGGLE_CENTER_FOCUS,
    FOCUS_MONITOR,
    SWITCH_WORKSPACE, MOVE_TO_WORKSPACE,
};

typedef union { int i; void *v; } Arg;
typedef struct { unsigned int mod; KeySym sym; int act; Arg arg; } Key;

/* ---- shell commands ---- */

static const char *termcmd[] __attribute__((unused))   = { "alacritty",  NULL };
static const char *menucmd[] __attribute__((unused))   = { "sh", "-c", "~/.config/rofi/launcher/launcher.sh", NULL };
static const char *browsercmd[] __attribute__((unused)) = { "firefox",    NULL };

/* ---- xf86 commands ---- */

static const char *vol_up[]      __attribute__((unused)) = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "2%+",  NULL };
static const char *vol_down[]    __attribute__((unused)) = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "2%-",  NULL };
static const char *vol_mute[]    __attribute__((unused)) = { "wpctl", "set-mute",   "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
static const char *bright_up[]   __attribute__((unused)) = { "brightnessctl", "s", "2%+",  NULL };
static const char *bright_down[] __attribute__((unused)) = { "brightnessctl", "s", "2%-",  NULL };
static const char *dim[] __attribute__((unused)) = { "pkill", "-USR1", "redshift",  NULL };

/* ---- keybindings ---- */

#define WS(n)                                                          \
        { MODKEY,         XK_##n, SWITCH_WORKSPACE,  { .i = n-1 } },   \
        { MODKEY|SHTKEY,  XK_##n, MOVE_TO_WORKSPACE, { .i = n-1 } }

static Key keys[] __attribute__((unused)) = {
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
    { MODKEY|SHTKEY,    XK_space,  TOGGLE_FLOAT,   { 0 } },
    { MODKEY,           XK_comma,  FOCUS_MONITOR,  { .i = 0 } },
    { MODKEY,           XK_period, FOCUS_MONITOR,  { .i = 1 } },
    { MODKEY,           XK_slash,  FOCUS_MONITOR,  { .i = 2 } },
    WS(1), WS(2), WS(3), WS(4), WS(5), WS(6), WS(7), WS(8), WS(9),
    { 0, XF86XK_AudioRaiseVolume,  SPAWN, { .v = vol_up      } },
    { 0, XF86XK_AudioLowerVolume,  SPAWN, { .v = vol_down    } },
    { 0, XF86XK_AudioMute,         SPAWN, { .v = vol_mute    } },
    { 0, XF86XK_MonBrightnessUp,   SPAWN, { .v = bright_up   } },
    { 0, XF86XK_MonBrightnessDown, SPAWN, { .v = bright_down } },
};

/* ---- globals (owned by main.c) ---- */

extern Display *dpy;
extern Window root;
extern int scrw, scrh;
extern int running;
extern int cur_ws;
extern Monitor mons[8];
extern int nmons;
extern Workspace spaces[NUM_WORKSPACES];

/* cached atoms (owned by main.c) */
extern Atom atom_wm_delete;
extern Atom atom_wm_protocols;
extern Atom atom_net_wm_strut;
extern Atom atom_net_wm_state;
extern Atom atom_net_wm_state_full;
extern Atom atom_net_current_desktop;
extern Atom atom_net_supported;
extern Atom atom_net_number_of_desktops;
extern Atom atom_net_active_window;
extern Atom atom_net_wm_name;
extern Atom atom_net_wm_window_type;
extern Atom atom_net_wm_type_desktop;
extern Atom atom_net_wm_type_dock;
extern Atom atom_net_wm_type_splash;
extern Atom atom_motif_wm_hints;

/* ---- globals (owned by layout.c) ---- */

extern int retile_pending;
extern int center_focused;

/* ---- layout.c prototypes ---- */

Workspace *curws(void);
Monitor   *curmon(void);

int  tiled_ensure_cap(Workspace *ws);
void tiled_add(Workspace *ws, ManagedWindow *mw);
void tiled_remove(Workspace *ws, Window w);
void rebuild_tiled(Workspace *ws);

void tile_horizontal(void);
void tile_windows(void);
void retile(void);
void retile_deferred(void);
void flush_retile(void);
void toggle_center_focus(void);
void toggle_layout(void);

/* ---- wm.c prototypes ---- */

void update_border(Window w, int focused);
void refocus(Workspace *ws, ManagedWindow *new);
void focus_monitor(void *arg);
void move_horizontal(int forward);
void swap_impl(int delta);
void swap_next(void);
void swap_prev(void);
void show_workspace(int idx, int visible);
void switch_workspace(void *arg);
void move_to_workspace(void *arg);
void manage_window(Window w);
void unmanage_window(Window w, int force);
void focus_next(void);
void focus_prev(void);
void close_window(void);
void quit_wm(void);
void toggle_fullscreen(void);
void toggle_float(void);
void resize_master(void *arg);
void resize_window(void *arg);
void fit_window(void);
void spawn(void *arg);

/* event handlers */
void handle_map_request(XMapRequestEvent *e);
void handle_destroy_notify(XDestroyWindowEvent *e);
void handle_unmap_notify(XUnmapEvent *e);
void handle_configure_request(XConfigureRequestEvent *e);
void handle_enter_notify(XCrossingEvent *e);
void handle_key_press(XKeyEvent *e);
void handle_button_press(XButtonEvent *e);

/* ---- main.c prototypes ---- */

void update_ewmh_current_desktop(void);
void setup_ewmh(void);
void cache_atoms(void);
void monitors_init(void);
void grab_keys(void);

#endif /* DSWM_H */
