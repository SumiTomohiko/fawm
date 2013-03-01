#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/cursorfont.h>

#include <fawm/config.h>
#include <fawm/private.h>

struct Frame {
    Window window;
    Window child;
    XftDraw* draw;
    Bool wm_delete_window;
    char title[64];
    int width_inc;
    int height_inc;
    GC line_gc;
    GC focused_gc;
    GC unfocused_gc;
    enum {
        FOCUS_NONE,
        FOCUS_MINIMIZE,
        FOCUS_MAXIMIZE,
        FOCUS_CLOSE
    } status;
};

typedef struct Frame Frame;

struct Array {
    int size;
    int capacity;
    struct Frame** items;
};

typedef struct Array Array;

enum GraspedPosition {
    GP_NONE,
    GP_TITLE_BAR,
    GP_NORTH,
    GP_NORTH_EAST,
    GP_EAST,
    GP_SOUTH_EAST,
    GP_SOUTH,
    GP_SOUTH_WEST,
    GP_WEST,
    GP_NORTH_WEST
};

typedef enum GraspedPosition GraspedPosition;

struct WindowManager {
    Display* display;
    Bool running;

    unsigned long focused_foreground_color;
    unsigned long unfocused_foreground_color;
    int border_size;
    int client_border_size;
    int frame_size;
    int title_height;
    int resizable_corner_size;
    int padding_size;

    Array all_frames;
    Array frames_z_order;

    GraspedPosition grasped_position;
    Window grasped_frame;
    int grasped_x;
    int grasped_y;
    int grasped_width;
    int grasped_height;

    XftFont* title_font;
    XftColor title_color;

    Cursor normal_cursor;
    Cursor bottom_left_cursor;
    Cursor bottom_right_cursor;
    Cursor bottom_cursor;
    Cursor left_cursor;
    Cursor right_cursor;
    Cursor top_left_cursor;
    Cursor top_right_cursor;
    Cursor top_cursor;

    struct {
        Window window;
        GC title_gc;
        GC selected_gc;
        XftDraw* draw;
        int margin;
        int selected_item;
    } popup_menu;

    struct {
        Window window;
        XftDraw* draw;

        XftFont* clock_font;
        int clock_margin;
        time_t clock;
        int clock_x;

        GC line_gc;
        GC focused_gc;
    } taskbar;

    struct {
        Atom wm_delete_window;
        Atom wm_protocols;
    } atoms;

    FILE* log_file; /* For debug */

    struct Config* config;
};

typedef struct WindowManager WindowManager;

#define array_sizeof(x) (sizeof((x)) / sizeof((x)[0]))

static void
print_message(FILE* fp, const char* fmt, va_list ap)
{
    if (fp == NULL) {
        return;
    }
    char buf[512];
    vsnprintf(buf, array_sizeof(buf), fmt, ap);
    fprintf(fp, "%s\n", buf);
    fflush(fp);
}

static void
output_log(WindowManager* wm, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    print_message(wm->log_file, fmt, ap);
    va_end(ap);
}

#define LOG_HEAD_FMT "%s:%u [%u] "
#define LOG_HEAD __FILE__, __LINE__, getpid()
#define LOG(wm, fmt, ...) \
    output_log(wm, LOG_HEAD_FMT fmt, LOG_HEAD, __VA_ARGS__)
#define LOG0(wm, msg) output_log(wm, LOG_HEAD_FMT msg, LOG_HEAD)
#define LOG_X(filename, lineno, wm, fmt, ...) \
    output_log(wm, LOG_HEAD_FMT fmt, filename, lineno, getpid(), __VA_ARGS__)
#define LOG_X0(filename, lineno, wm, msg) \
    output_log(wm, LOG_HEAD_FMT msg, filename, lineno, getpid())

static void
print_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    print_message(stderr, fmt, ap);
    va_end(ap);
}

static void
initialize_array(Array* a)
{
    a->size = a->capacity = 0;
    a->items = NULL;
}

static void
ensure_array_size(Array* a, int size)
{
    if (size <= a->capacity) {
        return;
    }
    int capacity = 8 + a->capacity;
    Frame** p = (Frame**)realloc(a->items, sizeof(a->items[0]) * capacity);
    assert(p != NULL);
    a->capacity = capacity;
    a->items = p;
}

static void
remove_from_array(Array* a, Frame* f)
{
    int size = a->size;
    int i;
    for (i = 0; (i < size) && (a->items[i] != f); i++) {
    }
    if (i == size) {
        return;
    }
    int rest = size - i - 1;
    memcpy(&a->items[i], &a->items[i + 1], sizeof(a->items[0]) * rest);
    a->size--;
}

static void
prepend_to_array(Array* a, Frame* f)
{
    ensure_array_size(a, a->size + 1);
    memmove(&a->items[1], &a->items[0], sizeof(a->items[0]) * a->size);
    a->items[0] = f;
    a->size++;
}

static void
append_to_array(Array* a, Frame* f)
{
    int size = a->size;
    ensure_array_size(a, size + 1);
    a->items[size] = f;
    a->size++;
}

static Frame*
search_in_array(Array* a, Bool (*pred)(Frame*, Window), Window w)
{
    Frame** items = a->items;
    int size = a->size;
    int i;
    for (i = 0; (i < size) && !pred(items[i], w); i++) {
    }
    return i == size ? NULL : items[i];
}

static Bool
is_child(Frame* f, Window w)
{
    return f->child == w;
}

static Frame*
search_frame_of_child(WindowManager* wm, Window w)
{
    return search_in_array(&wm->all_frames, is_child, w);
}

static Bool
is_frame(Frame* f, Window w)
{
    return f->window == w;
}

static Frame*
search_frame(WindowManager* wm, Window w)
{
    return search_in_array(&wm->all_frames, is_frame, w);
}

static int
__XAddToSaveSet__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w)
{
    LOG_X(filename, lineno, wm, "XAddToSaveSet(display, w=0x%08x)", w);
    return XAddToSaveSet(display, w);
}

#define XXAddToSaveSet(wm, a, b) \
    __XAddToSaveSet__(__FILE__, __LINE__, (wm), (a), (b))

static Status
__XAllocNamedColor__(const char* filename, int lineno, WindowManager* wm, Display* display, Colormap colormap, const char* color_name, XColor* color_def_return, XColor* exact_def_return)
{
    LOG_X(filename, lineno, wm, "XAllocNamedColor(display, colormap, color_name=\"%s\", color_def_return, exact_def_return)", color_name);
    return XAllocNamedColor(display, colormap, color_name, color_def_return, exact_def_return);
}

#define XXAllocNamedColor(wm, a, b, c, d, e) \
    __XAllocNamedColor__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e))

static int
__XAllowEvents__(const char* filename, int lineno, WindowManager* wm, Display* display, int event_mode, Time time)
{
    LOG_X0(filename, lineno, wm, "XAllowEvents(display, event_mode, time)");
    return XAllowEvents(display, event_mode, time);
}

#define XXAllowEvents(wm, a, b, c) \
    __XAllowEvents__(__FILE__, __LINE__, (wm), (a), (b), (c))

static int
__XChangeWindowAttributes__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, unsigned long valuemask, XSetWindowAttributes* attributes)
{
    LOG_X(filename, lineno, wm, "XChangeWindowAttributes(display, w=0x%08x, valuemask, attributes)", w);
    return XChangeWindowAttributes(display, w, valuemask, attributes);
}

#define XXChangeWindowAttributes(wm, a, b, c, d) \
    __XChangeWindowAttributes__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static Bool
__XCheckTypedWindowEvent__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, int event_type, XEvent* event_return)
{
    LOG_X(filename, lineno, wm, "XCheckTypedWindowEvent(display, w=0x%08x, event_type, event_return)", w);
    return XCheckTypedWindowEvent(display, w, event_type, event_return);
}

#define XXCheckTypedWindowEvent(wm, a, b, c, d) \
    __XCheckTypedWindowEvent__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static int
__XConfigureWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, unsigned value_mask, XWindowChanges* changes)
{
    LOG_X(filename, lineno, wm, "XConfigureWindow(display, w=0x%08x, value_mask, changes)", w);
    return XConfigureWindow(display, w, value_mask, changes);
}

#define XXConfigureWindow(wm, a, b, c, d) \
    __XConfigureWindow__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

#if 0
static int
__XCopyArea__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable src, Drawable dest, GC gc, int src_x, int src_y, unsigned int width, unsigned int height, int dest_x, int dest_y)
{
    LOG_X(filename, lineno, wm, "XCopyArea(display, src=0x%08x, dest=0x%08x, gc, src_x=%d, src_y=%d, width=%d, height=%d, dest_x=%d, dest_y=%d)", src, dest, src_x, src_y, width, height, dest_x, dest_y);
    return XCopyArea(display, src, dest, gc, src_x, src_y, width, height, dest_x, dest_y);
}

#define XXCopyArea(wm, a, b, c, d, e, f, g, h, i, j) \
    __XCopyArea__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g), (h), (i), (j))
#endif

static Cursor
__XCreateFontCursor__(const char* filename, int lineno, WindowManager* wm, Display* display, unsigned int shape)
{
    LOG_X(filename, lineno, wm, "XCreateFontCursor(display, shape=%d)", shape);
    return XCreateFontCursor(display, shape);
}

#define XXCreateFontCursor(wm, a, b) \
    __XCreateFontCursor__(__FILE__, __LINE__, (wm), (a), (b))

static GC
__XCreateGC__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable d, unsigned long valuemask, XGCValues* values)
{
    LOG_X(filename, lineno, wm, "XCreateGC(display, d=0x%08x, valuemask, values)", d);
    return XCreateGC(display, d, valuemask, values);
}

#define XXCreateGC(wm, a, b, c, d) \
    __XCreateGC__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

#if 0
static Pixmap
__XCreatePixmapFromBitmapData__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable d, char* data, unsigned int width, unsigned int height, unsigned long fg, unsigned long bg, unsigned int depth)
{
    LOG_X(filename, lineno, wm, "XCreatePixmapFromBitmapData(display, d=0x%08x, data, width=%u, height=%u, fg, bg, depth=%u)", d, width, height, depth);
    return XCreatePixmapFromBitmapData(display, d, data, width, height, fg, bg, depth);
}

#define XXCreatePixmapFromBitmapData(wm, a, b, c, d, e, f, g, h) \
    __XCreatePixmapFromBitmapData__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g), (h))
#endif

static Window
__XCreateSimpleWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window parent, int x, int y, unsigned int width, unsigned int height, unsigned int border_width, unsigned long border, unsigned long background)
{
    LOG_X(filename, lineno, wm, "XCreateSimpleWindow(display, parent=0x%08x, x=%d, y=%d, width=%u, height=%u, border_width=%u, border, background)", parent, x, y, width, height, border_width);
    return XCreateSimpleWindow(display, parent, x, y, width, height, border_width, border, background);
}

#define XXCreateSimpleWindow(wm, a, b, c, d, e, f, g, h, i) \
    __XCreateSimpleWindow__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g), (h), (i))

