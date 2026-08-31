#include "dswm.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>

void
setup_ewmh(void)
{
    Atom net_supported, net_number_of_desktops, net_current_desktop,
         net_active_window, net_wm_name;

    net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);

    XChangeProperty(dpy, root, net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char[]){
                        net_number_of_desktops,
                        net_current_desktop,
                        net_active_window,
                        net_wm_name,
                    }, 4);

    long ndesk = NUM_WORKSPACES;
    XChangeProperty(dpy, root, net_number_of_desktops, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&ndesk, 1);

    long cdesk = cur_ws;
    XChangeProperty(dpy, root, net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&cdesk, 1);
}

void
update_ewmh_current_desktop(void)
{
    Atom net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    long cdesk = cur_ws;
    XChangeProperty(dpy, root, net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&cdesk, 1);
    XFlush(dpy);
}
