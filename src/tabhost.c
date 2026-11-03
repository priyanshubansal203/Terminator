/*
 * tabhost.c - A tab's event loop: Terminal + messages to and from the GUI.
 */
#include "tabhost.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "process.h"
#include "proto.h"
#include "screen.h"
#include "terminal.h"

/* Screens are sent at most this often while output keeps coming. */
#define SCREEN_INTERVAL_MS 16

struct TabHost {
    Terminal  term;
    int       cmd_fd, ev_fd;
    int       wake_fd, wake_slot;   /* SIGCHLD */
    MsgReader in;

    /* The tab's directory and environment (see tabhost.h). */
    int       cwd_fd;               /* -1 if it cannot be opened */
    char     *cwd;
    dev_t     dev;
    ino_t     ino;
    char     *pwd, *oldpwd;

    TermScreen    screen;
    StrBuf    encoded;
    int       visible;
    int       dirty;                /* changed since the last TermScreen sent */
    int       activity_sent;        /* reported output while hidden */
    char     *sent_dir;             /* directory last reported */
    long long last_screen;          /* when the last TermScreen was sent (ms) */
};

/* ---- Thread mode: one tab at a time -------------------------------------------- */

static int             g_threaded;
static pthread_mutex_t g_big = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_count_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_count_zero = PTHREAD_COND_INITIALIZER;
static int             g_threads;

void tabhost_set_threaded(int threaded)
{
    g_threaded = threaded;
}

static void big_lock(void)
{
    if (g_threaded)
        pthread_mutex_lock(&g_big);
}

