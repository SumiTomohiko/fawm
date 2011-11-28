#include <stdio.h>
#include <strings.h>
#include <X11/Xlib.h>

#define MAX(a, b) ((b) < (a) ? (a) : (b))

static void
print_error(const char* msg)
{
    fprintf(stderr, "%s\n", msg);
}

int
main(int argc, const char* argv[])
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        print_error("Cannot open display");
        return 1;
    }

    Window root = DefaultRootWindow(display);

#if 0
    XGrabKey(display, XKeysymToKeycode(display, XStringToKeysym("F1")), Mod1Mask, root,
            True, GrabModeAsync, GrabModeAsync);
#endif
    XGrabButton(display, 1, Mod1Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(display, 3, Mod1Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);

    XButtonEvent start;
    bzero(&start, sizeof(start));
    XWindowAttributes attr;
    bzero(&attr, sizeof(attr));
    while (1) {
        XEvent ev;
        XNextEvent(display, &ev);
        if ((ev.type == KeyPress) && (ev.xkey.subwindow != None)) {
            XRaiseWindow(display, ev.xkey.subwindow);
        }
        else if ((ev.type == ButtonPress) && (ev.xbutton.subwindow != None)) {
            XGrabPointer(display, ev.xbutton.subwindow, True,
                    PointerMotionMask|ButtonReleaseMask, GrabModeAsync,
                    GrabModeAsync, None, None, CurrentTime);
            XGetWindowAttributes(display, ev.xbutton.subwindow, &attr);
            start = ev.xbutton;
        }
        else if (ev.type == MotionNotify) {
            while (XCheckTypedEvent(display, MotionNotify, &ev)) {
            }
            int xdiff = ev.xbutton.x_root - start.x_root;
            int ydiff = ev.xbutton.y_root - start.y_root;
            XMoveResizeWindow(display, ev.xmotion.window,
                attr.x + (start.button == 1 ? xdiff : 0),
                attr.y + (start.button == 1 ? ydiff : 0),
                MAX(1, attr.width + (start.button == 3 ? xdiff : 0)),
                MAX(1, attr.height + (start.button == 3 ? ydiff : 0)));
        }
        else if (ev.type == ButtonRelease) {
            XUngrabPointer(display, CurrentTime);
        }
    }
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
