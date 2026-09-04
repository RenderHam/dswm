#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void)
{
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "dswm-session: HOME not set\n");
        return 1;
    }

    /* environment */
    setenv("QT_QPA_PLATFORMTHEME",         "qt6ct",             1);
    setenv("QT_STYLE_OVERRIDE",            "kvantum",           1);
    setenv("QT_AUTO_SCREEN_SCALE_FACTOR",  "1",                 1);
    setenv("GDK_BACKEND",                  "x11",               1);
    setenv("GDK_CORE_DEVICE_EVENTS",       "1",                 1);

    /* autostart: check DSWM_AUTOSTART env or default path */
    const char *autostart = getenv("DSWM_AUTOSTART");
    char path[4096];
    if (autostart) {
        snprintf(path, sizeof(path), "%s", autostart);
    } else {
        snprintf(path, sizeof(path), "%s/.config/dswm/autostart.sh", home);
    }

    /* launch autostart in background if script exists and is executable */
    struct stat st;
    if (stat(path, &st) == 0 && (st.st_mode & S_IXUSR)) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            char *cmd[] = { "/bin/sh", path, NULL };
            execvp(cmd[0], cmd);
            _exit(127);
        }
    } else {
        fprintf(stderr, "dswm-session: autostart skipped (%s)\n", path);
    }

    /* exec the wm (replaces this process) */
    char *argv[] = { "dswm", NULL };
    execvp("dswm", argv);
    fprintf(stderr, "dswm-session: exec dswm failed\n");
    return 1;
}
