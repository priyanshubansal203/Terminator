/*
 * textbuf.h - Scrollback buffer: an ordered list of completed output lines.
 *
 * The window is always redrawn from this buffer (plus the line being edited),
 * so later milestones only need to append text here (e.g. command output read
 * from a pipe) and request a redraw.
 */
#ifndef TERMINATOR_TEXTBUF_H
#define TERMINATOR_TEXTBUF_H

#include <stddef.h>

/* Oldest lines are discarded once this many are stored. */
#define TEXTBUF_MAX_LINES 5000

typedef struct {
    char  **lines;  /* ring of heap-allocated, NUL-terminated strings */
    size_t  cap;    /* allocated slots in `lines` */
    size_t  start;  /* index of the oldest line */
    size_t  count;  /* number of stored lines */
} TextBuffer;

void        textbuf_init(TextBuffer *tb);
void        textbuf_free(TextBuffer *tb);

/* Removes every line. */
void        textbuf_clear(TextBuffer *tb);

/* Appends a copy of `text` (length `len`). Returns 0 on success, -1 on OOM. */
int         textbuf_append(TextBuffer *tb, const char *text, size_t len);

size_t      textbuf_count(const TextBuffer *tb);

/* Returns line `i` (0 = oldest) or NULL if out of range. */
const char *textbuf_line(const TextBuffer *tb, size_t i);

#endif
