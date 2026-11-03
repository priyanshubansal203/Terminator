/*
 * xsel.h - X11 selections (PRIMARY and CLIPBOARD) for copy and paste.
 *
 * Copying makes Terminator the owner of a selection and keeps the text, which
 * is handed to any client that asks (SelectionRequest) as UTF8_STRING or
 * STRING. Pasting asks the owner to convert the selection into a property on
 * our window (XConvertSelection); the answer arrives later as a
 * SelectionNotify event, so the event loop never waits for another client.
 * When Terminator owns the selection itself the text is used directly.
 *
 * Like xwin.c, this module talks to the X server directly.
 */
#ifndef TERMINATOR_XSEL_H
#define TERMINATOR_XSEL_H

#include <stddef.h>
#include <X11/Xlib.h>

#include "xwin.h"

typedef enum {
    XSEL_PRIMARY,
    XSEL_CLIPBOARD,
    XSEL_COUNT
} XSelWhich;

typedef struct {
    Atom   atoms[XSEL_COUNT];   /* PRIMARY, CLIPBOARD */
    Atom   utf8, targets, prop;
    char  *text[XSEL_COUNT];    /* what we serve while we own the selection */
    size_t len[XSEL_COUNT];
} XSel;

void xsel_init(XSel *xs, XWin *xw);
void xsel_free(XSel *xs);

/* Takes ownership of the selection with a copy of `text`. Returns 0 or -1. */
int  xsel_own(XSel *xs, XWin *xw, XSelWhich which, const char *text, size_t len, Time when);

/* Starts a paste. If the text is available at once (we own the selection)
 * returns 1 with a malloc'd copy in *out; otherwise returns 0 and the text
 * arrives through xsel_on_notify(). */
int  xsel_request(XSel *xs, XWin *xw, XSelWhich which, Time when, char **out, size_t *len);

/* Event handlers. xsel_on_notify returns 1 with malloc'd pasted text. */
void xsel_on_request(XSel *xs, XWin *xw, const XSelectionRequestEvent *req);
void xsel_on_clear(XSel *xs, const XSelectionClearEvent *ev);
int  xsel_on_notify(XSel *xs, XWin *xw, const XSelectionEvent *ev, char **out, size_t *len);

#endif
