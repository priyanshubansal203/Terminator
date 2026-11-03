/*
 * strbuf.c - Growable byte string.
 */
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

void sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb_init(sb);
}

void sb_clear(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

int sb_append(StrBuf *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 32;
        while (cap < sb->len + n + 1)
            cap *= 2;
        char *grown = realloc(sb->data, cap);
        if (!grown)
            return -1;
        sb->data = grown;
        sb->cap  = cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_putc(StrBuf *sb, char c)
{
    return sb_append(sb, &c, 1);
}

const char *sb_str(const StrBuf *sb)
{
    return sb->data ? sb->data : "";
}

char *sb_take(StrBuf *sb)
{
    char *s = sb->data;
    if (!s)
        s = calloc(1, 1);
    sb_init(sb);
    return s;
}
