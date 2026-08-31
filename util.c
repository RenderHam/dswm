#include "dswm.h"
#include <X11/Xlib.h>
#include <stdio.h>
#include <unistd.h>

#define NELEM(x)  (sizeof(x) / sizeof(x[0]))

void
grab_keys(void)
{
    unsigned int i, j;
    KeyCode code;
    unsigned int mods[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };

    XUngrabKey(dpy, AnyKey, AnyModifier, root);

    for (i = 0; i < NELEM(keys); i++) {
        code = XKeysymToKeycode(dpy, keys[i].sym);
        if (!code) continue;
        for (j = 0; j < NELEM(mods); j++)
            XGrabKey(dpy, code, keys[i].mod | mods[j], root,
                     True, GrabModeAsync, GrabModeAsync);
    }
}

int
xerror(Display *d, XErrorEvent *ee)
{
    (void)d;
    (void)ee;
    return 0;
}

void
spawn(void *arg)
{
    const char **cmd = (const char **)arg;
    if (fork() == 0) {
        close(ConnectionNumber(dpy));
        setsid();
        execvp(cmd[0], (char *const *)cmd);
        fprintf(stderr, "dswm: execvp %s failed\n", cmd[0]);
        _exit(1);
    }
}
