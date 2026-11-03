/*
 * multiwatch.c - multiWatch: several commands run side by side, repeatedly,
 * with each one's output printed as it arrives.
 */
#include "multiwatch.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#include "builtins.h"
#include "expand.h"
#include "terminal.h"

/* The separator line of the specification's output format (52 dashes). */
static const char SEPARATOR[] = "----------------------------------------------------";

static const char USAGE[] =
    "terminator: multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\", ...]";

/* A block is printed early once this much output is waiting, and at most
 * this much is read from one pipe per wakeup, so one chatty command cannot
 * starve the others or the GUI. */
#define MAX_BLOCK_BYTES (64 * 1024)
#define READ_BUDGET     (64 * 1024)

/* Upper bound on bytes drained from a pipe after its command has exited, in
 * case something it started keeps the pipe open and keeps writing. */
#define MAX_DRAIN_BYTES (1 << 20)

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int multiwatch_active(const MultiWatch *mw)
{
    return mw->slots != NULL;
}

/* ---- The command list --------------------------------------------------- */

typedef struct {
    char **v;
    int    n;
} Items;

static void items_free(Items *it)
{
    for (int i = 0; i < it->n; i++)
        free(it->v[i]);
    free(it->v);
    it->v = NULL;
    it->n = 0;
}

static int items_push(Items *it, char *s)
{
    char **grown = realloc(it->v, (size_t)(it->n + 1) * sizeof *grown);
    if (!grown)
        return -1;
    it->v = grown;
    it->v[it->n++] = s;
    return 0;
}

static const char *skip_blanks(const char *s)
{
    for (;;) {
        if (*s == ' ' || *s == '\t' || *s == '\n')
            s++;
        else if (s[0] == '\\' && s[1] == '\n')
            s += 2;
        else
            return s;
    }
}

/* Reads one item at *sp into `out`: "..." (where \" \\ \$ \` lose their
 * backslash, as in the shell), '...' (literal), or a bare word ending at a
 * blank, `,` or `]`. Returns 0, -1 for an unterminated quote, -2 on OOM. */
static int read_item(const char **sp, StrBuf *out)
{
    const char *s = *sp;
    int rc = 0;
    if (*s == '"') {
        for (s++; *s != '"'; s++) {
            if (*s == '\0')
                return -1;
            if (s[0] == '\\' && s[1] != '\0' && strchr("\"\\$`", s[1]))
                s++;
            rc |= sb_putc(out, *s);
        }
        s++;
    } else if (*s == '\'') {
        const char *end = strchr(s + 1, '\'');
        if (!end)
            return -1;
        rc = sb_append(out, s + 1, (size_t)(end - s - 1));
        s = end + 1;
    } else {
        while (*s != '\0' && !strchr(" \t\n,]", *s))
            rc |= sb_putc(out, *s++);
    }
    *sp = s;
    return rc != 0 ? -2 : 0;
}

/* Splits the text after `multiWatch` into commands. The specification's form
 * is ["cmd1", "cmd2", ...]; items may also be single-quoted, and the brackets
 * may be left out (multiWatch "cmd1" "cmd2"). */
static int parse_items(const char *text, Items *it, char *err, size_t errsize)
{
    const char *s = skip_blanks(text);
    int bracket = *s == '[';
    if (bracket)
        s++;

    for (;;) {
        s = skip_blanks(s);
        if (!bracket && *s == '\0')
            break;

        StrBuf item;
        sb_init(&item);
        int rc = read_item(&s, &item);
        char *str = rc == 0 && item.len > 0 ? sb_take(&item) : NULL;
        sb_free(&item);
        if (rc == -1) {
            snprintf(err, errsize, "terminator: multiWatch: unterminated quote");
            return -1;
        }
        if (rc == -2 || (str && items_push(it, str) != 0)) {
            free(str);
            snprintf(err, errsize, "terminator: multiWatch: out of memory");
            return -1;
        }
        if (!str)   /* an empty item, or a missing one as in [] or [, */
            goto usage;

        s = skip_blanks(s);
        if (bracket) {
            if (*s == ',') {
                s++;
                continue;
            }
            if (*s == ']') {
                s++;
                break;
            }
            goto usage;
        }
        if (*s == ',')
            s++;
    }
    if (*skip_blanks(s) != '\0' || it->n == 0)
        goto usage;
    if (it->n > MULTIWATCH_MAX_COMMANDS) {
        snprintf(err, errsize, "terminator: multiWatch: too many commands (at most %d)",
                 MULTIWATCH_MAX_COMMANDS);
        return -1;
    }
    return 0;

usage:
    snprintf(err, errsize, "%s", USAGE);
    return -1;
}

