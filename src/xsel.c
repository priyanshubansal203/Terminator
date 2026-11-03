/*
 * xsel.c - Owning and reading the PRIMARY and CLIPBOARD selections.
 */
#include "xsel.h"

#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>

void xsel_init(XSel *xs, XWin *xw)
{
    memset(xs, 0, sizeof *xs);
    xs->atoms[XSEL_PRIMARY]   = XA_PRIMARY;
    xs->atoms[XSEL_CLIPBOARD] = XInternAtom(xw->dpy, "CLIPBOARD", False);
    xs->utf8    = XInternAtom(xw->dpy, "UTF8_STRING", False);
    xs->targets = XInternAtom(xw->dpy, "TARGETS", False);
    xs->prop    = XInternAtom(xw->dpy, "TERMINATOR_SELECTION", False);
}

void xsel_free(XSel *xs)
{
    for (int i = 0; i < XSEL_COUNT; i++) {
        free(xs->text[i]);
        xs->text[i] = NULL;
        xs->len[i] = 0;
    }
}

static int which_of(const XSel *xs, Atom sel)
{
    for (int i = 0; i < XSEL_COUNT; i++)
        if (xs->atoms[i] == sel)
            return i;
    return -1;
}

static char *dup_bytes(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

int xsel_own(XSel *xs, XWin *xw, XSelWhich which, const char *text, size_t len, Time when)
{
    char *copy = dup_bytes(text, len);
    if (!copy)
        return -1;
    XSetSelectionOwner(xw->dpy, xs->atoms[which], xw->win, when);
    if (XGetSelectionOwner(xw->dpy, xs->atoms[which]) != xw->win) {
        free(copy);
        return -1;
    }
    free(xs->text[which]);
    xs->text[which] = copy;
    xs->len[which]  = len;
    return 0;
}

int xsel_request(XSel *xs, XWin *xw, XSelWhich which, Time when, char **out, size_t *len)
{
    if (xs->text[which] && XGetSelectionOwner(xw->dpy, xs->atoms[which]) == xw->win) {
        *out = dup_bytes(xs->text[which], xs->len[which]);
        *len = xs->len[which];
        return *out != NULL;
    }
    XConvertSelection(xw->dpy, xs->atoms[which], xs->utf8, xs->prop, xw->win, when);
    return 0;
}

void xsel_on_request(XSel *xs, XWin *xw, const XSelectionRequestEvent *req)
{
    XSelectionEvent reply;
    memset(&reply, 0, sizeof reply);
    reply.type      = SelectionNotify;
    reply.display   = req->display;
    reply.requestor = req->requestor;
    reply.selection = req->selection;
    reply.target    = req->target;
    reply.property  = None;   /* refusal, unless changed below */
    reply.time      = req->time;

    int w = which_of(xs, req->selection);
    /* Obsolete clients leave the property None and mean the target. */
    Atom prop = req->property != None ? req->property : req->target;
    if (w >= 0 && xs->text[w]) {
        if (req->target == xs->targets) {
            Atom list[] = { xs->targets, xs->utf8, XA_STRING };
            XChangeProperty(xw->dpy, req->requestor, prop, XA_ATOM, 32, PropModeReplace,
                            (const unsigned char *)list, (int)(sizeof list / sizeof list[0]));
            reply.property = prop;
        } else if (req->target == xs->utf8 || req->target == XA_STRING) {
            /* One request at most; larger text would need the INCR
             * protocol, which is not implemented. */
            long max = XExtendedMaxRequestSize(xw->dpy);
            if (max == 0)
                max = XMaxRequestSize(xw->dpy);
            size_t limit = (size_t)max * 4 - 256;
            size_t len = xs->len[w] < limit ? xs->len[w] : limit;
            XChangeProperty(xw->dpy, req->requestor, prop, req->target, 8, PropModeReplace,
                            (const unsigned char *)xs->text[w], (int)len);
            reply.property = prop;
        }
    }
    XSendEvent(xw->dpy, req->requestor, False, NoEventMask, (XEvent *)&reply);
}

void xsel_on_clear(XSel *xs, const XSelectionClearEvent *ev)
{
    int w = which_of(xs, ev->selection);
    if (w < 0)
        return;
    free(xs->text[w]);
    xs->text[w] = NULL;
    xs->len[w]  = 0;
}

int xsel_on_notify(XSel *xs, XWin *xw, const XSelectionEvent *ev, char **out, size_t *len)
{
    if (ev->property == None) {
        /* The owner cannot give UTF-8; older clients still offer STRING. */
        if (ev->target == xs->utf8)
            XConvertSelection(xw->dpy, ev->selection, XA_STRING, xs->prop, xw->win, ev->time);
        return 0;
    }

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(xw->dpy, xw->win, ev->property, 0, 0x1000000, True,
                           AnyPropertyType, &type, &format, &nitems, &after,
                           &data) != Success)
        return 0;
    int ok = data && format == 8 && (type == xs->utf8 || type == XA_STRING);
    if (ok) {
        *out = dup_bytes((const char *)data, nitems);
        *len = nitems;
        ok = *out != NULL;
    }
    /* Anything else (e.g. INCR for a very large selection) is ignored. */
    if (data)
        XFree(data);
    return ok;
}
