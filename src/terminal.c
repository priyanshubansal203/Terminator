/*
 * terminal.c - Keyboard handling (line editing, history, job-control keys),
 * line entry (including continuation lines), command output, the mouse
 * selection and rendering for a single terminal session. Running a
 * submitted line is eval.c's job; the job table is jobs.c's.
 */
#include "terminal.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <X11/keysym.h>

#include "keymap.h"
#include "parser.h"

/* Width of a tab stop when expanding tabs in command output. */
#define TAB_WIDTH 8

/* A line without a newline is committed once it reaches this length, so a
 * command printing one huge line cannot grow memory without bound. */
#define MAX_PENDING_LINE 4096

/* Upper bound on bytes drained from a pipe after its writer has exited, in
 * case a grandchild keeps the pipe open and keeps writing. */
#define MAX_DRAIN_BYTES (1 << 20)

/* Command substitution output beyond this aborts the command line, so
 * something like $(yes) cannot exhaust memory. */
#define MAX_CAPTURE_BYTES (16 << 20)

int terminal_init(Terminal *t, History *history)
{
    memset(t, 0, sizeof *t);
    textbuf_init(&t->scrollback);
    sb_init(&t->continued);
    sb_init(&t->captured);
    t->history = history;
    jobs_init(&t->jobs);
    source_init(&t->sources);
    t->prompt = TERMINAL_PROMPT;
    t->out_fd = -1;
    terminal_resize(t, 80, 24);   /* until the window reports its size */
    t->job.out_fd = -1;
    t->job.cap_fd = -1;
    t->job.in_fd  = -1;
    t->job.in_rd  = -1;
    sb_init(&t->typeahead_text);
    if (input_init(&t->input) != 0)
        return -1;
    if (input_init(&t->pending) != 0 || input_init(&t->job_input) != 0) {
        input_free(&t->input);
        input_free(&t->pending);
        return -1;
    }
    return 0;
}

void terminal_free(Terminal *t)
{
    if (t->job_active) {
        job_kill(&t->job);
        t->job_active = 0;
    }
    eval_free(&t->eval);
    multiwatch_free(&t->watch);
    jobs_free(&t->jobs);
    source_free(&t->sources);
    free(t->fg_cmd);
    free(t->hist_draft);
    free(t->search_saved_input);
    complete_free(&t->pick);
    free(t->sel_text);
    for (int i = 0; i < TERM_SEL_COUNT; i++)
        free(t->own_request[i]);
    textbuf_free(&t->scrollback);
    sb_free(&t->continued);
    sb_free(&t->captured);
    input_free(&t->input);
    input_free(&t->pending);
    input_free(&t->job_input);
    sb_free(&t->typeahead_text);
}

/* 1 while a command line, one of its substitutions, or multiWatch runs. */
static int line_running(const Terminal *t)
{
    return t->job_active || eval_busy(&t->eval) || multiwatch_active(&t->watch);
}

int terminal_busy(const Terminal *t)
{
    return line_running(t) || source_active(&t->sources);
}

static void write_line(int fd, const char *text)
{
    size_t len = strlen(text);
    char nl = '\n';
    while (len > 0) {
        ssize_t w = write(fd, text, len);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        text += w;
        len -= (size_t)w;
    }
    while (write(fd, &nl, 1) < 0 && errno == EINTR)
        ;
}

void terminal_out(Terminal *t, const char *text)
{
    if (t->out_fd >= 0) {
        write_line(t->out_fd, text);
    } else if (t->out_capture) {
        if (sb_append(&t->captured, text, strlen(text)) != 0 || sb_putc(&t->captured, '\n') != 0)
            fprintf(stderr, "terminator: out of memory, output dropped\n");
    } else {
        terminal_print(t, text);
    }
}

void terminal_outf(Terminal *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0)
        return;
    char *line = malloc((size_t)len + 1);
    if (!line)
        return;
    va_start(ap, fmt);
    vsnprintf(line, (size_t)len + 1, fmt, ap);
    va_end(ap);
    terminal_out(t, line);
    free(line);
}

void terminal_print(Terminal *t, const char *text)
{
    if (t->in_child) {
        write_line(STDERR_FILENO, text);
        return;
    }
    /* Messages can quote multi-line input (e.g. a bad ${...} typed over two
     * lines); each line gets its own scrollback entry. */
    for (;;) {
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : strlen(text);
        if (textbuf_append(&t->scrollback, text, len) != 0) {
            fprintf(stderr, "terminator: out of memory, output dropped\n");
            return;
        }
        t->lines_total++;
        if (!nl)
            return;
        text = nl + 1;
    }
}

void terminal_printf(Terminal *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0)
        return;

    char *line = malloc((size_t)len + 1);
    if (!line) {
        fprintf(stderr, "terminator: out of memory, output dropped\n");
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, (size_t)len + 1, fmt, ap);
    va_end(ap);
    terminal_print(t, line);
    free(line);
}

/* Appends `a` followed by `b` as a single scrollback line. */
static void print_joined(Terminal *t, const char *a, const char *b, size_t blen)
{
    size_t alen = strlen(a);
    char *line = malloc(alen + blen + 1);
    if (!line) {
        fprintf(stderr, "terminator: out of memory, output dropped\n");
        return;
    }
    memcpy(line, a, alen);
    memcpy(line + alen, b, blen);
    line[alen + blen] = '\0';
    terminal_print(t, line);
    free(line);
}

void terminal_flush_line(Terminal *t, InputLine *line)
{
    terminal_print(t, line->data);
    input_clear(line);
}

void terminal_feed(Terminal *t, InputLine *line, const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n') {
            terminal_flush_line(t, line);
            continue;
        }
        if (c == '\t') {
            do
                input_insert(line, ' ');
            while (line->len % TAB_WIDTH != 0);
        } else if (c >= 0x20 && c != 0x7f) {
            input_insert(line, (char)c);
        }
        if (line->len >= MAX_PENDING_LINE)
            terminal_flush_line(t, line);
    }
}

void terminal_write_output(Terminal *t, const char *buf, size_t n)
{
    terminal_feed(t, &t->pending, buf, n);
    if (t->pending.len > 0)
        terminal_flush_line(t, &t->pending);
}

/* Feeds keys typed during the last command back in, in order. If one of them
 * is Enter and starts another command, the rest are queued again. */
static void replay_typeahead(Terminal *t)
{
    int n = t->typeahead_len;
    if (n == 0)
        return;
    TermKey *keys = malloc((size_t)n * sizeof *keys);
    if (!keys) {
        t->typeahead_len = 0;
        return;
    }
    memcpy(keys, t->typeahead, (size_t)n * sizeof *keys);
    t->typeahead_len = 0;
    for (int i = 0; i < n; i++)
        terminal_handle_key(t, &keys[i]);
    free(keys);
}

