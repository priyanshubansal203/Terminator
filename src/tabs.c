/*
 * tabs.c - Starting, closing and switching tabs, talking to them, and the
 * tab bar.
 */
#include "tabs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "parser.h"
#include "process.h"
#include "tabhost.h"

/* Longest directory shown in a tab's label; longer ones keep their end. */
#define LABEL_MAX 28

/* How long closing Terminator waits for its tabs to end. */
#define SHUTDOWN_MS 3000

int tabs_init(Tabs *tabs, int process_mode)
{
    memset(tabs, 0, sizeof *tabs);
    tabs->process_mode = process_mode;
    tabs->home = strdup(home_dir());
    tabs->history = history_create(process_mode);
    tabhost_set_threaded(!process_mode);
    if (!tabs->history || !tabs->home)
        return -1;
    /* The history file: $TERMINATOR_HISTFILE if set (empty: none), else
     * ~/.terminator_history, as bash has ~/.bash_history. */
    char path[4096];
    const char *env = getenv("TERMINATOR_HISTFILE");
    if (env)
        snprintf(path, sizeof path, "%s", env);
    else
        snprintf(path, sizeof path, "%s/.terminator_history", tabs->home);
    if (history_set_file(tabs->history, path) != 0)
        fprintf(stderr, "terminator: cannot read %s: %s\n", path, strerror(errno));
    return 0;
}

