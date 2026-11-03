/*
 * strbuf.h - Growable byte string, used while building words, expanding them
 * and collecting command substitution output.
 */
#ifndef TERMINATOR_STRBUF_H
#define TERMINATOR_STRBUF_H

#include <stddef.h>

typedef struct {
    char   *data;  /* NUL-terminated once anything was added; NULL while empty */
    size_t  len;
    size_t  cap;
} StrBuf;

void  sb_init(StrBuf *sb);
void  sb_free(StrBuf *sb);
void  sb_clear(StrBuf *sb);

/* Return 0, or -1 on OOM (the buffer is then unchanged). */
int   sb_append(StrBuf *sb, const char *s, size_t n);
int   sb_putc(StrBuf *sb, char c);

/* Contents so far (never NULL). Valid until the next change. */
const char *sb_str(const StrBuf *sb);

/* Hands the contents over as a malloc'd string and empties the buffer.
 * Returns NULL only on OOM. */
char *sb_take(StrBuf *sb);

#endif
