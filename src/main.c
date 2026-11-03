/*
 * main.c - Terminator entry point and the GUI's X11 event loop.
 *
 *     ./terminator                   thread mode (default): each tab is a
 *                                    thread of this process
 *     ./terminator useProcess=True   process mode: each tab is a process
 *                                    forked for it
 *
 * Either way this loop, the GUI, is the only part that talks to the X
 * server (tabs.h). It sleeps in poll() on the X connection and the pipes
 * from every tab (and in process mode the SIGCHLD wakeup that reports tab
 * processes ending). It never waits for a tab, so the window stays
 * responsive while any tab is busy, and it uses no CPU while idle.
 *
 * Keys and mouse events over the text are translated here (keymap.h) and
 * sent to the shown tab; the tab answers with what it shows. Tab keys (new,
 * close, switch) and clicks on the tab bar are handled here. Copy and paste
 * go through X selections (xsel.h): a tab asks, this loop does it, and
 * pasted text is sent to the tab that asked.
 */
#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "keymap.h"
#include "process.h"
#include "proto.h"
#include "tabs.h"
#include "termkey.h"
#include "terminal.h"
#include "xsel.h"
#include "xwin.h"

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 500

/* Set by SIGINT/SIGTERM delivered to Terminator itself (e.g. from the
 * launching console), so we can still exit cleanly. */
static volatile sig_atomic_t g_quit = 0;

static void on_quit_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: poll() returns EINTR so the loop notices g_quit. */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Terminator itself must never stop: if it was started from a console,
     * that console's Ctrl+Z or a background tty access must not suspend it.
     * A tab that has gone must not kill it with SIGPIPE either. Commands
     * get the default dispositions back (process.c). */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
}

typedef struct {
    XWin  xw;
    XSel  xs;
    Tabs  tabs;
    int   dirty;        /* the window needs redrawing */
    int   exit_code;    /* Terminator's, once the last tab has closed */
    int   paste_tab;    /* id of the tab a pending paste is for, or 0 */
    Time  last_time;    /* of the latest user input, for selections (ICCCM) */
    char  title[512];   /* the window title last set */
} App;

/* ---- Tabs -------------------------------------------------------------------- */

static void open_tab(App *a)
{
    /* A new tab starts in the shown tab's directory. */
    Tab *cur = tabs_current(&a->tabs);
    char here[PATH_MAX];
    const char *dir = cur && cur->dir ? cur->dir : getcwd(here, sizeof here) ? here : "/";
    if (!tabs_open(&a->tabs, dir, NULL, xwin_cols(&a->xw), xwin_rows(&a->xw)) && cur) {
        char msg[128];
        int n = snprintf(msg, sizeof msg, a->tabs.n >= TABS_MAX
                         ? "terminator: too many tabs (at most %d)"
                         : "terminator: cannot open a tab", TABS_MAX);
        tab_send(cur, MSG_PRINT, msg, (size_t)n);
    }
    a->dirty = 1;
}

static void close_tab(App *a, int i, int exit_code)
{
    a->exit_code = exit_code;
    tabs_close(&a->tabs, i);
    a->dirty = 1;
}

static void select_tab(App *a, int i)
{
    int n = a->tabs.n;
    if (n == 0)
        return;
    tabs_select(&a->tabs, ((i % n) + n) % n);
    a->dirty = 1;
}

/* Keeps the window title on the shown tab's directory. */
static void update_title(App *a)
{
    Tab *cur = tabs_current(&a->tabs);
    char title[sizeof a->title], label[400];
    if (!cur)
        return;
    tab_label(&a->tabs, cur, label, sizeof label);
    snprintf(title, sizeof title, "Terminator - %s", label);
    if (strcmp(title, a->title) != 0) {
        snprintf(a->title, sizeof a->title, "%s", title);
        xwin_set_title(&a->xw, title);
    }
}

/* ---- Messages from tabs -------------------------------------------------------- */

static const XSelWhich WHICH[TERM_SEL_COUNT] = {
    [TERM_SEL_PRIMARY] = XSEL_PRIMARY, [TERM_SEL_CLIPBOARD] = XSEL_CLIPBOARD,
};

static void paste_into(Tab *tab, char *text, size_t len)
{
    if (tab)
        tab_send(tab, MSG_PASTE, text, len);
    free(text);
}

