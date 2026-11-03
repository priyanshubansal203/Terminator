/*
 * input.c - Growable input line buffer with a cursor.
 */
#include "input.h"

#include <stdlib.h>
#include <string.h>

#define INPUT_INITIAL_CAP 128

int input_init(InputLine *in)
{
    in->data = malloc(INPUT_INITIAL_CAP);
    if (!in->data)
        return -1;
    in->data[0] = '\0';
    in->len = 0;
    in->cap = INPUT_INITIAL_CAP;
    in->cursor = 0;
    return 0;
}

void input_free(InputLine *in)
{
    free(in->data);
    in->data = NULL;
    in->len = in->cap = in->cursor = 0;
}

/* Makes room for `extra` more bytes plus the NUL. */
static int reserve(InputLine *in, size_t extra)
{
    if (in->len + extra < in->cap)
        return 0;
    size_t new_cap = in->cap;
    while (in->len + extra >= new_cap)
        new_cap *= 2;
    char *grown = realloc(in->data, new_cap);
    if (!grown)
        return -1;
    in->data = grown;
    in->cap  = new_cap;
    return 0;
}

int input_insert_str(InputLine *in, const char *s, size_t n)
{
    if (reserve(in, n) != 0)
        return -1;
    memmove(in->data + in->cursor + n, in->data + in->cursor, in->len - in->cursor + 1);
    memcpy(in->data + in->cursor, s, n);
    in->len    += n;
    in->cursor += n;
    return 0;
}

int input_insert(InputLine *in, char c)
{
    return input_insert_str(in, &c, 1);
}

int input_delete_range(InputLine *in, size_t from, size_t to)
{
    if (to > in->len)
        to = in->len;
    if (from >= to)
        return 0;
    memmove(in->data + from, in->data + to, in->len - to + 1);
    in->len   -= to - from;
    in->cursor = from;
    return 1;
}

int input_backspace(InputLine *in)
{
    return in->cursor > 0 && input_delete_range(in, in->cursor - 1, in->cursor);
}

int input_delete(InputLine *in)
{
    size_t at = in->cursor;
    return input_delete_range(in, at, at + 1);
}

int input_set(InputLine *in, const char *s)
{
    size_t n = strlen(s);
    input_clear(in);
    return input_insert_str(in, s, n);
}

void input_clear(InputLine *in)
{
    in->len = 0;
    in->cursor = 0;
    in->data[0] = '\0';
}