/* ---- Input to the foreground job ------------------------------------------ */

static void terminal_submit(Terminal *t);
static int  kill_word(InputLine *in);
static int  key_complete(Terminal *t);

/* 1 if what the user types now goes to the foreground job's stdin: the
 * command itself is running (not a substitution) and its input is open. */
static int job_reads_input(const Terminal *t)
{
    return t->job_active && t->job.in_fd >= 0;
}

static void job_send(Terminal *t, const char *s, size_t n)
{
    if (job_write_input(&t->job, s, n) != (ssize_t)n)
        terminal_print(t, "terminator: input dropped: the command is not reading it");
}

/* Enter: the line is echoed into the output, as a terminal echoes it, and
 * sent with its newline. */
static void job_send_line(Terminal *t)
{
    InputLine *in = &t->job_input;
    in->cursor = in->len;
    if (input_insert(in, '\n') != 0)
        return;
    terminal_feed(t, &t->pending, in->data, in->len);
    job_send(t, in->data, in->len);
    input_clear(in);
}

/* When the job ends or stops, input it did not read (and the line being
 * typed) goes back to the shell, which reads it at the prompt, as with a
 * terminal. Ctrl+C / Ctrl+Z discard it instead (`keep` 0), as a terminal
 * flushes its input then. */
static void take_back_input(Terminal *t, int keep)
{
    char buf[4096];
    ssize_t n;
    while (t->job_active && (n = job_take_unread_input(&t->job, buf, sizeof buf)) > 0)
        if (keep)
            sb_append(&t->typeahead_text, buf, (size_t)n);
    if (keep && t->job_input.len > 0)
        sb_append(&t->typeahead_text, t->job_input.data, t->job_input.len);
    input_clear(&t->job_input);
}

/* Feeds text taken back from a job to the shell: complete lines run as
 * commands, the rest is left on the input line. If one of them starts a
 * command that reads input, what follows goes to it instead (it was echoed
 * when typed); if it starts one that does not, it waits for the prompt. */
static void replay_text(Terminal *t)
{
    if (t->typeahead_text.len == 0)
        return;
    char *text = sb_take(&t->typeahead_text);
    if (!text)
        return;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        if (terminal_busy(t)) {
            if (job_reads_input(t)) {
                size_t end = len;
                while (end > i && text[end - 1] != '\n')
                    end--;
                if (end > i)
                    job_send(t, text + i, end - i);
                input_insert_str(&t->job_input, text + end, len - end);
            } else {
                sb_append(&t->typeahead_text, text + i, len - i);
            }
            break;
        }
        unsigned char c = (unsigned char)text[i];
        if (c == '\n')
            terminal_submit(t);
        else if (c == '\t')
            key_complete(t);
        else if (c >= 0x20 && c != 0x7f)
            input_insert(&t->input, (char)c);
    }
    free(text);
}

/* The prompt is about to be shown again: report background jobs that
 * finished or stopped (as bash does before each prompt), then replay keys
 * typed meanwhile. */
static void back_to_prompt(Terminal *t)
{
    jobs_notify(t);
    replay_typeahead(t);
    replay_text(t);
}

/* Runs the commands of sourced scripts until one of them is left running
 * (its Job or multiWatch) or no script is left. */
static void run_scripts(Terminal *t)
{
    while (!line_running(t) && !t->quit_requested) {
        if (t->script_abort) {
            source_abort(t);
            t->script_abort = 0;
        }
        if (!source_active(&t->sources))
            break;
        source_step(t);
        /* Lines an earlier command of the script left unread are for the
         * next command that reads input. */
        if (job_reads_input(t))
            replay_text(t);
    }
}

/* A command line has finished: carry on with a sourced script, or return
 * to the prompt. */
static void line_finished(Terminal *t)
{
    run_scripts(t);
    if (!terminal_busy(t))
        back_to_prompt(t);
}

/* Once every child is reaped and both pipes are closed, hands the status to
 * eval.c, which either starts the next Job of the line (after a command
 * substitution) or records $? and returns to the prompt. */
static void finish_job_if_done(Terminal *t)
{
    if (!t->job_active || !job_done(&t->job) || t->job.out_fd >= 0 || t->job.cap_fd >= 0)
        return;
    if (t->pending.len > 0)
        terminal_flush_line(t, &t->pending);

    int st = job_last_status(&t->job);
    if (WIFSIGNALED(st) && WTERMSIG(st) != SIGINT && WTERMSIG(st) != SIGPIPE)
        terminal_printf(t, "%s", strsignal(WTERMSIG(st)));
    int status = job_exit_code(&t->job);
    take_back_input(t, 1);
    job_free(&t->job);
    t->job_active = 0;
    /* Ctrl+C during a command substitution abandons the whole line, as in
     * bash, instead of running the command with partial output. */
    if (t->interrupted && eval_in_subst(&t->eval))
        eval_abort(t, 130);
    else
        eval_job_finished(t, status);
    line_finished(t);
}

/* Adds command substitution output to t->captured, dropping NUL bytes as
 * bash does. Returns -1 (with a message printed) if it is too large. */
static int capture_append(Terminal *t, const char *buf, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t run = strnlen(buf + i, n - i);
        if (run > 0 && sb_append(&t->captured, buf + i, run) != 0) {
            terminal_print(t, "terminator: out of memory in command substitution");
            return -1;
        }
        i += run + 1;   /* skip the NUL (or step past the end) */
    }
    if (t->captured.len > MAX_CAPTURE_BYTES) {
        terminal_printf(t, "terminator: command substitution output exceeds %d MiB, "
                        "command abandoned", MAX_CAPTURE_BYTES >> 20);
        return -1;
    }
    return 0;
}

/* Gives up on the line after capture_append() failed. */
static void capture_failed(Terminal *t)
{
    if (t->pending.len > 0)
        terminal_flush_line(t, &t->pending);
    eval_abort(t, 1);
    line_finished(t);
}

static void history_reset_browsing(Terminal *t)
{
    t->hist_browsing = 0;
    free(t->hist_draft);
    t->hist_draft = NULL;
}

/* Commits the current line to the scrollback. If it completes a command,
 * runs it; if not (an unclosed quote, `$(` or backquote, or a trailing
 * backslash), keeps it and shows the continuation prompt. */