/* Parser and expander messages start with "terminator: "; drop that so the
 * message can be shown after the command it is about. */
static const char *without_prefix(const char *msg)
{
    static const char prefix[] = "terminator: ";
    return strncmp(msg, prefix, sizeof prefix - 1) == 0 ? msg + sizeof prefix - 1 : msg;
}

/* Parses and expands s->command into s->pl. */
static int prepare_slot(WatchSlot *s, int status, char *err, size_t errsize)
{
    RawPipeline *tree = NULL;
    char msg[256];
    ParseResult r = parse_line(s->command, &tree, msg, sizeof msg);
    if (r == PARSE_INCOMPLETE)
        snprintf(msg, sizeof msg, "incomplete command");
    else if (r == PARSE_NO_MEMORY)
        snprintf(msg, sizeof msg, "out of memory");
    if (r != PARSE_OK)
        goto fail;

    if (tree->background) {
        raw_pipeline_free(tree);
        snprintf(msg, sizeof msg, "background jobs (&) are not supported in multiWatch");
        goto fail;
    }
    /* A substitution would have to run (asynchronously) before every run. */
    if (tree->nsubsts > 0) {
        raw_pipeline_free(tree);
        snprintf(msg, sizeof msg, "command substitution is not supported in multiWatch");
        goto fail;
    }
    ExpandContext ctx = { NULL, NULL, status, NULL, 0 };
    int rc = expand_pipeline(tree, &ctx, &s->pl, msg, sizeof msg);
    raw_pipeline_free(tree);
    if (rc != 0)
        goto fail;

    for (int i = 0; i < s->pl.ncmds; i++) {
        const Command *cmd = &s->pl.cmds[i];
        if (cmd->argc > 0 && is_builtin(cmd->argv[0])) {
            snprintf(msg, sizeof msg, "built-ins cannot be used in multiWatch");
            goto fail;
        }
    }
    if (s->pl.ncmds == 1 && s->pl.cmds[0].argc == 0 && s->pl.cmds[0].nredirs == 0) {
        snprintf(msg, sizeof msg, "empty command");
        goto fail;
    }
    return 0;

fail:
    snprintf(err, errsize, "terminator: multiWatch: \"%s\": %s", s->command, without_prefix(msg));
    return -1;
}

/* ---- Output -------------------------------------------------------------- */

static void print_block(struct Terminal *t, WatchSlot *s, const char *text, size_t len)
{
    terminal_printf(t, "\"%s\", %lld:", s->command, (long long)time(NULL));
    terminal_print(t, SEPARATOR);
    terminal_write_output(t, text, len);
    terminal_print(t, SEPARATOR);
    s->printed++;
    t->watch.blocks++;
}

static void flush_pending(struct Terminal *t, WatchSlot *s)
{
    print_block(t, s, sb_str(&s->pending), s->pending.len);
    sb_clear(&s->pending);
    s->pending_since = -1;
}

/* Prints a block early because a lot of output is waiting: up to the last
 * complete line, so no line is split between blocks (unless there is no
 * newline at all). The rest stays pending. */
static void flush_large(struct Terminal *t, WatchSlot *s)
{
    const char *data = sb_str(&s->pending);
    const char *nl = NULL;
    for (size_t i = s->pending.len; i > 0; i--) {
        if (data[i - 1] == '\n') {
            nl = data + i - 1;
            break;
        }
    }
    if (!nl || (size_t)(nl - data) + 1 == s->pending.len) {
        flush_pending(t, s);
        return;
    }
    size_t head = (size_t)(nl - data) + 1;
    print_block(t, s, data, head);
    memmove(s->pending.data, data + head, s->pending.len - head);
    s->pending.len -= head;
    s->pending.data[s->pending.len] = '\0';
}

/* Collects output for the next block; it is printed MULTIWATCH_GROUP_MS
 * after its first byte arrived, when the run ends, or once it gets large. */
static void add_output(struct Terminal *t, WatchSlot *s, const char *buf, size_t n,
                       long long now)
{
    if (sb_append(&s->pending, buf, n) != 0) {
        if (s->pending.len > 0)
            flush_pending(t, s);
        terminal_print(t, "terminator: multiWatch: out of memory, output dropped");
        return;
    }
    if (s->pending_since < 0)
        s->pending_since = now;
    if (s->pending.len >= MAX_BLOCK_BYTES)
        flush_large(t, s);
}

