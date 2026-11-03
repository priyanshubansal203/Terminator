/*
 * textbuf.c - Fixed-capacity ring buffer of text lines.
 */
#include "textbuf.h"

#include <stdlib.h>
#include <string.h>

void textbuf_init(TextBuffer *tb)
{
    tb->lines = NULL;
    tb->cap   = 0;
    tb->start = 0;
    tb->count = 0;
}

void textbuf_free(TextBuffer *tb)
{
    for (size_t i = 0; i < tb->count; i++)
        free(tb->lines[(tb->start + i) % tb->cap]);
    free(tb->lines);
    textbuf_init(tb);
}

void textbuf_clear(TextBuffer *tb)
{
    textbuf_free(tb);
}

int textbuf_append(TextBuffer *tb, const char *text, size_t len)
{
    char *copy = malloc(len + 1);
    if (!copy)
        return -1;
    memcpy(copy, text, len);
    copy[len] = '\0';

    /* Grow the ring until it reaches its maximum size. Growth only happens
     * while the ring has never wrapped, so `start` is still 0 here. */
    if (tb->count == tb->cap && tb->cap < TEXTBUF_MAX_LINES) {
        size_t new_cap = tb->cap ? tb->cap * 2 : 64;
        if (new_cap > TEXTBUF_MAX_LINES)
            new_cap = TEXTBUF_MAX_LINES;
        char **grown = realloc(tb->lines, new_cap * sizeof *grown);
        if (!grown) {
            free(copy);
            return -1;
        }
        tb->lines = grown;
        tb->cap   = new_cap;
    }

    if (tb->count == tb->cap) {
        /* Full: overwrite the oldest line. */
        free(tb->lines[tb->start]);
        tb->lines[tb->start] = copy;
        tb->start = (tb->start + 1) % tb->cap;
    } else {
        tb->lines[(tb->start + tb->count) % tb->cap] = copy;
        tb->count++;
    }
    return 0;
}

size_t textbuf_count(const TextBuffer *tb)
{
    return tb->count;
}

const char *textbuf_line(const TextBuffer *tb, size_t i)
{
    if (i >= tb->count)
        return NULL;
    return tb->lines[(tb->start + i) % tb->cap];
}