static void tab_message(App *a, Tab *tab, int type, const char *data, size_t len)
{
    int which = -1;
    if (len >= sizeof which)
        memcpy(&which, data, sizeof which);
    switch (type) {
    case MSG_SCREEN:
        if (screen_decode(&tab->screen, data, len) == 0 && tab == tabs_current(&a->tabs))
            a->dirty = 1;
        break;
    case MSG_STATUS: {
        if (len < sizeof which)
            break;
        char *dir = strndup(data + sizeof which, len - sizeof which);
        if (dir) {
            free(tab->dir);
            tab->dir = dir;
        }
        if (which && tab != tabs_current(&a->tabs))
            tab->activity = 1;
        a->dirty = 1;   /* the tab bar */
        break;
    }
    case MSG_OWN:
        if (which >= 0 && which < TERM_SEL_COUNT) {
            const char *text = data + sizeof which;
            size_t n = len - sizeof which;
            if (xsel_own(&a->xs, &a->xw, WHICH[which], text, n, a->last_time) != 0)
                fprintf(stderr, "terminator: could not take the selection\n");
        }
        break;
    case MSG_PASTE_REQ:
        if (which >= 0 && which < TERM_SEL_COUNT) {
            char *text;
            size_t n;
            if (xsel_request(&a->xs, &a->xw, WHICH[which], a->last_time, &text, &n))
                paste_into(tab, text, n);
            else
                a->paste_tab = tab->id;   /* the answer comes later */
        }
        break;
    case MSG_KEYMAP: {
        /* `bind` in a tab changed the bindings (and saved them). */
        StrBuf msgs;
        sb_init(&msgs);
        keymap_load(keymap_path(), &msgs);
        sb_free(&msgs);
        break;
    }
    case MSG_EXIT:
        tab->exited = 1;
        tab->exit_code = which;
        break;
    default:
        break;
    }
}

/* Reads and handles what the tab has sent. */
static void tab_readable(App *a, Tab *tab)
{
    if (proto_fill(tab->from_tab, &tab->in) != 0)
        tab->gone = 1;
    int type, r;
    const char *data;
    size_t len;
    while ((r = proto_next(&tab->in, &type, &data, &len)) == 1)
        tab_message(a, tab, type, data, len);
    if (r < 0)
        tab->gone = 1;
}

/* Tabs whose shell has ended (exit, Ctrl+D) or whose thread or process is
 * gone (it crashed) close; the others are told about a crash. */
static void close_finished_tabs(App *a)
{
    for (int i = a->tabs.n - 1; i >= 0; i--) {
        Tab *tab = a->tabs.v[i];
        if (!tab->gone && !tab->exited)
            continue;
        int crashed = !tab->exited;
        int number = i + 1;
        close_tab(a, i, tab->exited ? tab->exit_code : 1);
        Tab *cur = tabs_current(&a->tabs);
        if (crashed && cur) {
            char msg[96];
            int n = snprintf(msg, sizeof msg, "terminator: tab %d ended unexpectedly", number);
            tab_send(cur, MSG_PRINT, msg, (size_t)n);
        }
    }
}

/* ---- X events ------------------------------------------------------------------- */

/* A key for the GUI rather than the shown tab: the tab actions, and
 * Alt+1 ... Alt+9 for tab N (unless bound to something else). Returns 1 if
 * it was one. */