static void terminal_submit(Terminal *t)
{
    print_joined(t, t->prompt, t->input.data, t->input.len);
    history_add(t->history, t->input.data);
    history_reset_browsing(t);
    t->lines_run++;

    /* Continuation lines are joined to the earlier ones with a newline. The
     * text kept so far is never empty: an empty line is always complete. */
    int rc = 0;
    if (t->continued.len > 0)
        rc = sb_putc(&t->continued, '\n');
    if (rc == 0)
        rc = sb_append(&t->continued, t->input.data, t->input.len);
    input_clear(&t->input);

    RawPipeline *line = NULL;
    char err[256];
    ParseResult r = rc == 0 ? parse_line(sb_str(&t->continued), &line, err, sizeof err)
                            : PARSE_NO_MEMORY;
    if (r == PARSE_INCOMPLETE) {
        t->prompt = CONTINUATION_PROMPT;
        return;
    }
    char *text = sb_take(&t->continued);
    t->prompt = TERMINAL_PROMPT;

    switch (r) {
    case PARSE_NO_MEMORY:
        terminal_print(t, "terminator: out of memory");
        break;
    case PARSE_SYNTAX_ERROR:
        terminal_print(t, err);
        t->last_status = 2;
        break;
    default:
        t->interrupted = 0;
        eval_start(t, line, text ? text : "");
        break;
    }
    free(text);
    /* A line that finished at once (a built-in, an error) may have been
     * `source`, whose script starts now; otherwise it is back at the
     * prompt. Replaying typeahead is the caller's business. */
    run_scripts(t);
    if (!terminal_busy(t))
        jobs_notify(t);
}

/* The running job's output pipe is readable. */
static int on_job_output(Terminal *t)
{
    char buf[4096];
    ssize_t n = job_read(&t->job, buf, sizeof buf);
    if (n > 0)
        terminal_feed(t, &t->pending, buf, (size_t)n);
    finish_job_if_done(t);
    return n >= 0;
}

/* A running command substitution's stdout is readable. */
static int on_job_capture(Terminal *t)
{
    char buf[4096];
    ssize_t n = job_read_capture(&t->job, buf, sizeof buf);
    if (n > 0 && capture_append(t, buf, (size_t)n) != 0) {
        capture_failed(t);
        return 1;
    }
    finish_job_if_done(t);
    /* Captured text is not shown; only the end of the stream can matter. */
    return n == 0;
}

int terminal_poll_fds(const Terminal *t, struct pollfd *pfd, int max)
{
    int n = 0;
    if (t->job_active) {
        int fds[2] = { t->job.out_fd, t->job.cap_fd };
        for (int i = 0; i < 2 && n < max; i++) {
            if (fds[i] < 0)
                continue;
            pfd[n].fd = fds[i];
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            n++;
        }
    }
    n += jobs_poll_fds(&t->jobs, pfd + n, max - n);
    return n + multiwatch_poll_fds(&t->watch, pfd + n, max - n);
}

int terminal_on_poll(Terminal *t, const struct pollfd *pfd, int n)
{
    int dirty = 0;
    for (int i = 0; i < n; i++) {
        /* POLLHUP on a pipe means the writer closed it: read() returns EOF.
         * A handler may close fds and open new ones with the same numbers;
         * a stale event then only causes a harmless non-blocking read. */
        if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        int fd = pfd[i].fd;
        if (t->job_active && fd == t->job.out_fd)
            dirty |= on_job_output(t);
        else if (t->job_active && fd == t->job.cap_fd)
            dirty |= on_job_capture(t);
        else if (jobs_on_fd(t, fd))
            dirty = 1;
        else if (multiwatch_active(&t->watch))
            dirty |= multiwatch_on_fd(t, fd);
    }
    return dirty;
}

int terminal_timeout_ms(const Terminal *t)
{
    return multiwatch_timeout_ms(&t->watch);
}

int terminal_on_timer(Terminal *t)
{
    return multiwatch_on_timer(t);
}

int terminal_on_child_exit(Terminal *t)
{
    int dirty = multiwatch_on_child_exit(t);
    if (jobs_on_child_exit(t))
        dirty = 1;
    if (!t->job_active)
        return dirty;

    int changed = job_try_reap(&t->job);
    if (!job_done(&t->job)) {
        /* Ctrl+Z (or a SIGSTOP from elsewhere) stopped the foreground job:
         * it becomes a stopped job and the prompt returns. A stopped
         * command substitution stays the foreground job; Ctrl+C ends it. */
        if (changed && job_stopped(&t->job) && t->fg_cmd) {
            take_back_input(t, 1);
            int status = jobs_stop_foreground(t);
            eval_job_finished(t, status);
            line_finished(t);
            return 1;
        }
        return dirty;
    }
    if (!changed)
        return dirty;

    /* Everything the children wrote is already in the pipes. Read it, then
     * close the pipes even if a background grandchild still holds them open,
     * so the prompt returns when the pipeline itself exits. */
    char buf[4096];
    size_t total = 0;
    while (t->job.out_fd >= 0 && total < MAX_DRAIN_BYTES) {
        ssize_t n = job_read(&t->job, buf, sizeof buf);
        if (n <= 0)
            break;
        terminal_feed(t, &t->pending, buf, (size_t)n);
        total += (size_t)n;
    }
    total = 0;
    while (t->job.cap_fd >= 0 && total < MAX_DRAIN_BYTES) {
        ssize_t n = job_read_capture(&t->job, buf, sizeof buf);
        if (n <= 0)
            break;
        if (capture_append(t, buf, (size_t)n) != 0) {
            capture_failed(t);
            return 1;
        }
        total += (size_t)n;
    }
    job_close_output(&t->job);
    finish_job_if_done(t);
    return 1;
}

int terminal_request_exit(Terminal *t, int code)
{
    int stopped = 0;
    int live = jobs_live(&t->jobs, &stopped);
    /* Warn once; an exit right after the warning goes ahead. */
    if (live > 0 && !(t->exit_warned_at != 0 && t->exit_warned_at + 1 == t->lines_run)) {
        terminal_print(t, stopped > 0 ? "There are stopped jobs." : "There are running jobs.");
        t->exit_warned_at = t->lines_run;
        return 0;
    }
    t->exit_code = code;
    t->quit_requested = 1;
    return 1;
}

static void pick_end(Terminal *t);
static void search_end(Terminal *t);

/* ---- Job-control keys ------------------------------------------------------ */

/* Ctrl+C. Keys arrive as X events, so Terminator never gets a SIGINT; it
 * sends one to the foreground job's whole process group itself. */
