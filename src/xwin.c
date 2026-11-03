/*
 * xwin.c - Window creation, font loading and primitive text drawing.
 */
#include "xwin.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <X11/Xutil.h>

/* Monospace fonts to try, in order of preference. "fixed" exists on every
 * X server, so it is the last resort. */
static const char *FONT_NAMES[] = {
    "-misc-fixed-medium-r-normal--15-*-*-*-c-*-iso8859-1",
    "9x15",
    "fixed",
};

static unsigned long alloc_color(XWin *xw, const char *name, unsigned long fallback)
{
    XColor exact, screen;
    Colormap cmap = DefaultColormap(xw->dpy, xw->screen);
    if (XAllocNamedColor(xw->dpy, cmap, name, &screen, &exact))
        return screen.pixel;
    return fallback;
}

static int create_back_buffer(XWin *xw)
{
    xw->back = XCreatePixmap(xw->dpy, xw->win, (unsigned)xw->width,
                             (unsigned)xw->height,
                             (unsigned)DefaultDepth(xw->dpy, xw->screen));
    return xw->back ? 0 : -1;
}

int xwin_open(XWin *xw, const char *title, int width, int height)
{
    memset(xw, 0, sizeof *xw);

    xw->dpy = XOpenDisplay(NULL);
    if (!xw->dpy) {
        fprintf(stderr, "terminator: cannot open X display (is DISPLAY set?)\n");
        return -1;
    }
    xw->screen = DefaultScreen(xw->dpy);
    /* Child processes must not inherit the X connection. */
    fcntl(ConnectionNumber(xw->dpy), F_SETFD, FD_CLOEXEC);

    for (size_t i = 0; i < sizeof FONT_NAMES / sizeof FONT_NAMES[0]; i++) {
        xw->font = XLoadQueryFont(xw->dpy, FONT_NAMES[i]);
        if (xw->font)
            break;
    }
    if (!xw->font) {
        fprintf(stderr, "terminator: cannot load a monospace font\n");
        XCloseDisplay(xw->dpy);
        xw->dpy = NULL;
        return -1;
    }
    xw->char_w = xw->font->max_bounds.width;
    xw->ascent = xw->font->ascent;
    xw->line_h = xw->font->ascent + xw->font->descent;
    if (xw->char_w <= 0)
        xw->char_w = 8;

    xw->fg = alloc_color(xw, "#d8d8d8", WhitePixel(xw->dpy, xw->screen));
    xw->bg = alloc_color(xw, "#1c1c1c", BlackPixel(xw->dpy, xw->screen));
    xw->bar_bg = alloc_color(xw, "#303030", xw->bg);
    xw->dim    = alloc_color(xw, "#a0a0a0", xw->fg);
    xw->bar_h  = xw->line_h + 6;

    xw->width  = width;
    xw->height = height;
    xw->win = XCreateSimpleWindow(xw->dpy, RootWindow(xw->dpy, xw->screen),
                                  0, 0, (unsigned)width, (unsigned)height, 0,
                                  xw->fg, xw->bg);

    XStoreName(xw->dpy, xw->win, title);
    XClassHint *class_hint = XAllocClassHint();
    if (class_hint) {
        class_hint->res_name  = (char *)"terminator";
        class_hint->res_class = (char *)"Terminator";
        XSetClassHint(xw->dpy, xw->win, class_hint);
        XFree(class_hint);
    }

    XSelectInput(xw->dpy, xw->win,
                 ExposureMask | KeyPressMask | StructureNotifyMask |
                 ButtonPressMask | ButtonReleaseMask | Button1MotionMask);

    /* Ask the window manager to send a ClientMessage instead of killing the
     * connection when the user clicks the close button. */
    xw->wm_delete = XInternAtom(xw->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(xw->dpy, xw->win, &xw->wm_delete, 1);

    xw->gc = XCreateGC(xw->dpy, xw->win, 0, NULL);
    XSetFont(xw->dpy, xw->gc, xw->font->fid);

    if (create_back_buffer(xw) != 0) {
        fprintf(stderr, "terminator: cannot create back buffer\n");
        xwin_close(xw);
        return -1;
    }

    XMapWindow(xw->dpy, xw->win);
    XFlush(xw->dpy);
    return 0;
}

void xwin_close(XWin *xw)
{
    if (!xw->dpy)
        return;
    if (xw->back)
        XFreePixmap(xw->dpy, xw->back);
    if (xw->gc)
        XFreeGC(xw->dpy, xw->gc);
    if (xw->font)
        XFreeFont(xw->dpy, xw->font);
    if (xw->win)
        XDestroyWindow(xw->dpy, xw->win);
    XCloseDisplay(xw->dpy);
    memset(xw, 0, sizeof *xw);
}

int xwin_resize(XWin *xw, int width, int height)
{
    if (width == xw->width && height == xw->height)
        return 0;
    xw->width  = width;
    xw->height = height;
    XFreePixmap(xw->dpy, xw->back);
    xw->back = 0;
    if (create_back_buffer(xw) != 0)
        fprintf(stderr, "terminator: cannot recreate back buffer\n");
    return 1;
}

int xwin_cols(const XWin *xw)
{
    int c = (xw->width - 2 * XWIN_PADDING) / xw->char_w;
    return c > 0 ? c : 1;
}

int xwin_rows(const XWin *xw)
{
    int r = (xw->height - xw->bar_h - 2 * XWIN_PADDING) / xw->line_h;
    return r > 0 ? r : 1;
}

/* Top pixel of grid row `row`. */
static int row_y(const XWin *xw, int row)
{
    return xw->bar_h + XWIN_PADDING + row * xw->line_h;
}

void xwin_clear(XWin *xw)
{
    XSetForeground(xw->dpy, xw->gc, xw->bg);
    XFillRectangle(xw->dpy, xw->back, xw->gc, 0, 0,
                   (unsigned)xw->width, (unsigned)xw->height);
}

void xwin_draw_text(XWin *xw, int col, int row, const char *s, int len)
{
    if (len <= 0)
        return;
    XSetForeground(xw->dpy, xw->gc, xw->fg);
    XDrawString(xw->dpy, xw->back, xw->gc,
                XWIN_PADDING + col * xw->char_w,
                row_y(xw, row) + xw->ascent,
                s, len);
}

void xwin_draw_inverse(XWin *xw, int col, int row, const char *s, int len)
{
    if (len <= 0)
        return;
    XSetForeground(xw->dpy, xw->gc, xw->fg);
    XFillRectangle(xw->dpy, xw->back, xw->gc,
                   XWIN_PADDING + col * xw->char_w,
                   row_y(xw, row),
                   (unsigned)(len * xw->char_w), (unsigned)xw->line_h);
    XSetForeground(xw->dpy, xw->gc, xw->bg);
    XDrawString(xw->dpy, xw->back, xw->gc,
                XWIN_PADDING + col * xw->char_w,
                row_y(xw, row) + xw->ascent,
                s, len);
}

void xwin_cell_at(const XWin *xw, int x, int y, int *col, int *row)
{
    int c = (x - XWIN_PADDING) / xw->char_w;
    int r = (y - xw->bar_h - XWIN_PADDING) / xw->line_h;
    if (x < XWIN_PADDING)
        c = 0;
    if (y < xw->bar_h + XWIN_PADDING)
        r = 0;
    int cols = xwin_cols(xw), rows = xwin_rows(xw);
    *col = c < 0 ? 0 : c >= cols ? cols - 1 : c;
    *row = r < 0 ? 0 : r >= rows ? rows - 1 : r;
}

void xwin_draw_bar(XWin *xw)
{
    XSetForeground(xw->dpy, xw->gc, xw->bar_bg);
    XFillRectangle(xw->dpy, xw->back, xw->gc, 0, 0, (unsigned)xw->width,
                   (unsigned)(xw->bar_h - 1));
    XSetForeground(xw->dpy, xw->gc, xw->dim);
    XDrawLine(xw->dpy, xw->back, xw->gc, 0, xw->bar_h - 1, xw->width, xw->bar_h - 1);
}

void xwin_bar_text(XWin *xw, int col, const char *s, int len, int active)
{
    if (len <= 0)
        return;
    int x = XWIN_PADDING + col * xw->char_w;
    if (active) {
        XSetForeground(xw->dpy, xw->gc, xw->fg);
        XFillRectangle(xw->dpy, xw->back, xw->gc, x, 1,
                       (unsigned)(len * xw->char_w), (unsigned)(xw->bar_h - 3));
    }
    XSetForeground(xw->dpy, xw->gc, active ? xw->bg : xw->dim);
    XDrawString(xw->dpy, xw->back, xw->gc, x, 3 + xw->ascent, s, len);
}

int xwin_in_bar(const XWin *xw, int y)
{
    return y < xw->bar_h;
}

int xwin_bar_col(const XWin *xw, int x)
{
    return x < XWIN_PADDING ? -1 : (x - XWIN_PADDING) / xw->char_w;
}

void xwin_set_title(XWin *xw, const char *title)
{
    XStoreName(xw->dpy, xw->win, title);
}

void xwin_present(XWin *xw)
{
    XCopyArea(xw->dpy, xw->back, xw->win, xw->gc, 0, 0,
              (unsigned)xw->width, (unsigned)xw->height, 0, 0);
}
