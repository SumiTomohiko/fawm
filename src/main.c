#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#include <uwm/bitmaps/close_icon>

struct Frame {
    struct Frame* prev;
    struct Frame* next;
    Window window;
    Window child;
    Pixmap active_close_icon;
    Pixmap inactive_close_icon;
    Pixmap close_icon;
    XftDraw* draw;
    Bool wm_delete_window;
    char title[64];
};

typedef struct Frame Frame;

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
    int frame_size;
    int title_height;
    int resizable_corner_size;

    Frame* frames;

    GraspedPosition grasped_position;
    Window grasped_frame;
    int grasped_x;
    int grasped_y;

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
        int items_num;
        int selected_item;
        struct {
            char caption[32];
            char command[32];
        } items[3];
    } popup_menu;

    struct {
        Window window;
        XftDraw* draw;
        time_t clock;
    } taskbar;

    struct {
        Atom wm_delete_window;
        Atom wm_protocols;
    } atoms;

    FILE* log_file; /* For debug */
};

typedef struct WindowManager WindowManager;

#define array_sizeof(x) (sizeof((x)) / sizeof((x)[0]))

static void
print_message(FILE* fp, const char* fmt, va_list ap)
{
    if (fp == NULL) {
        return;
    }
    char buf[128];
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

static void
print_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    print_message(stderr, fmt, ap);
    va_end(ap);
}

static Bool
is_sentinel_frame(Frame* frame)
{
    return frame->next == NULL ? True : False;
}

