/*
 * proto.h - Messages between the GUI and a tab, over a pair of pipes. Both
 * execution modes use them: to a tab thread (thread mode) or a tab process
 * (process mode) alike, so the two modes behave the same.
 *
 * A message is a header (type, payload length) and the payload. The GUI
 * never blocks on a tab: it queues what it sends and writes when the pipe
 * has room, and reads whatever has arrived. A tab writes its messages
 * whole (the GUI keeps reading, so this only waits for it to catch up).
 */
#ifndef TERMINATOR_PROTO_H
#define TERMINATOR_PROTO_H

#include <stddef.h>

#include "strbuf.h"

enum {
    /* GUI -> tab */
    MSG_KEY = 1,       /* TermKey */
    MSG_MOUSE,         /* TermMouse */
    MSG_PASTE,         /* the pasted text */
    MSG_RESIZE,        /* int cols, int rows */
    MSG_SHOW,          /* int: 1 the tab is shown, 0 hidden */
    MSG_PRINT,         /* a message to show in the tab */

    /* tab -> GUI */
    MSG_SCREEN = 64,   /* an encoded Screen (sent while shown) */
    MSG_STATUS,        /* int activity, then the tab's directory */
    MSG_OWN,           /* int which (TermSel), then text to put on it */
    MSG_PASTE_REQ,     /* int which (TermSel) to paste */
    MSG_KEYMAP,        /* the key bindings changed: reload the config */
    MSG_EXIT,          /* int exit code: the tab's shell has ended */
};

/* Longest payload accepted. */
#define MSG_MAX (16 << 20)

/* Writes a message, waiting until it is all written. Returns 0 or -1. */
int  proto_send(int fd, int type, const void *data, size_t len);

/* Queues a message in `out` / writes as much of `out` as the (non-blocking)
 * descriptor takes. proto_flush returns -1 if the reader is gone. */
int  proto_queue(StrBuf *out, int type, const void *data, size_t len);
int  proto_flush(int fd, StrBuf *out);

typedef struct {
    StrBuf buf;    /* bytes received, not parsed yet (from `pos`) */
    size_t pos;
} MsgReader;

void reader_init(MsgReader *r);
void reader_free(MsgReader *r);

/* Reads what is available from a non-blocking descriptor. Returns 0, or -1
 * at end of file (the writer is gone) or on error. */
int  proto_fill(int fd, MsgReader *r);

/* The next complete message, if there is one (returns 1). `data` stays
 * valid until the next proto_fill. Returns -1 for a malformed stream. */
int  proto_next(MsgReader *r, int *type, const char **data, size_t *len);

#endif
