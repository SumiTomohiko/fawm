#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <uwm/bitmaps/close>

struct Frame {
    struct Frame* prev;
    struct Frame* next;
    Window window;
    Window child;
    Pixmap close_icon;
    GC title_font_gc;
};

typedef struct Frame Frame;

enum GraspedPosition {
    GP_NONE,
    GP_TITLE_BAR,
    GP_NORTH,
    GP_EAST,
    GP_SOUTH,
    GP_WEST
};

typedef enum GraspedPosition GraspedPosition;

struct WindowManager {
    Display* display;
    unsigned long focused_foreground_color;
    int border_size;
    int frame_size;
    int title_height;
    Frame* frames;
    GraspedPosition grasped_position;
    Window grasped_frame;
    int grasped_x;
    int grasped_y;
    Window popup_menu;
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
    return wa.width - wm->frame_size - close_width;
}

static void
draw_close_icon(WindowManager* wm, Frame* frame)
{
    Display* display = wm->display;
    Window w = frame->window;
    XCopyArea(
        display,
        frame->close_icon,
        w,
        DefaultGC(display, DefaultScreen(display)),
        0, 0,
        close_width, close_height,
        compute_close_icon_x(wm, w), wm->frame_size);
}

static void
get_window_name(WindowManager* wm, char* dest, int size, Window w)
{
    XTextProperty prop;
    dest[0] = '\0';
    if (XGetTextProperty(wm->display, w, &prop, XA_WM_NAME) == 0) {
        return;
    }
    Atom encoding = prop.encoding;
    Display* display = wm->display;
    Atom compound_text_atom = XInternAtom(display, "XA_COMPOUND_TEXT", False);
    /* FIXME: What is XA_COMPOUND_TEXT? */
    if ((encoding != XA_STRING) && (encoding != compound_text_atom)) {
        return;
    }

    char** strings;
    int _;
    if (XTextPropertyToStringList(&prop, &strings, &_) == 0) {
        return;
    }
    snprintf(dest, size, "%s", strings[0]);
    XFreeStringList(strings);
}

static void
draw_title_text(WindowManager* wm, Frame* frame)
{
    char text[64];
    get_window_name(wm, text, array_sizeof(text), frame->child);
    int frame_size = wm->frame_size;
    XDrawString(
        wm->display, frame->window, frame->title_font_gc,
        frame_size, frame_size + wm->title_height,
        text, strlen(text));
}

static void
draw_frame(WindowManager* wm, Window w)
{
    Frame* frame = search_frame(wm, w);
    if (frame == NULL) {
        return;
    }
    draw_title_text(wm, frame);
    draw_close_icon(wm, frame);
}

