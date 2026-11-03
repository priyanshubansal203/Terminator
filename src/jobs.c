/*
 * jobs.c - The job table and the jobs / fg / bg built-ins.
 */
#include "jobs.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "terminal.h"

/* At most this much is read from one job's pipe per wakeup, so a chatty
 * background job cannot starve the GUI or the foreground job. */
#define READ_BUDGET (64 * 1024)

/* Upper bound on bytes drained from a pipe after its job has exited, in case
 * something it started keeps the pipe open and keeps writing. */
#define MAX_DRAIN_BYTES (1 << 20)

int jobs_init(JobTable *jt)
{
    memset(jt, 0, sizeof *jt);
    return 0;
}

static void entry_free(JobEntry *e)
{
    job_kill(&e->job);
    free(e->cmd);
    input_free(&e->out);
}

void jobs_free(JobTable *jt)
{
    for (int i = 0; i < jt->n; i++)
        entry_free(&jt->v[i]);
    free(jt->v);
    memset(jt, 0, sizeof *jt);
}

/* Removes entry `i`; its contents must already be freed or moved out. */
static void remove_at(JobTable *jt, int i)
{
    memmove(&jt->v[i], &jt->v[i + 1], (size_t)(jt->n - i - 1) * sizeof *jt->v);
    jt->n--;
}

static void job_reset(Job *j)
{
    memset(j, 0, sizeof *j);
    j->out_fd = -1;
    j->cap_fd = -1;
    j->in_fd  = -1;
    j->in_rd  = -1;
}

char *jobs_command_text(const char *line, int background)
{
    char *s = strdup(line);
    if (!s)
        return NULL;
    for (char *p = s; *p; p++)
        if (*p == '\n')
            *p = ' ';
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    if (background && len > 0 && s[len - 1] == '&') {
        s[--len] = '\0';
        while (len > 0 && isspace((unsigned char)s[len - 1]))
            s[--len] = '\0';
    }
    size_t start = 0;
    while (isspace((unsigned char)s[start]))
        start++;
    memmove(s, s + start, len - start + 1);
    return s;
}

/* ---- Current / previous job ---------------------------------------------- */

/* As in bash, the current job is the most recently stopped one or, if none
 * is stopped, the most recently started in (or sent to) the background. */
static int outranks(const JobEntry *a, const JobEntry *b)
{
    int ra = a->state == JOB_STOPPED ? 2 : a->state == JOB_RUNNING ? 1 : 0;
    int rb = b->state == JOB_STOPPED ? 2 : b->state == JOB_RUNNING ? 1 : 0;
    return ra != rb ? ra > rb : a->seq > b->seq;
}

static JobEntry *current_job(JobTable *jt, const JobEntry *except)
{
    JobEntry *best = NULL;
    for (int i = 0; i < jt->n; i++) {
        JobEntry *e = &jt->v[i];
        if (e != except && (!best || outranks(e, best)))
            best = e;
    }
    return best;
}

static char mark_of(JobTable *jt, const JobEntry *e)
{
    JobEntry *cur = current_job(jt, NULL);
    if (e == cur)
        return '+';
    if (cur && e == current_job(jt, cur))
        return '-';
    return ' ';
}

/* ---- Listing --------------------------------------------------------------- */

static void state_text(const JobEntry *e, char *buf, size_t size)
{
    switch (e->state) {
    case JOB_RUNNING:
        snprintf(buf, size, "Running");
        return;
    case JOB_STOPPED: {
        int sig = job_stop_signal(&e->job);
        snprintf(buf, size, "%s", sig == SIGTTIN ? "Stopped (tty input)" :
                                  sig == SIGTTOU ? "Stopped (tty output)" :
                                  sig == SIGSTOP ? "Stopped (signal)" : "Stopped");
        return;
    }
    case JOB_DONE:
    default: {
        int st = job_last_status(&e->job);
        if (WIFSIGNALED(st))
            snprintf(buf, size, "%s", strsignal(WTERMSIG(st)));
        else if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
            snprintf(buf, size, "Exit %d", WEXITSTATUS(st));
        else
            snprintf(buf, size, "Done");
        return;
    }
    }
}

/* Prints one job the way bash's `jobs` does; with `pids`, as `jobs -l`.
 * The listing of `jobs` is its output (`out`): it can be redirected or
 * piped. Notices (Done, Stopped) are messages. */
