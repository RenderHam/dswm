/* dswm.h — Public declarations for the dswm tiling window manager.
   Contains types, constants, extern globals, and function prototypes.
   Implementation lives in main.c, layout.c, and wm.c. */

#ifndef DSWM_H
#define DSWM_H

#include <X11/Xlib.h>
#include <X11/keysym.h>

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define PATCH_VERSION 0

#define NUM_WORKSPACES     9
#define SCRATCHPAD_IDX     9  /* hidden 10th slot, not in EWMH */

#define BORDER_WIDTH       3
#define BORDER_COLOR       0x181818
#define FOCUS_COLOR        0x005577
#define DIM_COLOR          0x000000CC  /* RRGGBBAA — alpha baked into pixel */

#define GAP_OUTER          10
#define GAP_INNER          10

#define COLUMN_DIVISOR     2
#define RESIZE_STEP        50
#define RESIZE_FACTOR_STEP 0.1f
#define MIN_WIDTH_FACTOR   0.3f
#define MAX_WIDTH_FACTOR   3.0f

#define CENTER_FOCUSED_DEFAULT 0

#define USE_XINERAMA       1

#define INITIAL_CAP        16
#define MIN_WIN_DIM        10
#define MIN_MASTER_VERT    0.1f
#define MAX_MASTER_VERT    0.9f
#define DWINDLE_SPLIT_RATIO    0.5f
#define DWINDLE_MIN_NODE       32
#define DWINDLE_RATIO_MIN  0.1f
#define DWINDLE_RATIO_MAX  0.9f
#define DWINDLE_SPLIT_STEP 100.0f

#define MAX_TREE_STACK     64
#define MAX_LEAVES         128
#define MAX_MONS           8
#define RESIZE_HANDLE_PX   16

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
    int is_above       : 1;  /* _NET_WM_STATE_ABOVE — always raised */
    int is_sticky      : 1;  /* _NET_WM_STATE_STICKY — visible all workspaces */
    int is_not_focusable : 1; /* _NET_WM_STATE_NOT_FOCUSABLE */
    int workspace      : 4;
    int monitor        : 3;
    /* cold fields: only on fullscreen toggle / save-restore */
    float saved_factor;
    int pre_fs_x, pre_fs_y;
    int pre_fs_width, pre_fs_height;
    int pre_float_x, pre_float_y;
    int pre_float_w, pre_float_h;
    int pre_float_idx;
    int pre_float_cam_x;
};

/* ---- dwindle tree node ---- */

#define DWINDLE_SPLIT_V 0  /* vertical split: left/right */
#define DWINDLE_SPLIT_H 1  /* horizontal split: top/bottom */

#define DWINDLE_DIR_WEST  0
#define DWINDLE_DIR_EAST  1
#define DWINDLE_DIR_NORTH 2
#define DWINDLE_DIR_SOUTH 3

typedef struct DwindleNode DwindleNode;
struct DwindleNode {
    int split_type;       /* DWINDLE_SPLIT_V or DWINDLE_SPLIT_H */
    float split_ratio;    /* 0.0–1.0 */
    int x, y, w, h;      /* computed rectangle */
    DwindleNode *parent;
    DwindleNode *first;       /* left/top child */
    DwindleNode *second;      /* right/bottom child */
    Window win;           /* X11 window ID (leaf nodes only, 0 = internal) */
};

/* ---- mouse drag state ---- */

