/*
 * xwin.h - Thin wrapper around the Xlib window, font and drawing resources.
 *
 * This module and xsel.c (selections) are the only ones that talk to the X
 * server directly.
 */
#ifndef TERMINATOR_XWIN_H
#define TERMINATOR_XWIN_H

#include <X11/Xlib.h>

/* Inner margin (pixels) between the window border and the text grid. */
#define XWIN_PADDING 6

typedef struct {
    Display     *dpy;
    int          screen;
    Window       win;
    Pixmap       back;        /* off-screen buffer, avoids flicker on redraw */
    GC           gc;
    XFontStruct *font;
    Atom         wm_delete;   /* WM_DELETE_WINDOW, sent when the user closes */
    unsigned long fg, bg;
    unsigned long bar_bg, dim; /* tab bar background, inactive tab text */
    int          width, height;
    int          char_w, line_h, ascent;
    int          bar_h;       /* pixels at the top taken by the tab bar */
} XWin;

/* Opens the display and creates/maps the window. Returns 0, or -1 on error
 * (a message is printed to stderr; partially created resources are freed). */
int  xwin_open(XWin *xw, const char *title, int width, int height);
void xwin_close(XWin *xw);

/* Called on ConfigureNotify; recreates the back buffer if the size changed.
 * Returns 1 if the size changed. */
int  xwin_resize(XWin *xw, int width, int height);

/* Text grid dimensions for the current window size (always >= 1). The grid
 * is the area below the tab bar; rows passed to the drawing functions count
 * from its top. */
int  xwin_cols(const XWin *xw);
int  xwin_rows(const XWin *xw);

/* Drawing happens on the back buffer; xwin_present() copies it on screen. */
void xwin_clear(XWin *xw);
void xwin_draw_text(XWin *xw, int col, int row, const char *s, int len);
/* Text in reverse video: the cursor and the mouse selection. */
void xwin_draw_inverse(XWin *xw, int col, int row, const char *s, int len);

/* The text cell under window pixel (x, y), clamped to the grid. */
void xwin_cell_at(const XWin *xw, int x, int y, int *col, int *row);

/* The tab bar: its background, text in column `col` (`active` in reverse
 * video, others dimmed), whether pixel row y is in it, and the column
 * under pixel x. */
void xwin_draw_bar(XWin *xw);
void xwin_bar_text(XWin *xw, int col, const char *s, int len, int active);
int  xwin_in_bar(const XWin *xw, int y);
int  xwin_bar_col(const XWin *xw, int x);

void xwin_set_title(XWin *xw, const char *title);
void xwin_present(XWin *xw);

#endif