static void print_job(Terminal *t, JobEntry *e, int pids, int out)
{
    void (*say)(Terminal *, const char *, ...) = out ? terminal_outf : terminal_printf;
    char state[64];
    state_text(e, state, sizeof state);
    const char *amp = e->state == JOB_RUNNING ? " &" : "";
    char mark = mark_of(&t->jobs, e);
    if (!pids) {
        say(t, "[%d]%c  %-24s%s%s", e->id, mark, state, e->cmd, amp);
        return;
    }
    /* -l: the process group leader's PID, which is also the PGID, then the
     * other processes of the pipeline. */
    pid_t first = e->job.nprocs > 0 ? e->job.procs[0].pid : 0;
    say(t, "[%d]%c %ld %-24s%s%s", e->id, mark, (long)first, state, e->cmd, amp);
    for (int i = 1; i < e->job.nprocs; i++)
        say(t, "     %ld", (long)e->job.procs[i].pid);
}

void jobs_notify(Terminal *t)
{
    JobTable *jt = &t->jobs;
    for (int i = 0; i < jt->n; ) {
        JobEntry *e = &jt->v[i];
        if (e->state == JOB_DONE) {
            print_job(t, e, 0, 0);
            entry_free(e);
            remove_at(jt, i);
            continue;
        }
        if (e->changed)
            print_job(t, e, 0, 0);
        e->changed = 0;
        i++;
    }
}

int jobs_live(const JobTable *jt, int *stopped)
{
    int live = 0, st = 0;
    for (int i = 0; i < jt->n; i++) {
        if (jt->v[i].state != JOB_DONE)
            live++;
        if (jt->v[i].state == JOB_STOPPED)
            st++;
    }
    if (stopped)
        *stopped = st;
    return live;
}

/* ---- Adding jobs ------------------------------------------------------------- */

static int next_id(const JobTable *jt)
{
    int max = 0;
    for (int i = 0; i < jt->n; i++)
        if (jt->v[i].id > max)
            max = jt->v[i].id;
    return max + 1;
}

/* Inserts `e` keeping the table sorted by job number. Returns the entry,
 * or NULL on OOM. */
static JobEntry *insert(JobTable *jt, const JobEntry *e)
{
    if (jt->n == jt->cap) {
        int cap = jt->cap ? jt->cap * 2 : 8;
        JobEntry *grown = realloc(jt->v, (size_t)cap * sizeof *grown);
        if (!grown)
            return NULL;
        jt->v = grown;
        jt->cap = cap;
    }
    int at = jt->n;
    while (at > 0 && jt->v[at - 1].id > e->id)
        at--;
    memmove(&jt->v[at + 1], &jt->v[at], (size_t)(jt->n - at) * sizeof *jt->v);
    jt->v[at] = *e;
    jt->n++;
    return &jt->v[at];
}

int jobs_add_background(Terminal *t, Job *job, const char *cmd)
{
    JobTable *jt = &t->jobs;
    JobEntry e;
    memset(&e, 0, sizeof e);
    if (jt->n >= JOBS_MAX) {
        terminal_printf(t, "terminator: too many jobs (at most %d); use `jobs' and `fg'", JOBS_MAX);
        job_kill(job);
        return -1;
    }
    e.id    = next_id(jt);
    e.job   = *job;
    e.state = JOB_RUNNING;
    e.seq   = ++jt->seq;
    e.cmd   = strdup(cmd);
    if (!e.cmd || input_init(&e.out) != 0 || !insert(jt, &e)) {
        free(e.cmd);
        input_free(&e.out);
        job_kill(job);
        terminal_print(t, "terminator: out of memory");
        return -1;
    }
    job_reset(job);
    terminal_printf(t, "[%d] %ld", e.id, (long)job_last_pid(&e.job));
    return 0;
}

int jobs_stop_foreground(Terminal *t)
{
    JobTable *jt = &t->jobs;
    int sig = job_stop_signal(&t->job);
    if (t->pending.len > 0)
        terminal_flush_line(t, &t->pending);

    JobEntry e;
    memset(&e, 0, sizeof e);
    e.id    = t->fg_id > 0 ? t->fg_id : next_id(jt);
    e.job   = t->job;
    e.state = JOB_STOPPED;
    e.seq   = ++jt->seq;
    e.cmd   = t->fg_cmd ? t->fg_cmd : strdup("(unknown)");
    t->fg_cmd = NULL;
    t->fg_id  = 0;
    job_reset(&t->job);
    t->job_active = 0;

    JobEntry *in = NULL;
    if (e.cmd && input_init(&e.out) == 0)
        in = insert(jt, &e);
    if (!in) {
        /* Without a table entry nobody could resume it; do not leave it
         * stopped forever. */
        terminal_print(t, "terminator: out of memory; stopped job killed");
        entry_free(&e);
        return 128 + sig;
    }
    print_job(t, in, 0, 0);
    return 128 + sig;
}