static int
__XDefineCursor__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, Cursor cursor)
{
    LOG_X(filename, lineno, wm, "XDefineCursor(display, w=0x%08x, cursor)", w);
    return XDefineCursor(display, w, cursor);
}

#define XXDefineCursor(wm, a, b, c) \
    __XDefineCursor__(__FILE__, __LINE__, (wm), (a), (b), (c))

static int
__XDrawRectangle__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable d, GC gc, int x, int y, int width, int height)
{
#define FMT "XDrawRectangle(display, d=0x%08x, gc, x=%d, y=%d, width=%d, height=%d)"
    LOG_X(filename, lineno, wm, FMT, d, x, y, width, height);
#undef FMT
    return XDrawRectangle(display, d, gc, x, y, width, height);
}

#define XXDrawRectangle(wm, a, b, c, d, e, f, g) \
    __XDrawRectangle__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g))

static int
__XDrawLine__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable d, GC gc, int x1, int y1, int x2, int y2)
{
    LOG_X(filename, lineno, wm, "XDrawLine(display, d=0x%08x, gc, x1=%d, y1=%d, x2=%d, y2=%d)", d, x1, y1, x2, y2);
    return XDrawLine(display, d, gc, x1, y1, x2, y2);
}

#define XXDrawLine(wm, a, b, c, d, e, f, g) \
    __XDrawLine__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g))

static int
__XFillRectangle__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height)
{
    LOG_X(filename, lineno, wm, "XFillRectangle(display, d=0x%08x, gc, x=%d, y=%d, width=%u, height=%u)", d, x, y, width, height);
    return XFillRectangle(display, d, gc, x, y, width, height);
}

#define XXFillRectangle(wm, a, b, c, d, e, f, g) \
    __XFillRectangle__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g))

#if 0
static int
__XFreePixmap__(const char* filename, int lineno, WindowManager* wm, Display* display, Pixmap pixmap)
{
    LOG_X0(filename, lineno, wm, "XFreePixmap(display, pixmap)");
    return XFreePixmap(display, pixmap);
}

#define XXFreePixmap(wm, a, b) \
    __XFreePixmap__(__FILE__, __LINE__, (wm), (a), (b))
#endif

static int
__XFree__(const char* filename, int lineno, WindowManager* wm, void* data)
{
    LOG_X(filename, lineno, wm, "XFree(data=%p)", data);
    return XFree(data);
}

#define XXFree(wm, data) __XFree__(__FILE__, __LINE__, (wm), (data))

static Status
__XGetGeometry__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable d, Window* root_return, int* x_return, int* y_return, unsigned int* width_return, unsigned int* height_return, unsigned int* border_width_return, unsigned int* depth_return)
{
    LOG_X(filename, lineno, wm, "XGetGeometry(display, d=0x%08x, root_return, x_return, y_return, width_return, height_return, border_width_return, depth_return)", d);
    return XGetGeometry(display, d, root_return, x_return, y_return, width_return, height_return, border_width_return, depth_return);
}

#define XXGetGeometry(wm, a, b, c, d, e, f, g, h, i) \
    __XGetGeometry__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g), (h), (i))

static Status
__XGetTextProperty__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, XTextProperty* text_prop_return, Atom property)
{
    LOG_X(filename, lineno, wm, "XGetTextProperty(display, w=0x%08x, text_prop_return, property)", w);
    return XGetTextProperty(display, w, text_prop_return, property);
}

#define XXGetTextProperty(wm, a, b, c, d) \
    __XGetTextProperty__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static Status
__XGetWindowAttributes__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, XWindowAttributes* window_attributes_return)
{
    LOG_X(filename, lineno, wm, "XGetWindowAttributes(display, w=0x%08x, window_attributes_return=%p)", w, window_attributes_return);
    return XGetWindowAttributes(display, w, window_attributes_return);
}

#define XXGetWindowAttributes(wm, a, b, c) \
    __XGetWindowAttributes__(__FILE__, __LINE__, (wm), (a), (b), (c))

static Status
__XGetWMProtocols__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, Atom** protocols_return, int* count_return)
{
    LOG_X(filename, lineno, wm, "XGetWMProtocols(display, w=0x%08x, protocols_return, count_return)", w);
    return XGetWMProtocols(display, w, protocols_return, count_return);
}

#define XXGetWMProtocols(wm, a, b, c, d) \
    __XGetWMProtocols__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static int
__XGrabButton__(const char* filename, int lineno, WindowManager* wm, Display* display, unsigned int button, unsigned int modifiers, Window grab_window, Bool owner_events, unsigned int event_mask, int pointer_mode, int keyboard_mode, Window confine_to, Cursor cursor)
{
    LOG_X(filename, lineno, wm, "XGrabButton(display, button, modifiers, grab_window=0x%08x, owner_events, event_mask, pointer_mode, keyboard_mode, confine_to=0x%08x, cursor)", grab_window, confine_to);
    return XGrabButton(display, button, modifiers, grab_window, owner_events, event_mask, pointer_mode, keyboard_mode, confine_to, cursor);
}

#define XXGrabButton(wm, a, b, c, d, e, f, g, h, i, j) \
    __XGrabButton__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g), (h), (i), (j))

static Atom
__XInternAtom__(const char* filename, int lineno, WindowManager* wm, Display* display, const char* name, Bool only_if_exists)
{
    LOG_X(filename, lineno, wm, "XInternAtom(display, name=\"%s\", only_if_exists)", name);
    return XInternAtom(display, name, only_if_exists);
}

#define XXInternAtom(wm, a, b, c) \
    __XInternAtom__(__FILE__, __LINE__, (wm), (a), (b), (c))

static int
__XKillClient__(const char* filename, int lineno, WindowManager* wm, Display* display, XID resource)
{
    LOG_X0(filename, lineno, wm, "XKillClient(display, resource)");
    return XKillClient(display, resource);
}

#define XXKillClient(wm, a, b) \
    __XKillClient__(__FILE__, __LINE__, (wm), (a), (b))

static int
__XMapRaised__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w)
{
    LOG_X(filename, lineno, wm, "XMapRaised(display, w=0x%08x)", w);
    return XMapRaised(display, w);
}

#define XXMapRaised(wm, a, b) __XMapRaised__(__FILE__, __LINE__, (wm), (a), (b))

static int
__XMapWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w)
{
    LOG_X(filename, lineno, wm, "XMapWindow(display, w=0x%08x)", w);
    return XMapWindow(display, w);
}

#define XXMapWindow(wm, a, b) __XMapWindow__(__FILE__, __LINE__, (wm), (a), (b))

static int
__XMoveResizeWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, int x, int y, unsigned width, unsigned height)
{
    LOG_X(filename, lineno, wm, "XMoveResizeWindow(display, w=0x%08x, x=%d, y=%d, width=%u, height=%u)", w, x, y, width, height);
    return XMoveResizeWindow(display, w, x, y, width, height);
}

#define XXMoveResizeWindow(wm, a, b, c, d, e, f) \
    __XMoveResizeWindow__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f))

static int
__XMoveWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, int x, int y)
{
    LOG_X(filename, lineno, wm, "XMoveWindow(display, w=0x%08x, x=%d, y=%d)", w, x, y);
    return XMoveWindow(display, w, x, y);
}

#define XXMoveWindow(wm, a, b, c, d) \
    __XMoveWindow__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static Status
__XQueryTree__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, Window* root_return, Window* parent_return, Window** children_return, unsigned int* nchildren_return)
{
    LOG_X(filename, lineno, wm, "XQueryTree(display, w=0x%08x, root_return, parent_return, children_return, nchildren_return)", w);
    return XQueryTree(display, w, root_return, parent_return, children_return, nchildren_return);
}

#define XXQueryTree(wm, a, b, c, d, e, f) \
    __XQueryTree__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f))

static int
__XRaiseWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w)
{
    LOG_X(filename, lineno, wm, "XRaiseWindow(display, w=0x%08x)", w);
    return XRaiseWindow(display, w);
}

#define XXRaiseWindow(wm, a, b) \
    __XRaiseWindow__(__FILE__, __LINE__, (wm), (a), (b))

static int
__XReparentWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, Window parent,  int x, int y)
{
    LOG_X(filename, lineno, wm, "XReparentWindow(display, w=0x%08x, parent=0x%08x, x=%d, y=%d)", w, parent, x, y);
    return XReparentWindow(display, w, parent, x, y);
}

#define XXReparentWindow(wm, a, b, c, d, e) \
    __XReparentWindow__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e))

static int
__XResizeWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, unsigned width, unsigned height)
{
    LOG_X(filename, lineno, wm, "XResizeWindow(display, w=0x%08x, width=%u, height=%u)", w, width, height);
    return XResizeWindow(display, w, width, height);
}

#define XXResizeWindow(wm, a, b, c, d) \
    __XResizeWindow__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static Status
__XSendEvent__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, Bool propagate, long event_mask, XEvent* event_send)
{
    LOG_X(filename, lineno, wm, "XSendEvent(display, w=0x%08x, propagate, event_mask, event_send)", w);
    return XSendEvent(display, w, propagate, event_mask, event_send);
}

#define XXSendEvent(wm, a, b, c, d, e) \
    __XSendEvent__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e))

static int
__XSetInputFocus__(const char* filename, int lineno, WindowManager* wm, Display* display, Window focus, int revert_to, Time time)
{
    LOG_X(filename, lineno, wm, "XSetInputFocus(display, focus=0x%08x, revert_to, time)", focus);
    return XSetInputFocus(display, focus, revert_to, time);
}

#define XXSetInputFocus(wm, a, b, c, d) \
    __XSetInputFocus__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static int
__XSetWindowBackground__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, unsigned long background_pixel)
{
    LOG_X(filename, lineno, wm, "XSetWindowBackground(display, w=0x%08x, background_pixel)", w);
    return XSetWindowBackground(display, w, background_pixel);
}

#define XXSetWindowBackground(wm, a, b, c) \
    __XSetWindowBackground__(__FILE__, __LINE__, (wm), (a), (b), (c))

static int
__XSetWindowBorderWidth__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, unsigned width)
{
    LOG_X(filename, lineno, wm, "XSetWindowBorderWidth(display, w=0x%08x, width=%u)", w, width);
    return XSetWindowBorderWidth(display, w, width);
}

#define XXSetWindowBorderWidth(wm, a, b, c) \
    __XSetWindowBorderWidth__(__FILE__, __LINE__, (wm), (a), (b), (c))

static Status
__XTextPropertyToStringList__(const char* filename, int lineno, WindowManager* wm, XTextProperty* text_prop, char*** list_return, int* count_return)
{
    LOG_X0(filename, lineno, wm, "XTextPropertyToStringList(text_prop, list_return, count_return)");
    return XTextPropertyToStringList(text_prop, list_return, count_return);
}

#define XXTextPropertyToStringList(wm, a, b, c) \
    __XTextPropertyToStringList__(__FILE__, __LINE__, (wm), (a), (b), (c))

