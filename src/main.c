#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>

#include <uwm/bitmaps/close>

struct Frame {
    struct Frame* prev;
    struct Frame* next;
    Window window;
    Window child;
    Pixmap close_icon;
};

typedef struct Frame Frame;

struct WindowManager {
    Display* display;
    unsigned long focused_foreground_color;
    int border_size;
    int frame_size;
    int title_height;
    Frame* frames;
};

typedef struct WindowManager WindowManager;

#define array_sizeof(x) (sizeof((x)) / sizeof((x)[0]))

static void
print_error(const char* msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    char buf[64];
    vsnprintf(buf, array_sizeof(buf), msg, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
}

static Bool
is_sentinel_frame(Frame* frame)
{
    return frame->next == NULL ? True : False;
}

static Frame*
search_frame(WindowManager* wm, Window w)
{
    Frame* frame = wm->frames->next;
    while (!is_sentinel_frame(frame) && (frame->window != w)) {
        frame = frame->next;
    }
    return !is_sentinel_frame(frame) ? frame : NULL;
}

static int
compute_close_icon_x(WindowManager* wm, Window w)
{
    XWindowAttributes wa;
    XGetWindowAttributes(wm->display, w, &wa);
    return wa.width - wm->border_size - wm->frame_size - close_width;
}

static void
draw_frame(WindowManager* wm, Window w)
{
    Display* display = wm->display;
    Frame* frame = search_frame(wm, w);
    if (frame == NULL) {
        return;
    }
    XCopyArea(
        display,
        frame->close_icon,
        w,
        DefaultGC(display, DefaultScreen(display)),
        0, 0,
        close_width, close_height,
        compute_close_icon_x(wm, w), wm->border_size + wm->frame_size);
}

static void
change_event_mask(WindowManager* wm, Window w)
{
    XSetWindowAttributes swa;
    swa.event_mask = ButtonPressMask | ExposureMask | SubstructureNotifyMask;
    XChangeWindowAttributes(wm->display, w, CWEventMask, &swa);
}

static Frame*
alloc_frame()
{
    Frame* frame = (Frame*)malloc(sizeof(Frame));
    if (frame == NULL) {
        print_error("malloc failed.");
        abort();
    }
    memset(frame, 0xfb, sizeof(*frame));
    return frame;
}

static void
insert_frame(WindowManager* wm, Frame* frame)
{
    Frame* anchor = wm->frames;
    frame->prev = anchor;
    frame->next = anchor->next;
    anchor->next = frame;
}

static Frame*
create_frame(WindowManager* wm, int x, int y, int child_width, int child_height)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    int border_size = wm->border_size;
    int frame_size = wm->frame_size;
    int width = child_width + 2 * (border_size + frame_size);
    int title_height = wm->title_height;
    int height = child_height + title_height + 2 * border_size + 3 * frame_size;
    Window w = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        x, y,
        width, height,
        border_size,
        BlackPixel(display, screen), wm->focused_foreground_color);
    change_event_mask(wm, w);

    Frame* frame = alloc_frame();
    frame->window = w;
    frame->close_icon = XCreatePixmapFromBitmapData(
        display,
        w,
        (char*)close_bits,
        close_width, close_height,
        BlackPixel(display, screen), wm->focused_foreground_color,
        DefaultDepth(display, screen));
    insert_frame(wm, frame);

    return frame;
}

static void
reparent_window(WindowManager* wm, Window w)
{
    Display* display = wm->display;
    XWindowAttributes wa;
    XGetWindowAttributes(display, w, &wa);
    Frame* frame = create_frame(wm, wa.x, wa.y, wa.width, wa.height);
    frame->child = w;
    int border_size = wm->border_size;
    int frame_size = wm->frame_size;
    int x = border_size + frame_size;
    int y = border_size + 2 * frame_size + wm->title_height;
    XReparentWindow(display, w, frame->window, x, y);
    XMapWindow(display, frame->window);
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
remove_frame(WindowManager* wm, Frame* frame)
{
    frame->prev->next = frame->next;
    frame->next->prev = frame->prev;
}

static void
free_frame(WindowManager* wm, Frame* frame)
{
    remove_frame(wm, frame);
    memset(frame, 0xfd, sizeof(*frame));
    free(frame);
}

static void
destroy_frame(WindowManager* wm, Window w)
{
    Frame* frame = search_frame(wm, w);
    assert(frame != NULL);
    XKillClient(wm->display, frame->child);
}

static void
process_destroy_notify(WindowManager* wm, XDestroyWindowEvent* e)
{
    Window w = e->event;
    Frame* frame = search_frame(wm, w);
    if (frame == NULL) {
        return;
    }
    free_frame(wm, frame);
    XDestroyWindow(wm->display, w);
}

static void
setup_frame_list(WindowManager* wm)
{
    Frame* anchor = alloc_frame();
    anchor->prev = NULL;
    Frame* sentinel = alloc_frame();
    sentinel->next = NULL;
    anchor->next = sentinel;
    sentinel->prev = anchor;
    wm->frames = anchor;
}

static Bool
is_range_inside(int range_begin, int range_size, int n)
{
    return (range_begin <= n) && (n < range_begin + range_size);
}

static Bool
is_region_inside(int region_x, int region_y, int region_width, int region_height, int x, int y)
{
    if (!is_range_inside(region_x, region_width, x)) {
        return False;
    }
    if (!is_range_inside(region_y, region_height, y)) {
        return False;
    }
    return True;
}

static void
process_button_event(WindowManager* wm, XButtonEvent* e)
{
    if (e->button != Button1) {
        return;
    }
    Window w = e->window;
    int close_x = compute_close_icon_x(wm, w);
    int close_y = wm->border_size + wm->frame_size;
    int x = e->x;
    int y = e->y;
    if (is_region_inside(close_x, close_y, close_width, close_height, x, y)) {
        destroy_frame(wm, w);
    }
}

static void
wm_main(WindowManager* wm, Display* display)
{
    wm->display = display;
    wm->focused_foreground_color = alloc_color(wm, "light pink");
    wm->border_size = 1;
    wm->frame_size = 4;
    wm->title_height = 16;
    setup_frame_list(wm);

    reparent_toplevels(wm);

    XSelectInput(display, DefaultRootWindow(display), SubstructureRedirectMask);

    while (1) {
        XEvent e;
        XNextEvent(display, &e);
        if (e.type == ButtonPress) {
            process_button_event(wm, &e.xbutton);
        } else if (e.type == DestroyNotify) {
            process_destroy_notify(wm, &e.xdestroywindow);
        }
        else if (e.type == Expose) {
            draw_frame(wm, e.xexpose.window);
        }
        else if (e.type == MapRequest) {
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
    wm_main(&wm, display);

    XCloseDisplay(display);

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