/* ---- Job specs ------------------------------------------------------------------ */

/* Finds the job named by `spec` (%n, n, %%, %+, %-, %string, %?string; NULL
 * means the current job). Prints an error naming `who` if there is none. */
static JobEntry *find_job(Terminal *t, const char *who, const char *spec)
{
    JobTable *jt = &t->jobs;
    if (!spec || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0 || strcmp(spec, "%") == 0) {
        JobEntry *cur = current_job(jt, NULL);
        if (!cur)
            terminal_printf(t, "terminator: %s: %s: no such job", who, spec ? spec : "current");
        return cur;
    }
    if (strcmp(spec, "%-") == 0) {
        JobEntry *cur = current_job(jt, NULL);
        JobEntry *prev = cur ? current_job(jt, cur) : NULL;
        if (!prev)
            prev = cur;
        if (!prev)
            terminal_printf(t, "terminator: %s: %s: no such job", who, spec);
        return prev;
    }

    const char *s = spec[0] == '%' ? spec + 1 : spec;
    if (*s && strspn(s, "0123456789") == strlen(s)) {
        long id = strtol(s, NULL, 10);
        for (int i = 0; i < jt->n; i++)
            if (jt->v[i].id == id)
                return &jt->v[i];
        terminal_printf(t, "terminator: %s: %s: no such job", who, spec);
        return NULL;
    }
    if (spec[0] != '%' || *s == '\0') {
        terminal_printf(t, "terminator: %s: %s: no such job", who, spec);
        return NULL;
    }

    /* %string: the command starts with it; %?string: contains it. */
    int contains = s[0] == '?';
    if (contains)
        s++;
    JobEntry *found = NULL;
    for (int i = 0; i < jt->n; i++) {
        JobEntry *e = &jt->v[i];
        int match = contains ? strstr(e->cmd, s) != NULL
                             : strncmp(e->cmd, s, strlen(s)) == 0;
        if (!match)
            continue;
        if (found) {
            terminal_printf(t, "terminator: %s: %s: ambiguous job spec", who, spec);
            return NULL;
        }
        found = e;
    }
    if (!found)
        terminal_printf(t, "terminator: %s: %s: no such job", who, spec);
    return found;
}

/* ---- Built-ins ------------------------------------------------------------------- */

int jobs_builtin_jobs(Terminal *t, int argc, char **argv)
{
    int pids = 0, pgids = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l')
                pids = 1;
            else if (*o == 'p')
                pgids = 1;
            else {
                terminal_printf(t, "terminator: jobs: -%c: invalid option", *o);
                terminal_print(t, "jobs: usage: jobs [-l | -p] [jobspec ...]");
                return 2;
            }
        }
    }

    JobTable *jt = &t->jobs;
    int status = 0;
    /* Mark what to list first: listing can then drop finished jobs. */
    char *listed = calloc((size_t)(jt->n > 0 ? jt->n : 1), 1);
    if (!listed) {
        terminal_print(t, "terminator: out of memory");
        return 1;
    }
    if (i == argc) {
        memset(listed, 1, (size_t)jt->n);
    } else {
        for (; i < argc; i++) {
            JobEntry *e = find_job(t, "jobs", argv[i]);
            if (e)
                listed[e - jt->v] = 1;
            else
                status = 1;
        }
    }

    for (int k = 0; k < jt->n; k++) {
        if (!listed[k])
            continue;
        JobEntry *e = &jt->v[k];
        if (pgids)
            terminal_outf(t, "%ld", (long)e->job.pgid);
        else
            print_job(t, e, pids, 1);
        e->changed = 0;
    }
    /* Finished jobs have now been reported. */
    for (int k = jt->n - 1; k >= 0; k--) {
        if (listed[k] && jt->v[k].state == JOB_DONE) {
            entry_free(&jt->v[k]);
            remove_at(jt, k);
        }
    }
    free(listed);
    return status;
}