static Frame*
search_frame_of_child(WindowManager* wm, Window w)
{
    Frame* frame = wm->frames->next;
    while (!is_sentinel_frame(frame) && (frame->child != w)) {
        frame = frame->next;
    }
    return !is_sentinel_frame(frame) ? frame : NULL;
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
x_add_to_save_set(WindowManager* wm, Display* display, Window w)
{
    LOG(wm, "XAddToSaveSet(display, w=0x%08x)", w);
    return XAddToSaveSet(display, w);
}

static Status
x_alloc_named_color(WindowManager* wm, Display* display, Colormap colormap, const char* color_name, XColor* color_def_return, XColor* exact_def_return)
{
    LOG(wm, "XAllocNamedColor(display, colormap, color_name=\"%s\", color_def_return, exact_def_return)", color_name);
    return XAllocNamedColor(display, colormap, color_name, color_def_return, exact_def_return);
}

static int
x_allow_events(WindowManager* wm, Display* display, int event_mode, Time time)
{
    LOG0(wm, "XAllowEvents(display, event_mode, time)");
    return XAllowEvents(display, event_mode, time);
}

static int
x_change_window_attributes(WindowManager* wm, Display* display, Window w, unsigned long valuemask, XSetWindowAttributes* attributes)
{
    LOG(wm, "XChangeWindowAttributes(display, w=0x%08x, valuemask, attributes)", w);
    return XChangeWindowAttributes(display, w, valuemask, attributes);
}

static Bool
x_check_typed_window_event(WindowManager* wm, Display* display, Window w, int event_type, XEvent* event_return)
{
    LOG(wm, "XCheckTypedWindowEvent(display, w=0x%08x, event_type, event_return)", w);
    return XCheckTypedWindowEvent(display, w, event_type, event_return);
}

static int
x_configure_window(WindowManager* wm, Display* display, Window w, unsigned value_mask, XWindowChanges* changes)
{
    LOG(wm, "XConfigureWindow(display, w=0x%08x, value_mask, changes)", w);
    return XConfigureWindow(display, w, value_mask, changes);
}

static int
x_copy_area(WindowManager* wm, Display* display, Drawable src, Drawable dest, GC gc, int src_x, int src_y, unsigned int width, unsigned int height, int dest_x, int dest_y)
{
    LOG(wm, "XCopyArea(display, src=0x%08x, dest=0x%08x, gc, src_x=%d, src_y=%d, width=%d, height=%d, dest_x=%d, dest_y=%d)", src, dest, src_x, src_y, width, height, dest_x, dest_y);
    return XCopyArea(display, src, dest, gc, src_x, src_y, width, height, dest_x, dest_y);
}

static Cursor
x_create_font_cursor(WindowManager* wm, Display* display, unsigned int shape)
{
    LOG(wm, "XCreateFontCursor(display, shape=%d)", shape);
    return XCreateFontCursor(display, shape);
}

static GC
x_create_gc(WindowManager* wm, Display* display, Drawable d, unsigned long valuemask, XGCValues* values)
{
    LOG(wm, "XCreateGC(display, d=0x%08x, valuemask, values)", d);
    return XCreateGC(display, d, valuemask, values);
}

static Pixmap
x_create_pixmap_from_bitmap_data(WindowManager* wm, Display* display, Drawable d, char* data, unsigned int width, unsigned int height, unsigned long fg, unsigned long bg, unsigned int depth)
{
    LOG(wm, "XCreatePixmapFromBitmapData(display, d=0x%08x, data, width=%u, height=%u, fg, bg, depth=%u)", d, width, height, depth);
    return XCreatePixmapFromBitmapData(display, d, data, width, height, fg, bg, depth);
}

static Window
x_create_simple_window(WindowManager* wm, Display* display, Window parent, int x, int y, unsigned int width, unsigned int height, unsigned int border_width, unsigned long border, unsigned long background)
{
    LOG(wm, "XCreateSimpleWindow(display, parent=0x%08x, x=%d, y=%d, width=%u, height=%u, border_width=%u, border, background)", parent, x, y, width, height, border_width);
    return XCreateSimpleWindow(display, parent, x, y, width, height, border_width, border, background);
}

static int
x_define_cursor(WindowManager* wm, Display* display, Window w, Cursor cursor)
{
    LOG(wm, "XDefineCursor(display, w=0x%08x, cursor)", w);
    return XDefineCursor(display, w, cursor);
}

static int
x_draw_line(WindowManager* wm, Display* display, Drawable d, GC gc, int x1, int y1, int x2, int y2)
{
    LOG(wm, "XDrawLine(display, d=0x%08x, gc, x1=%d, y1=%d, x2=%d, y2=%d)", d, x1, y1, x2, y2);
    return XDrawLine(display, d, gc, x1, y1, x2, y2);
}

static int
x_fill_rectagle(WindowManager* wm, Display* display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height)
{
    LOG(wm, "XFillRectangle(display, d=0x%08x, gc, x=%d, y=%d, width=%u, height=%u)", d, x, y, width, height);
    return XFillRectangle(display, d, gc, x, y, width, height);
}

static int
x_free_pixmap(WindowManager* wm, Display* display, Pixmap pixmap)
{
    LOG0(wm, "XFreePixmap(display, pixmap)");
    return XFreePixmap(display, pixmap);
}

static Status
x_get_geomerty(WindowManager* wm, Display* display, Drawable d, Window* root_return, int* x_return, int* y_return, unsigned int* width_return, unsigned int* height_return, unsigned int* border_width_return, unsigned int* depth_return)
{
    LOG(wm, "XGetGeometry(display, d=0x%08x, root_return, x_return, y_return, width_return, height_return, border_width_return, depth_return)", d);
    return XGetGeometry(display, d, root_return, x_return, y_return, width_return, height_return, border_width_return, depth_return);
}

static Status
x_get_text_property(WindowManager* wm, Display* display, Window w, XTextProperty* text_prop_return, Atom property)
{
    LOG(wm, "XGetTextProperty(display, w=0x%08x, text_prop_return, property)", w);
    return XGetTextProperty(display, w, text_prop_return, property);
}

static Status
x_get_window_attributes(WindowManager* wm, Display* display, Window w, XWindowAttributes* window_attributes_return)
{
    LOG(wm, "XGetWindowAttributes(display, w=0x%08x, window_attributes_return)", w);
    return XGetWindowAttributes(display, w, window_attributes_return);
}

static Status
x_get_wm_protocols(WindowManager* wm, Display* display, Window w, Atom** protocols_return, int* count_return)
{
    LOG(wm, "XGetWMProtocols(display, w=0x%08x, protocols_return, count_return)", w);
    return XGetWMProtocols(display, w, protocols_return, count_return);
}

static int
x_grab_button(WindowManager* wm, Display* display, unsigned int button, unsigned int modifiers, Window grab_window, Bool owner_events, unsigned int event_mask, int pointer_mode, int keyboard_mode, Window confine_to, Cursor cursor)
{
    LOG(wm, "XGrabButton(display, button, modifiers, grab_window=0x%08x, owner_events, event_mask, pointer_mode, keyboard_mode, confine_to=0x%08x, cursor)", grab_window, confine_to);
    return XGrabButton(display, button, modifiers, grab_window, owner_events, event_mask, pointer_mode, keyboard_mode, confine_to, cursor);
}

static Atom
x_intern_atom(WindowManager* wm, Display* display, const char* name, Bool only_if_exists)
{
    LOG(wm, "XInternAtom(display, name=\"%s\", only_if_exists)", name);
    return XInternAtom(display, name, only_if_exists);
}

static int
x_kill_client(WindowManager* wm, Display* display, XID resource)
{
    LOG0(wm, "XKillClient(display, resource)");
    return XKillClient(display, resource);
}

static int
x_map_raised(WindowManager* wm, Display* display, Window w)
{
    LOG(wm, "XMapRaised(display, w=0x%08x)", w);
    return XMapRaised(display, w);
}

static int
x_map_window(WindowManager* wm, Display* display, Window w)
{
    LOG(wm, "XMapWindow(display, w=0x%08x)", w);
    return XMapWindow(display, w);
}

static int
x_move_resize_window(WindowManager* wm, Display* display, Window w, int x, int y, unsigned width, unsigned height)
{
    LOG(wm, "XMoveResizeWindow(display, w=0x%08x, x=%d, y=%d, width=%u, height=%u)", w, x, y, width, height);
    return XMoveResizeWindow(display, w, x, y, width, height);
}

static int
x_move_window(WindowManager* wm, Display* display, Window w, int x, int y)
{
    LOG(wm, "XMoveWindow(display, w=0x%08x, x=%d, y=%d)", w, x, y);
    return XMoveWindow(display, w, x, y);
}

static Status
x_query_tree(WindowManager* wm, Display* display, Window w, Window* root_return, Window* parent_return, Window** children_return, unsigned int* nchildren_return)
{
    LOG(wm, "XQueryTree(display, w=0x%08x, root_return, parent_return, children_return, nchildren_return)", w);
    return XQueryTree(display, w, root_return, parent_return, children_return, nchildren_return);
}

static int
x_raise_window(WindowManager* wm, Display* display, Window w)
{
    LOG(wm, "XRaiseWindow(display, w=0x%08x)", w);
    return XRaiseWindow(display, w);
}

static int
x_reparent_window(WindowManager* wm, Display* display, Window w, Window parent,  int x, int y)
{
    LOG(wm, "XReparentWindow(display, w=0x%08x, parent=0x%08x, x=%d, y=%d)", x, parent, x, y);
    return XReparentWindow(display, w, parent, x, y);
}

static int
x_resize_window(WindowManager* wm, Display* display, Window w, unsigned width, unsigned height)
{
    LOG(wm, "XResizeWindow(display, w=0x%08x, width=%u, height=%u)", w, width, height);
    return XResizeWindow(display, w, width, height);
}

static Status
x_send_event(WindowManager* wm, Display* display, Window w, Bool propagate, long event_mask, XEvent* event_send)
{
    LOG(wm, "XSendEvent(display, w=0x%08x, propagate, event_mask, event_send)", w);
    return XSendEvent(display, w, propagate, event_mask, event_send);
}

static int
x_set_input_focus(WindowManager* wm, Display* display, Window focus, int revert_to, Time time)
{
    LOG(wm, "XSetInputFocus(display, focus=0x%08x, revert_to, time)", focus);
    return XSetInputFocus(display, focus, revert_to, time);
}

static int
x_set_window_background(WindowManager* wm, Display* display, Window w, unsigned long background_pixel)
{
    LOG(wm, "XSetWindowBackground(display, w=0x%08x, background_pixel)", w);
    return XSetWindowBackground(display, w, background_pixel);
}

static int
x_set_window_border_width(WindowManager* wm, Display* display, Window w, unsigned width)
{
    LOG(wm, "XSetWindowBorderWidth(display, w=0x%08x, width=%u)", w, width);
    return XSetWindowBorderWidth(display, w, width);
}

static Status
x_text_property_to_string_list(WindowManager* wm, XTextProperty* text_prop, char*** list_return, int* count_return)
{
    LOG0(wm, "XTextPropertyToStringList(text_prop, list_return, count_return)");
    return XTextPropertyToStringList(text_prop, list_return, count_return);
}

static int
x_unmap_window(WindowManager* wm, Display* display, Window w)
{
    LOG(wm, "XUnmapWindow(display, w=0x%08x)", w);
    return XUnmapWindow(display, w);
}

static Bool
xft_color_alloc_name(WindowManager* wm, Display* display, Visual* visual, Colormap colormap, char* name, XftColor* result)
{
    LOG(wm, "XftColorAllocName(display, visual, colormap, name=\"%s\", result)", name);
    return XftColorAllocName(display, visual, colormap, name, result);
}

static XftDraw*
xft_draw_create(WindowManager* wm, Display* display, Drawable d, Visual* visual, Colormap colormap)
{
    LOG(wm, "XftDrawCreate(display, d=0x%08x, visual, colormap)", d);
    return XftDrawCreate(display, d, visual, colormap);
}

static void
xft_draw_destroy(WindowManager* wm, XftDraw* draw)
{
    LOG0(wm, "XftDrawDestroy(draw)");
    return XftDrawDestroy(draw);
}

static void
xft_draw_string_utf8(WindowManager* wm, XftDraw* draw, XftColor* color, XftFont* pub, int x, int y, FcChar8* string, int len)
{
    LOG(wm, "XftDrawStringUtf8(draw, color, pub, x=%d, y=%d, string, len=%d)", x, y, len);
    return XftDrawStringUtf8(draw, color, pub, x, y, string, len);
}

static XftFont*
xft_font_open_name(WindowManager* wm, Display* display, int screen, const char* name)
{
    LOG(wm, "XftFontOpenName(display, screen, name=\"%s\")", name);
    return XftFontOpenName(display, screen, name);
}

static int
compute_close_icon_x(WindowManager* wm, Window w)
{
    XWindowAttributes wa;
    x_get_window_attributes(wm, wm->display, w, &wa);
    return wa.width - wm->frame_size - close_icon_width;
}

static void
draw_close_icon(WindowManager* wm, Frame* frame)
{
    Display* display = wm->display;
    Window w = frame->window;
    x_copy_area(
        wm,
        display,
        frame->close_icon,
        w,
        DefaultGC(display, DefaultScreen(display)),
        0, 0,
        close_icon_width, close_icon_height,
        compute_close_icon_x(wm, w), wm->frame_size);
}

static Atom
intern(WindowManager* wm, const char* name)
{
    return x_intern_atom(wm, wm->display, name, False);
}

static void
get_window_name(WindowManager* wm, char* dest, int size, Window w)
{
    XTextProperty prop;
    dest[0] = '\0';
    if (x_get_text_property(wm, wm->display, w, &prop, XA_WM_NAME) == 0) {
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
    if (x_text_property_to_string_list(wm, &prop, &strings, &_) == 0) {
        return;
    }
    if (strings == NULL) {
        return;
    }
    snprintf(dest, size, "%s", strings[0]);
    XFreeStringList(strings);
}

static void
draw_title_font_string(WindowManager* wm, XftDraw* draw, int x, int y, const char* text)
{
    XftColor* color = &wm->title_color;
    XftFont* font = wm->title_font;
    int len = strlen(text);
    xft_draw_string_utf8(wm, draw, color, font, x, y, (XftChar8*)text, len);
}

static void
draw_title_text(WindowManager* wm, Frame* frame)
{
    int frame_size = wm->frame_size;
    int x = frame_size;
    int y = frame_size + wm->title_height;
    draw_title_font_string(wm, frame->draw, x, y, frame->title);
}

static void
get_geometry(WindowManager* wm, Window w, unsigned int* width, unsigned int* height)
{
    Window _;
    int __;
    unsigned int ___;
    x_get_geomerty(wm, wm->display, w, &_, &__, &__, width, height, &___, &___);
}

static void
draw_corner(WindowManager* wm, Window w)
{
    unsigned int width;
    unsigned int height;
    get_geometry(wm, w, &width, &height);
    Display* display = wm->display;
    GC gc = DefaultGC(display, DefaultScreen(display));
    int frame_size = wm->frame_size;
    int corner_size = wm->resizable_corner_size;
#define DRAW_LINE(x1, y1, x2, y2) \
    x_draw_line(wm, display, w, gc, (x1), (y1), (x2), (y2))
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
    draw_title_text(wm, frame);
    draw_close_icon(wm, frame);
    draw_corner(wm, w);
}

static void
change_frame_event_mask(WindowManager* wm, Window w)
{
    XSetWindowAttributes swa;
    swa.event_mask = ButtonPressMask | ButtonReleaseMask | ExposureMask | FocusChangeMask | LeaveWindowMask | PointerMotionMask | SubstructureNotifyMask | SubstructureRedirectMask;
    x_change_window_attributes(wm, wm->display, w, CWEventMask, &swa);
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
    Frame* anchor = wm->frames;
    Frame* next = anchor->next;
    next->prev = frame;
    frame->prev = anchor;
    frame->next = next;
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

static XftDraw*
create_draw(WindowManager* wm, Window w)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    Visual* visual = DefaultVisual(display, screen);
    Colormap colormap = DefaultColormap(display, screen);
    return xft_draw_create(wm, display, w, visual, colormap);
}

static Pixmap
create_close_icon(WindowManager* wm, Window w, int pixel)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    return x_create_pixmap_from_bitmap_data(
        wm,
        display,
        w,
        (char*)close_icon_bits,
        close_icon_width, close_icon_height,
        BlackPixel(display, screen), pixel,
        DefaultDepth(display, screen));
}

static Frame*
create_frame(WindowManager* wm, int x, int y, int child_width, int child_height)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    int width = child_width + compute_frame_width(wm);
    int height = child_height + compute_frame_height(wm);
    int focused_color = wm->focused_foreground_color;
    Window w = x_create_simple_window(
        wm,
        display, DefaultRootWindow(display),
        x, y,
        width, height,
        wm->border_size,
        BlackPixel(display, screen), focused_color);
    change_frame_event_mask(wm, w);

    Frame* frame = alloc_frame();
    frame->window = w;
    Pixmap active_close_icon = create_close_icon(wm, w, focused_color);
    frame->active_close_icon = active_close_icon;
    int unfocused_color = wm->unfocused_foreground_color;
    frame->inactive_close_icon = create_close_icon(wm, w, unfocused_color);
    frame->close_icon = active_close_icon;
    frame->draw = create_draw(wm, w);
    frame->wm_delete_window = False;
    assert(frame->draw != NULL);

    insert_frame(wm, frame);

    return frame;
}

