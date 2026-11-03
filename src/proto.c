/*
 * proto.c - Framing of GUI <-> tab messages.
 */
#include "proto.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t type;
    uint32_t len;
} MsgHeader;

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int proto_send(int fd, int type, const void *data, size_t len)
{
    MsgHeader h = { (uint32_t)type, (uint32_t)len };
    if (write_all(fd, (const char *)&h, sizeof h) != 0)
        return -1;
    return len > 0 ? write_all(fd, data, len) : 0;
}

int proto_queue(StrBuf *out, int type, const void *data, size_t len)
{
    MsgHeader h = { (uint32_t)type, (uint32_t)len };
    if (sb_append(out, (const char *)&h, sizeof h) != 0)
        return -1;
    return len > 0 ? sb_append(out, data, len) : 0;
}

int proto_flush(int fd, StrBuf *out)
{
    size_t done = 0;
    while (done < out->len) {
        ssize_t w = write(fd, out->data + done, out->len - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (w <= 0)
            return -1;
        done += (size_t)w;
    }
    if (done > 0) {
        memmove(out->data, out->data + done, out->len - done);
        out->len -= done;
        out->data[out->len] = '\0';
    }
    return 0;
}

void reader_init(MsgReader *r)
{
    sb_init(&r->buf);
    r->pos = 0;
}

void reader_free(MsgReader *r)
{
    sb_free(&r->buf);
    r->pos = 0;
}

int proto_fill(int fd, MsgReader *r)
{
    /* Drop what has been parsed already. */
    if (r->pos > 0) {
        memmove(r->buf.data, r->buf.data + r->pos, r->buf.len - r->pos);
        r->buf.len -= r->pos;
        r->buf.data[r->buf.len] = '\0';
        r->pos = 0;
    }
    char chunk[65536];
    for (int rounds = 0; rounds < 16; rounds++) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n > 0) {
            if (sb_append(&r->buf, chunk, (size_t)n) != 0)
                return -1;
            continue;
        }
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return 0;
}

int proto_next(MsgReader *r, int *type, const char **data, size_t *len)
{
    size_t avail = r->buf.len - r->pos;
    if (avail < sizeof(MsgHeader))
        return 0;
    MsgHeader h;
    memcpy(&h, r->buf.data + r->pos, sizeof h);
    if (h.len > MSG_MAX)
        return -1;
    if (avail - sizeof h < h.len)
        return 0;
    *type = (int)h.type;
    *data = r->buf.data + r->pos + sizeof h;
    *len  = h.len;
    r->pos += sizeof h + h.len;
    return 1;
}