int jobs_builtin_bg(Terminal *t, int argc, char **argv)
{
    int status = 0;
    int n = argc > 1 ? argc - 1 : 1;
    for (int k = 0; k < n; k++) {
        JobEntry *e = find_job(t, "bg", argc > 1 ? argv[k + 1] : NULL);
        if (!e) {
            status = 1;
            continue;
        }
        if (e->state == JOB_DONE) {
            terminal_print(t, "terminator: bg: job has terminated");
            status = 1;
            continue;
        }
        if (e->state == JOB_RUNNING) {
            terminal_printf(t, "terminator: bg: job %d already in background", e->id);
            continue;
        }
        job_continue(&e->job);
        e->state   = JOB_RUNNING;
        e->changed = 0;
        terminal_printf(t, "[%d]%c %s &", e->id, mark_of(&t->jobs, e), e->cmd);
    }
    return status;
}

int jobs_fg(Terminal *t, int argc, char **argv, int *status)
{
    *status = 1;
    if (argc > 2) {
        terminal_print(t, "terminator: fg: too many arguments");
        return 0;
    }
    JobEntry *e = find_job(t, "fg", argc > 1 ? argv[1] : NULL);
    if (!e)
        return 0;
    if (e->state == JOB_DONE) {
        /* That is its report; it goes. */
        terminal_print(t, "terminator: fg: job has terminated");
        entry_free(e);
        remove_at(&t->jobs, (int)(e - t->jobs.v));
        return 0;
    }

    terminal_print(t, e->cmd);
    /* Output the job printed after its last newline continues on the
     * foreground job's line, which is empty at the prompt. */
    InputLine swap = t->pending;
    t->pending = e->out;
    e->out = swap;

    t->job        = e->job;
    t->job_active = 1;
    t->fg_cmd     = e->cmd;
    t->fg_id      = e->id;
    input_free(&e->out);
    remove_at(&t->jobs, (int)(e - t->jobs.v));

    /* Sent whether or not it is stopped, as bash does. */
    job_continue(&t->job);
    return 1;
}

/* ---- Events --------------------------------------------------------------------- */

static void read_output(Terminal *t, JobEntry *e, size_t budget)
{
    char buf[4096];
    size_t total = 0;
    while (e->job.out_fd >= 0 && total < budget) {
        ssize_t n = job_read(&e->job, buf, sizeof buf);
        if (n <= 0)
            break;
        terminal_feed(t, &e->out, buf, (size_t)n);
        total += (size_t)n;
    }
}

int jobs_poll_fds(const JobTable *jt, struct pollfd *pfd, int max)
{
    int n = 0;
    for (int i = 0; i < jt->n && n < max; i++) {
        if (jt->v[i].job.out_fd < 0)
            continue;
        pfd[n].fd = jt->v[i].job.out_fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }
    return n;
}

int jobs_on_fd(Terminal *t, int fd)
{
    JobTable *jt = &t->jobs;
    for (int i = 0; i < jt->n; i++) {
        JobEntry *e = &jt->v[i];
        if (e->job.out_fd == fd) {
            read_output(t, e, READ_BUDGET);
            return 1;
        }
    }
    return 0;
}

int jobs_on_child_exit(Terminal *t)
{
    JobTable *jt = &t->jobs;
    int printed = 0;
    for (int i = 0; i < jt->n; i++) {
        JobEntry *e = &jt->v[i];
        if (e->state == JOB_DONE)
            continue;
        job_try_reap(&e->job);
        if (job_done(&e->job)) {
            /* Everything it wrote is in the pipe now. Read it, then close
             * the pipe even if something it started still holds it open. */
            read_output(t, e, MAX_DRAIN_BYTES);
            job_close_output(&e->job);
            if (e->out.len > 0)
                terminal_flush_line(t, &e->out);
            printed = 1;
            e->state   = JOB_DONE;
            e->changed = 1;
        } else if (job_stopped(&e->job) && e->state == JOB_RUNNING) {
            /* Stopped by a signal from elsewhere (or tty access). */
            e->state   = JOB_STOPPED;
            e->changed = 1;
            e->seq     = ++jt->seq;
        } else if (!job_stopped(&e->job) && e->state == JOB_STOPPED) {
            /* Continued from elsewhere (kill -CONT). */
            e->state   = JOB_RUNNING;
            e->changed = 0;
        }
    }
    return printed;
}