static int key_interrupt(Terminal *t)
{
    /* As in bash, Ctrl+C also ends a sourced script, once its current
     * command is gone. */
    if (source_active(&t->sources))
        t->script_abort = 1;
    if (multiwatch_active(&t->watch)) {
        multiwatch_interrupt(t);
        line_finished(t);
        return 1;
    }
    if (t->job_active) {
        int stopped = job_stopped(&t->job);
        job_signal(&t->job, SIGINT);
        if (stopped)   /* the SIGINT is delivered once it runs again */
            job_continue(&t->job);
        take_back_input(t, 0);
        t->interrupted = 1;
        terminal_feed(t, &t->pending, "^C\n", 3);
        return 1;
    }
    /* At the prompt: abandon the line being typed (or the history search),
     * as bash does. */
    if (input_insert_str(&t->input, "^C", 2) == 0) {
        print_joined(t, t->prompt, t->input.data, t->input.len);
    }
    pick_end(t);
    if (t->searching) {
        search_end(t);
        return 1;
    }
    input_clear(&t->input);
    sb_clear(&t->continued);
    t->prompt = TERMINAL_PROMPT;
    history_reset_browsing(t);
    t->last_status = 130;
    jobs_notify(t);
    return 1;
}

/* Ctrl+Z: SIGTSTP to the foreground job's process group. The prompt returns
 * once SIGCHLD reports it stopped (terminal_on_child_exit). Only the
 * command itself can be suspended, not a command substitution or
 * multiWatch. */
static int key_suspend(Terminal *t)
{
    if (!t->job_active || !t->fg_cmd || multiwatch_active(&t->watch))
        return 0;
    job_signal(&t->job, SIGTSTP);
    take_back_input(t, 0);
    terminal_feed(t, &t->pending, "^Z\n", 3);
    return 1;
}

/* ---- Line editing ---------------------------------------------------------- */

/* Up / Down. The history is shared with the other tabs, which can add
 * entries (or clear it) while this tab is browsing, so browsing is a state
 * of its own rather than "the position after the newest entry". */
static int history_move(Terminal *t, int dir)
{
    size_t n = history_count(t->history);
    if (!t->hist_browsing) {
        if (dir > 0 || n == 0)
            return 0;
        free(t->hist_draft);
        t->hist_draft = strdup(t->input.data);
        t->hist_pos = n;
        t->hist_browsing = 1;
    }
    if (t->hist_pos > n)
        t->hist_pos = n;
    if (dir < 0) {
        if (t->hist_pos == 0)
            return 0;
        t->hist_pos--;
    } else {
        t->hist_pos++;
    }
    char *entry = t->hist_pos < n ? history_dup(t->history, t->hist_pos) : NULL;
    if (entry) {
        input_set(&t->input, entry);
        free(entry);
    } else {
        /* Past the newest entry (or the history was cleared meanwhile):
         * back to the line being typed. */
        input_set(&t->input, t->hist_draft ? t->hist_draft : "");
        history_reset_browsing(t);
    }
    return 1;
}

/* Ctrl+D: on an empty line ends the shell (bash's "exit"); on an empty
 * continuation line it is the end of input, so the unfinished command is
 * dropped with bash's error; otherwise deletes the character under the
 * cursor. */
static int key_eof(Terminal *t)
{
    if (t->input.len > 0)
        return input_delete(&t->input);
    if (t->searching) {   /* an empty search term: give up the search */
        print_joined(t, t->prompt, "", 0);
        search_end(t);
        return 1;
    }
    if (t->continued.len > 0) {
        print_joined(t, t->prompt, "", 0);
        terminal_print(t, "terminator: syntax error: unexpected end of file");
        sb_clear(&t->continued);
        t->prompt = TERMINAL_PROMPT;
        t->last_status = 2;
        return 1;
    }
    print_joined(t, t->prompt, "exit", 4);
    t->lines_run++;
    terminal_request_exit(t, t->last_status);
    return 1;
}

static void clear_selection(Terminal *t)
{
    t->sel_state = 0;
}

static int key_copy(Terminal *t)
{
    const char *text = t->sel_text ? t->sel_text : t->input.len > 0 ? t->input.data : NULL;
    if (!text)
        return 0;
    free(t->own_request[TERM_SEL_CLIPBOARD]);
    t->own_request[TERM_SEL_CLIPBOARD] = strdup(text);
    return 0;
}

/* ---- Ctrl+R: searching the history ------------------------------------------- */

/* Shows SEARCH_PROMPT for the term; the line typed so far (committed to the
 * scrollback, as bash shows it) comes back afterwards. */
static void search_start(Terminal *t)
{
    print_joined(t, t->prompt, t->input.data, t->input.len);
    free(t->search_saved_input);
    t->search_saved_input = strdup(t->input.data);
    t->search_saved_prompt = t->prompt;
    t->prompt = SEARCH_PROMPT;
    input_clear(&t->input);
    history_reset_browsing(t);
    t->searching = 1;
}

static void search_end(Terminal *t)
{
    t->searching = 0;
    t->prompt = t->search_saved_prompt ? t->search_saved_prompt : TERMINAL_PROMPT;
    input_set(&t->input, t->search_saved_input ? t->search_saved_input : "");
    free(t->search_saved_input);
    t->search_saved_input = NULL;
}

/* Length of the longest common substring of `a` and `b`. `row` has room
 * for strlen(b) + 1 ints. */
static size_t common_substring(const char *a, const char *b, size_t blen, size_t *row)
{
    size_t best = 0;
    memset(row, 0, (blen + 1) * sizeof *row);
    for (; *a; a++) {
        /* Right to left, so row[j - 1] is still the previous row's. */
        for (size_t j = blen; j > 0; j--) {
            row[j] = *a == b[j - 1] ? row[j - 1] + 1 : 0;
            if (row[j] > best)
                best = row[j];
        }
    }
    return best;
}

/* Prints, as the specification asks: the most recent command that is
 * exactly `term`; otherwise every (distinct) command sharing the longest
 * common substring with it, newest first, if that is longer than 2
 * characters; otherwise that nothing matched. */
static void search_history(Terminal *t, const char *term)
{
    /* Copy the history out: the search can take a moment, and the lock is
     * shared with every tab. */
    History *h = t->history;
    history_lock(h);
    size_t n = history_count_locked(h);
    char **lines = calloc(n ? n : 1, sizeof *lines);
    for (size_t i = 0; lines && i < n; i++)
        lines[i] = strdup(history_get_locked(h, i));
    history_unlock(h);
    if (!lines) {
        terminal_print(t, "terminator: out of memory");
        return;
    }

    int found = 0;
    for (size_t i = n; i-- > 0 && !found; ) {
        if (lines[i] && strcmp(lines[i], term) == 0) {
            terminal_print(t, lines[i]);
            found = 1;
        }
    }

    size_t tlen = strlen(term);
    size_t *row = !found && tlen > 2 ? malloc((tlen + 1) * sizeof *row) : NULL;
    size_t *best = row ? malloc((n ? n : 1) * sizeof *best) : NULL;
    if (best) {
        size_t best_len = 0, k = 0;
        for (size_t i = n; i-- > 0; ) {
            if (!lines[i])
                continue;
            size_t len = common_substring(lines[i], term, tlen, row);
            if (len > best_len) {
                best_len = len;
                k = 0;
            }
            if (len == best_len)
                best[k++] = i;
        }
        if (best_len > 2) {
            for (size_t j = 0; j < k; j++) {
                const char *line = lines[best[j]];
                int seen = 0;
                for (size_t p = 0; p < j && !seen; p++)
                    seen = strcmp(lines[best[p]], line) == 0;
                if (!seen)
                    terminal_print(t, line);
            }
            found = 1;
        }
    }
    if (!found)
        terminal_print(t, "No match for search term in history");
    free(best);
    free(row);
    for (size_t i = 0; i < n; i++)
        free(lines[i]);
    free(lines);
}

