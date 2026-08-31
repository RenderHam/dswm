#ifndef DSWM_H
#define DSWM_H

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define PATCH_VERSION 0

#include <X11/Xlib.h>
#include <X11/keysym.h>

#define NUM_WORKSPACES     9

#define BORDER_WIDTH       3
#define BORDER_COLOR       0xffffff
#define FOCUS_COLOR        0x005577

#define GAP_OUTER          10
#define GAP_INNER          10

#define SCROLL_WINDOWS_VISIBLE 2
#define SCROLL_STEP        550
#define RESIZE_STEP        60

#define BAR_POSITION       0
#define BAR_HEIGHT         0

#define USE_XINERAMA       1

#define INITIAL_CAP        16
#define MAX_MONS           8
#define MAX_TILED          256
#define MIN_WIN_DIM        10
#define MIN_WIN_W          200
#define MIN_MASTER_HORIZ   0.3f
#define MAX_MASTER_HORIZ   3.0f
#define MIN_MASTER_VERT    0.1f
#define MAX_MASTER_VERT    0.9f
#define MIN_SCROLL_VIS     1
#define MAX_SCROLL_VIS     10

#define MODKEY             Mod4Mask
#define SHTKEY             ShiftMask

/* data structures */
typedef struct ManagedWindow ManagedWindow;
struct ManagedWindow {
    Window window;
    int x, y, width, height;
    int is_floating;
    int is_focused;
    int is_fullscreen;
    int workspace;
    int monitor;
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
    int scroll_windows_visible;
};

typedef struct Workspace Workspace;
struct Workspace {
    ManagedWindow *wins;
    int nwin;
    int cap;
    ManagedWindow *focused;
    int scroll_offset;
};

/* window rules */
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

/* actions */
enum {
    SPAWN, CLOSE, QUIT,
    FOCUS_NEXT, FOCUS_PREV,
    SWAP_PREV, SWAP_NEXT,
    RESIZE_MASTER,
    SCROLL_LEFT, SCROLL_RIGHT,
    TOGGLE_LAYOUT, TOGGLE_FULLSCREEN, TOGGLE_FLOAT,
    FOCUS_MONITOR,
    SET_SCROLL_VISIBLE,
    INCR_SCROLL_VISIBLE, DECR_SCROLL_VISIBLE,
    SWITCH_WORKSPACE, MOVE_TO_WORKSPACE,
};

typedef union { int i; void *v; } Arg;
typedef struct { unsigned int mod; KeySym sym; int act; Arg arg; } Key;

/* shell commands */
static const char *termcmd[] __attribute__((unused))   = { "alacritty",  NULL };
static const char *menucmd[] __attribute__((unused))   = { "rofi",       NULL };
static const char *browsercmd[] __attribute__((unused)) = { "firefox",    NULL };

static int ws0 __attribute__((unused)) = 0;
static int ws1 __attribute__((unused)) = 1;
static int ws2 __attribute__((unused)) = 2;
static int ws3 __attribute__((unused)) = 3;
static int ws4 __attribute__((unused)) = 4;
static int ws5 __attribute__((unused)) = 5;
static int ws6 __attribute__((unused)) = 6;
static int ws7 __attribute__((unused)) = 7;
static int ws8 __attribute__((unused)) = 8;

