#ifndef DSWM_H
#define DSWM_H

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define PATCH_VERSION 0

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#define NUM_WORKSPACES     9

#define BORDER_WIDTH       3
#define BORDER_COLOR       0x181818
#define FOCUS_COLOR        0x005577

#define GAP_OUTER          10       /* px gap between windows and screen edges */
#define GAP_INNER          10       /* px gap between adjacent tiled windows (per side) */

#define COLUMN_DIVISOR     2        /* windows per viewport in scroll mode */
#define RESIZE_STEP        60

#define BAR_POSITION       0       /* 0 = top, 1 = bottom */
#define BAR_HEIGHT         0

#define USE_XINERAMA       1

#define INITIAL_CAP        16
#define MIN_WIN_DIM        10
#define MIN_MASTER_VERT    0.1f
#define MAX_MASTER_VERT    0.9f

/* modifier keys */
#define MODKEY             Mod4Mask
#define SHTKEY             ShiftMask

/* window rules */
typedef struct {
    const char *wm_class;
    int is_floating;
} Rule;

static Rule rules[] __attribute__((unused)) = {
    /* class name           floating */
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
    SWITCH_WORKSPACE, MOVE_TO_WORKSPACE,
};

typedef union { int i; void *v; } Arg;
typedef struct { unsigned int mod; KeySym sym; int act; Arg arg; } Key;

/* shell commands */
static const char *termcmd[] __attribute__((unused))   = { "alacritty",  NULL };
static const char *menucmd[] __attribute__((unused))   = { "sh", "-c", "~/.config/rofi/launcher/launcher.sh", NULL };
static const char *browsercmd[] __attribute__((unused)) = { "firefox",    NULL };

/* xf86 commands */
static const char *vol_up[]      __attribute__((unused)) = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "2%+",  NULL };
static const char *vol_down[]    __attribute__((unused)) = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "2%-",  NULL };
static const char *vol_mute[]    __attribute__((unused)) = { "wpctl", "set-mute",   "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
static const char *bright_up[]   __attribute__((unused)) = { "brightnessctl", "s", "2%+",  NULL };
static const char *bright_down[] __attribute__((unused)) = { "brightnessctl", "s", "2%-",  NULL };
static const char *dim[] __attribute__((unused)) = { "pkill", "-USR1", "redshift",  NULL };

#define WS(n)                                                          \
        { MODKEY,         XK_##n, SWITCH_WORKSPACE,  { .i = n-1 } },   \
        { MODKEY|SHTKEY,  XK_##n, MOVE_TO_WORKSPACE, { .i = n-1 } }

static Key keys[] __attribute__((unused)) = {
    /* launch */
    { MODKEY,           XK_Return, SPAWN,          { .v = termcmd  } },
    { MODKEY,           XK_r,      SPAWN,          { .v = menucmd  } },
    { MODKEY,           XK_b,      SPAWN,          { .v = browsercmd } },

    /* fun things */
    { MODKEY,           XK_i,      SPAWN,          { .v = dim } },


    /* wm control */
    { MODKEY,           XK_w,      CLOSE,          { 0 } },
    { MODKEY|SHTKEY,    XK_q,      QUIT,           { 0 } },

    /* focus cycling */
    { MODKEY,           XK_h,      FOCUS_PREV,     { 0 } },
    { MODKEY,           XK_l,      FOCUS_NEXT,     { 0 } },

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

    /* workspaces */
    WS(1), WS(2), WS(3), WS(4), WS(5), WS(6), WS(7), WS(8), WS(9),

    /* xf86 media keys */
    { 0, XF86XK_AudioRaiseVolume,  SPAWN, { .v = vol_up      } },
    { 0, XF86XK_AudioLowerVolume,  SPAWN, { .v = vol_down    } },
    { 0, XF86XK_AudioMute,         SPAWN, { .v = vol_mute    } },
    { 0, XF86XK_MonBrightnessUp,   SPAWN, { .v = bright_up   } },
    { 0, XF86XK_MonBrightnessDown, SPAWN, { .v = bright_down } },
};

#endif /* DSWM_H */