/* Enter on the search term. */
static void search_submit(Terminal *t)
{
    print_joined(t, t->prompt, t->input.data, t->input.len);
    char *term = strdup(t->input.data);
    search_end(t);
    if (term)
        search_history(t, term);
    free(term);
    jobs_notify(t);
}

/* ---- Tab: file-name completion ------------------------------------------------ */

/* At most this many completions are listed (and can be picked). */
#define PICK_SHOW_MAX 100

static void pick_end(Terminal *t)
{
    if (t->picking)
        complete_free(&t->pick);
    t->picking = 0;
    t->pick_number = 0;
}

/* Adds what turns the word into `name` at the cursor. */
static void complete_to(Terminal *t, const Completion *c, const char *name)
{
    char *ins = complete_insertion(c, name);
    if (ins)
        input_insert_str(&t->input, ins, strlen(ins));
    free(ins);
}

/* Tab: one match is completed; several are completed as far as they agree,
 * then listed with numbers to pick from; none does nothing. */
static int key_complete(Terminal *t)
{
    if (t->searching)
        return 0;
    pick_end(t);
    Completion c;
    int n = complete_find(t->input.data, t->input.cursor, &c);
    if (n == 0) {
        complete_free(&c);
        return 0;
    }
    if (n == 1) {
        complete_to(t, &c, c.names[0]);
        /* A file name is complete: close a quote it was typed in, as
         * bash does. A directory may go on. */
        size_t len = strlen(c.names[0]);
        if (c.quote && len > 0 && c.names[0][len - 1] != '/')
            input_insert(&t->input, (char)c.quote);
        complete_free(&c);
        return 1;
    }

    char *common = complete_common_prefix(&c);
    if (common) {
        complete_to(t, &c, common);
        c.typed = strlen(common);
        free(common);
    }

    /* The line so far, then the list, as the specification shows them;
     * the line stays on the prompt below. */
    print_joined(t, t->prompt, t->input.data, t->input.len);
    int shown = n < PICK_SHOW_MAX ? n : PICK_SHOW_MAX;
    StrBuf list;
    sb_init(&list);
    for (int i = 0; i < shown; i++) {
        char item[300];
        int len = snprintf(item, sizeof item, "%s%d. %s", i ? " " : "", i + 1, c.names[i]);
        sb_append(&list, item, (size_t)(len < (int)sizeof item ? len : (int)sizeof item - 1));
    }
    if (n > shown) {
        char more[96];
        int len = snprintf(more, sizeof more, "  (and %d more: type more of the name)", n - shown);
        sb_append(&list, more, (size_t)len);
    }
    if (shown > 9)
        sb_append(&list, "  (type a number, then Enter)", 29);
    terminal_print(t, sb_str(&list));
    sb_free(&list);

    t->pick = c;
    t->picking = 1;
    t->pick_number = 0;
    t->pick_shown = shown;
    return 1;
}

/* A key while a completion list is shown. Returns 1 or 0 if it was for the
 * list (redraw or not), -1 if it was not. */
static int pick_key(Terminal *t, const TermKey *key)
{
    int choose = 0;
    if (key->len == 1 && isdigit((unsigned char)key->text[0]) &&
        !(key->state & (ControlMask | Mod1Mask))) {
        int num = t->pick_number * 10 + (key->text[0] - '0');
        if (num == 0 || num > t->pick_shown) {
            t->pick_number = 0;   /* no such entry: start the number again */
            return 0;
        }
        t->pick_number = num;
        /* As soon as no more digits could follow. */
        if (num * 10 > t->pick_shown)
            choose = num;
    } else if ((key->sym == XK_Return || key->sym == XK_KP_Enter) && t->pick_number > 0) {
        choose = t->pick_number;
    } else if (key->sym == XK_Escape) {
        pick_end(t);
        return 0;
    } else {
        return -1;
    }
    if (!choose)
        return 0;
    const char *name = t->pick.names[choose - 1];
    complete_to(t, &t->pick, name);
    size_t len = strlen(name);
    if (t->pick.quote && len > 0 && name[len - 1] != '/')
        input_insert(&t->input, (char)t->pick.quote);
    pick_end(t);
    return 1;
}

static int edit_action(Terminal *t, KeyAction act)
{
    InputLine *in = &t->input;
    switch (act) {
    case ACT_BACKWARD_CHAR:
        if (in->cursor == 0)
            return 0;
        in->cursor--;
        return 1;
    case ACT_FORWARD_CHAR:
        if (in->cursor >= in->len)
            return 0;
        in->cursor++;
        return 1;
    case ACT_BEGINNING_OF_LINE:
        in->cursor = 0;
        return 1;
    case ACT_END_OF_LINE:
        in->cursor = in->len;
        return 1;
    case ACT_PREVIOUS_HISTORY:
        return t->searching ? 0 : history_move(t, -1);
    case ACT_NEXT_HISTORY:
        return t->searching ? 0 : history_move(t, +1);
    case ACT_HISTORY_SEARCH:
        if (t->searching)
            return 0;
        search_start(t);
        return 1;
    case ACT_KILL_TO_START:
        return input_delete_range(in, 0, in->cursor);
    case ACT_KILL_TO_END:
        return input_delete_range(in, in->cursor, in->len);
    case ACT_KILL_WORD:
        return kill_word(in);
    case ACT_DELETE_CHAR:
        return input_delete(in);
    case ACT_CLEAR_SCREEN:
        textbuf_clear(&t->scrollback);
        clear_selection(t);
        t->scrolled = 0;
        return 1;
    case ACT_EOF:
        return key_eof(t);
    case ACT_PASTE:
        t->paste_request = 1 + TERM_SEL_CLIPBOARD;
        return 0;
    default:
        return 0;
    }
}

static int handle_key(Terminal *t, const TermKey *key, KeyAction act);
static int scroll_action(Terminal *t, KeyAction act);
static int scroll_to_bottom(Terminal *t);