static int
__XUnmapWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w)
{
    LOG_X(filename, lineno, wm, "XUnmapWindow(display, w=0x%08x)", w);
    return XUnmapWindow(display, w);
}

#define XXUnmapWindow(wm, a, b) \
    __XUnmapWindow__(__FILE__, __LINE__, (wm), (a), (b))

static Bool
__XftColorAllocName__(const char* filename, int lineno, WindowManager* wm, Display* display, Visual* visual, Colormap colormap, char* name, XftColor* result)
{
    LOG_X(filename, lineno, wm, "XftColorAllocName(display, visual, colormap, name=\"%s\", result)", name);
    return XftColorAllocName(display, visual, colormap, name, result);
}

#define XXftColorAllocName(wm, a, b, c, d, e) \
    __XftColorAllocName__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e))

static XftDraw*
__XftDrawCreate__(const char* filename, int lineno, WindowManager* wm, Display* display, Drawable d, Visual* visual, Colormap colormap)
{
    LOG_X(filename, lineno, wm, "XftDrawCreate(display, d=0x%08x, visual, colormap)", d);
    return XftDrawCreate(display, d, visual, colormap);
}

#define XXftDrawCreate(wm, a, b, c, d) \
    __XftDrawCreate__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static void
__XftDrawDestroy__(const char* filename, int lineno, WindowManager* wm, XftDraw* draw)
{
    LOG_X0(filename, lineno, wm, "XftDrawDestroy(draw)");
    return XftDrawDestroy(draw);
}

#define XXftDrawDestroy(wm, a) __XftDrawDestroy__(__FILE__, __LINE__, (wm), (a))

static void
__XftDrawStringUtf8__(const char* filename, int lineno, WindowManager* wm, XftDraw* draw, XftColor* color, XftFont* pub, int x, int y, FcChar8* string, int len)
{
    LOG_X(filename, lineno, wm, "XftDrawStringUtf8(draw, color, pub, x=%d, y=%d, string, len=%d)", x, y, len);
    return XftDrawStringUtf8(draw, color, pub, x, y, string, len);
}

#define XXftDrawStringUtf8(wm, a, b, c, d, e, f, g) \
    __XftDrawStringUtf8__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g))

static void
__XftTextExtentsUtf8__(const char* filename, int lineno, WindowManager* wm, Display* display, XftFont* font, XftChar8* string, int len, XGlyphInfo* extents)
{
    LOG_X(filename, lineno, wm, "XftTextExtentsUtf8(display=0x%08x, font=0x%08x, string=\"%s\", len=%d, extents=%p)", display, font, string, len, extents);
    XftTextExtentsUtf8(display, font, string, len, extents);
}

#define XXftTextExtentsUtf8(wm, a, b, c, d, e) \
    __XftTextExtentsUtf8__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e))

static XftFont*
__XftFontOpenName__(const char* filename, int lineno, WindowManager* wm, Display* display, int screen, const char* name)
{
    LOG_X(filename, lineno, wm, "XftFontOpenName(display, screen, name=\"%s\")", name);
    return XftFontOpenName(display, screen, name);
}

#define XXftFontOpenName(wm, a, b, c) \
    __XftFontOpenName__(__FILE__, __LINE__, (wm), (a), (b), (c))

static void
draw_box(WindowManager* wm, Frame* frame, int width, int height, int n, int status)
{
    Display* display = wm->display;
    Window w = frame->window;

    int size = wm->title_height;
    int frame_size = wm->frame_size;
    int x = width - frame_size - n * size;
    int y = frame_size;
    GC unfocused_gc = frame->unfocused_gc;
    GC fill_gc = frame->status == status ? frame->focused_gc : unfocused_gc;
    XXFillRectangle(wm, display, w, fill_gc, x, y, size, size);
    XXDrawRectangle(wm, display, w, frame->line_gc, x, y, size, size);
}

static void
draw_boxes(WindowManager* wm, Frame* frame, int width, int height)
{
    draw_box(wm, frame, width, height, 1, FOCUS_CLOSE);
    draw_box(wm, frame, width, height, 2, FOCUS_MAXIMIZE);
    draw_box(wm, frame, width, height, 3, FOCUS_MINIMIZE);
}

static Atom
intern(WindowManager* wm, const char* name)
{
    return XXInternAtom(wm, wm->display, name, False);
}

static void
__XFreeStringList__(const char* filename, int lineno, WindowManager* wm, char** list)
{
    LOG_X(filename, lineno, wm, "XFreeStringList(list=%p)", list);
    XFreeStringList(list);
}

#define XXFreeStringList(wm, a) \
    __XFreeStringList__(__FILE__, __LINE__, (wm), (a))

static void
get_window_name(WindowManager* wm, char* dest, int size, Window w)
{
    XTextProperty prop;
    dest[0] = '\0';
    if (XXGetTextProperty(wm, wm->display, w, &prop, XA_WM_NAME) == 0) {
        return;
    }
    Atom encoding = prop.encoding;
    Atom compound_text_atom = intern(wm, "XA_COMPOUND_TEXT");
    /* FIXME: What is XA_COMPOUND_TEXT? */
    if ((encoding != XA_STRING) && (encoding != compound_text_atom)) {
        return;
    }

    char** strings;
    int _;
    if (XXTextPropertyToStringList(wm, &prop, &strings, &_) == 0) {
        return;
    }
    if (strings == NULL) {
        return;
    }
    snprintf(dest, size, "%s", strings[0]);
    XXFreeStringList(wm, strings);
}

static void
draw_title_font_string(WindowManager* wm, XftDraw* draw, int x, int y, const char* text)
{
    XftColor* color = &wm->title_color;
    XftFont* font = wm->title_font;
    int len = strlen(text);
    XXftDrawStringUtf8(wm, draw, color, font, x, y, (XftChar8*)text, len);
}

static void
draw_title_text(WindowManager* wm, Frame* frame)
{
    int frame_size = wm->frame_size;
    int x = frame_size;
    int y = frame_size + wm->title_font->ascent;
    draw_title_font_string(wm, frame->draw, x, y, frame->title);
}

static void
get_geometry(WindowManager* wm, Window w, unsigned int* width, unsigned int* height)
{
    Window _;
    int __;
    unsigned int ___;
    XXGetGeometry(wm, wm->display, w, &_, &__, &__, width, height, &___, &___);
}

static void
draw_corner(WindowManager* wm, Window w, int width, int height)
{
    Display* display = wm->display;
    GC gc = DefaultGC(display, DefaultScreen(display));
    int frame_size = wm->frame_size;
    int corner_size = wm->resizable_corner_size;
#define DRAW_LINE(x1, y1, x2, y2) \
    XXDrawLine(wm, display, w, gc, (x1), (y1), (x2), (y2))
#define DRAW_HORIZONTAL_LINE(x1, y1, x2) DRAW_LINE((x1), (y1), (x2), (y1))
#define DRAW_VIRTICAL_LINE(x1, y1, y2) DRAW_LINE((x1), (y1), (x1), (y2))
    DRAW_HORIZONTAL_LINE(0, corner_size, frame_size);
    DRAW_VIRTICAL_LINE(corner_size, 0, frame_size);
    int east_x1 = width - corner_size;
    DRAW_VIRTICAL_LINE(east_x1, 0, frame_size);
    int east_x2 = width - frame_size;
    DRAW_HORIZONTAL_LINE(east_x2, corner_size, width);
    int south_y1 = height - corner_size;
    DRAW_HORIZONTAL_LINE(east_x2, south_y1, width);
    int south_y2 = height - frame_size;
    DRAW_VIRTICAL_LINE(east_x1, south_y2, height);
    DRAW_VIRTICAL_LINE(corner_size, south_y2, height);
    DRAW_HORIZONTAL_LINE(0, south_y1, frame_size);
#undef DRAW_VIRTICAL_LINE
#undef DRAW_HORIZONTAL_LINE
#undef DRAW_LINE
}

static void
draw_frame(WindowManager* wm, Window w)
{
    Frame* frame = search_frame(wm, w);
    if (frame == NULL) {
        return;
    }
    unsigned int width;
    unsigned int height;
    get_geometry(wm, frame->window, &width, &height);

    draw_title_text(wm, frame);
    draw_boxes(wm, frame, width, height);
    draw_corner(wm, w, width, height);
}

static void
change_frame_event_mask(WindowManager* wm, Window w)
{
    XSetWindowAttributes swa;
    swa.event_mask = 0
        | ButtonPressMask
        | ButtonReleaseMask
        | ExposureMask
        | FocusChangeMask
        | LeaveWindowMask
        | PointerMotionMask
        | PropertyChangeMask
        | SubstructureNotifyMask
        | SubstructureRedirectMask;
    XXChangeWindowAttributes(wm, wm->display, w, CWEventMask, &swa);
}

static void*
alloc_memory(size_t size)
{
    void* p = malloc(size);
    if (p == NULL) {
        print_error("malloc failed.");
        abort();
    }
    memset(p, 0xfb, size);
    return p;
}

static Frame*
alloc_frame()
{
    return (Frame*)alloc_memory(sizeof(Frame));
}

static void
insert_frame(WindowManager* wm, Frame* frame)
{
    append_to_array(&wm->all_frames, frame);
    prepend_to_array(&wm->frames_z_order, frame);
}

static int
compute_frame_width(WindowManager* wm)
{
    /**
     * Name of this function is "compute_frame_width", but this returns value
     * including client borders size.
     */
    return 2 * (wm->frame_size + wm->client_border_size);
}

static int
compute_frame_height(WindowManager* wm)
{
    /**
     * Name of this function is "compute_frame_height", but this returns value
     * including client borders size.
     */
    return wm->title_height + 3 * wm->frame_size + 2 * wm->client_border_size;
}

static XftDraw*
create_draw(WindowManager* wm, Window w)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    Visual* visual = DefaultVisual(display, screen);
    Colormap colormap = DefaultColormap(display, screen);
    return XXftDrawCreate(wm, display, w, visual, colormap);
}

static GC
create_foreground_gc(WindowManager* wm, Window w, int pixel)
{
    XGCValues v;
    v.foreground = pixel;
    return XXCreateGC(wm, wm->display, w, GCForeground, &v);
}

static Frame*
create_frame(WindowManager* wm, int x, int y, int child_width, int child_height)
{
    Display* display = wm->display;
    int width = child_width + compute_frame_width(wm);
    int height = child_height + compute_frame_height(wm);
    int focused_color = wm->focused_foreground_color;
    int black = BlackPixel(display, DefaultScreen(display));
    Window w = XXCreateSimpleWindow(
        wm,
        display, DefaultRootWindow(display),
        x, y,
        width, height,
        wm->border_size,
        black, focused_color);
    change_frame_event_mask(wm, w);

    Frame* frame = alloc_frame();
    frame->window = w;
    frame->draw = create_draw(wm, w);
    assert(frame->draw != NULL);
    frame->wm_delete_window = False;
    frame->width_inc = frame->height_inc = 1;

    frame->line_gc = create_foreground_gc(wm, w, black);
    frame->focused_gc = create_foreground_gc(wm, w, focused_color);
    int color = wm->unfocused_foreground_color;
    frame->unfocused_gc = create_foreground_gc(wm, w, color);
    frame->status = FOCUS_NONE;

    insert_frame(wm, frame);

    return frame;
}

