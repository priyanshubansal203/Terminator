/*
 * screen.h - A snapshot of what a tab shows: the rows of its text area, the
 * cursor, the selected (reverse-video) part of each row, and the "more lines
 * below" note while scrolled back. A tab produces it (terminal_snapshot) and
 * the GUI draws it; between a tab in another thread or process and the GUI
 * it travels in the encoded form below.
 */
#ifndef TERMINATOR_SCREEN_H
#define TERMINATOR_SCREEN_H

#include <stddef.h>

#include "strbuf.h"
#include "xwin.h"

/* Larger grids are refused when decoding. */
#define SCREEN_MAX_DIM 4096

typedef struct {
    int   cols, rows;
    int   cur_row, cur_col;   /* the cursor cell; cur_row -1 for none */
    char  note[96];           /* reverse video at the top right; "" for none */
    char *cells;              /* rows * cols characters */
    int  *len;                /* characters used in each row */
    int  *sel_from, *sel_to;  /* reverse-video columns [from, to) of each row */
} TermScreen;

void screen_init(TermScreen *s);
void screen_free(TermScreen *s);

/* Makes the grid cols x rows and empties it. Returns 0 or -1 on OOM. */
int  screen_resize(TermScreen *s, int cols, int rows);

/* Serialised form (both ends are the same program on the same machine).
 * screen_decode() returns 0, or -1 if the data is malformed. */
int  screen_encode(const TermScreen *s, StrBuf *out);
int  screen_decode(TermScreen *s, const char *data, size_t len);

/* Draws the snapshot into the window's text area (GUI only). */
void screen_draw(const TermScreen *s, XWin *xw);

#endif
