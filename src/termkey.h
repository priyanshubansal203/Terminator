/*
 * termkey.h - Input as a tab's shell sees it. Only the GUI talks to X11, so
 * it translates KeyPress and mouse events into these plain structures, which
 * can be handed to a tab in another thread or process.
 */
#ifndef TERMINATOR_TERMKEY_H
#define TERMINATOR_TERMKEY_H

/* A key press. */
typedef struct {
    int           action;    /* the KeyAction bound to it (keymap.h), or ACT_NONE */
    unsigned long sym;       /* keysym with Shift etc. applied (XLookupString) */
    unsigned int  state;     /* modifier mask: ShiftMask, ControlMask, Mod1Mask ... */
    int           modifier;  /* a modifier key itself (Shift, Ctrl, ...) */
    int           len;       /* the characters it types */
    char          text[16];
} TermKey;

/* A mouse event over the text area, in cells of the tab's grid. Buttons are
 * X's: 1 left, 2 middle, 4 / 5 the wheel. */
enum {
    TERM_MOUSE_PRESS,
    TERM_MOUSE_RELEASE,
    TERM_MOUSE_MOTION,
};

typedef struct {
    int type;
    int button;
    int col, row;
} TermMouse;

#endif
