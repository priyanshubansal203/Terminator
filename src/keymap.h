/*
 * keymap.h - Configurable key bindings for the line editor and job control.
 *
 * Every action has a list of keys (up to KEYMAP_MAX_KEYS). The defaults can be
 * changed in a config file, read at startup:
 *
 *     ~/.terminatorrc   (or the file named by $TERMINATORRC)
 *
 *     # action = key[, key...]      or      action = none
 *     beginning-of-line = Ctrl+A, Home
 *     copy  = Ctrl+Shift+C
 *     paste = Ctrl+Shift+V
 *
 * A key is any of Ctrl+, Alt+ and Shift+ followed by a key name: one
 * character (`Ctrl+A`, `Alt+.`) or an X keysym name (`Left`, `Home`,
 * `Delete`, `F5`, `Page_Up`; case does not matter). Letters are matched
 * whatever their case, so write `Ctrl+Shift+C`, not `Ctrl+C` with a capital,
 * to require Shift.
 *
 * A binding that is invalid (unknown action or key name, a printable key with
 * no Ctrl/Alt, which would stop it being typed, or a reserved key: Enter,
 * Backspace, Tab without Ctrl/Alt) is reported and that action keeps its
 * default. A key bound
 * to two actions is a conflict: it is reported and every action involved
 * that was customised goes back to its default (so swapping two keys works,
 * but stealing another action's default key does not).
 *
 * The `bind` built-in shows and changes the bindings at run time and writes
 * the change to the config file, so it is still in place after a restart.
 *
 * The bindings are shared by the GUI (which looks keys up) and, in thread
 * mode, by the tabs (whose `bind` changes them), so every function here
 * takes a lock.
 */
#ifndef TERMINATOR_KEYMAP_H
#define TERMINATOR_KEYMAP_H

#include <stddef.h>
#include <X11/Xlib.h>

#include "strbuf.h"
#include "termkey.h"

#define KEYMAP_MAX_KEYS 4

typedef enum {
    ACT_NONE = -1,
    ACT_BACKWARD_CHAR,
    ACT_FORWARD_CHAR,
    ACT_BEGINNING_OF_LINE,
    ACT_END_OF_LINE,
    ACT_PREVIOUS_HISTORY,
    ACT_NEXT_HISTORY,
    ACT_KILL_TO_START,      /* unix-line-discard */
    ACT_KILL_TO_END,        /* kill-line */
    ACT_KILL_WORD,          /* unix-word-rubout */
    ACT_DELETE_CHAR,
    ACT_CLEAR_SCREEN,
    ACT_EOF,
    ACT_COPY,
    ACT_PASTE,
    ACT_INTERRUPT,
    ACT_SUSPEND,
    ACT_SCROLL_PAGE_UP,     /* scrolling back through output: these six */
    ACT_SCROLL_PAGE_DOWN,   /* must stay together, in this order */
    ACT_SCROLL_LINE_UP,
    ACT_SCROLL_LINE_DOWN,
    ACT_SCROLL_TOP,
    ACT_SCROLL_BOTTOM,
    ACT_NEW_TAB,            /* tabs: handled by the GUI, not a terminal */
    ACT_CLOSE_TAB,
    ACT_NEXT_TAB,
    ACT_PREV_TAB,
    ACT_HISTORY_SEARCH,     /* Ctrl+R */
    ACT_COUNT
} KeyAction;

/* Restores every default binding. */
void        keymap_reset(void);

/* keymap_reset(), then applies the config file at `path` (a missing file is
 * fine). Each problem is appended to `msgs` as one line. Returns the number
 * of problems. */
int         keymap_load(const char *path, StrBuf *msgs);

/* The config file: $TERMINATORRC if set, else ~/.terminatorrc, as found
 * at the first call (later changes to the environment do not count). */
const char *keymap_path(void);

/* The action bound to a KeyPress, or ACT_NONE. */
KeyAction   keymap_lookup(XKeyEvent *ev);

/* Translates a KeyPress for a tab: its action, keysym, text and modifiers
 * (GUI only: it asks the X server's keyboard mapping). */
void        keymap_translate(XKeyEvent *ev, TermKey *key);

const char *keymap_action_name(KeyAction a);
const char *keymap_action_help(KeyAction a);
KeyAction   keymap_action_by_name(const char *name);

/* Writes the action's keys ("Ctrl+A, Home", or "none") into buf. */
void        keymap_describe(KeyAction a, char *buf, size_t size);
void        keymap_describe_default(KeyAction a, char *buf, size_t size);

/* Binds `a` to the comma-separated keys in `spec` ("none" for no key).
 * Fails, leaving the binding unchanged, if a key is invalid or bound to
 * another action. Returns 0, or -1 with a message in `err`. */
int         keymap_bind(KeyAction a, const char *spec, char *err, size_t errsize);

/* Restores the default for one action. Fails if a default key is now bound
 * to another action. Returns 0 or -1 with a message. */
int         keymap_bind_default(KeyAction a, char *err, size_t errsize);

/* Records the action's current binding in the config file (replacing its
 * earlier line, or removing it if the binding is the default); other lines
 * and comments are kept. Returns 0, or -1 with a message in `err`. */
int         keymap_save(const char *path, KeyAction a, char *err, size_t errsize);

#endif