static void
change_frame_event_mask(WindowManager* wm, Window w)
{
    XSetWindowAttributes swa;
    swa.event_mask = ButtonMotionMask | ButtonPressMask | ButtonReleaseMask | ExposureMask | SubstructureNotifyMask;
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

static int
compute_frame_width(WindowManager* wm)
{
    return 2 * wm->frame_size;
}

static int
compute_frame_height(WindowManager* wm)
{
    return wm->title_height + 3 * wm->frame_size;
}

static GC
load_font_gc(WindowManager* wm, Window w)
{
    Display* display = wm->display;
    const char* name = "-vlgothic-vl_pgothic-medium-i-normal--0-0-0-0-p-0-iso10646-1";
    XFontStruct* font = XLoadQueryFont(display, name);
    if (font == NULL) {
        print_error("XLoadQueryFont failed: %s", name);
        return DefaultGC(display, DefaultScreen(display));
    }
    XGCValues gcv;
    gcv.foreground = BlackPixel(display, DefaultScreen(display));
    gcv.font = font->fid;
    GC gc = XCreateGC(display, w, GCFont | GCForeground, &gcv);
    XFreeFont(display, font);
    return gc;
}

static Frame*
create_frame(WindowManager* wm, int x, int y, int child_width, int child_height)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    int width = child_width + compute_frame_width(wm);
    int height = child_height + compute_frame_height(wm);
    Window w = XCreateSimpleWindow(
        display, DefaultRootWindow(display),
        x, y,
        width, height,
        wm->border_size,
        BlackPixel(display, screen), wm->focused_foreground_color);
    change_frame_event_mask(wm, w);

    Frame* frame = alloc_frame();
    frame->window = w;
    frame->close_icon = XCreatePixmapFromBitmapData(
        display,
        w,
        (char*)close_bits,
        close_width, close_height,
        BlackPixel(display, screen), wm->focused_foreground_color,
        DefaultDepth(display, screen));
    frame->title_font_gc = load_font_gc(wm, w);

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
    int frame_size = wm->frame_size;
    int x = frame_size;
    int y = 2 * frame_size + wm->title_height;
    XSetWindowBorderWidth(display, w, 0);
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
free_gc(WindowManager* wm, GC gc)
{
    Display* display = wm->display;
    if (gc == DefaultGC(display, DefaultScreen(display))) {
        return;
    }
    XFreeGC(display, gc);
}

static void
free_frame(WindowManager* wm, Frame* frame)
{
    remove_frame(wm, frame);

    Display* display = wm->display;
    free_gc(wm, frame->title_font_gc);
    XFreePixmap(display, frame->close_icon);

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
release_frame(WindowManager* wm)
{
    wm->grasped_position = GP_NONE;
}

static void
grasp_frame(WindowManager* wm, GraspedPosition pos, Window w, int x, int y)
{
    wm->grasped_position = pos;
    wm->grasped_frame = w;
    wm->grasped_x = x;
    wm->grasped_y = y;
}

static void
map_popup_menu(WindowManager* wm, int x, int y)
{
    Display* display = wm->display;
    Window popup_menu = wm->popup_menu;
    XMoveWindow(display, popup_menu, x - 16, y - 16);
    XMapRaised(display, popup_menu);
}

static void
process_button_press(WindowManager* wm, XButtonEvent* e)
{
    if (e->button != Button1) {
        return;
    }
    Window w = e->window;
    Display* display = wm->display;
    if (w == DefaultRootWindow(display)) {
        map_popup_menu(wm, e->x, e->y);
        return;
    }
    int frame_size = wm->frame_size;
    int close_x = compute_close_icon_x(wm, w);
    int close_y = wm->border_size + frame_size;
    int x = e->x;
    int y = e->y;
    if (is_region_inside(close_x, close_y, close_width, close_height, x, y)) {
        destroy_frame(wm, w);
        return;
    }
    XWindowAttributes wa;
    XGetWindowAttributes(wm->display, w, &wa);
    int width = wa.width;
    int height = wa.height;
    if (is_region_inside(0, 0, width, frame_size, x, y)) {
        grasp_frame(wm, GP_NORTH, w, x, y);
        return;
    }
    if (is_region_inside(width - frame_size, 0, frame_size, height, x, y)) {
        grasp_frame(wm, GP_EAST, w, x, y);
        return;
    }
    if (is_region_inside(0, height - frame_size, width, frame_size, x, y)) {
        grasp_frame(wm, GP_SOUTH, w, x, y);
        return;
    }
    if (is_region_inside(0, 0, frame_size, height, x, y)) {
        grasp_frame(wm, GP_WEST, w, x, y);
        return;
    }
    if (is_region_inside(0, 0, width, height, x, y)) {
        grasp_frame(wm, GP_TITLE_BAR, w, x, y);
        return;
    }
}

static void
unmap_popup_menu(WindowManager* wm)
{
    XUnmapWindow(wm->display, wm->popup_menu);
}

static void
unmap_popup_menu_if_out(WindowManager* wm, int x, int y)
{
    XWindowAttributes wa;
    XGetWindowAttributes(wm->display, wm->popup_menu, &wa);
    if (!is_region_inside(wa.x, wa.y, wa.width, wa.height, x, y)) {
        unmap_popup_menu(wm);
    }
}

static void
process_motion_notify(WindowManager* wm, XMotionEvent* e)
{
    Display* display = wm->display;
    Window w = e->window;
    if (w == DefaultRootWindow(display)) {
        unmap_popup_menu_if_out(wm, e->x, e->y);
        return;
    }
    GraspedPosition pos = wm->grasped_position;
    if (pos == GP_NONE) {
        return;
    }
    int border_size = wm->border_size;
    int new_x = e->x_root - wm->grasped_x - border_size;
    int new_y = e->y_root - wm->grasped_y - border_size;
    if (pos == GP_TITLE_BAR) {
        XMoveWindow(display, w, new_x, new_y);
        return;
    }
    XWindowAttributes frame_attrs;
    XGetWindowAttributes(display, w, &frame_attrs);
    Frame* frame = search_frame(wm, w);
    assert(frame != NULL);
    Window child = frame->child;
    XWindowAttributes child_attrs;
    XGetWindowAttributes(display, child, &child_attrs);
    int x = e->x;
    int y = e->y;
    int new_width;
    int new_height;
    switch (wm->grasped_position) {
    case GP_NORTH:
        new_height = frame_attrs.y + frame_attrs.height - new_y;
        XMoveResizeWindow(display, w,
            frame_attrs.x, new_y,
            frame_attrs.width, new_height);
        XResizeWindow(
            display, child,
            child_attrs.width, new_height - compute_frame_height(wm));
        return;
    case GP_EAST:
        new_width = x + (frame_attrs.width - wm->grasped_x);
        XResizeWindow(display, w, new_width, frame_attrs.height);
        XResizeWindow(
            display, child,
            new_width - compute_frame_width(wm), child_attrs.height);
        wm->grasped_x = x;
        return;
    case GP_SOUTH:
        new_height = y + (frame_attrs.height - wm->grasped_y);
        XResizeWindow(display, w, frame_attrs.width, new_height);
        XResizeWindow(
            display, child,
            child_attrs.width, new_height - compute_frame_height(wm));
        wm->grasped_y = y;
        return;
    case GP_WEST:
        new_width = frame_attrs.x + frame_attrs.width - new_x;
        XMoveResizeWindow(display, w,
            new_x, frame_attrs.y,
            new_width, frame_attrs.height);
        XResizeWindow(
            display, child,
            new_width - compute_frame_width(wm), child_attrs.height);
        return;
    case GP_NONE:
    case GP_TITLE_BAR:
    default:
        assert(False);
    }
}

static void
get_last_event(WindowManager* wm, Window w, int event_type, XEvent* e)
{
    Display* display = wm->display;
    while (XCheckTypedWindowEvent(display, w, event_type, e));
}

static void
process_expose(WindowManager* wm, XExposeEvent* e)
{
    if (e->x == wm->frame_size) {
        /**
         * If above condition is true, this Expose event may be a result of
         * killing a child window. At this time, WM does not draw a frame,
         * because parent frame will be destroyed soon.
         * FIXME: More strict checking?
         */
        return;
    }
    draw_frame(wm, e->window);
}

static void
process_event(WindowManager* wm, XEvent* e)
{
    if (e->type == ButtonPress) {
        process_button_press(wm, &e->xbutton);
    }
    else if (e->type == ButtonRelease) {
        unmap_popup_menu(wm);
        release_frame(wm);
    }
    else if (e->type == DestroyNotify) {
        process_destroy_notify(wm, &e->xdestroywindow);
    }
    else if (e->type == Expose) {
        process_expose(wm, &e->xexpose);
    }
    else if (e->type == MotionNotify) {
        get_last_event(wm, e->xmotion.window, MotionNotify, e);
        process_motion_notify(wm, &e->xmotion);
    }
    else if (e->type == MapRequest) {
        reparent_window(wm, e->xmaprequest.window);
    }
}

static Window
create_popup_menu(WindowManager* wm)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    return XCreateSimpleWindow(
        display, DefaultRootWindow(display),
        0, 0,
        256, 256,
        wm->border_size,
        BlackPixel(display, screen), WhitePixel(display, screen));
}

static void
setup_window_manager(WindowManager* wm, Display* display)
{
    wm->display = display;
    wm->focused_foreground_color = alloc_color(wm, "light pink");
    wm->border_size = 1;
    wm->frame_size = 4;
    wm->title_height = 16;
    setup_frame_list(wm);
    release_frame(wm);
    wm->popup_menu = create_popup_menu(wm);
}

static void
wm_main(WindowManager* wm, Display* display)
{
    setup_window_manager(wm, display);
    reparent_toplevels(wm);
    long mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | SubstructureRedirectMask;
    XSelectInput(display, DefaultRootWindow(display), mask);

    while (1) {
        XEvent e;
        XNextEvent(display, &e);
        process_event(wm, &e);
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
