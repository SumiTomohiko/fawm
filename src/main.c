#include <stdio.h>
#include <X11/Xlib.h>

struct WindowManager {
    Display* display;
    unsigned long focused_foreground_color;
};

typedef struct WindowManager WindowManager;

static void
print_error(const char* msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void
reparent_window(WindowManager* wm, Window w)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    XWindowAttributes wa;
    XGetWindowAttributes(display, w, &wa);
    int border_size = 4;
    int title_height = 32;
    Window frame = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        wa.x, wa.y,
        wa.width + 2 * border_size, wa.height + title_height + border_size,
        0,
        BlackPixel(display, screen), wm->focused_foreground_color);
    XReparentWindow(display, w, frame, border_size, title_height);
    XMapWindow(display, frame);
    XMapWindow(display, w);
    XAddToSaveSet(display, w);
}

static void
reparent_toplevels(WindowManager* wm)
{
    Display* display = wm->display;
    Window root = DefaultRootWindow(display);
    Window _;
    Window* children;
    unsigned int nchildren;
    if (XQueryTree(display, root, &_, &_, &children, &nchildren) == 0) {
        print_error("XQueryTree failed.");
        return;
    }
    int i;
    for (i = 0; i < nchildren; i++) {
        XWindowAttributes wa;
        XGetWindowAttributes(display, children[i], &wa);
        if (IsUnmapped == wa.map_state) {
            continue;
        }
        reparent_window(wm, children[i]);
    }
}

static unsigned long
alloc_color(WindowManager* wm, const char* name)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    Colormap colormap = DefaultColormap(display, screen);
    XColor c;
    XColor _;
    if (XAllocNamedColor(display, colormap, name, &c, &_) == 0) {
        return BlackPixel(display, screen);
    }
    return c.pixel;
}

static void
wm_main(WindowManager* wm)
{
    wm->focused_foreground_color = alloc_color(wm, "light pink");

    reparent_toplevels(wm);
    Display* display = wm->display;
    XSelectInput(display, DefaultRootWindow(display), SubstructureRedirectMask);

    while (1) {
        XEvent e;
        XNextEvent(display, &e);
        if (e.type == MapRequest) {
            reparent_window(wm, e.xmaprequest.window);
        }
    }
}

int
main(int argc, const char* argv[])
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        print_error("XOpenDisplay failed.");
        return 1;
    }

    WindowManager wm;
    wm.display = display;
    wm_main(&wm);

    XCloseDisplay(display);

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