typedef struct {
    int active;           /* 0=idle, 1=dragging */
    int resizing;         /* 1=resizing floating window */
    ManagedWindow *win;   /* window being dragged/resized */
    int start_x, start_y; /* cursor position at grab */
    int orig_x, orig_y;   /* original window position */
    int orig_w, orig_h;   /* original window size (for resize) */
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
    Window dim_win;  /* fullscreen dim overlay for scratchpad */
    Colormap dim_colormap;
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
    /* Dwindle tree (dwindle mode) */
    DwindleNode *dwindle_root;
    DwindleNode *dwindle_focus;
    int dwindle_monocle;   /* monocle sub-mode within dwindle */
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
    TOGGLE_LAYOUT,     TOGGLE_FULLSCREEN, TOGGLE_FLOAT, FIT_WINDOW, TOGGLE_CENTER_FOCUS,
    TOGGLE_SCRATCHPAD, MOVE_TO_SCRATCHPAD,
    TOGGLE_MONOCLE,
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

extern Key keys[];
extern const size_t num_keys;

/* ---- globals (owned by main.c) ---- */

extern Display *dpy;
extern Window root;
extern int scrw, scrh;
extern int running;
extern int cur_ws;
extern Monitor mons[MAX_MONS];
extern int nmons;
extern Workspace spaces[NUM_WORKSPACES + 1];
extern MouseState mouse;
extern int scratch_visible;
extern ManagedWindow *scratch_saved_focus;

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
extern Atom atom_net_wm_type_normal;
extern Atom atom_net_wm_type_dialog;
extern Atom atom_net_wm_type_util;
extern Atom atom_net_wm_type_toolbar;
extern Atom atom_net_wm_type_notification;
extern Atom atom_net_wm_type_popup_menu;
extern Atom atom_net_wm_type_menu;
extern Atom atom_net_wm_state_above;
extern Atom atom_net_wm_state_sticky;
extern Atom atom_net_wm_state_not_focusable;
extern Atom atom_motif_wm_hints;

/* ---- globals (owned by layout.c) ---- */

extern int retile_pending;
extern int center_focused;

/* ---- layout.c prototypes ---- */

Workspace *curws(void);
Workspace *active_ws(void);
Monitor   *curmon(void);

int  tiled_ensure_cap(Workspace *ws);
void tiled_add(Workspace *ws, ManagedWindow *mw);
void tiled_remove(Workspace *ws, Window w);
void rebuild_tiled(Workspace *ws);

void update_camera_ws(Workspace *ws);
void tile_horizontal_ws(Workspace *ws);
void tile_windows_ws(Workspace *ws);
void retile_ws(Workspace *ws);
void retile_deferred(void);
void flush_retile(void);
void toggle_center_focus(void);
void toggle_layout(void);
void toggle_monocle(void);

/* ---- layout.c prototypes — dwindle ---- */

DwindleNode *dwindle_node_new(Window w);
void     dwindle_insert(Workspace *ws, Window w);
void     dwindle_remove(Workspace *ws, Window w);
void     dwindle_arrange(Workspace *ws, Monitor *mon);
void     dwindle_cleanup(Workspace *ws);
void     dwindle_focus_prevnext(Workspace *ws, int delta);
void     dwindle_resize(Workspace *ws, int dir, int delta);
ManagedWindow *dwindle_focused_mw(Workspace *ws);
ManagedWindow *dwindle_find_mw(Workspace *ws, Window w);
void           dwindle_set_focus(Workspace *ws, Window w);

/* ---- wm.c prototypes ---- */

void update_border(Window w, int focused);
void refocus(Workspace *ws, ManagedWindow *next);
void focus_monitor(void *arg);
void move_horizontal(int forward);
void swap_impl(int delta);
void show_workspace(int idx, int visible);
void switch_workspace(void *arg);
void move_to_workspace(void *arg);
void manage_window(Window w);
void unmanage_window(Window w, int force);
void focus_cycle(int delta);
void close_window(void);
void quit_wm(void);
void toggle_fullscreen(void);
void toggle_float(void);
void toggle_scratchpad(void);
void move_to_scratchpad(void);
void resize_master(void *arg);
void resize_window(void *arg);
int fit_window(void);
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
void handle_property_notify(XPropertyEvent *e);

/* ---- main.c prototypes ---- */

void update_ewmh_current_desktop(void);
void setup_ewmh(void);
void cache_atoms(void);
void monitors_init(void);
void grab_keys(void);

#endif /* DSWM_H */