static void tab_free(Tab *tab)
{
    if (tab->to_tab >= 0)
        close(tab->to_tab);
    if (tab->from_tab >= 0)
        close(tab->from_tab);
    sb_free(&tab->outbox);
    reader_free(&tab->in);
    screen_free(&tab->screen);
    free(tab->dir);
    free(tab);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void tabs_free(Tabs *tabs)
{
    /* Closing the pipes ends every tab: each kills its own jobs. */
    for (int i = 0; i < tabs->n; i++)
        tab_free(tabs->v[i]);
    free(tabs->v);
    tabs->v = NULL;
    tabs->n = 0;

    int all_ended;
    if (!tabs->process_mode) {
        all_ended = tabhost_wait_threads(SHUTDOWN_MS) == 0;
    } else {
        /* Wait for the tab processes (closed tabs too); any still there
         * after a while are killed. */
        for (int waited = 0; tabs->nprocs > 0 && waited < SHUTDOWN_MS; waited += 10) {
            tabs_reap(tabs);
            if (tabs->nprocs > 0)
                sleep_ms(10);
        }
        for (int i = 0; i < tabs->nprocs; i++)
            kill(tabs->procs[i], SIGKILL);
        for (int i = 0; i < tabs->nprocs; i++)
            waitpid(tabs->procs[i], NULL, 0);
        all_ended = 1;
    }
    free(tabs->procs);
    tabs->procs = NULL;
    tabs->nprocs = 0;
    /* Only once no tab can be using it. */
    if (all_ended)
        history_destroy(tabs->history);
    tabs->history = NULL;
    free(tabs->home);
    tabs->home = NULL;
}

Tab *tabs_current(const Tabs *tabs)
{
    return tabs->n > 0 ? tabs->v[tabs->current] : NULL;
}

Tab *tabs_find(const Tabs *tabs, int id)
{
    for (int i = 0; i < tabs->n; i++)
        if (tabs->v[i]->id == id)
            return tabs->v[i];
    return NULL;
}

void tab_flush(Tab *tab)
{
    if (tab->to_tab >= 0 && tab->outbox.len > 0 && proto_flush(tab->to_tab, &tab->outbox) != 0)
        sb_clear(&tab->outbox);   /* the tab is gone; its pipe says so too */
}

void tab_send(Tab *tab, int type, const void *data, size_t len)
{
    if (proto_queue(&tab->outbox, type, data, len) != 0)
        fprintf(stderr, "terminator: out of memory, message to a tab dropped\n");
    tab_flush(tab);
}

void tab_send_ints(Tab *tab, int type, int a, int b, int n)
{
    int v[2] = { a, b };
    tab_send(tab, type, v, (size_t)n * sizeof v[0]);
}

/* A pipe with both ends close-on-exec (commands must not inherit them). */
static int make_pipe(int p[2])
{
    if (pipe(p) != 0)
        return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

Tab *tabs_open(Tabs *tabs, const char *dir, const char *oldpwd, int cols, int rows)
{
    if (tabs->n >= TABS_MAX)
        return NULL;
    Tab **grown = realloc(tabs->v, (size_t)(tabs->n + 1) * sizeof *grown);
    if (!grown)
        return NULL;
    tabs->v = grown;
    Tab *tab = calloc(1, sizeof *tab);
    if (!tab)
        return NULL;
    sb_init(&tab->outbox);
    reader_init(&tab->in);
    screen_init(&tab->screen);
    tab->to_tab = tab->from_tab = -1;
    tab->dir = strdup(dir);

    /* to[]: GUI -> tab, from[]: tab -> GUI. The GUI never blocks on a tab;
     * the tab polls its input and writes its output whole. */
    int to[2], from[2];
    if (make_pipe(to) != 0) {
        tab_free(tab);
        return NULL;
    }
    if (make_pipe(from) != 0) {
        close(to[0]);
        close(to[1]);
        tab_free(tab);
        return NULL;
    }
    set_nonblock(to[0]);
    set_nonblock(to[1]);
    set_nonblock(from[0]);

    if (tabs->process_mode) {
        pid_t pid = fork();
        if (pid == 0) {
            /* The tab's process: everything of the GUI but the history
             * mapping and these two pipes is let go (the X connection
             * above all). Console Ctrl+C is the GUI's business. */
            process_after_fork();
            signal(SIGINT, SIG_IGN);
            signal(SIGTERM, SIG_DFL);
            int keep[2] = { to[0], from[1] };
            process_close_fds_except(keep, 2);
            tab_free(tab);   /* the GUI's record of this tab: not ours */
            tabhost_set_threaded(0);
            TabHost *h = tabhost_new(tabs->history, dir, oldpwd, to[0], from[1], cols, rows);
            _exit(h ? tabhost_run(h) & 0xff : 1);
        }
        close(to[0]);
        close(from[1]);
        if (pid < 0) {
            close(to[1]);
            close(from[0]);
            tab_free(tab);
            return NULL;
        }
        tab->pid = pid;
        pid_t *procs = realloc(tabs->procs, (size_t)(tabs->nprocs + 1) * sizeof *procs);
        if (procs) {
            tabs->procs = procs;
            tabs->procs[tabs->nprocs++] = pid;
        }
    } else {
        TabHost *h = tabhost_new(tabs->history, dir, oldpwd, to[0], from[1], cols, rows);
        if (!h || tabhost_start_thread(h) != 0) {
            if (h)
                tabhost_destroy(h);
            else {
                close(to[0]);
                close(from[1]);
            }
            close(to[1]);
            close(from[0]);
            tab_free(tab);
            return NULL;
        }
    }
    tab->to_tab = to[1];
    tab->from_tab = from[0];
    tab->id = ++tabs->next_id;

    tabs->v[tabs->n] = tab;
    tabs_select(tabs, tabs->n++);
    return tab;
}

void tabs_close(Tabs *tabs, int i)
{
    if (i < 0 || i >= tabs->n)
        return;
    tab_free(tabs->v[i]);
    memmove(&tabs->v[i], &tabs->v[i + 1], (size_t)(tabs->n - i - 1) * sizeof *tabs->v);
    tabs->n--;
    /* The tab after a closed one moves into its place, as in most
     * terminals; closing the last tab shows the one before. */
    int was_shown = tabs->current == i;
    if (tabs->current > i || tabs->current >= tabs->n)
        tabs->current = tabs->current > 0 ? tabs->current - 1 : 0;
    if (tabs->n > 0 && was_shown)
        tabs_select(tabs, tabs->current);
}

void tabs_select(Tabs *tabs, int i)
{
    if (i < 0 || i >= tabs->n)
        return;
    if (tabs->current != i && tabs->current < tabs->n)
        tab_send_ints(tabs->v[tabs->current], MSG_SHOW, 0, 0, 1);
    tabs->current = i;
    tabs->v[i]->activity = 0;
    tab_send_ints(tabs->v[i], MSG_SHOW, 1, 0, 1);
}

void tabs_reap(Tabs *tabs)
{
    for (int i = 0; i < tabs->nprocs; ) {
        if (waitpid(tabs->procs[i], NULL, WNOHANG) != 0) {   /* ended, or not ours */
            tabs->procs[i] = tabs->procs[--tabs->nprocs];
            continue;
        }
        i++;
    }
}

void tab_label(const Tabs *tabs, const Tab *tab, char *buf, size_t size)
{
    const char *path = tab->dir ? tab->dir : "?";
    const char *home = tabs->home ? tabs->home : "";
    size_t hl = strlen(home);
    if (hl > 1 && strncmp(path, home, hl) == 0 && (path[hl] == '/' || path[hl] == '\0'))
        snprintf(buf, size, "~%s", path + hl);
    else
        snprintf(buf, size, "%s", path);
}

/* ---- The tab bar ------------------------------------------------------------- */

typedef struct {
    int  hit;      /* tab index, or TABS_HIT_NEW / _LEFT / _RIGHT */
    int  col;
    int  len;
    char text[LABEL_MAX + 24];
} BarItem;

/* "[ 2: ~/src ]", the directory cut to `w` characters (keeping its end),
 * or "[ 2 ]" when w is 0. A '*' instead of the first space marks a hidden
 * tab that has printed something. */
static int tab_text(const Tabs *tabs, int i, int w, char *buf, size_t size)
{
    const Tab *tab = tabs->v[i];
    char mark = tab->activity ? '*' : ' ';
    if (w <= 0)
        return snprintf(buf, size, "[%c%d ]", mark, i + 1);
    char label[PATH_MAX + 2];
    tab_label(tabs, tab, label, sizeof label);
    size_t len = strlen(label);
    if (len > (size_t)w)
        return snprintf(buf, size, "[%c%d: ...%s ]", mark, i + 1, label + len - (size_t)(w - 3));
    return snprintf(buf, size, "[%c%d: %s ]", mark, i + 1, label);
}

static void add_item(BarItem *items, int *n, int hit, int *col, const char *text)
{
    BarItem *it = &items[(*n)++];
    it->hit = hit;
    it->col = *col;
    snprintf(it->text, sizeof it->text, "%s", text);
    it->len = (int)strlen(it->text);
    *col += it->len + 1;
}

/* Lays the bar out in `cols` columns: every tab with as much of its
 * directory as fits, then [ + ]. If even bare numbers do not fit, only the
 * tabs around the current one are shown, with < and > for the others.
 * `items` has room for n + 3. Returns how many items there are. */
static int bar_layout(const Tabs *tabs, int cols, BarItem *items)
{
    static const char PLUS[] = "[ + ]";
    char buf[LABEL_MAX + 24];
    int n = 0, col = 0;

    for (int w = LABEL_MAX; w >= 0; w--) {
        if (w > 0 && w < 6)   /* too short to be any use */
            continue;
        int total = (int)sizeof PLUS - 1;
        for (int i = 0; i < tabs->n; i++)
            total += tab_text(tabs, i, w, buf, sizeof buf) + 1;
        if (total > cols && w > 0)
            continue;
        if (total <= cols) {
            for (int i = 0; i < tabs->n; i++) {
                tab_text(tabs, i, w, buf, sizeof buf);
                add_item(items, &n, i, &col, buf);
            }
            add_item(items, &n, TABS_HIT_NEW, &col, PLUS);
            return n;
        }
    }

    /* A window of tabs around the current one. */
    int room = cols - ((int)sizeof PLUS - 1) - 5;   /* "< " ... " >" and spaces */
    int first = tabs->current, last = tabs->current;
    int used = tab_text(tabs, first, 0, buf, sizeof buf) + 1;
    for (int grew = 1; grew; ) {
        grew = 0;
        if (last + 1 < tabs->n) {
            int wd = tab_text(tabs, last + 1, 0, buf, sizeof buf) + 1;
            if (used + wd <= room) {
                used += wd;
                last++;
                grew = 1;
            }
        }
        if (first > 0) {
            int wd = tab_text(tabs, first - 1, 0, buf, sizeof buf) + 1;
            if (used + wd <= room) {
                used += wd;
                first--;
                grew = 1;
            }
        }
    }
    if (first > 0)
        add_item(items, &n, TABS_HIT_LEFT, &col, "<");
    for (int i = first; i <= last; i++) {
        tab_text(tabs, i, 0, buf, sizeof buf);
        add_item(items, &n, i, &col, buf);
    }
    if (last + 1 < tabs->n)
        add_item(items, &n, TABS_HIT_RIGHT, &col, ">");
    add_item(items, &n, TABS_HIT_NEW, &col, PLUS);
    return n;
}

void tabs_draw_bar(const Tabs *tabs, XWin *xw)
{
    xwin_draw_bar(xw);
    BarItem *items = malloc((size_t)(tabs->n + 3) * sizeof *items);
    if (!items)
        return;
    int n = bar_layout(tabs, xwin_cols(xw), items);
    for (int i = 0; i < n; i++)
        xwin_bar_text(xw, items[i].col, items[i].text, items[i].len,
                      items[i].hit == tabs->current);
    free(items);
}

int tabs_bar_hit(const Tabs *tabs, const XWin *xw, int x)
{
    int col = xwin_bar_col(xw, x);
    BarItem *items = malloc((size_t)(tabs->n + 3) * sizeof *items);
    if (!items || col < 0) {
        free(items);
        return TABS_HIT_NONE;
    }
    int n = bar_layout(tabs, xwin_cols(xw), items);
    int hit = TABS_HIT_NONE;
    for (int i = 0; i < n; i++)
        if (col >= items[i].col && col < items[i].col + items[i].len)
            hit = items[i].hit;
    free(items);
    return hit;
}
