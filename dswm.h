/* dswm.h — Public declarations for the dswm tiling window manager.
   Contains types, constants, extern globals, and function prototypes.
   Implementation lives in main.c, layout.c, and wm.c. */

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

/* Managed window descriptor — fields ordered by access frequency. */
typedef struct ManagedWindow ManagedWindow;
struct ManagedWindow {
    /* hot fields: touched every tiling pass */
    Window window;
    int x, y, width, height;
    float width_factor;
    /* warm fields: touched on state changes */
    int is_floating    : 1;
    int is_fullscreen  : 1;
    int is_fit         : 1;
    int pre_fs_floating : 1;
    int workspace      : 4;
    int monitor        : 3;
    /* cold fields: only on fullscreen toggle / save-restore */
    float saved_factor;
    int pre_fs_x, pre_fs_y;
    int pre_fs_width, pre_fs_height;
};

/* ---- mouse drag state ---- */

typedef struct {
    int active;           /* 0=idle, 1=dragging */
    ManagedWindow *win;   /* window being dragged */
    int start_x, start_y; /* cursor position at grab */
    int orig_x, orig_y;   /* original window position */
} MouseState;

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

extern Rule rules[];
extern const size_t num_rules;

/* ---- actions ---- */

/* User actions dispatched from key bindings. */
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

/* Key binding: modifier + keysym -> action + argument. */
typedef struct { unsigned int mod; KeySym sym; int act; Arg arg; } Key;

/* ---- shell commands ---- */

extern const char *termcmd[];
extern const char *menucmd[];
extern const char *browsercmd[];

/* ---- xf86 commands ---- */

extern const char *vol_up[];
extern const char *vol_down[];
extern const char *vol_mute[];
extern const char *bright_up[];
extern const char *bright_down[];
extern const char *dim[];

/* ---- keybindings ---- */

#define WS(n)                                                          \
        { MODKEY,         XK_##n, SWITCH_WORKSPACE,  { .i = n-1 } },   \
        { MODKEY|SHTKEY,  XK_##n, MOVE_TO_WORKSPACE, { .i = n-1 } }

extern Key keys[];
extern const size_t num_keys;

/* ---- globals (owned by main.c) ---- */

extern Display *dpy;
extern Window root;
extern int scrw, scrh;
extern int running;
extern int cur_ws;
extern Monitor mons[8];
extern int nmons;
extern Workspace spaces[NUM_WORKSPACES];
extern MouseState mouse;

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

void update_camera(void);
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
void handle_button_release(XButtonEvent *e);
void handle_motion_notify(XMotionEvent *e);

/* ---- main.c prototypes ---- */

void update_ewmh_current_desktop(void);
void setup_ewmh(void);
void cache_atoms(void);
void monitors_init(void);
void grab_keys(void);

#endif /* DSWM_H */
