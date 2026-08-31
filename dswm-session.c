#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void)
{
    printf("welcome to dswm\n");

    const char *home = getenv("HOME");
    if (home) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/Pictures", home);
        mkdir(path, 0755);
    }
    setenv("QT_QPA_PLATFORMTHEME",      "qt6ct", 1);
    setenv("QT_STYLE_OVERRIDE",       "kvantum", 1);
    setenv("QT_AUTO_SCREEN_SCALE_FACTOR", "1",   1);
    setenv("GDK_BACKEND",               "x11",   1);
    setenv("GDK_CORE_DEVICE_EVENTS", "1", 1);

    if (fork() == 0) {
        setsid();
        const char *h = getenv("HOME");
        if (h) {
            char script[4096];
            snprintf(script, sizeof(script), "%s/.config/dswm/autostart.sh", h);
            char *cmd[] = { "/bin/sh", script, NULL };
            execvp(cmd[0], cmd);
        }
        _exit(0);
    }

    char *argv[] = { "dswm", NULL };
    execvp("dswm", argv);
    return 1;
}