static void
move_frame_to_z_order_head(WindowManager* wm, Frame* frame)
{
    Array* a = &wm->frames_z_order;
    /* FIXME: Do it in one function. */
    remove_from_array(a, frame);
    prepend_to_array(a, frame);
}

static int
__XClearArea__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, int x, int y, unsigned width, unsigned height, Bool exposures)
{
    LOG_X(filename, lineno, wm, "XClearArea(display, w=0x%08x, x=%d, y=%d, width=%u, height=%u, exposures)", w, x, y, width, height);
    return XClearArea(display, w, x, y, width, height, exposures);
}

#define XXClearArea(wm, a, b, c, d, e, f, g) \
    __XClearArea__(__FILE__, __LINE__, (wm), (a), (b), (c), (d), (e), (f), (g))

static void
expose(WindowManager* wm, Window w)
{
    XXClearArea(wm, wm->display, w, 0, 0, 0, 0, True);
}

static void
focus(WindowManager* wm, Frame* frame)
{
    move_frame_to_z_order_head(wm, frame);
    XXSetInputFocus(wm, wm->display, frame->child, RevertToNone, CurrentTime);
    expose(wm, wm->taskbar.window);
}

static void
read_protocol(WindowManager* wm, Frame* frame, Atom atom)
{
    if (atom == wm->atoms.wm_delete_window) {
        frame->wm_delete_window = True;
    }
}

static void
read_protocols(WindowManager* wm, Frame* frame)
{
    Atom* protos;
    int n;
    if (XXGetWMProtocols(wm, wm->display, frame->child, &protos, &n) == 0) {
        return;
    }
    int i;
    for (i = 0; i < n; i++) {
        read_protocol(wm, frame, protos[i]);
    }
    XFree(protos);
}

static Status
__XGetWMNormalHints__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, XSizeHints* hints, long* supplied_return)
{
    LOG_X(filename, lineno, wm, "XGetWMNormalHints(display, w=0x%08x, hints=%p, supplied_return=%p)", w, hints, supplied_return);
    return XGetWMNormalHints(display, w, hints, supplied_return);
}

#define XXGetWMNormalHints(wm, a, b, c, d) \
    __XGetWMNormalHints__(__FILE__, __LINE__, (wm), (a), (b), (c), (d))

static void
get_normal_hints(WindowManager* wm, Frame* frame)
{
    XSizeHints hints;
    long _;
    XXGetWMNormalHints(wm, wm->display, frame->child, &hints, &_);
    if ((hints.flags & PResizeInc) == 0) {
        return;
    }
    frame->width_inc = hints.width_inc;
    frame->height_inc = hints.height_inc;
    LOG(wm, "PResizeInc: window=0x%08x, width_inc=%d, height_inc=%d", frame->child, hints.width_inc, hints.height_inc);
}

static void
reparent_window(WindowManager* wm, Window w)
{
    LOG(wm, "reparent_window: w=0x%08x", w);
    Display* display = wm->display;
    XWindowAttributes wa;
    if (XXGetWindowAttributes(wm, display, w, &wa) == 0) {
        return;
    }
    Frame* frame = create_frame(wm, wa.x, wa.y, wa.width, wa.height);
    frame->child = w;
    get_window_name(wm, frame->title, array_sizeof(frame->title), w);
    LOG(wm, "Window Name: window=0x%08x, name=%s", w, frame->title);
    int frame_size = wm->frame_size;
    int x = frame_size;
    int y = 2 * frame_size + wm->title_height;
    XXSetWindowBorderWidth(wm, display, w, wm->client_border_size);
    Window parent = frame->window;
    LOG(wm, "Reparenting: frame=0x%08x, child=0x%08x", parent, w);
    XXReparentWindow(wm, display, w, parent, x, y);

    get_normal_hints(wm, frame);
    read_protocols(wm, frame);

    XXGrabButton(wm, display, Button1, AnyModifier, w, True, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);

    XXMapWindow(wm, display, frame->window);
    XXMapWindow(wm, display, w);
    focus(wm, frame);
    XXAddToSaveSet(wm, display, w);
}

static Bool
is_mapped(WindowManager* wm, Window w)
{
    XWindowAttributes wa;
    XXGetWindowAttributes(wm, wm->display, w, &wa);
    return wa.map_state != IsUnmapped;
}

static void
reparent_mapped_child(WindowManager* wm, Window w)
{
    if (!is_mapped(wm, w)) {
        return;
    }
    reparent_window(wm, w);
}

static void
reparent_toplevels(WindowManager* wm)
{
    Display* display = wm->display;
    Window root = DefaultRootWindow(display);
    Window _;
    Window* children;
    unsigned int nchildren;
    if (XXQueryTree(wm, display, root, &_, &_, &children, &nchildren) == 0) {
        print_error("XQueryTree failed.");
        return;
    }
    int i;
    for (i = 0; i < nchildren; i++) {
        reparent_mapped_child(wm, children[i]);
    }
    XXFree(wm, children);
}

static unsigned long
alloc_color(WindowManager* wm, const char* name)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    Colormap colormap = DefaultColormap(display, screen);
    XColor c;
    XColor _;
    if (XXAllocNamedColor(wm, display, colormap, name, &c, &_) == 0) {
        return BlackPixel(display, screen);
    }
    return c.pixel;
}

static int
__XFreeGC__(const char* filename, int lineno, WindowManager* wm, Display* display, GC gc)
{
    LOG_X0(filename, lineno, wm, "XFreeGC(display, gc)");
    return XFreeGC(display, gc);
}

#define XXFreeGC(wm, display, gc) \
    __XFreeGC__(__FILE__, __LINE__, (wm), (display), (gc))

static void
free_frame(WindowManager* wm, Frame* frame)
{
    remove_from_array(&wm->all_frames, frame);
    remove_from_array(&wm->frames_z_order, frame);

    XXftDrawDestroy(wm, frame->draw);

    Display* display = wm->display;
    XXFreeGC(wm, display, frame->line_gc);
    XXFreeGC(wm, display, frame->focused_gc);
    XXFreeGC(wm, display, frame->unfocused_gc);

    memset(frame, 0xfd, sizeof(*frame));
    free(frame);
}

static int
__XDestroyWindow__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w)
{
    LOG_X(filename, lineno, wm, "XDestroyWindow(display, w=0x%08x)", w);
    return XDestroyWindow(display, w);
}

#define XXDestroyWindow(wm, a, b) \
    __XDestroyWindow__(__FILE__, __LINE__, (wm), (a), (b))

static void
destroy_frame(WindowManager* wm, Frame* frame)
{
    Window w = frame->window;
    free_frame(wm, frame);
    XXDestroyWindow(wm, wm->display, w);
}

static void
focus_top_frame(WindowManager* wm)
{
    if (wm->frames_z_order.size == 0) {
        expose(wm, wm->taskbar.window);
        return;
    }
    focus(wm, wm->frames_z_order.items[0]);
}

static void
process_destroy_notify(WindowManager* wm, XDestroyWindowEvent* e)
{
    Window w = e->window;
    LOG(wm, "process_destroy_notify: event=0x%08x, window=0x%08x", e->event, w);
    Frame* frame = search_frame_of_child(wm, w);
    if (frame == NULL) {
        return;
    }
    destroy_frame(wm, frame);
    focus_top_frame(wm);
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

    XWindowAttributes wa;
    XXGetWindowAttributes(wm, wm->display, w, &wa);
    wm->grasped_width = wa.width;
    wm->grasped_height = wa.height;
}

static void
map_popup_menu(WindowManager* wm, int x, int y)
{
    Display* display = wm->display;
    Window w = wm->popup_menu.window;
    unsigned int menu_width;
    unsigned int menu_height;
    get_geometry(wm, w, &menu_width, &menu_height);
    unsigned int root_width;
    unsigned int root_height;
    get_geometry(wm, DefaultRootWindow(display), &root_width, &root_height);
    int menu_x = x;
    int menu_y = y + 1;
    if (root_width < menu_x + menu_width) {
        menu_x = x - menu_width;
    }
    if (root_height < menu_y + menu_height) {
        menu_y = y - menu_height - 1;
    }

    wm->popup_menu.selected_item = -1;
    XXMoveWindow(wm, display, w, menu_x, menu_y);
    XXMapRaised(wm, display, w);
}

static GraspedPosition
detect_frame_position(WindowManager* wm, Window w, int x, int y)
{
    unsigned int width;
    unsigned int height;
    get_geometry(wm, w, &width, &height);
    int frame_size = wm->frame_size;
    int corner_size = wm->resizable_corner_size;
    int virt_corner_size = corner_size - frame_size;

    if (is_region_inside(0, frame_size, frame_size, virt_corner_size, x, y)) {
        return GP_NORTH_WEST;
    }
    if (is_region_inside(0, 0, corner_size, frame_size, x, y)) {
        return GP_NORTH_WEST;
    }

    int middle_width = width - 2 * corner_size;
    if (is_region_inside(corner_size, 0, middle_width, frame_size, x, y)) {
        return GP_NORTH;
    }

    int east_corner_x = width - corner_size;
    if (is_region_inside(east_corner_x, 0, corner_size, frame_size, x, y)) {
        return GP_NORTH_EAST;
    }
    int virt_east_x = width - frame_size;
    if (is_region_inside(virt_east_x, frame_size, frame_size, virt_corner_size, x, y)) {
        return GP_NORTH_EAST;
    }

    int middle_height = height - 2 * corner_size;
    if (is_region_inside(virt_east_x, corner_size, frame_size, middle_height, x, y)) {
        return GP_EAST;
    }

    if (is_region_inside(virt_east_x, height - corner_size, frame_size, virt_corner_size, x, y)) {
        return GP_SOUTH_EAST;
    }
    int bottom_y = height - frame_size;
    if (is_region_inside(east_corner_x, bottom_y, corner_size, frame_size, x, y)) {
        return GP_SOUTH_EAST;
    }

    if (is_region_inside(corner_size, height - frame_size, middle_width, frame_size, x, y)) {
        return GP_SOUTH;
    }

    if (is_region_inside(0, bottom_y, corner_size, frame_size, x, y)) {
        return GP_SOUTH_WEST;
    }
    if (is_region_inside(0, height - corner_size, frame_size, corner_size - frame_size, x, y)) {
        return GP_SOUTH_WEST;
    }

    if (is_region_inside(0, corner_size, frame_size, middle_height, x, y)) {
        return GP_WEST;
    }

    if (is_region_inside(0, 0, width, height, x, y)) {
        return GP_TITLE_BAR;
    }

    return GP_NONE;
}

