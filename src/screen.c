/*
 * screen.c - TermScreen snapshots: storage, encoding and drawing.
 */
#include "screen.h"

#include <stdlib.h>
#include <string.h>

void screen_init(TermScreen *s)
{
    memset(s, 0, sizeof *s);
    s->cur_row = -1;
}

void screen_free(TermScreen *s)
{
    free(s->cells);
    free(s->len);
    free(s->sel_from);
    free(s->sel_to);
    screen_init(s);
}

int screen_resize(TermScreen *s, int cols, int rows)
{
    if (cols < 1 || rows < 1 || cols > SCREEN_MAX_DIM || rows > SCREEN_MAX_DIM)
        return -1;
    if (cols != s->cols || rows != s->rows) {
        char *cells = malloc((size_t)cols * (size_t)rows);
        int *len  = calloc((size_t)rows, sizeof *len);
        int *from = calloc((size_t)rows, sizeof *from);
        int *to   = calloc((size_t)rows, sizeof *to);
        if (!cells || !len || !from || !to) {
            free(cells);
            free(len);
            free(from);
            free(to);
            return -1;
        }
        screen_free(s);
        s->cells = cells;
        s->len = len;
        s->sel_from = from;
        s->sel_to = to;
        s->cols = cols;
        s->rows = rows;
    }
    memset(s->len, 0, (size_t)rows * sizeof *s->len);
    memset(s->sel_from, 0, (size_t)rows * sizeof *s->sel_from);
    memset(s->sel_to, 0, (size_t)rows * sizeof *s->sel_to);
    s->cur_row = -1;
    s->cur_col = 0;
    s->note[0] = '\0';
    return 0;
}

static int put_int(StrBuf *out, int v)
{
    return sb_append(out, (const char *)&v, sizeof v);
}

int screen_encode(const TermScreen *s, StrBuf *out)
{
    int rc = 0;
    int note_len = (int)strlen(s->note);
    rc |= put_int(out, s->cols);
    rc |= put_int(out, s->rows);
    rc |= put_int(out, s->cur_row);
    rc |= put_int(out, s->cur_col);
    rc |= put_int(out, note_len);
    rc |= sb_append(out, s->note, (size_t)note_len);
    for (int r = 0; r < s->rows; r++) {
        rc |= put_int(out, s->len[r]);
        rc |= put_int(out, s->sel_from[r]);
        rc |= put_int(out, s->sel_to[r]);
        rc |= sb_append(out, s->cells + (size_t)r * (size_t)s->cols, (size_t)s->len[r]);
    }
    return rc;
}

/* Reads an int at *p, if there is one before `end`. */
static int get_int(const char **p, const char *end, int *v)
{
    if ((size_t)(end - *p) < sizeof *v)
        return -1;
    memcpy(v, *p, sizeof *v);
    *p += sizeof *v;
    return 0;
}

int screen_decode(TermScreen *s, const char *data, size_t len)
{
    const char *p = data, *end = data + len;
    int cols, rows, cur_row, cur_col, note_len;
    if (get_int(&p, end, &cols) || get_int(&p, end, &rows) || get_int(&p, end, &cur_row) ||
        get_int(&p, end, &cur_col) || get_int(&p, end, &note_len))
        return -1;
    if (note_len < 0 || (size_t)note_len >= sizeof s->note || end - p < note_len)
        return -1;
    if (screen_resize(s, cols, rows) != 0)
        return -1;
    memcpy(s->note, p, (size_t)note_len);
    s->note[note_len] = '\0';
    p += note_len;
    for (int r = 0; r < rows; r++) {
        int n, from, to;
        if (get_int(&p, end, &n) || get_int(&p, end, &from) || get_int(&p, end, &to))
            return -1;
        if (n < 0 || n > cols || end - p < n)
            return -1;
        memcpy(s->cells + (size_t)r * (size_t)cols, p, (size_t)n);
        p += n;
        s->len[r] = n;
        s->sel_from[r] = from < 0 ? 0 : from > n ? n : from;
        s->sel_to[r] = to < s->sel_from[r] ? s->sel_from[r] : to > n ? n : to;
    }
    s->cur_row = cur_row >= 0 && cur_row < rows ? cur_row : -1;
    s->cur_col = cur_col >= 0 && cur_col < cols ? cur_col : 0;
    return 0;
}

void screen_draw(const TermScreen *s, XWin *xw)
{
    /* The window may have been resized since this snapshot was taken. */
    int rows = s->rows < xwin_rows(xw) ? s->rows : xwin_rows(xw);
    int cols = xwin_cols(xw);
    for (int r = 0; r < rows; r++) {
        const char *row = s->cells + (size_t)r * (size_t)s->cols;
        int n = s->len[r] < cols ? s->len[r] : cols;
        xwin_draw_text(xw, 0, r, row, n);
        int from = s->sel_from[r], to = s->sel_to[r] < n ? s->sel_to[r] : n;
        if (from < to)
            xwin_draw_inverse(xw, from, r, row + from, to - from);
    }
    if (s->cur_row >= 0 && s->cur_row < rows && s->cur_col < cols) {
        const char *row = s->cells + (size_t)s->cur_row * (size_t)s->cols;
        char ch = s->cur_col < s->len[s->cur_row] ? row[s->cur_col] : ' ';
        xwin_draw_inverse(xw, s->cur_col, s->cur_row, &ch, 1);
    }
    int n = (int)strlen(s->note);
    if (n > 0 && n <= cols)
        xwin_draw_inverse(xw, cols - n, 0, s->note, n);
}