static int tab_key(App *a, XKeyEvent *key, int action)
{
    switch (action) {
    case ACT_NEW_TAB:
        open_tab(a);
        return 1;
    case ACT_CLOSE_TAB:
        close_tab(a, a->tabs.current, 0);
        return 1;
    case ACT_NEXT_TAB:
        select_tab(a, a->tabs.current + 1);
        return 1;
    case ACT_PREV_TAB:
        select_tab(a, a->tabs.current - 1);
        return 1;
    case ACT_NONE: {
        KeySym sym = XLookupKeysym(key, 0);
        if ((key->state & Mod1Mask) && !(key->state & ControlMask) &&
            sym >= XK_1 && sym <= XK_9) {
            if ((int)(sym - XK_1) < a->tabs.n)
                select_tab(a, (int)(sym - XK_1));
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* A press on the tab bar: click a tab to show it, [ + ] for a new one,
 * middle-click a tab to close it, the wheel to go through them. */
static void bar_press(App *a, const XButtonEvent *b)
{
    int hit = tabs_bar_hit(&a->tabs, &a->xw, b->x);
    switch (b->button) {
    case Button1:
        if (hit >= 0)
            select_tab(a, hit);
        else if (hit == TABS_HIT_NEW)
            open_tab(a);
        else if (hit == TABS_HIT_LEFT)
            select_tab(a, a->tabs.current - 1);
        else if (hit == TABS_HIT_RIGHT)
            select_tab(a, a->tabs.current + 1);
        break;
    case Button2:
        if (hit >= 0)
            close_tab(a, hit, 0);
        break;
    case Button4:
        select_tab(a, a->tabs.current - 1);
        break;
    case Button5:
        select_tab(a, a->tabs.current + 1);
        break;
    default:
        break;
    }
}

/* A mouse event over the text area, for the shown tab, in text cells. */
static void send_mouse(App *a, int type, int button, int x, int y)
{
    TermMouse m = { type, button, 0, 0 };
    xwin_cell_at(&a->xw, x, y, &m.col, &m.row);
    tab_send(tabs_current(&a->tabs), MSG_MOUSE, &m, sizeof m);
}

/* Handles one X event. Returns 0 when the window should close. */
static int handle_event(App *a, XEvent *ev)
{
    Tab *cur = tabs_current(&a->tabs);
    switch (ev->type) {
    case ButtonPress:
        a->last_time = ev->xbutton.time;
        if (xwin_in_bar(&a->xw, ev->xbutton.y))
            bar_press(a, &ev->xbutton);
        else
            send_mouse(a, TERM_MOUSE_PRESS, (int)ev->xbutton.button, ev->xbutton.x, ev->xbutton.y);
        break;
    case ButtonRelease:
        a->last_time = ev->xbutton.time;
        send_mouse(a, TERM_MOUSE_RELEASE, (int)ev->xbutton.button, ev->xbutton.x, ev->xbutton.y);
        break;
    case MotionNotify:
        /* Only the latest position of a drag matters. */
        while (XCheckTypedWindowEvent(a->xw.dpy, a->xw.win, MotionNotify, ev))
            ;
        send_mouse(a, TERM_MOUSE_MOTION, 0, ev->xmotion.x, ev->xmotion.y);
        break;
    case SelectionRequest:
        xsel_on_request(&a->xs, &a->xw, &ev->xselectionrequest);
        break;
    case SelectionClear:
        xsel_on_clear(&a->xs, &ev->xselectionclear);
        break;
    case SelectionNotify: {
        char *text;
        size_t len;
        if (xsel_on_notify(&a->xs, &a->xw, &ev->xselection, &text, &len))
            paste_into(tabs_find(&a->tabs, a->paste_tab), text, len);
        break;
    }
    case Expose:
        if (ev->xexpose.count == 0)
            a->dirty = 1;
        break;
    case ConfigureNotify:
        if (xwin_resize(&a->xw, ev->xconfigure.width, ev->xconfigure.height)) {
            for (int i = 0; i < a->tabs.n; i++)
                tab_send_ints(a->tabs.v[i], MSG_RESIZE, xwin_cols(&a->xw), xwin_rows(&a->xw), 2);
            a->dirty = 1;
        }
        break;
    case KeyPress: {
        a->last_time = ev->xkey.time;
        TermKey key;
        keymap_translate(&ev->xkey, &key);
        if (!tab_key(a, &ev->xkey, key.action))
            tab_send(cur, MSG_KEY, &key, sizeof key);
        break;
    }
    case MappingNotify:
        XRefreshKeyboardMapping(&ev->xmapping);
        break;
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == a->xw.wm_delete)
            return 0;
        break;
    default:
        break;
    }
    return 1;
}

/* ---- Main loop -------------------------------------------------------------------- */

static void render(App *a)
{
    Tab *cur = tabs_current(&a->tabs);
    xwin_clear(&a->xw);
    screen_draw(&cur->screen, &a->xw);
    tabs_draw_bar(&a->tabs, &a->xw);
    xwin_present(&a->xw);
    update_title(a);
}

/* `useProcess=True` selects process mode; anything else is an error. */
static int parse_args(int argc, char **argv, int *process_mode)
{
    *process_mode = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = strncmp(argv[i], "useProcess=", 11) == 0 ? argv[i] + 11 : NULL;
        if (v && (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0))
            *process_mode = 1;
        else if (v && (strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0))
            *process_mode = 0;
        else {
            fprintf(stderr, "terminator: unknown argument `%s'\n"
                            "usage: %s [useProcess=True|False]\n", argv[i], argv[0]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    static App app;
    App *a = &app;
    int process_mode;
    if (parse_args(argc, argv, &process_mode) != 0)
        return 2;

    install_signal_handlers();
    /* Sort glob matches the way bash does in this locale. Only collation:
     * character handling stays in the C locale. */
    setlocale(LC_COLLATE, "");

    /* Key bindings from ~/.terminatorrc; problems are shown in the first
     * tab and the affected actions keep their defaults. (keymap_path()
     * reads the environment now, before any tab can change it.) */
    StrBuf msgs;
    sb_init(&msgs);
    keymap_load(keymap_path(), &msgs);

    if (xwin_open(&a->xw, "Terminator", WINDOW_WIDTH, WINDOW_HEIGHT) != 0) {
        sb_free(&msgs);
        return EXIT_FAILURE;
    }
    xsel_init(&a->xs, &a->xw);

    int wake_slot = -1, wake_fd = -1;
    if (process_init() != 0 || tabs_init(&a->tabs, process_mode) != 0) {
        perror("terminator: cannot start");
        xsel_free(&a->xs);
        xwin_close(&a->xw);
        sb_free(&msgs);
        return EXIT_FAILURE;
    }
    /* Process mode: the tab processes are the GUI's children. */
    if (process_mode)
        wake_fd = process_sigchld_open(&wake_slot);

    char here[PATH_MAX];
    const char *dir = getcwd(here, sizeof here) ? here : "/";
    Tab *first_tab = tabs_open(&a->tabs, dir, getenv("OLDPWD"), xwin_cols(&a->xw), xwin_rows(&a->xw));
    if (!first_tab) {
        fprintf(stderr, "terminator: cannot start a tab\n");
        tabs_free(&a->tabs);
        xsel_free(&a->xs);
        xwin_close(&a->xw);
        sb_free(&msgs);
        return EXIT_FAILURE;
    }
    if (msgs.len > 0)
        tab_send(first_tab, MSG_PRINT, msgs.data, msgs.len);
    sb_free(&msgs);

    struct pollfd *pfd = NULL;
    int pfd_cap = 0;
    int status = EXIT_SUCCESS;
    a->dirty = 1;

    while (!g_quit) {
        /* Drain every event Xlib already has queued before sleeping. */
        int running = 1;
        while (running && XPending(a->xw.dpy) > 0) {
            XEvent ev;
            XNextEvent(a->xw.dpy, &ev);
            running = handle_event(a, &ev);
        }
        if (!running)
            break;
        close_finished_tabs(a);
        if (a->tabs.n == 0) {
            status = a->exit_code;
            break;
        }
        if (a->dirty) {
            render(a);
            a->dirty = 0;
        }
        XFlush(a->xw.dpy);

        /* Block until there is an X event, a message from a tab, room in a
         * tab's pipe for what is queued for it, or a tab process ended. */
        int n = a->tabs.n, need = 2 + 2 * n;
        if (need > pfd_cap) {
            struct pollfd *grown = realloc(pfd, (size_t)need * sizeof *grown);
            if (!grown) {
                status = EXIT_FAILURE;
                break;
            }
            pfd = grown;
            pfd_cap = need;
        }
        pfd[0] = (struct pollfd){ .fd = ConnectionNumber(a->xw.dpy), .events = POLLIN };
        pfd[1] = (struct pollfd){ .fd = wake_fd, .events = POLLIN };
        for (int i = 0; i < n; i++) {
            Tab *tab = a->tabs.v[i];
            pfd[2 + 2 * i] = (struct pollfd){ .fd = tab->from_tab, .events = POLLIN };
            pfd[3 + 2 * i] = (struct pollfd){ .fd = tab->outbox.len > 0 ? tab->to_tab : -1,
                                              .events = POLLOUT };
        }
        if (poll(pfd, (nfds_t)need, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("terminator: poll");
            status = EXIT_FAILURE;
            break;
        }
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "terminator: lost connection to X server\n");
            status = EXIT_FAILURE;
            break;
        }
        if (pfd[1].revents & POLLIN) {
            process_sigchld_drain(wake_fd);
            tabs_reap(&a->tabs);
        }
        /* Tabs only close in close_finished_tabs(), so indices hold. */
        for (int i = 0; i < n; i++) {
            Tab *tab = a->tabs.v[i];
            if (pfd[2 + 2 * i].revents)
                tab_readable(a, tab);
            if (pfd[3 + 2 * i].revents)
                tab_flush(tab);
        }
        close_finished_tabs(a);
        if (a->tabs.n == 0) {
            status = a->exit_code;
            break;
        }
    }

    tabs_free(&a->tabs);   /* every tab kills its running commands and jobs */
    if (wake_slot >= 0)
        process_sigchld_close(wake_slot);
    free(pfd);
    process_cleanup();
    xsel_free(&a->xs);
    xwin_close(&a->xw);
    return status;
}