static void
focus(WindowManager* wm, Window w)
{
    x_set_input_focus(wm, wm->display, w, RevertToNone, CurrentTime);
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
    if (x_get_wm_protocols(wm, wm->display, frame->child, &protos, &n) == 0) {
        return;
    }
    int i;
    for (i = 0; i < n; i++) {
        read_protocol(wm, frame, protos[i]);
    }
    XFree(protos);
}

static void
reparent_window(WindowManager* wm, Window w)
{
    Display* display = wm->display;
    XWindowAttributes wa;
    if (x_get_window_attributes(wm, display, w, &wa) == 0) {
        return;
    }
    Frame* frame = create_frame(wm, wa.x, wa.y, wa.width, wa.height);
    frame->child = w;
    get_window_name(wm, frame->title, array_sizeof(frame->title), w);
    LOG(wm, "Window Name: window=0x%08x, name=%s", w, frame->title);
    int frame_size = wm->frame_size;
    int x = frame_size;
    int y = 2 * frame_size + wm->title_height;
    x_set_window_border_width(wm, display, w, 0);
    Window parent = frame->window;
    LOG(wm, "Reparented: frame=0x%08x, child=0x%08x", parent, w);
    x_reparent_window(wm, display, w, parent, x, y);
    read_protocols(wm, frame);

    x_grab_button(wm, display, Button1, AnyModifier, w, True, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);

    x_map_window(wm, display, frame->window);
    x_map_window(wm, display, w);
    focus(wm, frame->child);
    x_add_to_save_set(wm, display, w);
}