int terminal_handle_key(Terminal *t, const TermKey *key)
{
    KeyAction act = (KeyAction)key->action;
    /* Scrolling and copying leave the view where it is, even while a
     * command runs; any other key (but a lone modifier, as in the Shift of
     * Shift+PgUp) brings it back to the bottom, as in xterm. */
    if (act >= ACT_SCROLL_PAGE_UP && act <= ACT_SCROLL_BOTTOM)
        return scroll_action(t, act);
    int moved = 0;
    if (act != ACT_COPY && !key->modifier)
        moved = scroll_to_bottom(t);
    return handle_key(t, key, act) | moved;
}

/* unix-word-rubout: the blank-separated word before the cursor, and the
 * blanks after it. */
static int kill_word(InputLine *in)
{
    size_t start = in->cursor;
    while (start > 0 && isspace((unsigned char)in->data[start - 1]))
        start--;
    while (start > 0 && !isspace((unsigned char)in->data[start - 1]))
        start--;
    return input_delete_range(in, start, in->cursor);
}

/* A key typed to the running command. Like a terminal in its normal (line)
 * mode: characters collect into a line, which Backspace, Ctrl+U and Ctrl+W
 * edit and Enter sends. Ctrl+D on an empty line is end of input (the input
 * pipe is closed); on a partial line it sends that much at once. Other
 * hotkeys are kept for the prompt. */
static int job_input_key(Terminal *t, const TermKey *key, KeyAction act)
{
    InputLine *in = &t->job_input;
    switch (act) {
    case ACT_EOF:
        if (in->len == 0) {
            job_close_input(&t->job);
            return 0;
        }
        terminal_feed(t, &t->pending, in->data, in->len);
        job_send(t, in->data, in->len);
        input_clear(in);
        return 1;
    case ACT_KILL_TO_START:
        input_clear(in);
        return 1;
    case ACT_KILL_WORD:
        return kill_word(in);
    case ACT_PASTE:
        t->paste_request = 1 + TERM_SEL_CLIPBOARD;
        return 0;
    case ACT_NONE:
        break;
    default:
        if (t->typeahead_len < TYPEAHEAD_MAX)
            t->typeahead[t->typeahead_len++] = *key;
        return 0;
    }

    if (key->sym == XK_Return || key->sym == XK_KP_Enter) {
        job_send_line(t);
        return 1;
    }
    if (key->sym == XK_BackSpace)
        return input_backspace(in);
    if (key->state & (ControlMask | Mod1Mask))
        return 0;
    /* A tab is part of the line, as on a terminal: the command reads it,
     * or, left unread, it completes a file name at the prompt. */
    if (key->sym == XK_Tab || key->sym == XK_KP_Tab)
        return input_insert(in, '\t') == 0;
    int changed = 0;
    for (int i = 0; i < key->len; i++) {
        unsigned char c = (unsigned char)key->text[i];
        if (c >= 0x20 && c != 0x7f && input_insert(in, (char)c) == 0)
            changed = 1;
    }
    return changed;
}

static int handle_key(Terminal *t, const TermKey *key, KeyAction act)
{

    /* These act at once, even while a command runs. */
    switch (act) {
    case ACT_INTERRUPT:
        return key_interrupt(t);
    case ACT_SUSPEND:
        return key_suspend(t);
    case ACT_COPY:
        return key_copy(t);
    default:
        break;
    }

    /* While the command runs, typing goes to it. */
    if (job_reads_input(t))
        return job_input_key(t, key, act);

    /* Busy with something that reads no input (a command substitution,
     * a command whose input was closed): keys are kept for the next
     * prompt, like bash's typeahead. Ctrl+D is not: a delayed one would
     * close the shell. */
    if (terminal_busy(t)) {
        if (act != ACT_EOF && t->typeahead_len < TYPEAHEAD_MAX)
            t->typeahead[t->typeahead_len++] = *key;
        return 0;
    }

    /* A completion list is shown: a number picks from it; anything else
     * dismisses it and does what it always does. */
    if (t->picking) {
        int r = pick_key(t, key);
        if (r >= 0)
            return r;
        pick_end(t);
    }

    if (act != ACT_NONE)
        return edit_action(t, act);

    switch (key->sym) {
    case XK_Return:
    case XK_KP_Enter:
        if (t->searching)
            search_submit(t);
        else
            terminal_submit(t);
        return 1;
    case XK_BackSpace:
        return input_backspace(&t->input);
    case XK_Tab:
    case XK_KP_Tab:
    case XK_ISO_Left_Tab:
        if (key->state & (ControlMask | Mod1Mask))
            return 0;
        return key_complete(t);
    default:
        break;
    }

    /* Other Ctrl/Alt combinations are not bound to anything. */
    if (key->state & (ControlMask | Mod1Mask))
        return 0;

    int changed = 0;
    for (int i = 0; i < key->len; i++) {
        unsigned char c = (unsigned char)key->text[i];
        if (c < 0x20 || c == 0x7f)   /* skip control characters */
            continue;
        if (input_insert(&t->input, (char)c) == 0)
            changed = 1;
        else
            fprintf(stderr, "terminator: out of memory, key dropped\n");
    }
    return changed;
}

int terminal_paste(Terminal *t, const char *text, size_t len)
{
    /* Pasted into a running command's input, each line goes to it, as if
     * typed with Enter. */
    if (job_reads_input(t)) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)text[i];
            if (c == '\r' && i + 1 < len && text[i + 1] == '\n')
                continue;
            if (c == '\n' || c == '\r')
                job_send_line(t);
            else if (c == '\t' || (c >= 0x20 && c != 0x7f))
                input_insert(&t->job_input, (char)c);
        }
        return 1;
    }
    /* A trailing newline (from copying whole lines) would only add a blank. */
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    int changed = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' && i + 1 < len && text[i + 1] == '\n')
            continue;
        /* Pasting never runs anything: line breaks become spaces. */
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        else if (c < 0x20 || c == 0x7f)
            continue;
        if (input_insert(&t->input, (char)c) == 0)
            changed = 1;
    }
    return changed;
}

/* ---- Layout ------------------------------------------------------------------- */

/* One row of the window: a slice of a logical line, which is `a` followed by
 * `b` (the prompt and the input for the current line). */
typedef struct {
    unsigned long long line;   /* TextPos line number */
    size_t      off;           /* offset of the row's first character */
    int         len;           /* characters in the row */
    const char *a, *b;
    size_t      alen;
} LayoutRow;

typedef struct {
    LayoutRow *rows;
    int        nrows;
    int        cur_row, cur_col;   /* cursor; cur_row -1 if not in view */
    int        below;              /* rows below the view (scrolled back) */
} Layout;

/* A row of the text: row `row` of line `line` once wrapped. */
typedef struct {
    unsigned long long line;
    int                row;
} ViewPos;