#define WS(n)                                                          \
        { MODKEY,         XK_##n, SWITCH_WORKSPACE,  { .i = n-1 } },   \
        { MODKEY|SHTKEY,  XK_##n, MOVE_TO_WORKSPACE, { .i = n-1 } }

static Key keys[] __attribute__((unused)) = {
    /* launch */
    { MODKEY,           XK_Return, SPAWN,          { .v = termcmd  } },
    { MODKEY,           XK_d,      SPAWN,          { .v = menucmd  } },
    { MODKEY,           XK_b,      SPAWN,          { .v = browsercmd } },

    /* wm control */
    { MODKEY,           XK_w,      CLOSE,          { 0 } },
    { MODKEY|SHTKEY,    XK_w,      QUIT,           { 0 } },

    /* focus cycling */
    { MODKEY,           XK_j,      FOCUS_NEXT,     { 0 } },
    { MODKEY,           XK_k,      FOCUS_PREV,     { 0 } },

    /* swap windows */
    { MODKEY|SHTKEY,    XK_h,      SWAP_PREV,      { 0 } },
    { MODKEY|SHTKEY,    XK_l,      SWAP_NEXT,      { 0 } },

    /* resize master */
    { MODKEY,           XK_h,      RESIZE_MASTER,  { .i = -RESIZE_STEP } },
    { MODKEY,           XK_l,      RESIZE_MASTER,  { .i = +RESIZE_STEP } },

    /* scroll windows */
    { MODKEY,           XK_Left,   SCROLL_LEFT,    { 0 } },
    { MODKEY,           XK_Right,  SCROLL_RIGHT,   { 0 } },

    /* toggle states */
    { MODKEY,           XK_t,      TOGGLE_LAYOUT,  { 0 } },
    { MODKEY,           XK_f,      TOGGLE_FULLSCREEN, { 0 } },
    { MODKEY|SHTKEY,    XK_space,  TOGGLE_FLOAT,   { 0 } },

    /* focus monitor */
    { MODKEY,           XK_comma,  FOCUS_MONITOR,  { .i = 0 } },
    { MODKEY,           XK_period, FOCUS_MONITOR,  { .i = 1 } },
    { MODKEY,           XK_slash,  FOCUS_MONITOR,  { .i = 2 } },

    /* scroll visible */
    { MODKEY|SHTKEY,    XK_comma,  SET_SCROLL_VISIBLE, { .i = 2 } },
    { MODKEY|SHTKEY,    XK_period, SET_SCROLL_VISIBLE, { .i = 3 } },
    { MODKEY|SHTKEY,    XK_slash,  SET_SCROLL_VISIBLE, { .i = 4 } },

    { MODKEY,           XK_equal,  INCR_SCROLL_VISIBLE, { 0 } },
    { MODKEY,           XK_minus,  DECR_SCROLL_VISIBLE, { 0 } },

    /* workspaces */
    WS(1), WS(2), WS(3), WS(4), WS(5), WS(6), WS(7), WS(8), WS(9),
};

/* extern globals */
extern Display *dpy;
extern Window root;
extern int screen;
extern int scrw, scrh;
extern int running;
extern int cur_ws;
extern Monitor mons[];
extern int nmons;
extern Workspace spaces[];

/* layout.c */
void tile(void);
void resize_master(void *arg);
void move_horizontal(int forward);
void toggle_layout(void);
void swap_next(void);
void swap_prev(void);
void adjust_scroll_visible(int delta);

/* workspace.c */
void show_workspace(int idx);
void switch_workspace(void *arg);
void move_to_workspace(void *arg);
void manage_window(Window w);
void unmanage_window(Window w);
void focus_next(void);
void focus_prev(void);
void focus_monitor(void *arg);
void set_scroll_visible(void *arg);
void close_window(void);
void quit_wm(void);
void toggle_fullscreen(void);
void toggle_float(void);

/* events.c */
void handle_map_request(XMapRequestEvent *e);
void handle_destroy_notify(XDestroyWindowEvent *e);
void handle_unmap_notify(XUnmapEvent *e);
void handle_configure_request(XConfigureRequestEvent *e);
void handle_enter_notify(XCrossingEvent *e);
void handle_key_press(XKeyEvent *e);
void handle_button_press(XButtonEvent *e);

/* ewmh.c */
void setup_ewmh(void);
void update_ewmh_current_desktop(void);

/* util.c */
void grab_keys(void);
int xerror(Display *d, XErrorEvent *ee);
void spawn(void *arg);

/* dswm.c */
Workspace *curws(void);
Monitor *curmon(void);

#endif /* DSWM_H */