/* Reads what is available from the slot's pipe, up to `budget` bytes. */
static void slot_read(struct Terminal *t, WatchSlot *s, size_t budget, long long now)
{
    char buf[4096];
    size_t total = 0;
    while (s->job.out_fd >= 0 && total < budget) {
        ssize_t n = job_read(&s->job, buf, sizeof buf);
        if (n <= 0)
            break;
        add_output(t, s, buf, (size_t)n, now);
        total += (size_t)n;
    }
}

/* ---- Running ------------------------------------------------------------- */

/* The next run starts on the first 2-second step after this run's start
 * that is not before the time it finished. */
static void schedule_next(WatchSlot *s, long long finished)
{
    long long steps = (finished - s->started + MULTIWATCH_INTERVAL_MS - 1) / MULTIWATCH_INTERVAL_MS;
    if (steps < 1)
        steps = 1;
    s->next_start = s->started + steps * MULTIWATCH_INTERVAL_MS;
}

/* Starts a run. `when` is the time it was due, which the schedule is kept
 * on, so the small delay in waking up for it does not add up over runs. */
static void start_run(struct Terminal *t, WatchSlot *s, long long when)
{
    s->started = when;
    s->printed = 0;
    /* Its own process group, so Ctrl+C also stops what the command starts. */
    if (job_spawn(&s->job, &s->pl, JOB_OWN_PGRP, NULL, NULL) != 0) {
        char msg[256];
        int len = snprintf(msg, sizeof msg, "terminator: cannot run command: %s\n", strerror(errno));
        print_block(t, s, msg, (size_t)len < sizeof msg ? (size_t)len : sizeof msg - 1);
        schedule_next(s, when);
        return;
    }
    s->running = 1;
}

/* Once every child of the run is reaped and its pipe closed, prints what is
 * left (an empty block if the run printed nothing) and schedules the next. */
static void finish_run_if_done(struct Terminal *t, WatchSlot *s, long long now)
{
    if (!s->running || !job_done(&s->job) || s->job.out_fd >= 0)
        return;

    /* Like the shell, say why a command was killed by a signal. */
    int st = job_last_status(&s->job);
    if (WIFSIGNALED(st) && WTERMSIG(st) != SIGPIPE && WTERMSIG(st) != SIGINT) {
        const char *why = strsignal(WTERMSIG(st));
        if (s->pending.len > 0 && s->pending.data[s->pending.len - 1] != '\n')
            add_output(t, s, "\n", 1, now);
        add_output(t, s, why, strlen(why), now);
        add_output(t, s, "\n", 1, now);
    }
    job_free(&s->job);
    s->running = 0;
    if (s->pending.len > 0 || s->printed == 0)
        flush_pending(t, s);
    schedule_next(s, now);
}

int multiwatch_is_invocation(const RawPipeline *line)
{
    if (line->ncmds != 1 || line->cmds[0].nwords == 0)
        return 0;
    const Word *w = &line->cmds[0].words[0];
    return w->nparts == 1 && w->parts[0].type == PART_TEXT && !w->parts[0].quoted &&
           strcmp(w->parts[0].text, "multiWatch") == 0;
}

int multiwatch_start(struct Terminal *t, const RawPipeline *line, int *status)
{
    const RawCommand *cmd = &line->cmds[0];
    char err[512];
    if (cmd->nredirs > 0) {
        terminal_print(t, "terminator: multiWatch: redirections are not supported");
        *status = 1;
        return 0;
    }

    /* The items as typed: the words after `multiWatch`, one space apart. */
    StrBuf text;
    sb_init(&text);
    int rc = 0;
    for (int i = 1; i < cmd->nwords; i++) {
        if (i > 1)
            rc |= sb_putc(&text, ' ');
        rc |= sb_append(&text, cmd->words[i].source, strlen(cmd->words[i].source));
    }
    Items items = { NULL, 0 };
    if (rc != 0)
        snprintf(err, sizeof err, "terminator: multiWatch: out of memory");
    if (rc != 0 || parse_items(sb_str(&text), &items, err, sizeof err) != 0) {
        sb_free(&text);
        items_free(&items);
        terminal_print(t, err);
        *status = 2;
        return 0;
    }
    sb_free(&text);

    MultiWatch mw = { calloc((size_t)items.n, sizeof *mw.slots), 0, 0 };
    if (!mw.slots) {
        items_free(&items);
        terminal_print(t, "terminator: multiWatch: out of memory");
        *status = 1;
        return 0;
    }
    for (int i = 0; i < items.n; i++) {
        WatchSlot *s = &mw.slots[i];
        s->command = items.v[i];
        items.v[i] = NULL;
        pipeline_init(&s->pl);
        s->job.out_fd = -1;
        s->job.cap_fd = -1;
        s->job.in_fd  = -1;
        s->job.in_rd  = -1;
        sb_init(&s->pending);
        s->pending_since = -1;
        mw.nslots++;
    }
    items_free(&items);

    for (int i = 0; i < mw.nslots; i++) {
        if (prepare_slot(&mw.slots[i], t->last_status, err, sizeof err) != 0) {
            terminal_print(t, err);
            multiwatch_free(&mw);
            *status = 1;
            return 0;
        }
    }

    /* All commands start together. */
    t->watch = mw;
    long long now = now_ms();
    for (int i = 0; i < mw.nslots; i++)
        start_run(t, &t->watch.slots[i], now);
    return 1;
}