/* Number of screen rows a line of `len` characters occupies. */
static int wrapped_rows(size_t len, int cols)
{
    return len == 0 ? 1 : (int)((len + (size_t)cols - 1) / (size_t)cols);
}

static char row_char(const LayoutRow *r, size_t i)
{
    size_t at = r->off + i;
    return at < r->alen ? r->a[at] : r->b[at - r->alen];
}

static unsigned long long first_line(const Terminal *t)
{
    return t->lines_total - textbuf_count(&t->scrollback);
}

/* The current line: prompt and input, or while a command runs its
 * unfinished output line. */
static void current_line(const Terminal *t, const char **head, const char **tail, size_t *cursor)
{
    /* While a command runs: its output after the last newline, then what
     * is being typed to it. */
    int busy = terminal_busy(t);
    *head = busy ? t->pending.data : t->prompt;
    *tail = busy ? (job_reads_input(t) ? t->job_input.data : "") : t->input.data;
    *cursor = strlen(*head) + (busy ? strlen(*tail) : t->input.cursor);
}

/* Text of line `line` as `a` followed by `b`; returns its length. */
static size_t line_parts(const Terminal *t, unsigned long long line, const char **a,
                         size_t *alen, const char **b)
{
    if (line < t->lines_total) {
        *a = textbuf_line(&t->scrollback, (size_t)(line - first_line(t)));
        *b = "";
        *alen = strlen(*a);
        return *alen;
    }
    size_t cursor;
    current_line(t, a, b, &cursor);
    *alen = strlen(*a);
    return *alen + strlen(*b);
}

/* Rows a line takes; the current line has one more cell for the cursor. */
static int line_rows(const Terminal *t, unsigned long long line, int cols)
{
    const char *a, *b;
    size_t alen;
    size_t len = line_parts(t, line, &a, &alen, &b);
    if (line == t->lines_total)
        return (int)(len / (size_t)cols) + 1;
    return wrapped_rows(len, cols);
}

static int vpos_cmp(ViewPos a, ViewPos b)
{
    if (a.line != b.line)
        return a.line < b.line ? -1 : 1;
    return a.row < b.row ? -1 : a.row > b.row;
}

/* The top row of the live view: the newest rows, ending with the current
 * line. Text starts at the top until the window is full; then the oldest
 * rows scroll off (if the current line alone is taller than the window,
 * only its end is shown). */
static ViewPos live_top(const Terminal *t, int cols, int rows)
{
    unsigned long long line = t->lines_total;
    unsigned long long first = first_line(t);
    int used = line_rows(t, line, cols);
    while (line > first && used < rows) {
        line--;
        used += line_rows(t, line, cols);
    }
    return (ViewPos){ line, used > rows ? used - rows : 0 };
}

/* The top row of what is shown: the live view, or while scrolled back the
 * row it was scrolled to (which stays put as output arrives below). */
static ViewPos view_top(const Terminal *t, int cols, int rows)
{
    if (!t->scrolled)
        return live_top(t, cols, rows);
    ViewPos p = { t->view_line, t->view_row };
    if (p.line < first_line(t))   /* dropped from the scrollback */
        return (ViewPos){ first_line(t), 0 };
    int n = line_rows(t, p.line, cols);
    if (p.row >= n)               /* the window got wider */
        p.row = n - 1;
    return p;
}

/* Lays out `rows` rows starting at view_top(). Returns 0 or -1 on OOM. */
static int screen_build(const Terminal *t, int cols, int rows, Layout *s)
{
    s->rows = malloc((size_t)rows * sizeof *s->rows);
    if (!s->rows)
        return -1;
    s->nrows = 0;
    s->cur_row = -1;
    s->cur_col = 0;
    s->below = 0;

    ViewPos top = view_top(t, cols, rows);
    for (unsigned long long line = top.line; line <= t->lines_total; line++) {
        const char *a, *b;
        size_t alen;
        size_t len = line_parts(t, line, &a, &alen, &b);
        int nr = line_rows(t, line, cols);
        for (int r = line == top.line ? top.row : 0; r < nr; r++) {
            if (s->nrows == rows) {
                s->below += nr - r;
                break;
            }
            if (line == t->lines_total) {
                const char *h, *tl;
                size_t cursor;
                current_line(t, &h, &tl, &cursor);
                if ((int)(cursor / (size_t)cols) == r) {
                    s->cur_row = s->nrows;
                    s->cur_col = (int)(cursor % (size_t)cols);
                }
            }
            size_t off = (size_t)r * (size_t)cols;
            size_t n = len - off < (size_t)cols ? len - off : (size_t)cols;
            s->rows[s->nrows++] = (LayoutRow){ line, off, (int)n, a, b, alen };
        }
    }
    return 0;
}

void terminal_resize(Terminal *t, int cols, int rows)
{
    t->view_cols = cols > 0 ? cols : 1;
    t->view_rows = rows > 0 ? rows : 1;
}

/* Moves the view `delta` rows (negative: back into older output). Reaching
 * the live view ends scrolling. Returns 1 if the view changed. */
static int scroll_view(Terminal *t, long delta)
{
    int cols = t->view_cols, rows = t->view_rows;
    ViewPos live = live_top(t, cols, rows);
    ViewPos p = view_top(t, cols, rows);
    ViewPos was = p;
    int was_scrolled = t->scrolled;
    unsigned long long first = first_line(t);
    for (; delta < 0; delta++) {
        if (p.row > 0)
            p.row--;
        else if (p.line > first)
            p = (ViewPos){ p.line - 1, line_rows(t, p.line - 1, cols) - 1 };
        else
            break;
    }
    for (; delta > 0 && vpos_cmp(p, live) < 0; delta--) {
        if (p.row + 1 < line_rows(t, p.line, cols))
            p.row++;
        else
            p = (ViewPos){ p.line + 1, 0 };
    }
    t->scrolled = vpos_cmp(p, live) < 0;
    t->view_line = p.line;
    t->view_row  = p.row;
    return t->scrolled != was_scrolled || vpos_cmp(p, was) != 0;
}

/* Back to the live view (any key except scrolling ones does this). */
static int scroll_to_bottom(Terminal *t)
{
    int was = t->scrolled;
    t->scrolled = 0;
    return was;
}

static int scroll_action(Terminal *t, KeyAction act)
{
    long page = t->view_rows > 1 ? t->view_rows - 1 : 1;
    switch (act) {
    case ACT_SCROLL_PAGE_UP:   return scroll_view(t, -page);
    case ACT_SCROLL_PAGE_DOWN: return scroll_view(t, page);
    case ACT_SCROLL_LINE_UP:   return scroll_view(t, -1);
    case ACT_SCROLL_LINE_DOWN: return scroll_view(t, 1);
    case ACT_SCROLL_TOP:       return scroll_view(t, -(long)(textbuf_count(&t->scrollback) + 1) * t->view_cols);
    case ACT_SCROLL_BOTTOM:    return scroll_to_bottom(t);
    default:                   return 0;
    }
}