static void
reparent_toplevels(WindowManager* wm)
{
    Display* display = wm->display;
    Window root = DefaultRootWindow(display);
    Window _;
    Window* children;
    unsigned int nchildren;
    if (x_query_tree(wm, display, root, &_, &_, &children, &nchildren) == 0) {
        print_error("XQueryTree failed.");
        return;
    }
    int i;
    for (i = 0; i < nchildren; i++) {
        XWindowAttributes wa;
        x_get_window_attributes(wm, display, children[i], &wa);
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
    if (x_alloc_named_color(wm, display, colormap, name, &c, &_) == 0) {
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

    xft_draw_destroy(wm, frame->draw);
    Display* display = wm->display;
    x_free_pixmap(wm, display, frame->inactive_close_icon);
    x_free_pixmap(wm, display, frame->active_close_icon);

    memset(frame, 0xfd, sizeof(*frame));
    free(frame);
}

static int
x_destroy_window(WindowManager* wm, Display* display, Window w)
{
    LOG(wm, "XDestroyWindow(display, w=0x%08x)", w);
    return XDestroyWindow(display, w);
}

static void
destroy_frame(WindowManager* wm, Frame* frame)
{
    Window w = frame->window;
    free_frame(wm, frame);
    x_destroy_window(wm, wm->display, w);
}

static void
process_destroy_notify(WindowManager* wm, XDestroyWindowEvent* e)
{
    Window w = e->event;
    Frame* frame = search_frame(wm, w);
    if (frame == NULL) {
        return;
    }
    destroy_frame(wm, frame);
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
    Window popup_menu = wm->popup_menu.window;
    x_move_window(wm, display, popup_menu, x - 16, y - 16);
    x_map_raised(wm, display, popup_menu);
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

static Bool
is_on_close_icon(WindowManager* wm, Window w, int x, int y)
{
    int frame_size = wm->frame_size;
    int close_x = compute_close_icon_x(wm, w);
    int close_y = wm->border_size + frame_size;
    int width = close_icon_width;
    int height = close_icon_height;
    return is_region_inside(close_x, close_y, width, height, x, y);
}

static void
close_frame(WindowManager* wm, Frame* frame)
{
    Display* display = wm->display;
    Window child = frame->child;
    if (!frame->wm_delete_window) {
        x_kill_client(wm, display, child);
        destroy_frame(wm, frame);
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
    x_send_event(wm, display, child, False, 0, &e);
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
    Frame* frame = search_frame_of_child(wm, w);
    if (frame != NULL) {
        x_raise_window(wm, display, frame->window);
        focus(wm, w);
        x_allow_events(wm, display, ReplayPointer, CurrentTime);
        return;
    }
    frame = search_frame(wm, w);
    assert(frame != NULL);
    int x = e->x;
    int y = e->y;
    if (is_on_close_icon(wm, w, x, y)) {
        close_frame(wm, frame);
        return;
    }
    x_raise_window(wm, display, w);
    focus(wm, frame->child);
    grasp_frame(wm, detect_frame_position(wm, w, x, y), w, x, y);
}

static void
unmap_popup_menu(WindowManager* wm)
{
    x_unmap_window(wm, wm->display, wm->popup_menu.window);
}

static void
resize_child(WindowManager* wm, Window w, int frame_width, int frame_height)
{
    int width = frame_width - compute_frame_width(wm);
    int height = frame_height - compute_frame_height(wm);
    x_resize_window(wm, wm->display, w, width, height);
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
    x_get_window_attributes(wm, wm->display, wm->popup_menu.window, &wa);
    if (!is_region_inside(wa.x, wa.y, wa.width, wa.height, x, y)) {
        return -1;
    }
    int index = (y - wa.y) / compute_font_height(wm->title_font) - 1;
    return wm->popup_menu.items_num <= index ? -1 : index;
}

const char* popup_title = "Applications";

static void
draw_popup_menu(WindowManager* wm)
{
    Display* display = wm->display;
    Window w = wm->popup_menu.window;
    GC title_gc = wm->popup_menu.title_gc;
    unsigned int window_width;
    unsigned int _;
    get_geometry(wm, w, &window_width, &_);
    XftFont* font = wm->title_font;
    int item_height = font->ascent + font->descent;
    x_fill_rectagle(wm, display, w, title_gc, 0, 0, window_width, item_height);

    int x = 0;
    int selected_item = wm->popup_menu.selected_item;
    int y = item_height * (selected_item + 1);
    if (0 <= selected_item) {
        GC gc = wm->popup_menu.selected_gc;
        x_fill_rectagle(wm, display, w, gc, x, y, window_width, item_height);
    }

    XftDraw* draw = wm->popup_menu.draw;
    y = font->ascent;
    draw_title_font_string(wm, draw, x, y, popup_title);
    int i;
    for (i = 0; i < wm->popup_menu.items_num; i++) {
        y += item_height;
        const char* text = wm->popup_menu.items[i].caption;
        draw_title_font_string(wm, draw, x, y, text);
    }
}

static int
x_clear_area(WindowManager* wm, Display* display, Window w, int x, int y, unsigned width, unsigned height, Bool exposures)
{
    LOG(wm, "XClearArea(display, w=0x%08x, x=%d, y=%d, width=%u, height=%u, exposures)", w, x, y, width, height);
    return XClearArea(display, w, x, y, width, height, exposures);
}

static void
expose(WindowManager* wm, Window w)
{
    x_clear_area(wm, wm->display, w, 0, 0, 0, 0, True);
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

static Pixmap
select_close_icon(WindowManager* wm, Frame* frame, int x, int y)
{
    if (is_on_close_icon(wm, frame->window, x, y)) {
        return frame->active_close_icon;
    }
    return frame->inactive_close_icon;
}

static void
change_close_icon(WindowManager* wm, Window w, int x, int y)
{
    Frame* frame = search_frame(wm, w);
    assert(frame != NULL);
    Pixmap icon = select_close_icon(wm, frame, x, y);
    if (icon == frame->close_icon) {
        return;
    }
    frame->close_icon = icon;
    expose(wm, w);
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
    x_define_cursor(wm, wm->display, w, cursor);
}

static void
process_motion_notify(WindowManager* wm, XMotionEvent* e)
{
    Display* display = wm->display;
    Window w = e->window;
    int x = e->x;
    int y = e->y;
    if (w == DefaultRootWindow(display)) {
        highlight_selected_popup_item(wm, x, y);
        return;
    }
    if ((e->state & Button1Mask) == 0) {
        change_cursor(wm, w, x, y);
        change_close_icon(wm, w, x, y);
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
        x_move_window(wm, display, w, new_x, new_y);
        return;
    }
    XWindowAttributes frame_attrs;
    x_get_window_attributes(wm, display, w, &frame_attrs);
    Frame* frame = search_frame(wm, w);
    assert(frame != NULL);
    Window child = frame->child;
    XWindowAttributes child_attrs;
    x_get_window_attributes(wm, display, child, &child_attrs);
    int new_width;
    int new_height;
    switch (wm->grasped_position) {
    case GP_NORTH:
        new_width = frame_attrs.width;
        new_height = frame_attrs.y + frame_attrs.height - new_y;
        x_move_resize_window(
            wm,
            display, w,
            frame_attrs.x, new_y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_NORTH_EAST:
        new_width = x + (frame_attrs.width - wm->grasped_x);
        new_height = frame_attrs.y + frame_attrs.height - new_y;
        x_move_resize_window(
            wm,
            display, w,
            frame_attrs.x, new_y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        wm->grasped_x = x;
        return;
    case GP_EAST:
        new_width = x + (frame_attrs.width - wm->grasped_x);
        new_height = frame_attrs.height;
        x_resize_window(wm, display, w, new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        wm->grasped_x = x;
        return;
    case GP_SOUTH_EAST:
        new_width = x + (frame_attrs.width - wm->grasped_x);
        new_height = y + (frame_attrs.height - wm->grasped_y);
        x_resize_window(wm, display, w, new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        wm->grasped_x = x;
        wm->grasped_y = y;
        return;
    case GP_SOUTH:
        new_width = frame_attrs.width;
        new_height = y + (frame_attrs.height - wm->grasped_y);
        x_resize_window(wm, display, w, new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        wm->grasped_y = y;
        return;
    case GP_SOUTH_WEST:
        new_width = frame_attrs.x + frame_attrs.width - new_x;
        new_height = y + (frame_attrs.height - wm->grasped_y);
        x_move_resize_window(
            wm,
            display, w,
            new_x, frame_attrs.y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        wm->grasped_y = y;
        return;
    case GP_WEST:
        new_width = frame_attrs.x + frame_attrs.width - new_x;
        new_height = frame_attrs.height;
        x_move_resize_window(
            wm,
            display, w,
            new_x, frame_attrs.y,
            new_width, new_height);
        resize_child(wm, child, new_width, new_height);
        return;
    case GP_NORTH_WEST:
        new_width = frame_attrs.x + frame_attrs.width - new_x;
        new_height = frame_attrs.y + frame_attrs.height - new_y;
        x_move_resize_window(
            wm,
            display, w,
            new_x, new_y,
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
    while (x_check_typed_window_event(wm, display, w, event_type, e));
}

static void
draw_taskbar(WindowManager* wm)
{
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    char text[64];
    strftime(text, array_sizeof(text), "%Y-%m-%dT%H:%M", &tm);

    XftDraw* draw = wm->taskbar.draw;
    XftColor* color = &wm->title_color;
    XftFont* font = wm->title_font;
    int y = font->ascent;
    int len = strlen(text);
    xft_draw_string_utf8(wm, draw, color, font, 0, y, (XftChar8*)text, len);
}

static void
process_expose(WindowManager* wm, XExposeEvent* e)
{
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
fork_child(const char* cmd)
{
    pid_t pid = do_fork();
    if (pid == 0) {
        execlp(cmd, cmd, NULL);
        exit(1);
    }
    exit(0);
}

static void
execute(WindowManager* wm, const char* cmd)
{
    if (strcmp(cmd, "exit") == 0) {
        wm->running = False;
        return;
    }

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
    if (e->window != DefaultRootWindow(wm->display)) {
        release_frame(wm);
        return;
    }
    unmap_popup_menu(wm);
    int index = detect_selected_popup_item(wm, e->x, e->y);
    if (index < 0) {
        return;
    }
    execute(wm, wm->popup_menu.items[index].command);
}

static void
change_frame_background(WindowManager* wm, Window w, int pixel)
{
    x_set_window_background(wm, wm->display, w, pixel);
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
    x_raise_window(wm, wm->display, w);
    change_frame_background(wm, w, wm->focused_foreground_color);
}

static void
process_map_request(WindowManager* wm, XMapRequestEvent* e)
{
    Window w = e->window;
    Frame* frame = search_frame_of_child(wm, w);
    if (frame != NULL) {
        return;
    }
    reparent_window(wm, w);
}

static void
process_unmap_notify(WindowManager* wm, XUnmapEvent* e)
{
    Frame* frame = search_frame_of_child(wm, e->window);
    if (frame == NULL) {
        /* XXX: Who sends UnmapNotify without a frame? */
        return;
    }
    x_unmap_window(wm, wm->display, frame->window);
}

static const char*
dmxEventName(int type)
{
    switch (type) {
    case KeyPress:         return "KeyPress";
    case KeyRelease:       return "KeyRelease";
    case ButtonPress:      return "ButtonPress";
    case ButtonRelease:    return "ButtonRelease";
    case MotionNotify:     return "MotionNotify";
    case EnterNotify:      return "EnterNotify";
    case LeaveNotify:      return "LeaveNotify";
    case FocusIn:          return "FocusIn";
    case FocusOut:         return "FocusOut";
    case KeymapNotify:     return "KeymapNotify";
    case Expose:           return "Expose";
    case GraphicsExpose:   return "GraphicsExpose";
    case NoExpose:         return "NoExpose";
    case VisibilityNotify: return "VisibilityNotify";
    case CreateNotify:     return "CreateNotify";
    case DestroyNotify:    return "DestroyNotify";
    case UnmapNotify:      return "UnmapNotify";
    case MapNotify:        return "MapNotify";
    case MapRequest:       return "MapRequest";
    case ReparentNotify:   return "ReparentNotify";
    case ConfigureNotify:  return "ConfigureNotify";
    case ConfigureRequest: return "ConfigureRequest";
    case GravityNotify:    return "GravityNotify";
    case ResizeRequest:    return "ResizeRequest";
    case CirculateNotify:  return "CirculateNotify";
    case CirculateRequest: return "CirculateRequest";
    case PropertyNotify:   return "PropertyNotify";
    case SelectionClear:   return "SelectionClear";
    case SelectionRequest: return "SelectionRequest";
    case SelectionNotify:  return "SelectionNotify";
    case ColormapNotify:   return "ColormapNotify";
    case ClientMessage:    return "ClientMessage";
    case MappingNotify:    return "MappingNotify";
    default:               return "<unknown>";
    }
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
        x_configure_window(wm, display, parent, CWX, &changes);
    }
    if (value_mask & CWY) {
        XWindowChanges changes;
        changes.y = e->y - (frame_size + wm->title_height);
        x_configure_window(wm, display, parent, CWY, &changes);
    }
    if (value_mask & CWWidth) {
        XWindowChanges changes;
        int width = e->width;
        changes.width = width + compute_frame_width(wm);
        unsigned int value_mask = CWWidth;
        x_configure_window(wm, display, parent, value_mask, &changes);
        changes.width = width;
        x_configure_window(wm, display, w, value_mask, &changes);
    }
    if (value_mask & CWHeight) {
        XWindowChanges changes;
        int height = e->height;
        changes.height = height + compute_frame_height(wm);
        unsigned int value_mask = CWHeight;
        x_configure_window(wm, display, parent, value_mask, &changes);
        changes.height = height;
        x_configure_window(wm, display, w, value_mask, &changes);
    }
    /* Ignore CWBorderWidth, CWSibling and CWStackMode */
}

static void
process_configure_request(WindowManager* wm, XConfigureRequestEvent* e)
{
    Window parent = e->parent;
    Frame* frame = search_frame(wm, parent);
    Window w = e->window;
    if (frame != NULL) {
        configure_frame(wm, parent, w, e);
        return;
    }

    Display* display = wm->display;
    unsigned long value_mask = e->value_mask;
    if (value_mask & CWX) {
        XWindowChanges changes;
        changes.x = e->x;
        x_configure_window(wm, display, w, CWX, &changes);
    }
    if (value_mask & CWY) {
        XWindowChanges changes;
        changes.y = e->y;
        x_configure_window(wm, display, w, CWY, &changes);
    }
    if (value_mask & CWWidth) {
        XWindowChanges changes;
        changes.width = e->width;
        x_configure_window(wm, display, w, CWWidth, &changes);
    }
    if (value_mask & CWHeight) {
        XWindowChanges changes;
        changes.height = e->height;
        x_configure_window(wm, display, w, CWHeight, &changes);
    }
    if (value_mask & CWBorderWidth) {
        XWindowChanges changes;
        changes.border_width = e->border_width;
        x_configure_window(wm, display, w, CWBorderWidth, &changes);
    }
    if (value_mask & CWSibling) {
        LOG0(wm, "CWSibling");
    }
    if (value_mask & CWStackMode) {
        XWindowChanges changes;
        changes.stack_mode = e->detail;
        x_configure_window(wm, display, w, CWStackMode, &changes);
    }
}

static int
x_undefine_cursor(WindowManager* wm, Display* display, Window w)
{
    LOG(wm, "XUndefineCursor(display, w=0x%08x)", w);
    return XUndefineCursor(display, w);
}

static void
process_leave_notify(WindowManager* wm, XCrossingEvent* e)
{
    Window w = e->window;
    if (search_frame(wm, w) == NULL) {
        return;
    }
    x_undefine_cursor(wm, wm->display, w);
}

static void
process_event(WindowManager* wm, XEvent* e)
{
    int type = e->type;
    if (type != MotionNotify) {
        LOG(wm, "%s: window=0x%08x", dmxEventName(type), e->xany.window);
    }
    if (type == ButtonPress) {
        XButtonEvent* ev = &e->xbutton;
        process_button_press(wm, ev);
    }
    else if (type == ButtonRelease) {
        XButtonEvent* ev = &e->xbutton;
        process_button_release(wm, ev);
    }
    else if (type == ConfigureRequest) {
        XConfigureRequestEvent* cre = &e->xconfigurerequest;
        process_configure_request(wm, cre);
    }
    else if (type == DestroyNotify) {
        XDestroyWindowEvent* ev = &e->xdestroywindow;
        process_destroy_notify(wm, ev);
    }
    else if (type == Expose) {
        XExposeEvent* ev = &e->xexpose;
        process_expose(wm, ev);
    }
    else if (type == LeaveNotify) {
        XCrossingEvent* ev = &e->xcrossing;
        process_leave_notify(wm, ev);
    }
    else if (type == FocusIn) {
        XFocusChangeEvent* ev = &e->xfocus;
        process_focus_in(wm, ev);
    }
    else if (type == FocusOut) {
        XFocusChangeEvent* ev = &e->xfocus;
        process_focus_out(wm, ev);
    }
    else if (type == MotionNotify) {
        get_last_event(wm, e->xmotion.window, MotionNotify, e);
        XMotionEvent* ev = &e->xmotion;
        process_motion_notify(wm, ev);
    }
    else if (type == MapRequest) {
        XMapRequestEvent* ev = &e->xmaprequest;
        process_map_request(wm, ev);
    }
    else if (type == UnmapNotify) {
        XUnmapEvent* ev = &e->xunmap;
        process_unmap_notify(wm, ev);
    }
}

static void
change_event_mask_exposure(WindowManager* wm, Window w)
{
    XSetWindowAttributes swa;
    swa.event_mask = ExposureMask;
    x_change_window_attributes(wm, wm->display, w, CWEventMask, &swa);
}

static void
change_taskbar_event_mask(WindowManager* wm, Window w)
{
    change_event_mask_exposure(wm, w);
}

static void
change_popup_menu_event_mask(WindowManager* wm, Window w)
{
    change_event_mask_exposure(wm, w);
}

static void
setup_popup_menu(WindowManager* wm)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    Window w = x_create_simple_window(
        wm,
        display, DefaultRootWindow(display),
        0, 0,
        42, 42, /* They are dummy. They will be defined after. */
        wm->border_size,
        BlackPixel(display, screen), WhitePixel(display, screen));
    LOG(wm, "popup menu: 0x%08x", w);
    change_popup_menu_event_mask(wm, w);
    wm->popup_menu.window = w;

    XGCValues title_gc;
    title_gc.foreground = wm->focused_foreground_color;
    int mask = GCForeground;
    wm->popup_menu.title_gc = x_create_gc(wm, display, w, mask, &title_gc);

    XGCValues selected_gc;
    selected_gc.foreground = alloc_color(wm, "yellow");
    wm->popup_menu.selected_gc = x_create_gc(wm, display, w, mask, &selected_gc);

    wm->popup_menu.draw = create_draw(wm, w);
    assert(wm->popup_menu.draw != NULL);

    wm->popup_menu.items_num = array_sizeof(wm->popup_menu.items);
    strcpy(wm->popup_menu.items[0].caption, "Firefox");
    strcpy(wm->popup_menu.items[0].command, "firefox");
    strcpy(wm->popup_menu.items[1].caption, "mlterm");
    strcpy(wm->popup_menu.items[1].command, "mlterm");
    strcpy(wm->popup_menu.items[2].caption, "exit");
    strcpy(wm->popup_menu.items[2].command, "exit");

    XftFont* font = wm->title_font;
    int width = font->max_advance_width * strlen(popup_title);
    int font_height = compute_font_height(font);
    int height = font_height * (wm->popup_menu.items_num + 1);
    x_resize_window(wm, display, w, width, height);
}

static void
setup_title_font(WindowManager* wm)
{
    Display* display = wm->display;
    int screen = DefaultScreen(display);
    const char* name = "VL PGothic-18";
    XftFont* title_font = xft_font_open_name(wm, display, screen, name);
    if (title_font == NULL) {
        print_error("Cannot find font (XftFontOpenName failed): %s", name);
        exit(1);
    }
    wm->title_font = title_font;
    Visual* visual = DefaultVisual(display, screen);
    Colormap colormap = DefaultColormap(display, screen);
    XftColor* result = &wm->title_color;
    /**
     * XXX: I could not find a description about a mean of XftColorAllocName's
     * return value.
     */
    xft_color_alloc_name(wm, display, visual, colormap, "black", result);
}

static void
setup_cursors(WindowManager* wm)
{
    Display* display = wm->display;
#define CREATE_CURSOR(member, cursor) \
    wm->member = x_create_font_cursor(wm, display, cursor)
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
    int border_size = wm->border_size;
    int screen = DefaultScreen(display);
    Window w = x_create_simple_window(
        wm,
        display, root,
        - border_size, root_height - font_height,
        root_width, font_height,
        border_size,
        BlackPixel(display, screen), wm->unfocused_foreground_color);
    LOG(wm, "taskbar: 0x%08x", w);
    change_taskbar_event_mask(wm, w);
    wm->taskbar.window = w;
    wm->taskbar.draw = create_draw(wm, w);
    wm->taskbar.clock = -1;
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
    wm->running = True;
    wm->focused_foreground_color = alloc_color(wm, "light pink");
    wm->unfocused_foreground_color = alloc_color(wm, "grey");
    wm->border_size = 1;
    wm->frame_size = 4;
    wm->title_height = 16;
    wm->resizable_corner_size = 32;
    setup_frame_list(wm);
    release_frame(wm);
    setup_title_font(wm);
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
    FILE* fp = fopen("uwm-error.log", "w");
    assert(fp != NULL);
    log_error(fp, "X Error at pid %u", getpid());
    log_error(fp, "Serial Number of Request Code: %lu", e->serial);
    long code = e->error_code;
    char msg[64];
    XGetErrorText(display, code, msg, array_sizeof(msg));
    log_error(fp, "Error Code of Failed Request: %u (%s)", code, msg);
    log_error(fp, "Major Opcode of Failed Request: %u", e->request_code);
    log_error(fp, "Minor Opcode of Failed Request: %u", e->minor_code);
    fclose(fp);
    abort();

    return 0;
}

static int
x_select_input(WindowManager* wm, Display* display, Window w, int event_mask)
{
    LOG(wm, "XSelectInput(display, w=0x%08x, event_mask)", w);
    return XSelectInput(display, w, event_mask);
}

static void
wm_main(WindowManager* wm, Display* display, const char* log_file)
{
    XSetErrorHandler(error_handler);

    setup_window_manager(wm, display, log_file);
    Window root = DefaultRootWindow(display);
    x_define_cursor(wm, display, root, wm->normal_cursor);
    reparent_toplevels(wm);
    x_map_window(wm, display, wm->taskbar.window);
    long mask = Button1MotionMask | ButtonPressMask | ButtonReleaseMask | SubstructureRedirectMask;
    x_select_input(wm, display, root, mask);
    LOG(wm, "root window=0x%08x", root);

    while (wm->running) {
        wait_event(wm);
        XEvent e;
        XNextEvent(display, &e);
        process_event(wm, &e);
    }

    fclose(wm->log_file);
}

int
main(int argc, char* argv[])
{
    char log_file[64] = { '\0' };
    struct option longopts[] = {
        { "log-file", required_argument, NULL, 'l' },
        { NULL, 0, NULL, 0 }
    };
    int val;
    while ((val = getopt_long_only(argc, argv, "l", longopts, NULL)) != -1) {
        switch (val) {
        case 'l':
            if (array_sizeof(log_file) - 1 < strlen(optarg)) {
                print_error("Log Filename Too Long.");
                return 1;
            }
            strcpy(log_file, optarg);
            break;
        case ':':
        case '?':
        default:
            return 1;
        }
    }

    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        print_error("XOpenDisplay failed.");
        return 1;
    }

    WindowManager wm;
    wm_main(&wm, display, log_file);

    XCloseDisplay(display);

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