void multiwatch_interrupt(struct Terminal *t)
{
    MultiWatch *mw = &t->watch;
    /* Output that already arrived is still shown. */
    for (int i = 0; i < mw->nslots; i++)
        if (mw->slots[i].pending.len > 0)
            flush_pending(t, &mw->slots[i]);
    multiwatch_free(mw);
    terminal_print(t, "^C");
    t->last_status = 130;
}

int multiwatch_poll_fds(const MultiWatch *mw, struct pollfd *pfd, int max)
{
    int n = 0;
    for (int i = 0; i < mw->nslots && n < max; i++) {
        const WatchSlot *s = &mw->slots[i];
        if (s->running && s->job.out_fd >= 0) {
            pfd[n].fd = s->job.out_fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            n++;
        }
    }
    return n;
}

int multiwatch_on_fd(struct Terminal *t, int fd)
{
    MultiWatch *mw = &t->watch;
    unsigned long before = mw->blocks;
    for (int i = 0; i < mw->nslots; i++) {
        WatchSlot *s = &mw->slots[i];
        if (s->running && s->job.out_fd == fd) {
            long long now = now_ms();
            slot_read(t, s, READ_BUDGET, now);
            finish_run_if_done(t, s, now);
            break;
        }
    }
    return mw->blocks != before;
}

int multiwatch_on_child_exit(struct Terminal *t)
{
    MultiWatch *mw = &t->watch;
    unsigned long before = mw->blocks;
    long long now = now_ms();
    for (int i = 0; i < mw->nslots; i++) {
        WatchSlot *s = &mw->slots[i];
        if (!s->running)
            continue;
        job_try_reap(&s->job);
        if (!job_done(&s->job))
            continue;
        /* Everything the command wrote is in the pipe now. Read it, then
         * close the pipe even if something it started still holds it open. */
        slot_read(t, s, MAX_DRAIN_BYTES, now);
        job_close_output(&s->job);
        finish_run_if_done(t, s, now);
    }
    return mw->blocks != before;
}

int multiwatch_timeout_ms(const MultiWatch *mw)
{
    long long next = LLONG_MAX;
    for (int i = 0; i < mw->nslots; i++) {
        const WatchSlot *s = &mw->slots[i];
        if (!s->running && s->next_start < next)
            next = s->next_start;
        if (s->pending_since >= 0 && s->pending_since + MULTIWATCH_GROUP_MS < next)
            next = s->pending_since + MULTIWATCH_GROUP_MS;
    }
    if (next == LLONG_MAX)
        return -1;
    long long wait = next - now_ms();
    return wait < 0 ? 0 : wait > INT_MAX ? INT_MAX : (int)wait;
}

int multiwatch_on_timer(struct Terminal *t)
{
    MultiWatch *mw = &t->watch;
    unsigned long before = mw->blocks;
    long long now = now_ms();
    for (int i = 0; i < mw->nslots; i++) {
        WatchSlot *s = &mw->slots[i];
        if (s->pending_since >= 0 && now >= s->pending_since + MULTIWATCH_GROUP_MS)
            flush_pending(t, s);
        if (!s->running && now >= s->next_start)
            start_run(t, s, s->next_start);
    }
    return mw->blocks != before;
}

void multiwatch_free(MultiWatch *mw)
{
    for (int i = 0; i < mw->nslots; i++) {
        WatchSlot *s = &mw->slots[i];
        if (s->running)
            job_kill(&s->job);
        free(s->command);
        pipeline_free(&s->pl);
        sb_free(&s->pending);
    }
    free(mw->slots);
    mw->slots  = NULL;
    mw->nslots = 0;
}