static void
close_frame(WindowManager* wm, Frame* frame)
{
    Display* display = wm->display;
    Window child = frame->child;
    if (!frame->wm_delete_window) {
        XXKillClient(wm, display, child);
        return;
    }
    XEvent e;
    bzero(&e, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.window = child;
    e.xclient.message_type = wm->atoms.wm_protocols;
    e.xclient.format = 32;
    e.xclient.data.l[0] = wm->atoms.wm_delete_window;
    e.xclient.data.l[1] = CurrentTime;
    XXSendEvent(wm, display, child, False, 0, &e);
}

static void
focus_window_of_taskbar(WindowManager* wm, int x, int y)
{
    if (wm->taskbar.clock_x < x) {
        return;
    }
    Display* display = wm->display;
    unsigned int _;
    unsigned int taskbar_height;
    get_geometry(wm, wm->taskbar.window, &_, &taskbar_height);
    unsigned int root_height;
    get_geometry(wm, DefaultRootWindow(display), &_, &root_height);
    if (x < taskbar_height) {
        map_popup_menu(wm, 0, root_height - taskbar_height);
        return;
    }

    int nframes = wm->all_frames.size;
    if (nframes == 0) {
        return;
    }
    Frame* frame = wm->all_frames.items[x / (wm->taskbar.clock_x / nframes)];
    Window w = frame->window;
    XXMapWindow(wm, display, w);
    XXRaiseWindow(wm, display, w);
    focus(wm, frame);
}

static void
unmap_frame(WindowManager* wm, Frame* frame)
{
    remove_from_array(&wm->frames_z_order, frame);
    XXUnmapWindow(wm, wm->display, frame->window);

    focus_top_frame(wm);
}

static void
process_button_press(WindowManager* wm, XButtonEvent* e)
{
    LOG(wm, "process_button_press: window=0x%08x, root=0x%08x, subwindow=0x%08x", e->window, e->root, e->subwindow);
    if (e->button != Button1) {
        return;
    }
    Window w = e->window;
    Display* display = wm->display;
    if (w == DefaultRootWindow(display)) {
        map_popup_menu(wm, e->x, e->y);
        return;
    }
    else if (w == wm->taskbar.window) {
        focus_window_of_taskbar(wm, e->x, e->y);
        return;
    }
    Frame* frame = search_frame_of_child(wm, w);
    if (frame != NULL) {
        XXRaiseWindow(wm, display, frame->window);
        focus(wm, frame);
        XXAllowEvents(wm, display, ReplayPointer, CurrentTime);
        return;
    }
    frame = search_frame(wm, w);
    if (frame == NULL) {
        return;
    }
    if (frame->status == FOCUS_CLOSE) {
        close_frame(wm, frame);
        return;
    }
    if (frame->status == FOCUS_MINIMIZE) {
        unmap_frame(wm, frame);
        return;
    }
    XXRaiseWindow(wm, display, w);
    focus(wm, frame);
    int x = e->x;
    int y = e->y;
    grasp_frame(wm, detect_frame_position(wm, w, x, y), w, x, y);
}

static void
unmap_popup_menu(WindowManager* wm)
{
    XXUnmapWindow(wm, wm->display, wm->popup_menu.window);
}

static void
resize_child(WindowManager* wm, Window w, int frame_width, int frame_height)
{
    int width = frame_width - compute_frame_width(wm);
    int height = frame_height - compute_frame_height(wm);
    XXResizeWindow(wm, wm->display, w, width, height);
}

static int
compute_font_height(XftFont* font)
{
    return font->ascent + font->descent;
}

static int
detect_selected_popup_item(WindowManager* wm, int x, int y)
{
    XWindowAttributes wa;
    XXGetWindowAttributes(wm, wm->display, wm->popup_menu.window, &wa);
    if (!is_region_inside(wa.x, wa.y, wa.width, wa.height, x, y)) {
        return -1;
    }
    int index = (y - wa.y) / compute_font_height(wm->title_font);
    return wm->config->menu.ptr->items_num <= index ? -1 : index;
}

static const char* caption_of_exit = "exit";

static const char*
get_menu_item_caption(MenuItem* item)
{
    switch (item->type) {
    case MENU_ITEM_TYPE_EXIT:
        return caption_of_exit;
    case MENU_ITEM_TYPE_EXEC:
        return item->u.exec.caption.ptr;
    default:
        assert(false);
        break;
    }
    return NULL;
}

static void
draw_popup_menu(WindowManager* wm)
{
    Window w = wm->popup_menu.window;
    unsigned int window_width;
    unsigned int _;
    get_geometry(wm, w, &window_width, &_);

    XftFont* font = wm->title_font;
    int item_height = font->ascent + font->descent;

    int selected_item = wm->popup_menu.selected_item;
    if (0 <= selected_item) {
        Display* display = wm->display;
        GC gc = wm->popup_menu.selected_gc;
        int y = item_height * selected_item;
        XXFillRectangle(wm, display, w, gc, 0, y, window_width, item_height);
    }

    XftDraw* draw = wm->popup_menu.draw;
    int y = - font->descent;
    Menu* menu = wm->config->menu.ptr;
    int items_num = menu->items_num;
    int i;
    for (i = 0; i < items_num; i++) {
        y += item_height;
        const char* caption = get_menu_item_caption(&menu->items.ptr[i]);
        draw_title_font_string(wm, draw, wm->popup_menu.margin, y, caption);
    }
}

static void
highlight_selected_popup_item(WindowManager* wm, int x, int y)
{
    int new_item = detect_selected_popup_item(wm, x, y);
    if (new_item == wm->popup_menu.selected_item) {
        return;
    }
    wm->popup_menu.selected_item = new_item;
    expose(wm, wm->popup_menu.window);
}

static void
change_cursor(WindowManager* wm, Window w, int x, int y)
{
    Cursor cursor;
    switch (detect_frame_position(wm, w, x, y)) {
    case GP_NONE:
    case GP_TITLE_BAR:
        cursor = wm->normal_cursor;
        break;
    case GP_NORTH:
        cursor = wm->top_cursor;
        break;
    case GP_NORTH_EAST:
        cursor = wm->top_right_cursor;
        break;
    case GP_EAST:
        cursor = wm->right_cursor;
        break;
    case GP_SOUTH_EAST:
        cursor = wm->bottom_right_cursor;
        break;
    case GP_SOUTH:
        cursor = wm->bottom_cursor;
        break;
    case GP_SOUTH_WEST:
        cursor = wm->bottom_left_cursor;
        break;
    case GP_WEST:
        cursor = wm->left_cursor;
        break;
    case GP_NORTH_WEST:
        cursor = wm->top_left_cursor;
        break;
    default:
        assert(False);
        return; /* A statement to surpress GCC warning. */
    }
    XXDefineCursor(wm, wm->display, w, cursor);
}

static int
floor_int(int n, int inc)
{
    return (n / inc) * inc;
}

static void
update_frame_status(WindowManager* wm, Frame* frame, int status)
{
    if (frame->status == status) {
        return;
    }
    frame->status = status;
    expose(wm, frame->window);
}

static int
detect_frame_status(WindowManager* wm, Frame* frame, int x, int y)
{
    int frame_size = wm->frame_size;
    int size = wm->title_height;
    if ((y < frame_size) || (frame_size + size < y)) {
        return FOCUS_NONE;
    }
    unsigned int width;
    unsigned int height;
    get_geometry(wm, frame->window, &width, &height);
    if (x < width - (3 * size + frame_size)) {
        return FOCUS_NONE;
    }
    if (x < width - (2 * size + frame_size)) {
        return FOCUS_MINIMIZE;
    }
    if (x < width - (size + frame_size)) {
        return FOCUS_MAXIMIZE;
    }
    return FOCUS_CLOSE;
}

static void
change_frame_status(WindowManager* wm, Frame* frame, int x, int y)
{
    update_frame_status(wm, frame, detect_frame_status(wm, frame, x, y));
}

static void
process_motion_notify(WindowManager* wm, XMotionEvent* e)
{
    LOG(wm, "process_motion_notify: window=0x%08x, root=0x%08x, subwindow=0x%08x", e->window, e->root, e->subwindow);
    Display* display = wm->display;
    Window w = e->window;
    Window root = DefaultRootWindow(display);
    if ((w == root) || (w == wm->taskbar.window)) {
        highlight_selected_popup_item(wm, e->x_root, e->y_root);
        return;
    }
    Frame* frame = search_frame(wm, w);
    if (frame == NULL) {
        return;
    }
    int x = e->x;
    int y = e->y;
    if ((e->state & Button1Mask) == 0) {
        change_cursor(wm, w, x, y);
        change_frame_status(wm, frame, x, y);
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
        XXMoveWindow(wm, display, w, new_x, new_y);
        return;
    }
    XWindowAttributes frame_attrs;
    XXGetWindowAttributes(wm, display, w, &frame_attrs);
    Window child = frame->child;
    XWindowAttributes child_attrs;
    XXGetWindowAttributes(wm, display, child, &child_attrs);
    int new_width;
    int new_height;
    int inc_x;
    int inc_y;
    switch (wm->grasped_position) {
    case GP_NORTH:
        new_width = frame_attrs.width;
        inc_y = floor_int(frame_attrs.y - new_y, frame->height_inc);
        new_height = frame_attrs.height + inc_y;
        XXMoveResizeWindow(
            wm,
            display, w,
            frame_attrs.x, frame_attrs.y - inc_y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_NORTH_EAST:
        inc_x = floor_int(x - wm->grasped_x, frame->width_inc);
        new_width = wm->grasped_width + inc_x;
        inc_y = floor_int(frame_attrs.y - new_y, frame->height_inc);
        new_height = frame_attrs.height + inc_y;
        XXMoveResizeWindow(
            wm,
            display, w,
            frame_attrs.x, frame_attrs.y - inc_y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_EAST:
        inc_x = floor_int(x - wm->grasped_x, frame->width_inc);
        new_width = wm->grasped_width + inc_x;
        new_height = frame_attrs.height;
        XXResizeWindow(wm, display, w, new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_SOUTH_EAST:
        inc_x = floor_int(x - wm->grasped_x, frame->width_inc);
        new_width = wm->grasped_width + inc_x;
        inc_y = floor_int(y - wm->grasped_y, frame->height_inc);
        new_height = wm->grasped_height + inc_y;
        XXResizeWindow(wm, display, w, new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_SOUTH:
        new_width = frame_attrs.width;
        inc_y = floor_int(y - wm->grasped_y, frame->height_inc);
        new_height = wm->grasped_height + inc_y;
        XXResizeWindow(wm, display, w, new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_SOUTH_WEST:
        inc_x = floor_int(frame_attrs.x - new_x, frame->width_inc);
        new_width = frame_attrs.width + inc_x;
        inc_y = floor_int(y - wm->grasped_y, frame->height_inc);
        new_height = wm->grasped_height + inc_y;
        XXMoveResizeWindow(
            wm,
            display, w,
            frame_attrs.x - inc_x, frame_attrs.y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_WEST:
        inc_x = floor_int(frame_attrs.x - new_x, frame->width_inc);
        new_width = frame_attrs.width + inc_x;
        new_height = frame_attrs.height;
        XXMoveResizeWindow(
            wm,
            display, w,
            frame_attrs.x - inc_x, frame_attrs.y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_NORTH_WEST:
        inc_x = floor_int(frame_attrs.x - new_x, frame->width_inc);
        new_width = frame_attrs.width + inc_x;
        inc_y = floor_int(frame_attrs.y - new_y, frame->height_inc);
        new_height = frame_attrs.height + inc_y;
        XXMoveResizeWindow(
            wm,
            display, w,
            frame_attrs.x - inc_x, frame_attrs.y - inc_y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
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
    while (XXCheckTypedWindowEvent(wm, display, w, event_type, e));
}

static int
compute_text_width(WindowManager* wm, XftFont* font, const char* text, int len)
{
    XGlyphInfo glyph;
    XXftTextExtentsUtf8(wm, wm->display, font, (XftChar8*)text, len, &glyph);
    return glyph.width;
}

static void
draw_clock(WindowManager* wm)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        print_error("gettimeofday failed: %s", strerror(errno));
        return;
    }
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char text[64];
    strftime(text, array_sizeof(text), "%Y-%m-%dT%H:%M", &tm);

    Display* display = wm->display;
    unsigned int width;
    unsigned int _;
    get_geometry(wm, DefaultRootWindow(display), &width, &_);

    XftFont* font = wm->taskbar.clock_font;
    int len = strlen(text);
    int x = width - compute_text_width(wm, font, text, len) - wm->padding_size;

    XftDraw* draw = wm->taskbar.draw;
    XftColor* color = &wm->title_color;
    int y = font->ascent + wm->padding_size;
    XXftDrawStringUtf8(wm, draw, color, font, x, y, (XftChar8*)text, len);

    wm->taskbar.clock_x = x;
}

static void
fill_top_frame_rect(WindowManager* wm, Frame* frame, int x, int width, int height)
{
    if (wm->frames_z_order.size == 0) {
        return;
    }
    if (frame != wm->frames_z_order.items[0]) {
        return;
    }
    Window w = wm->taskbar.window;
    GC gc = wm->taskbar.focused_gc;
    XXFillRectangle(wm, wm->display, w, gc, x, 0, width, height);
}

static void
draw_vertical_line(WindowManager* wm, Drawable d, GC gc, int x, int y0, int y1)
{
    XXDrawLine(wm, wm->display, d, gc, x, y0, x, y1);
}

static void
draw_list_rect(WindowManager* wm, Frame* frame, int x, int width, int height)
{
    fill_top_frame_rect(wm, frame, x, width, height);

    Window w = wm->taskbar.window;
    GC gc = wm->taskbar.line_gc;
    int y0 = 0;
    int y1 = height;
    draw_vertical_line(wm, w, gc, x, y0, y1);
    draw_vertical_line(wm, w, gc, x + width, y0, y1);
}

static void
draw_list_entry(WindowManager* wm, Frame* frame, int x, int width, int height)
{
    draw_list_rect(wm, frame, x, width, height);

    XftDraw* d = wm->taskbar.draw;
    XftColor* color = &wm->title_color;
    XftFont* font = wm->title_font;
    int padding_size = wm->padding_size;
    int pos = x + padding_size;
    int y = padding_size + wm->title_font->ascent;
    XftChar8* pstr = (XftChar8*)frame->title;
    XXftDrawStringUtf8(wm, d, color, font, pos, y, pstr, strlen(frame->title));
}

static void
draw_window_list(WindowManager* wm, int list_right_x)
{
    Window w = wm->taskbar.window;
    unsigned int _;
    unsigned int taskbar_height;
    get_geometry(wm, w, &_, &taskbar_height);

    int nframes = wm->all_frames.size;
    if (nframes == 0) {
        return;
    }
    int item_width = (list_right_x - taskbar_height) / nframes;
    int i;
    for (i = 0; i < nframes; i++) {
        Frame* frame = wm->all_frames.items[i];
        int x = taskbar_height + item_width * i;
        draw_list_entry(wm, frame, x, item_width, taskbar_height);
    }
}

static void
draw_taskbar(WindowManager* wm)
{
    draw_clock(wm);
    draw_window_list(wm, wm->taskbar.clock_x - wm->padding_size);
}

static void
process_expose(WindowManager* wm, XExposeEvent* e)
{
    LOG(wm, "process_expose: window=0x%08x", e->window);
    Window w = e->window;
    if (w == wm->popup_menu.window) {
        draw_popup_menu(wm);
        return;
    }
    if (w == wm->taskbar.window) {
        draw_taskbar(wm);
        return;
    }
    if (e->x == wm->frame_size) {
        /**
         * If above condition is true, this Expose event may be a result of
         * killing a child window. At this time, WM does not draw a frame,
         * because parent frame will be destroyed soon.
         * FIXME: More strict checking?
         */
        return;
    }
    draw_frame(wm, w);
}

static pid_t
do_fork()
{
    pid_t pid = fork();
    if (pid == -1) {
        print_error("fork failed: %s", strerror(errno));
        exit(1);
    }
    return pid;
}

static void
fork_child(char* cmd)
{
    pid_t pid = do_fork();
    char* argv[] = { "/bin/sh", "-c", cmd, NULL };
    if (pid == 0) {
        execv(argv[0], argv);
        exit(1);
    }
    exit(0);
}

static void
execute(WindowManager* wm, char* cmd)
{
    pid_t pid = do_fork();
    if (pid == 0) {
        fork_child(cmd);
        exit(1);
    }
    waitpid(pid, NULL, 0);
}

static void
process_button_release(WindowManager* wm, XButtonEvent* e)
{
    LOG(wm, "process_button_release: window=0x%08x, root=0x%08x, subwindow=0x%08x", e->window, e->root, e->subwindow);
    Frame* frame = search_frame(wm, e->window);
    if (frame != NULL) {
        release_frame(wm);
        return;
    }
    unmap_popup_menu(wm);
    int index = detect_selected_popup_item(wm, e->x_root, e->y_root);
    if (index < 0) {
        return;
    }
    MenuItem* item = &wm->config->menu.ptr->items.ptr[index];
    switch (item->type) {
    case MENU_ITEM_TYPE_EXIT:
        wm->running = False;
        break;
    default:
        assert(item->type == MENU_ITEM_TYPE_EXEC);
        execute(wm, item->u.exec.command.ptr);
        break;
    }
}

static void
change_frame_background(WindowManager* wm, Window w, int pixel)
{
    XXSetWindowBackground(wm, wm->display, w, pixel);
    expose(wm, w);
}

static Bool
is_alive_frame(WindowManager* wm, Window w)
{
    return search_frame(wm, w) != NULL;
}

static void
process_focus_out(WindowManager* wm, XFocusChangeEvent* e)
{
    LOG(wm, "process_focus_out: window=0x%08x", e->window);
    if (e->mode != NotifyNormal) {
        return;
    }
    int detail = e->detail;
    if ((detail != NotifyNonlinear) && (detail != NotifyNonlinearVirtual)) {
        return;
    }
    Window w = e->window;
    if (!is_alive_frame(wm, w)) {
        /* XXX: X seems to throw FocusOut event for XDestroyWindow'ed window? */
        return;
    }
    change_frame_background(wm, w, wm->unfocused_foreground_color);
}

static void
process_focus_in(WindowManager* wm, XFocusChangeEvent* e)
{
    LOG(wm, "process_focus_in: window=0x%08x", e->window);
    if (e->mode != NotifyNormal) {
        return;
    }
    int detail = e->detail;
    if ((detail != NotifyNonlinear) && (detail != NotifyNonlinearVirtual)) {
        return;
    }
    Window w = e->window;
    if (!is_alive_frame(wm, w)) {
        return;
    }
    XXRaiseWindow(wm, wm->display, w);
    change_frame_background(wm, w, wm->focused_foreground_color);
}

static void
map_frame_of_child(WindowManager* wm, Window w)
{
    Frame* frame = search_frame_of_child(wm, w);
    if (frame != NULL) {
        Display* display = wm->display;
        XXMapWindow(wm, display, frame->window);
        XXMapWindow(wm, display, frame->child);
        XXRaiseWindow(wm, display, frame->window);
        focus(wm, frame);
        return;
    }
    reparent_window(wm, w);
    map_frame_of_child(wm, w);
}

static void
process_map_request(WindowManager* wm, XMapRequestEvent* e)
{
    Window w = e->window;
#define FMT "process_map_request: parent=0x%08x, window=0x%08x"
    LOG(wm, FMT, e->parent, w);
#undef FMT
    map_frame_of_child(wm, w);
}

static void
process_unmap_notify(WindowManager* wm, XUnmapEvent* e)
{
    LOG(wm, "process_unmap_notify: event=0x%08x, window=0x%08x", e->event, e->window);
    Frame* frame = search_frame_of_child(wm, e->window);
    if (frame == NULL) {
        return;
    }
    unmap_frame(wm, frame);
}

static char event_name[LASTEvent][32];

static void
initialize_event_name()
{
    bzero(event_name, sizeof(event_name));
#define REGISTER_NAME(type) \
    strncpy(event_name[(type)], #type, sizeof(event_name[(type)]))
    REGISTER_NAME(KeyPress);
    REGISTER_NAME(KeyRelease);
    REGISTER_NAME(ButtonPress);
    REGISTER_NAME(ButtonRelease);
    REGISTER_NAME(MotionNotify);
    REGISTER_NAME(EnterNotify);
    REGISTER_NAME(LeaveNotify);
    REGISTER_NAME(FocusIn);
    REGISTER_NAME(FocusOut);
    REGISTER_NAME(KeymapNotify);
    REGISTER_NAME(Expose);
    REGISTER_NAME(GraphicsExpose);
    REGISTER_NAME(NoExpose);
    REGISTER_NAME(VisibilityNotify);
    REGISTER_NAME(CreateNotify);
    REGISTER_NAME(DestroyNotify);
    REGISTER_NAME(UnmapNotify);
    REGISTER_NAME(MapNotify);
    REGISTER_NAME(MapRequest);
    REGISTER_NAME(ReparentNotify);
    REGISTER_NAME(ConfigureNotify);
    REGISTER_NAME(ConfigureRequest);
    REGISTER_NAME(GravityNotify);
    REGISTER_NAME(ResizeRequest);
    REGISTER_NAME(CirculateNotify);
    REGISTER_NAME(CirculateRequest);
    REGISTER_NAME(PropertyNotify);
    REGISTER_NAME(SelectionClear);
    REGISTER_NAME(SelectionRequest);
    REGISTER_NAME(SelectionNotify);
    REGISTER_NAME(ColormapNotify);
    REGISTER_NAME(ClientMessage);
    REGISTER_NAME(MappingNotify);
    REGISTER_NAME(GenericEvent);
#undef REGISTER_NAME
}

static void
configure_frame(WindowManager* wm, Window parent, Window w, XConfigureRequestEvent* e)
{
    Display* display = wm->display;
    unsigned long value_mask = e->value_mask;
    int frame_size = wm->frame_size;
    if (value_mask & CWX) {
        XWindowChanges changes;
        changes.x = e->x - frame_size;
        XXConfigureWindow(wm, display, parent, CWX, &changes);
    }
    if (value_mask & CWY) {
        XWindowChanges changes;
        changes.y = e->y - (frame_size + wm->title_height);
        XXConfigureWindow(wm, display, parent, CWY, &changes);
    }
    if (value_mask & CWWidth) {
        XWindowChanges changes;
        int width = e->width;
        changes.width = width + compute_frame_width(wm);
        unsigned int value_mask = CWWidth;
        XXConfigureWindow(wm, display, parent, value_mask, &changes);
        changes.width = width;
        XXConfigureWindow(wm, display, w, value_mask, &changes);
    }
    if (value_mask & CWHeight) {
        XWindowChanges changes;
        int height = e->height;
        changes.height = height + compute_frame_height(wm);
        unsigned int value_mask = CWHeight;
        XXConfigureWindow(wm, display, parent, value_mask, &changes);
        changes.height = height;
        XXConfigureWindow(wm, display, w, value_mask, &changes);
    }
    /* Ignore CWBorderWidth, CWSibling and CWStackMode */
}

static void
process_configure_request(WindowManager* wm, XConfigureRequestEvent* e)
{
    LOG(wm, "process_configure_request: parent=0x%08x, window=0x%08x, above=0x%08x", e->parent, e->window, e->above);
    Window w = e->window;
    Frame* frame = search_frame_of_child(wm, w);
    if (frame != NULL) {
        configure_frame(wm, e->parent, w, e);
        return;
    }

    Display* display = wm->display;
    unsigned long value_mask = e->value_mask;
    if (value_mask & CWX) {
        XWindowChanges changes;
        changes.x = e->x;
        XXConfigureWindow(wm, display, w, CWX, &changes);
    }
    if (value_mask & CWY) {
        XWindowChanges changes;
        changes.y = e->y;
        XXConfigureWindow(wm, display, w, CWY, &changes);
    }
    if (value_mask & CWWidth) {
        XWindowChanges changes;
        changes.width = e->width;
        XXConfigureWindow(wm, display, w, CWWidth, &changes);
    }
    if (value_mask & CWHeight) {
        XWindowChanges changes;
        changes.height = e->height;
        XXConfigureWindow(wm, display, w, CWHeight, &changes);
    }
    if (value_mask & CWBorderWidth) {
        XWindowChanges changes;
        changes.border_width = e->border_width;
        XXConfigureWindow(wm, display, w, CWBorderWidth, &changes);
    }
    if (value_mask & CWSibling) {
        LOG0(wm, "CWSibling");
    }
    if (value_mask & CWStackMode) {
        XWindowChanges changes;
        changes.stack_mode = e->detail;
        XXConfigureWindow(wm, display, w, CWStackMode, &changes);
    }
}

static int
__XUndefineCursor__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w)
{
    LOG_X(filename, lineno, wm, "XUndefineCursor(display, w=0x%08x)", w);
    return XUndefineCursor(display, w);
}

#define XXUndefineCursor(wm, a, b) \
    __XUndefineCursor__(__FILE__, __LINE__, (wm), (a), (b))

static void
process_leave_notify(WindowManager* wm, XCrossingEvent* e)
{
    LOG(wm, "process_leave_notify: window=0x%08x, root=0x%08x, subwindow=0x%08x", e->window, e->root, e->subwindow);
    Window w = e->window;
    if (search_frame(wm, w) == NULL) {
        return;
    }
    XXUndefineCursor(wm, wm->display, w);
}

static void
expose_frame(WindowManager* wm, Frame* frame)
{
    expose(wm, frame->window);
}

static void
expose_taskbar(WindowManager* wm)
{
    expose(wm, wm->taskbar.window);
}

static void
process_property_notify(WindowManager* wm, XPropertyEvent* e)
{
    Window w = e->window;
    LOG(wm, "process_property_notify: window=0x%08x", w);
    Frame* frame = search_frame_of_child(wm, w);
    if (frame == NULL) {
        return;
    }
    if ((e->atom != XA_WM_NAME) || (e->state != PropertyNewValue)) {
        return;
    }
    get_window_name(wm, frame->title, array_sizeof(frame->title), w);
    expose_frame(wm, frame);
    expose_taskbar(wm);
}

typedef void (*EventHandler)(WindowManager*, XEvent*);

static EventHandler event_handlers[LASTEvent];

static void
handle_button_press(WindowManager* wm, XEvent* event)
{
    XButtonEvent* e = &event->xbutton;
    process_button_press(wm, e);
}

static void
handle_button_release(WindowManager* wm, XEvent* event)
{
    XButtonEvent* e = &event->xbutton;
    process_button_release(wm, e);
}

static void
handle_configure_request(WindowManager* wm, XEvent* event)
{
    XConfigureRequestEvent* e = &event->xconfigurerequest;
    process_configure_request(wm, e);
}

static void
handle_destroy_notify(WindowManager* wm, XEvent* event)
{
    XDestroyWindowEvent* e = &event->xdestroywindow;
    process_destroy_notify(wm, e);
}

static void
handle_expose(WindowManager* wm, XEvent* event)
{
    XExposeEvent* e = &event->xexpose;
    process_expose(wm, e);
}

static void
handle_leave_notify(WindowManager* wm, XEvent* event)
{
    XCrossingEvent* e = &event->xcrossing;
    process_leave_notify(wm, e);
}

static void
handle_focus_in(WindowManager* wm, XEvent* event)
{
    XFocusChangeEvent* e = &event->xfocus;
    process_focus_in(wm, e);
}

static void
handle_focus_out(WindowManager* wm, XEvent* event)
{
    XFocusChangeEvent* e = &event->xfocus;
    process_focus_out(wm, e);
}

static void
handle_motion_notify(WindowManager* wm, XEvent* event)
{
    /*
     * Hmm... I dislike to reuse an object (event) for diffent usages.
     */
    XEvent last_event;
    memcpy(&last_event, event, sizeof(last_event));
    get_last_event(wm, event->xmotion.window, MotionNotify, &last_event);

    XMotionEvent* e = &last_event.xmotion;
    process_motion_notify(wm, e);
}

static void
handle_map_request(WindowManager* wm, XEvent* event)
{
    XMapRequestEvent* e = &event->xmaprequest;
    process_map_request(wm, e);
}

static void
handle_property_notify(WindowManager* wm, XEvent* event)
{
    XPropertyEvent* e = &event->xproperty;
    process_property_notify(wm, e);
}

static void
handle_unmap_notify(WindowManager* wm, XEvent* event)
{
    XUnmapEvent* e = &event->xunmap;
    process_unmap_notify(wm, e);
}

static void
nop(WindowManager* _, XEvent* __)
{
}

static void
initialize_event_handlers()
{
    int i;
    for (i = 0; i < array_sizeof(event_handlers); i++) {
        event_handlers[i] = nop;
    }

#define REGISTER_HANDLER(type, handler) \
    event_handlers[(type)] = handler
    REGISTER_HANDLER(ButtonPress, handle_button_press);
    REGISTER_HANDLER(ButtonRelease, handle_button_release);
    REGISTER_HANDLER(ConfigureRequest, handle_configure_request);
    REGISTER_HANDLER(DestroyNotify, handle_destroy_notify);
    REGISTER_HANDLER(Expose, handle_expose);
    REGISTER_HANDLER(LeaveNotify, handle_leave_notify);
    REGISTER_HANDLER(FocusIn, handle_focus_in);
    REGISTER_HANDLER(FocusOut, handle_focus_out);
    REGISTER_HANDLER(MotionNotify, handle_motion_notify);
    REGISTER_HANDLER(MapRequest, handle_map_request);
    REGISTER_HANDLER(PropertyNotify, handle_property_notify);
    REGISTER_HANDLER(UnmapNotify, handle_unmap_notify);
#undef REGISTER_HANDLER
}

static void
process_event(WindowManager* wm, XEvent* e)
{
    int type = e->type;
    LOG(wm, "%s: window=0x%08x", event_name[type], e->xany.window);
    event_handlers[type](wm, e);
}

static void
change_event_mask(WindowManager* wm, Window w, long event_mask)
{
    XSetWindowAttributes swa;
    swa.event_mask = event_mask;
    XXChangeWindowAttributes(wm, wm->display, w, CWEventMask, &swa);
}

static void
change_taskbar_event_mask(WindowManager* wm, Window w)
{
    long mask = 0
        | Button1MotionMask
        | ButtonPressMask
        | ButtonReleaseMask
        | ExposureMask;
    change_event_mask(wm, w, mask);
}

static void
change_popup_menu_event_mask(WindowManager* wm, Window w)
{
    change_event_mask(wm, w, ExposureMask);
}

static int
compute_popup_menu_width(WindowManager* wm)
{
    int max = 0;
    Menu* menu = wm->config->menu.ptr;
    int i;
    for (i = 0; i < menu->items_num; i++) {
        const char* caption = get_menu_item_caption(&menu->items.ptr[i]);
        int len = strlen(caption);
        int width = compute_text_width(wm, wm->title_font, caption, len);
        max = max < width ? width : max;
    }
    return max;
}

static void
setup_popup_menu(WindowManager* wm)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    Window w = XXCreateSimpleWindow(
        wm,
        display, DefaultRootWindow(display),
        0, 0,
        42, 42, /* They are dummy. They will be defined later. */
        wm->border_size,
        BlackPixel(display, screen), wm->unfocused_foreground_color);
    LOG(wm, "popup menu: 0x%08x", w);
    change_popup_menu_event_mask(wm, w);
    wm->popup_menu.window = w;

    XGCValues title_gc;
    title_gc.foreground = wm->focused_foreground_color;
    int mask = GCForeground;
    wm->popup_menu.title_gc = XXCreateGC(wm, display, w, mask, &title_gc);

    XGCValues selected_gc;
    selected_gc.foreground = wm->focused_foreground_color;
    wm->popup_menu.selected_gc = XXCreateGC(wm, display, w, mask, &selected_gc);

    wm->popup_menu.draw = create_draw(wm, w);
    assert(wm->popup_menu.draw != NULL);
    wm->popup_menu.margin = 8;

    int width = 2 * wm->popup_menu.margin + compute_popup_menu_width(wm);
    int font_height = compute_font_height(wm->title_font);
    int height = font_height * wm->config->menu.ptr->items_num;
    XXResizeWindow(wm, display, w, width, height);
}

static void
setup_title_font(WindowManager* wm)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);

#define OPEN_FONT(var, name) \
XftFont* var; \
do { \
        var = XXftFontOpenName(wm, display, screen, (name)); \
        if (var == NULL) { \
            const char* fmt = "Cannot find font (XftFontOpenName failed): %s"; \
            print_error(fmt, (name)); \
            exit(1); \
        } \
} while (0)
    OPEN_FONT(title_font, "VL PGothic-18");
    wm->title_font = title_font;
    OPEN_FONT(clock_font, "VL Gothic-18");
    wm->taskbar.clock_font = clock_font;
#undef OPEN_FONT
    wm->taskbar.clock_margin = 8;

    Visual* visual = DefaultVisual(display, screen);
    Colormap colormap = DefaultColormap(display, screen);
    XftColor* result = &wm->title_color;
    /**
     * XXX: I could not find a description about a mean of XftColorAllocName's
     * return value.
     */
    XXftColorAllocName(wm, display, visual, colormap, "black", result);
}

static void
setup_cursors(WindowManager* wm)
{
    Display* display = wm->display;
#define CREATE_CURSOR(member, cursor) \
    wm->member = XXCreateFontCursor(wm, display, cursor)
    CREATE_CURSOR(normal_cursor, XC_top_left_arrow);
    CREATE_CURSOR(bottom_left_cursor, XC_bottom_left_corner);
    CREATE_CURSOR(bottom_right_cursor, XC_bottom_right_corner);
    CREATE_CURSOR(bottom_cursor, XC_bottom_side);
    CREATE_CURSOR(left_cursor, XC_left_side);
    CREATE_CURSOR(right_cursor, XC_right_side);
    CREATE_CURSOR(top_left_cursor, XC_top_left_corner);
    CREATE_CURSOR(top_right_cursor, XC_top_right_corner);
    CREATE_CURSOR(top_cursor, XC_top_side);
#undef CREATE_CURSOR
}

static void
setup_taskbar(WindowManager* wm)
{
    Display* display = wm->display;
    Window root = DefaultRootWindow(display);
    unsigned int root_width;
    unsigned int root_height;
    get_geometry(wm, root, &root_width, &root_height);

    int font_height = compute_font_height(wm->title_font);
    int height = font_height + 2 * wm->padding_size;
    int border_size = wm->border_size;
    int screen = DefaultScreen(display);
    Window w = XXCreateSimpleWindow(
        wm,
        display, root,
        - border_size, root_height - height,
        root_width, height,
        border_size,
        BlackPixel(display, screen), wm->unfocused_foreground_color);
    LOG(wm, "taskbar: 0x%08x", w);
    change_taskbar_event_mask(wm, w);
    wm->taskbar.window = w;
    wm->taskbar.draw = create_draw(wm, w);
    wm->taskbar.clock = -1;
    wm->taskbar.clock_x = 0;

    wm->taskbar.line_gc = create_foreground_gc(wm, w, BlackPixel(display, screen));
    wm->taskbar.focused_gc = create_foreground_gc(wm, w, wm->focused_foreground_color);
}

static FILE*
open_log(const char* log_file)
{
    if (strlen(log_file) == 0) {
        return NULL;
    }
    FILE* fp = fopen(log_file, "w");
    if (fp == NULL) {
        print_error("Cannot open %s: %s", log_file, strerror(errno));
    }
    return fp;
}

static void
setup_window_manager(WindowManager* wm, Display* display, const char* log_file)
{
    wm->log_file = open_log(log_file);

    wm->display = display;
    setup_title_font(wm);

    wm->running = True;
    wm->focused_foreground_color = alloc_color(wm, "light pink");
    wm->unfocused_foreground_color = alloc_color(wm, "light grey");
    wm->border_size = wm->client_border_size = 1;
    wm->frame_size = 4;
    wm->title_height = wm->title_font->height;
    wm->resizable_corner_size = 32;
    wm->padding_size = wm->frame_size;
    initialize_array(&wm->all_frames);
    initialize_array(&wm->frames_z_order);
    release_frame(wm);
    setup_cursors(wm);
    setup_popup_menu(wm);
    setup_taskbar(wm);
    wm->atoms.wm_delete_window = intern(wm, "WM_DELETE_WINDOW");
    wm->atoms.wm_protocols = intern(wm, "WM_PROTOCOLS");
}

static void
update_clock(WindowManager* wm)
{
    time_t now;
    time(&now);
    if (wm->taskbar.clock / 60 == now / 60) {
        return;
    }
    expose(wm, wm->taskbar.window);
    wm->taskbar.clock = now;
}

static void
do_select(WindowManager* wm)
{
    Display* display = wm->display;
    int fd = XConnectionNumber(display);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    int status = select(fd + 1, &fds, NULL, NULL, &timeout);
    if (status < 0) {
        print_error("select failed: %s", strerror(errno));
        abort();
    }
    else if (status == 0) {
        update_clock(wm);
    }
}

static void
wait_event(WindowManager* wm)
{
    Display* display = wm->display;
    while (XPending(display) == 0) {
        do_select(wm);
    }
}

static void
log_error(FILE* fp, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    print_message(stderr, fmt, ap);
    print_message(fp, fmt, ap);
    va_end(ap);
}

static int
error_handler(Display* display, XErrorEvent* e)
{
    FILE* fp = fopen("fawm-error.log", "a");
    assert(fp != NULL);
    log_error(fp, "**********");
    log_error(fp, "X Error at pid %u", getpid());
    log_error(fp, "Serial Number of Request Code: %lu", e->serial);
    long code = e->error_code;
    char msg[64];
    XGetErrorText(display, code, msg, array_sizeof(msg));
    log_error(fp, "Error Code: %u (%s)", code, msg);
    log_error(fp, "Major Opcode: %u", e->request_code);
    log_error(fp, "Minor Opcode: %u", e->minor_code);
    log_error(fp, "Resource ID: 0x%08x", e->resourceid);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d", e->request_code);
    char msg2[64];
    XGetErrorDatabaseText(display, "XRequest", buf, "?", msg2, sizeof(msg2));
    log_error(fp, "XRequest: %s", msg2);

    fclose(fp);

    return 0;
}

static int
__XSelectInput__(const char* filename, int lineno, WindowManager* wm, Display* display, Window w, int event_mask)
{
    LOG_X(filename, lineno, wm, "XSelectInput(display, w=0x%08x, event_mask)", w);
    return XSelectInput(display, w, event_mask);
}

#define XXSelectInput(wm, a, b, c) \
    __XSelectInput__(__FILE__, __LINE__, (wm), (a), (b), (c))

static void
execute_startup(WindowManager* wm, int argc, char* argv[])
{
    int i;
    for (i = 0; i < argc; i++) {
        execute(wm, argv[i]);
    }
}

static void
wm_main(WindowManager* wm, Display* display, const char* log_file, int argc, char* argv[])
{
    XSetErrorHandler(error_handler);

    setup_window_manager(wm, display, log_file);
    Window root = DefaultRootWindow(display);
    XXDefineCursor(wm, display, root, wm->normal_cursor);
    reparent_toplevels(wm);
    XXMapWindow(wm, display, wm->taskbar.window);
    long mask = Button1MotionMask | ButtonPressMask | ButtonReleaseMask | SubstructureRedirectMask;
    XXSelectInput(wm, display, root, mask);
    LOG(wm, "root window=0x%08x", root);

    execute_startup(wm, argc, argv);

    while (wm->running) {
        wait_event(wm);
        XEvent e;
        XNextEvent(display, &e);
        process_event(wm, &e);
    }

    if (wm->log_file != NULL) {
        fclose(wm->log_file);
    }
}

static Bool
read_file(void* dest, FILE* src, size_t size)
{
    size_t nbytes = 0;
    while (!ferror(src) && (nbytes < size)) {
        void* ptr = (void*)((uintptr_t)dest + nbytes);
        nbytes += fread(ptr, size - nbytes, 1, src);
    }
    return nbytes == size ? True : False;
}

static void
convert_offset_to_pointer(Config* config)
{
    uintptr_t base = (uintptr_t)config;
#define offset2ptr(type, ref)   ref.ptr = (type*)(base + ref.offset)
    offset2ptr(Menu, config->menu);

    Menu* menu = config->menu.ptr;
    offset2ptr(MenuItem, menu->items);

    int i;
    for (i = 0; i < menu->items_num; i++) {
        MenuItem* item = &menu->items.ptr[i];
        offset2ptr(char, item->u.exec.caption);
        offset2ptr(char, item->u.exec.command);
    }
#undef offset2ptr
}

static char* config_exe = "__fawm_config__";

static char*
make_config_exe_path(char* dest, size_t size, const char* fawm_exe)
{
    /*
     * For compatibility, I prepare a writable buffer for dirname(3).
     */
    char* buf = (char*)alloca(strlen(fawm_exe) + 1);
    strcpy(buf, fawm_exe);
    const char* dir = dirname(buf);
    snprintf(dest, size, "%s/%s", dir, config_exe);

    return dest;
}

static Bool
read_config(char* fawm_exe, const char* config_file, Config** config)
{
    char buf[MAXPATHLEN];
    const char* p = strchr(fawm_exe, '/');
    const char* exe = p != NULL ? make_config_exe_path(buf, array_sizeof(buf), fawm_exe) : config_exe;

    char cmd[MAXPATHLEN];
    snprintf(cmd, array_sizeof(cmd), "%s %s", exe, config_file);

    FILE* fpin = popen(cmd, "r");
    assert(fpin != NULL);
    size_t size;
    if (!read_file(&size, fpin, sizeof(size))) {
        pclose(fpin);
        return False;
    }
    *config = (Config*)malloc(size);
    if (!read_file(*config, fpin, size)) {
        pclose(fpin);
        return False;
    }
    if (pclose(fpin) != 0) {
        return False;
    }

    convert_offset_to_pointer(*config);
    return True;
}

int
main(int argc, char* argv[])
{
    char config_file[MAXPATHLEN];
    const char* home = getenv("HOME");
    snprintf(config_file, array_sizeof(config_file), "%s/.fawm.conf", home);
    char log_file[MAXPATHLEN] = "";
    struct option longopts[] = {
        { "config", required_argument, NULL, 'c' },
        { "log-file", required_argument, NULL, 'l' },
        { "version", no_argument, NULL, 'v' },
        { NULL, 0, NULL, 0 }
    };
    int val;
    while ((val = getopt_long_only(argc, argv, "l", longopts, NULL)) != -1) {
        switch (val) {
        case 'c':
            strcpy(config_file, optarg);
            break;
        case 'l':
            if (array_sizeof(log_file) - 1 < strlen(optarg)) {
                print_error("Log Filename Too Long.");
                return 1;
            }
            strcpy(log_file, optarg);
            break;
        case 'v':
            printf("fawm %s\n", FAWM_PACKAGE_VERSION);
            return 0;
        case ':':
        case '?':
        default:
            return 1;
        }
    }

    initialize_event_handlers();
    initialize_event_name();

    Config* config;
    if (!read_config(argv[0], config_file, &config)) {
        print_error("Cannot read config file: %s", config_file);
        return 1;
    }

    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        print_error("XOpenDisplay failed.");
        return 1;
    }

    WindowManager wm;
    wm.config = config;
    wm_main(&wm, display, log_file, argc - optind, argv + optind);

    XCloseDisplay(display);
    free(config);

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
