/*
 * input.h - An editable line of text with a cursor: the command line the user
 * is typing (and, appended to at the end, a job's unfinished output line).
 */
#ifndef TERMINATOR_INPUT_H
#define TERMINATOR_INPUT_H

#include <stddef.h>

typedef struct {
    char   *data;    /* always NUL-terminated */
    size_t  len;
    size_t  cap;
    size_t  cursor;  /* insertion point, 0..len */
} InputLine;

int         input_init(InputLine *in);
void        input_free(InputLine *in);

/* Inserts one byte / `n` bytes at the cursor and moves the cursor past them.
 * Return 0, or -1 on OOM (the line is then unchanged). */
int         input_insert(InputLine *in, char c);
int         input_insert_str(InputLine *in, const char *s, size_t n);

/* Removes the byte before the cursor. Returns 1 if something was removed. */
int         input_backspace(InputLine *in);

/* Removes the byte under the cursor. Returns 1 if something was removed. */
int         input_delete(InputLine *in);

/* Removes bytes [from, to) and puts the cursor at `from`. Returns 1 if
 * something was removed. */
int         input_delete_range(InputLine *in, size_t from, size_t to);

/* Replaces the whole line with `s`, cursor at the end. Returns 0 or -1. */
int         input_set(InputLine *in, const char *s);

void        input_clear(InputLine *in);

#endif