static int pos_cmp(const TextPos *a, const TextPos *b)
{
    if (a->line != b->line)
        return a->line < b->line ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

/* The selection in text order; returns 0 if nothing is selected. */
static int selection_range(const Terminal *t, TextPos *start, TextPos *end)
{
    if (t->sel_state == 0 || pos_cmp(&t->sel_anchor, &t->sel_end) == 0)
        return 0;
    int fwd = pos_cmp(&t->sel_anchor, &t->sel_end) < 0;
    *start = fwd ? t->sel_anchor : t->sel_end;
    *end   = fwd ? t->sel_end : t->sel_anchor;
    return 1;
}

/* Text position of the cell (col, row) of the view. */
static int pos_at(const Terminal *t, int col, int row, TextPos *pos)
{
    Layout s;
    if (screen_build(t, t->view_cols, t->view_rows, &s) != 0)
        return -1;
    if (s.nrows == 0) {
        free(s.rows);
        return -1;
    }
    if (col < 0)
        col = 0;
    if (row < 0)
        row = 0;
    if (row >= s.nrows) {
        row = s.nrows - 1;
        col = t->view_cols;
    }
    const LayoutRow *r = &s.rows[row];
    pos->line = r->line;
    pos->off  = r->off + (size_t)(col < r->len ? col : r->len);
    free(s.rows);
    return 0;
}

/* Text of logical line `line` (malloc'd), or NULL if no longer kept. */
static char *line_text(const Terminal *t, unsigned long long line)
{
    if (line < first_line(t))
        return NULL;
    const char *a, *b;
    size_t alen;
    size_t len = line_parts(t, line, &a, &alen, &b);
    char *s = malloc(len + 1);
    if (s) {
        memcpy(s, a, alen);
        memcpy(s + alen, b, len - alen + 1);
    }
    return s;
}

/* The selected text, lines joined with '\n'; NULL if empty. */
static char *selection_text(const Terminal *t)
{
    TextPos start, end;
    if (!selection_range(t, &start, &end))
        return NULL;
    StrBuf sb;
    sb_init(&sb);
    for (unsigned long long l = start.line; l <= end.line; l++) {
        char *text = line_text(t, l);
        if (text) {
            size_t len = strlen(text);
            size_t lo = l == start.line ? start.off : 0;
            size_t hi = l == end.line ? end.off + 1 : len;
            if (hi > len)
                hi = len;
            if (lo < hi)
                sb_append(&sb, text + lo, hi - lo);
            free(text);
        }
        if (l < end.line)
            sb_putc(&sb, '\n');
    }
    if (sb.len == 0) {
        sb_free(&sb);
        return NULL;
    }
    return sb_take(&sb);
}

int terminal_mouse(Terminal *t, const TermMouse *m)
{
    TextPos pos;
    switch (m->type) {
    case TERM_MOUSE_PRESS:
        /* The wheel scrolls through the output, three rows a notch. */
        if (m->button == Button4 || m->button == Button5)
            return scroll_view(t, m->button == Button4 ? -3 : 3);
        if (m->button == Button2) {
            /* Middle click pastes the PRIMARY selection, as in xterm. */
            t->paste_request = 1 + TERM_SEL_PRIMARY;
            return 0;
        }
        if (m->button != Button1 || pos_at(t, m->col, m->row, &pos) != 0)
            return 0;
        t->sel_anchor = t->sel_end = pos;
        t->sel_state = 1;
        free(t->sel_text);
        t->sel_text = NULL;
        return 1;
    case TERM_MOUSE_MOTION:
        if (t->sel_state != 1 || pos_at(t, m->col, m->row, &pos) != 0)
            return 0;
        t->sel_end = pos;
        return 1;
    case TERM_MOUSE_RELEASE:
        if (m->button != Button1 || t->sel_state != 1)
            return 0;
        if (pos_at(t, m->col, m->row, &pos) == 0)
            t->sel_end = pos;
        t->sel_text = selection_text(t);
        if (!t->sel_text) {
            t->sel_state = 0;   /* a plain click */
            return 1;
        }
        t->sel_state = 2;
        /* Selecting makes the text the PRIMARY selection, as in xterm. */
        free(t->own_request[TERM_SEL_PRIMARY]);
        t->own_request[TERM_SEL_PRIMARY] = strdup(t->sel_text);
        return 1;
    default:
        return 0;
    }
}

/* ---- Snapshot ------------------------------------------------------------------- */

int terminal_snapshot(const Terminal *t, TermScreen *scr)
{
    int cols = t->view_cols, rows = t->view_rows;
    if (screen_resize(scr, cols, rows) != 0)
        return -1;
    Layout s;
    if (screen_build(t, cols, rows, &s) != 0)
        return -1;
    TextPos start = { 0, 0 }, end = { 0, 0 };
    int selected = selection_range(t, &start, &end);

    for (int i = 0; i < s.nrows; i++) {
        const LayoutRow *r = &s.rows[i];
        char *cells = scr->cells + (size_t)i * (size_t)cols;
        for (int c = 0; c < r->len; c++) {
            char ch = row_char(r, (size_t)c);
            cells[c] = (unsigned char)ch < 0x20 ? ' ' : ch;
        }
        scr->len[i] = r->len;

        /* The selected part of this row, [from, to), in reverse video. */
        if (!selected || r->line < start.line || r->line > end.line)
            continue;
        size_t lo = r->line == start.line ? start.off : 0;
        size_t hi = r->line == end.line ? end.off + 1 : (size_t)-1;   /* exclusive */
        size_t row_end = r->off + (size_t)r->len;
        size_t from = lo > r->off ? lo : r->off;
        size_t to   = hi < row_end ? hi : row_end;
        if (from < to) {
            scr->sel_from[i] = (int)(from - r->off);
            scr->sel_to[i]   = (int)(to - r->off);
        }
    }
    if (s.cur_row >= 0 && s.cur_row < s.nrows) {
        scr->cur_row = s.cur_row;
        scr->cur_col = s.cur_col;
    }
    /* While scrolled back, say so (and how far) in the top right corner. */
    if (t->scrolled && s.below > 0) {
        int n = snprintf(scr->note, sizeof scr->note, " %d more line%s below (Shift+End) ",
                         s.below, s.below == 1 ? "" : "s");
        if (n > cols)
            n = snprintf(scr->note, sizeof scr->note, "+%d", s.below);
        if (n > cols)
            scr->note[0] = '\0';
    }
    free(s.rows);
    return 0;
}
