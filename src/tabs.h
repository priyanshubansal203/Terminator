/*
 * tabs.h - The GUI's side of the tabs, shown one at a time below a tab bar:
 *
 *     [ 1: ~/Downloads ] [ 2: /tmp ] [ + ]
 *
 * Each tab is an independent shell (tabhost.h) that runs on its own: in a
 * thread of this process (thread mode, the default) or in a process forked
 * for it (process mode, `./terminator useProcess=True`). The GUI is the only
 * part that touches X11. It talks to every tab through a pair of pipes:
 * keys, mouse events, pastes, the window size and whether the tab is shown
 * go one way; the tab's screen, directory, copy / paste requests and exit
 * come back. The GUI keeps the last screen of every tab and draws the shown
 * one; a hidden tab that printed something is marked in the bar
 * ("[*2: /tmp ]").
 *
 * Shared between tabs: the command history (history.h, locked) and the key
 * bindings. Everything else (scrollback, jobs, directory, environment) is
 * the tab's own; in process mode one tab crashing does not affect the
 * others, its tab just closes.
 */
#ifndef TERMINATOR_TABS_H
#define TERMINATOR_TABS_H

#include <sys/types.h>

#include "history.h"
#include "proto.h"
#include "screen.h"
#include "xwin.h"

#define TABS_MAX 32

typedef struct {
    int       id;          /* stable, for answers that arrive later (paste) */
    int       to_tab;      /* the GUI's ends of the pipes (non-blocking) */
    int       from_tab;
    StrBuf    outbox;      /* messages not written to the tab yet */
    MsgReader in;
    pid_t     pid;         /* process mode: the tab's process */
    TermScreen    screen;      /* what it showed last */
    char     *dir;         /* its directory, for the label */
    int       activity;    /* printed something while not shown */
    int       exited;      /* its shell ended (exit, Ctrl+D) ... */
    int       exit_code;   /* ... with this code */
    int       gone;        /* its end of the pipes is closed */
} Tab;

typedef struct {
    Tab     **v;
    int       n;
    int       current;     /* index of the shown tab */
    int       next_id;
    int       process_mode;
    History  *history;     /* shared by all tabs */
    pid_t    *procs;       /* process mode: tab processes not reaped yet */
    int       nprocs;
    char     *home;        /* $HOME, for labels (read once: tab threads
                            * change the environment) */
} Tabs;

/* Where a click on the tab bar landed. */
enum {
    TABS_HIT_NONE = -1,
    TABS_HIT_NEW  = -2,   /* [ + ] */
    TABS_HIT_LEFT = -3,   /* < : more tabs to the left */
    TABS_HIT_RIGHT = -4,  /* > : more to the right */
};

/* Returns 0, or -1 if the shared history cannot be created. */
int  tabs_init(Tabs *tabs, int process_mode);

/* Closes every tab and waits (a few seconds at most) for them to end. */
void tabs_free(Tabs *tabs);

/* Starts a tab in `dir` (OLDPWD `oldpwd`, NULL if unset) and shows it.
 * Returns it, or NULL (TABS_MAX reached, or it could not be started). */
Tab *tabs_open(Tabs *tabs, const char *dir, const char *oldpwd, int cols, int rows);

/* Closes tab `i`: it ends on its own (killing its jobs) once it sees its
 * pipe closed. Another tab is shown if it was. */
void tabs_close(Tabs *tabs, int i);

/* Shows tab `i`. */
void tabs_select(Tabs *tabs, int i);

Tab *tabs_current(const Tabs *tabs);
Tab *tabs_find(const Tabs *tabs, int id);

/* Queues a message for the tab (written as the pipe takes it). */
void tab_send(Tab *tab, int type, const void *data, size_t len);
void tab_send_ints(Tab *tab, int type, int a, int b, int n);

/* Writes what the pipe takes of the tab's queued messages. */
void tab_flush(Tab *tab);

/* Process mode: collects tab processes that have ended. */
void tabs_reap(Tabs *tabs);

/* The tab's directory as shown in the bar, with ~ for $HOME. */
void tab_label(const Tabs *tabs, const Tab *tab, char *buf, size_t size);

void tabs_draw_bar(const Tabs *tabs, XWin *xw);

/* What is at pixel column x of the bar: a tab index, or a TABS_HIT_ value. */
int  tabs_bar_hit(const Tabs *tabs, const XWin *xw, int x);

#endif