static void big_unlock(void)
{
    if (g_threaded)
        pthread_mutex_unlock(&g_big);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- The tab's directory and environment --------------------------------------- */

static int same_str(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

static void record_env(const char *name, char **slot)
{
    const char *v = getenv(name);
    if (same_str(v, *slot))
        return;
    char *copy = v ? strdup(v) : NULL;
    if (v && !copy)
        return;
    free(*slot);
    *slot = copy;
}

static void apply_env(const char *name, const char *value)
{
    if (same_str(getenv(name), value))
        return;
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
}

/* Puts the process in the tab's directory and environment. */
static void tab_enter(TabHost *h)
{
    if (h->cwd_fd < 0 || fchdir(h->cwd_fd) != 0) {
        if (h->cwd && chdir(h->cwd) != 0)
            terminal_printf(&h->term, "terminator: cannot return to %s: %s",
                            h->cwd, strerror(errno));
    }
    apply_env("PWD", h->pwd);
    apply_env("OLDPWD", h->oldpwd);
}

/* Records the directory and environment the tab leaves the process in. */
static void tab_leave(TabHost *h)
{
    struct stat st;
    if (stat(".", &st) == 0 && (h->cwd_fd < 0 || st.st_dev != h->dev || st.st_ino != h->ino)) {
        if (h->cwd_fd >= 0)
            close(h->cwd_fd);
        /* A directory that may be entered but not read cannot be opened;
         * its path is then the way back. */
        h->cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        char path[PATH_MAX];
        if (getcwd(path, sizeof path)) {
            char *copy = strdup(path);
            if (copy) {
                free(h->cwd);
                h->cwd = copy;
            }
        }
        h->dev = st.st_dev;
        h->ino = st.st_ino;
    }
    record_env("PWD", &h->pwd);
    record_env("OLDPWD", &h->oldpwd);
}

/* ---- Lifetime -------------------------------------------------------------------- */

TabHost *tabhost_new(History *history, const char *dir, const char *oldpwd,
                     int cmd_fd, int ev_fd, int cols, int rows)
{
    TabHost *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->cmd_fd = cmd_fd;
    h->ev_fd = ev_fd;
    h->wake_fd = -1;
    h->wake_slot = -1;
    h->cwd_fd = -1;
    h->cwd = strdup(dir);
    h->pwd = strdup(dir);
    h->oldpwd = oldpwd ? strdup(oldpwd) : NULL;
    reader_init(&h->in);
    screen_init(&h->screen);
    sb_init(&h->encoded);
    if (!h->cwd || !h->pwd || (oldpwd && !h->oldpwd) ||
        terminal_init(&h->term, history) != 0) {
        free(h->cwd);
        free(h->pwd);
        free(h->oldpwd);
        free(h);
        return NULL;
    }
    terminal_resize(&h->term, cols, rows);
    return h;
}

void tabhost_destroy(TabHost *h)
{
    big_lock();
    terminal_free(&h->term);   /* kills and reaps the tab's jobs */
    big_unlock();
    if (h->wake_slot >= 0)
        process_sigchld_close(h->wake_slot);
    if (h->cwd_fd >= 0)
        close(h->cwd_fd);
    close(h->cmd_fd);
    close(h->ev_fd);
    reader_free(&h->in);
    screen_free(&h->screen);
    sb_free(&h->encoded);
    free(h->cwd);
    free(h->pwd);
    free(h->oldpwd);
    free(h->sent_dir);
    free(h);
}

/* ---- Messages from the GUI -------------------------------------------------------- */

static int get_int(const char *data, size_t len, size_t at, int *v)
{
    if (len < at + sizeof *v)
        return -1;
    memcpy(v, data + at, sizeof *v);
    return 0;
}

/* Handles one message; returns 1 if what the tab shows may have changed. */
static int handle_message(TabHost *h, int type, const char *data, size_t len)
{
    Terminal *t = &h->term;
    switch (type) {
    case MSG_KEY: {
        TermKey key;
        if (len != sizeof key)
            return 0;
        memcpy(&key, data, sizeof key);
        if (key.len < 0 || key.len >= (int)sizeof key.text)
            key.len = 0;
        return terminal_handle_key(t, &key);
    }
    case MSG_MOUSE: {
        TermMouse m;
        if (len != sizeof m)
            return 0;
        memcpy(&m, data, sizeof m);
        return terminal_mouse(t, &m);
    }
    case MSG_PASTE:
        return terminal_paste(t, data, len);
    case MSG_RESIZE: {
        int cols, rows;
        if (get_int(data, len, 0, &cols) || get_int(data, len, sizeof cols, &rows))
            return 0;
        terminal_resize(t, cols, rows);
        return 1;
    }
    case MSG_SHOW: {
        int v;
        if (get_int(data, len, 0, &v))
            return 0;
        h->visible = v;
        h->activity_sent = 0;
        return 1;
    }
    case MSG_PRINT: {
        char *text = malloc(len + 1);
        if (!text)
            return 0;
        memcpy(text, data, len);
        text[len] = '\0';
        terminal_print(t, text);
        free(text);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- Messages to the GUI ------------------------------------------------------------ */

static int send_with_int(TabHost *h, int type, int v, const char *text)
{
    size_t tl = text ? strlen(text) : 0;
    char *buf = malloc(sizeof v + tl);
    if (!buf)
        return -1;
    memcpy(buf, &v, sizeof v);
    if (tl)
        memcpy(buf + sizeof v, text, tl);
    int rc = proto_send(h->ev_fd, type, buf, sizeof v + tl);
    free(buf);
    return rc;
}

/* Tells the GUI what changed; `output` is whether the shell itself (not a
 * message from the GUI) changed what the tab shows. Returns the poll()
 * timeout this asks for (a TermScreen held back by the rate limit), or -1;
 * *gone set if the GUI is. */
static int send_updates(TabHost *h, int output, int *gone)
{
    Terminal *t = &h->term;
    int rc = 0;
    for (int i = 0; i < TERM_SEL_COUNT; i++) {
        if (!t->own_request[i])
            continue;
        rc |= send_with_int(h, MSG_OWN, i, t->own_request[i]);
        free(t->own_request[i]);
        t->own_request[i] = NULL;
    }
    if (t->paste_request) {
        rc |= send_with_int(h, MSG_PASTE_REQ, t->paste_request - 1, NULL);
        t->paste_request = 0;
    }
    if (t->keymap_changed) {
        rc |= proto_send(h->ev_fd, MSG_KEYMAP, NULL, 0);
        t->keymap_changed = 0;
    }

    /* Directory, and output while hidden (for the mark in the tab bar). */
    int activity = !h->visible && output && !h->activity_sent;
    if (activity || !same_str(h->sent_dir, h->cwd)) {
        rc |= send_with_int(h, MSG_STATUS, activity, h->cwd ? h->cwd : "?");
        free(h->sent_dir);
        h->sent_dir = h->cwd ? strdup(h->cwd) : NULL;
        if (activity)
            h->activity_sent = 1;
    }
    if (!h->visible)
        h->dirty = 0;   /* a full TermScreen goes when it is shown */

    int timeout = -1;
    if (h->visible && h->dirty) {
        long long wait = h->last_screen + SCREEN_INTERVAL_MS - now_ms();
        if (wait > 0) {
            timeout = (int)wait;
        } else if (terminal_snapshot(t, &h->screen) == 0) {
            sb_clear(&h->encoded);
            if (screen_encode(&h->screen, &h->encoded) == 0)
                rc |= proto_send(h->ev_fd, MSG_SCREEN, h->encoded.data, h->encoded.len);
            h->dirty = 0;
            h->last_screen = now_ms();
        }
    }
    *gone = rc != 0;
    return timeout;
}

/* ---- The loop ----------------------------------------------------------------------- */

int tabhost_run(TabHost *h)
{
    big_lock();
    h->wake_fd = process_sigchld_open(&h->wake_slot);
    tab_enter(h);
    tab_leave(h);
    big_unlock();
    if (h->wake_fd < 0)
        terminal_print(&h->term, "terminator: too many tabs to watch for child processes");

    struct pollfd *pfd = malloc((2 + TERMINAL_MAX_POLLFDS) * sizeof *pfd);
    int closing = pfd == NULL;
    int code = 0;
    int extra_timeout = -1;
    h->dirty = 1;

    while (!closing) {
        pfd[0] = (struct pollfd){ .fd = h->cmd_fd, .events = POLLIN };
        pfd[1] = (struct pollfd){ .fd = h->wake_fd, .events = POLLIN };
        int n = terminal_poll_fds(&h->term, pfd + 2, TERMINAL_MAX_POLLFDS);
        int timeout = terminal_timeout_ms(&h->term);
        if (extra_timeout >= 0 && (timeout < 0 || extra_timeout < timeout))
            timeout = extra_timeout;
        if (poll(pfd, (nfds_t)(2 + n), timeout) < 0 && errno != EINTR)
            break;

        big_lock();
        tab_enter(h);
        int dirty = 0, output = 0;
        if (pfd[0].revents) {
            if (proto_fill(h->cmd_fd, &h->in) != 0)
                closing = 1;   /* the GUI closed the tab, or is gone */
            int type, r;
            const char *data;
            size_t len;
            while ((r = proto_next(&h->in, &type, &data, &len)) == 1)
                dirty |= handle_message(h, type, data, len);
            if (r < 0)
                closing = 1;
        }
        output |= terminal_on_poll(&h->term, pfd + 2, n);
        if (pfd[1].revents & POLLIN) {
            process_sigchld_drain(h->wake_fd);
            output |= terminal_on_child_exit(&h->term);
        }
        if (terminal_timeout_ms(&h->term) == 0)
            output |= terminal_on_timer(&h->term);
        tab_leave(h);
        big_unlock();

        if (dirty || output)
            h->dirty = 1;
        int gone = 0;
        extra_timeout = send_updates(h, output, &gone);
        if (gone)
            closing = 1;
        if (h->term.quit_requested) {
            code = h->term.exit_code;
            send_with_int(h, MSG_EXIT, code, NULL);
            break;
        }
    }
    free(pfd);
    tabhost_destroy(h);
    return code;
}

static void *thread_main(void *arg)
{
    /* Signals for the whole program (SIGINT, SIGTERM) go to the GUI's
     * thread; SIGCHLD may arrive anywhere, its handler wakes every tab. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    tabhost_run(arg);

    pthread_mutex_lock(&g_count_lock);
    if (--g_threads == 0)
        pthread_cond_broadcast(&g_count_zero);
    pthread_mutex_unlock(&g_count_lock);
    return NULL;
}

int tabhost_start_thread(TabHost *h)
{
    pthread_mutex_lock(&g_count_lock);
    g_threads++;
    pthread_mutex_unlock(&g_count_lock);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, thread_main, h);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_count_lock);
        g_threads--;
        pthread_mutex_unlock(&g_count_lock);
        return -1;
    }
    return 0;
}

int tabhost_wait_threads(int ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_count_lock);
    int rc = 0;
    while (g_threads > 0 && rc == 0)
        rc = pthread_cond_timedwait(&g_count_zero, &g_count_lock, &until);
    int left = g_threads;
    pthread_mutex_unlock(&g_count_lock);
    return left == 0 ? 0 : -1;
}
