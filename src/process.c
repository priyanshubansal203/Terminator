/*
 * process.c - fork()/execvp() of pipelines, with output captured through a
 * pipe.
 */
#include "process.h"

#include "redirect.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* SIGCHLD wakes every event loop (one per tab) through a pipe of its own;
 * the handler writes a byte to each. Pipes are handed out from a fixed pool
 * and never closed while the process runs, only marked free and reused: the
 * handler may run at any moment in any thread, and must never write to a
 * descriptor that has been closed and its number reused. */
#define WAKE_SLOTS 64

/* Atomic (and lock-free, so safe in a signal handler): tab threads set
 * them while the handler may be reading them in another thread. */
static _Atomic int           g_wake_wr[WAKE_SLOTS];   /* write ends, -1 if none */
static int                   g_wake_rd[WAKE_SLOTS];
static int                   g_wake_used[WAKE_SLOTS];
static int                   g_pool_ready;
static pthread_mutex_t       g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    for (int i = 0; i < WAKE_SLOTS; i++) {
        int fd = atomic_load_explicit(&g_wake_wr[i], memory_order_acquire);
        /* If the pipe is full a wakeup is already pending: EAGAIN is fine. */
        if (fd >= 0) {
            ssize_t r = write(fd, "c", 1);
            (void)r;
        }
    }
    errno = saved;
}

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void pool_init(void)
{
    if (g_pool_ready)
        return;
    for (int i = 0; i < WAKE_SLOTS; i++) {
        atomic_store(&g_wake_wr[i], -1);
        g_wake_rd[i] = -1;
        g_wake_used[i] = 0;
    }
    g_pool_ready = 1;
}

int process_init(void)
{
    pthread_mutex_lock(&g_pool_lock);
    pool_init();
    pthread_mutex_unlock(&g_pool_lock);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    /* No SA_NOCLDSTOP: a child stopping (Ctrl+Z) must wake the loop too. */
    sa.sa_flags = SA_RESTART;
    return sigaction(SIGCHLD, &sa, NULL);
}

void process_cleanup(void)
{
    signal(SIGCHLD, SIG_DFL);
}

int process_sigchld_open(int *slot)
{
    pthread_mutex_lock(&g_pool_lock);
    pool_init();
    int fd = -1;
    for (int i = 0; i < WAKE_SLOTS && fd < 0; i++) {
        if (g_wake_used[i])
            continue;
        if (g_wake_rd[i] < 0) {
            int p[2];
            if (pipe(p) != 0)
                break;
            if (set_cloexec(p[0]) || set_cloexec(p[1]) || set_nonblock(p[0]) || set_nonblock(p[1])) {
                close(p[0]);
                close(p[1]);
                break;
            }
            g_wake_rd[i] = p[0];
            atomic_store_explicit(&g_wake_wr[i], p[1], memory_order_release);
        }
        g_wake_used[i] = 1;
        *slot = i;
        fd = g_wake_rd[i];
    }
    pthread_mutex_unlock(&g_pool_lock);
    if (fd >= 0)
        process_sigchld_drain(fd);
    return fd;
}

void process_sigchld_close(int slot)
{
    if (slot < 0 || slot >= WAKE_SLOTS)
        return;
    pthread_mutex_lock(&g_pool_lock);
    g_wake_used[slot] = 0;
    pthread_mutex_unlock(&g_pool_lock);
}

void process_sigchld_drain(int fd)
{
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0)
        ;
}

void process_after_fork(void)
{
    /* The pool's pipes are shared with the parent: give them up and start
     * afresh (the child is single-threaded here). */
    for (int i = 0; i < WAKE_SLOTS; i++) {
        int wr = atomic_exchange(&g_wake_wr[i], -1);
        if (wr >= 0)
            close(wr);
        if (g_wake_rd[i] >= 0)
            close(g_wake_rd[i]);
        g_wake_rd[i] = -1;
        g_wake_used[i] = 0;
    }
    pthread_mutex_init(&g_pool_lock, NULL);
}

/* Creates a pipe whose ends are both close-on-exec. */
static int make_pipe(int fds[2])
{
    if (pipe(fds) != 0)
        return -1;
    if (set_cloexec(fds[0]) != 0 || set_cloexec(fds[1]) != 0) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

/* dup2(fd, target), except that when fd already is `target` (dup2 would then
 * do nothing) its FD_CLOEXEC flag is cleared so it still survives exec. */
static int dup_onto(int fd, int target)
{
    if (fd == target) {
        int flags = fcntl(fd, F_GETFD);
        return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
    }
    return dup2(fd, target) < 0 ? -1 : 0;
}

static int kept(int fd, const int *keep, int nkeep)
{
    for (int i = 0; i < nkeep; i++)
        if (keep[i] == fd)
            return 1;
    return 0;
}

void process_close_fds_except(const int *keep, int nkeep)
{
    /* /proc lists exactly the open descriptors; without it, a loop over the
     * usual range has to do. */
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        for (int fd = 3; fd < 1024; fd++)
            if (!kept(fd, keep, nkeep))
                close(fd);
        return;
    }
    int self = dirfd(d);
    int fds[1024], n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 1024) {
        int fd = atoi(e->d_name);
        if (fd > 2 && fd != self && !kept(fd, keep, nkeep))
            fds[n++] = fd;
    }
    closedir(d);
    for (int i = 0; i < n; i++)
        close(fds[i]);
}

void process_close_other_fds(void)
{
    process_close_fds_except(NULL, 0);
}

/* Runs in the child after fork(); never returns. `in_fd` is the read end of
 * the previous command's pipe, or -1 for /dev/null. `pgid` is the process
 * group to join: 0 starts a new one, -1 stays in Terminator's. */
static void exec_child(int in_fd, int out_fd, int err_fd, pid_t pgid, const Command *cmd,
                       JobBuiltinFn builtin, void *ctx)
{
    char *const *argv = cmd->argv;

    /* The parent makes the same call, so the group is set whichever of
     * the two runs first. */
    if (pgid >= 0)
        setpgid(0, pgid);

    /* Terminator ignores the job-control signals and catches others; an
     * ignored disposition would survive exec, so restore the defaults. */
    static const int SIGNALS[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU,
                                   SIGPIPE, SIGCHLD, SIGTERM };
    for (size_t i = 0; i < sizeof SIGNALS / sizeof SIGNALS[0]; i++)
        signal(SIGNALS[i], SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);

    if (in_fd < 0)
        in_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (in_fd >= 0 && dup_onto(in_fd, STDIN_FILENO) != 0)
        _exit(126);
    /* The duplicates do not inherit FD_CLOEXEC, so only fds 0-2 survive
     * exec; every pipe fd the shell made is closed automatically. */
    if (dup_onto(out_fd, STDOUT_FILENO) != 0 || dup_onto(err_fd, STDERR_FILENO) != 0)
        _exit(126);

    /* File redirections override the pipe connections above, as in bash.
     * They only affect this child; the shell's own fds are never touched. */
    if (redirect_apply(cmd->redirs, cmd->nredirs) != 0)
        _exit(1);
    if (cmd->argc == 0)
        _exit(0);

    /* A built-in in a pipeline: run it here, in this copy of the shell,
     * like bash's subshell; nothing it changes reaches the shell itself.
     * It must not keep the shell's other descriptors (other jobs' pipes,
     * the X connection) open, which exec would have closed. */
    if (cmd->builtin && builtin) {
        process_close_other_fds();
        _exit(builtin(ctx, cmd) & 0xff);
    }

    execvp(argv[0], argv);

    int err = errno;
    if (err == ENOENT && strchr(argv[0], '/') == NULL)
        dprintf(STDERR_FILENO, "terminator: %s: command not found\n", argv[0]);
    else
        dprintf(STDERR_FILENO, "terminator: %s: %s\n", argv[0], strerror(err));
    _exit(err == ENOENT ? 127 : 126);
}

int job_spawn(Job *j, const Pipeline *pl, int flags, JobBuiltinFn builtin, void *ctx)
{
    int n = pl->ncmds;
    int capture  = (flags & JOB_CAPTURE) != 0;
    int own_pgrp = (flags & JOB_OWN_PGRP) != 0;
    j->nprocs = 0;
    j->out_fd = -1;
    j->cap_fd = -1;
    j->in_fd  = -1;
    j->in_rd  = -1;
    j->pgid   = 0;
    j->procs  = calloc((size_t)n, sizeof *j->procs);
    if (!j->procs)
        return -1;

    /* The input pipe. Terminator keeps both ends: the write end to send
     * typed lines, and the read end to take back what the job left unread.
     * Only the write end may be non-blocking: the read end shares its file
     * status with the child's stdin. */
    int in[2] = { -1, -1 };
    if ((flags & JOB_STDIN) && make_pipe(in) != 0) {
        free(j->procs);
        j->procs = NULL;
        return -1;
    }
    if (in[1] >= 0)
        set_nonblock(in[1]);
    j->in_fd = in[1];
    j->in_rd = in[0];

    int out[2], cap[2] = { -1, -1 };
    if (make_pipe(out) != 0) {
        int saved = errno;
        job_free(j);
        errno = saved;
        return -1;
    }
    if (capture && make_pipe(cap) != 0) {
        int saved = errno;
        close(out[0]);
        close(out[1]);
        job_free(j);
        errno = saved;
        return -1;
    }
    /* Where the last command's stdout goes. */
    int last_out = capture ? cap[1] : out[1];

    /* The parent holds at most the shared output pipe plus one link of the
     * chain at a time: each pipe's write end is closed as soon as the command
     * writing to it is forked, its read end once the reader is forked. */
    int prev_read = -1;   /* read end of the pipe from the previous command */
    for (int i = 0; i < n; i++) {
        int last = i == n - 1;
        int next[2] = { -1, -1 };
        if (!last && make_pipe(next) != 0)
            goto fail;

        pid_t pid = fork();
        if (pid < 0) {
            if (!last) {
                int saved = errno;
                close(next[0]);
                close(next[1]);
                errno = saved;
            }
            goto fail;
        }
        if (pid == 0)
            exec_child(i == 0 && in[0] >= 0 ? in[0] : prev_read, last ? last_out : next[1], out[1],
                       own_pgrp ? j->pgid : -1, &pl->cmds[i], builtin, ctx);

        /* The first command leads the group; the others join it. */
        if (own_pgrp) {
            if (i == 0)
                j->pgid = pid;
            setpgid(pid, j->pgid);
        }
        j->procs[i].pid    = pid;
        j->procs[i].reaped = 0;
        j->procs[i].status = 0;
        j->nprocs++;

        if (prev_read >= 0)
            close(prev_read);
        prev_read = -1;
        if (!last) {
            close(next[1]);
            prev_read = next[0];
        }
    }

    /* Keep only the read end, so EOF arrives when every child (and anything
     * they spawned) has closed its copies of the write end. */
    close(out[1]);
    set_nonblock(out[0]);
    j->out_fd = out[0];
    if (capture) {
        close(cap[1]);
        set_nonblock(cap[0]);
        j->cap_fd = cap[0];
    }
    return 0;

fail: {
        int saved = errno;
        if (prev_read >= 0)
            close(prev_read);
        close(out[0]);
        close(out[1]);
        if (capture) {
            close(cap[0]);
            close(cap[1]);
        }
        job_kill(j);
        errno = saved;
        return -1;
    }
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static ssize_t read_pipe(int *fd, char *buf, size_t size)
{
    if (*fd < 0)
        return 0;
    for (;;) {
        ssize_t n = read(*fd, buf, size);
        if (n > 0)
            return n;
        if (n == 0) {
            close_fd(fd);
            return 0;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1;
        /* Any other error: treat the pipe as finished. */
        close_fd(fd);
        return 0;
    }
}

ssize_t job_read(Job *j, char *buf, size_t size)
{
    return read_pipe(&j->out_fd, buf, size);
}

ssize_t job_read_capture(Job *j, char *buf, size_t size)
{
    return read_pipe(&j->cap_fd, buf, size);
}

void job_close_output(Job *j)
{
    close_fd(&j->out_fd);
    close_fd(&j->cap_fd);
}

ssize_t job_write_input(Job *j, const char *buf, size_t n)
{
    if (j->in_fd < 0)
        return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(j->in_fd, buf + done, n - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    return done > 0 || n == 0 ? (ssize_t)done : -1;
}

void job_close_input(Job *j)
{
    close_fd(&j->in_fd);
}

ssize_t job_take_unread_input(Job *j, char *buf, size_t size)
{
    if (j->in_rd < 0)
        return 0;
    /* poll() first: a read on this (blocking) end must not wait. */
    struct pollfd p = { .fd = j->in_rd, .events = POLLIN, .revents = 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN))
        return 0;
    ssize_t n;
    do
        n = read(j->in_rd, buf, size);
    while (n < 0 && errno == EINTR);
    return n > 0 ? n : 0;
}

static int process_try_reap(Process *p)
{
    if (p->reaped)
        return 0;
    int changed = 0;
    /* Several stop/continue reports can be pending; take them all. */
    for (;;) {
        int st;
        pid_t r;
        do
            r = waitpid(p->pid, &st, WNOHANG | WUNTRACED | WCONTINUED);
        while (r < 0 && errno == EINTR);

        if (r == p->pid) {
            if (WIFSTOPPED(st)) {
                changed |= !p->stopped;
                p->stopped  = 1;
                p->stop_sig = WSTOPSIG(st);
                continue;
            }
            if (WIFCONTINUED(st)) {
                changed |= p->stopped;
                p->stopped = 0;
                continue;
            }
            p->reaped  = 1;
            p->stopped = 0;
            p->status  = st;
            return 1;
        }
        if (r < 0 && errno == ECHILD) {
            /* Already collected elsewhere; nothing more to wait for. */
            p->reaped  = 1;
            p->stopped = 0;
            p->status  = 0;
            return 1;
        }
        return changed;
    }
}

int job_try_reap(Job *j)
{
    int any = 0;
    for (int i = 0; i < j->nprocs; i++)
        if (process_try_reap(&j->procs[i]))
            any = 1;
    return any;
}

int job_done(const Job *j)
{
    for (int i = 0; i < j->nprocs; i++)
        if (!j->procs[i].reaped)
            return 0;
    return 1;
}

int job_stopped(const Job *j)
{
    int any = 0;
    for (int i = 0; i < j->nprocs; i++) {
        const Process *p = &j->procs[i];
        if (p->reaped)
            continue;
        if (!p->stopped)
            return 0;
        any = 1;
    }
    return any;
}

int job_stop_signal(const Job *j)
{
    for (int i = 0; i < j->nprocs; i++)
        if (!j->procs[i].reaped && j->procs[i].stopped)
            return j->procs[i].stop_sig;
    return SIGTSTP;
}

void job_signal(Job *j, int sig)
{
    if (job_done(j))
        return;
    /* While one of our children is unreaped the group exists, so its id
     * cannot have been reused; Terminator's own group is never signalled. */
    if (j->pgid > 0 && j->pgid != getpgrp() && kill(-j->pgid, sig) == 0)
        return;
    for (int i = 0; i < j->nprocs; i++)
        if (!j->procs[i].reaped)
            kill(j->procs[i].pid, sig);
}

void job_continue(Job *j)
{
    for (int i = 0; i < j->nprocs; i++)
        j->procs[i].stopped = 0;
    job_signal(j, SIGCONT);
}

pid_t job_last_pid(const Job *j)
{
    return j->nprocs > 0 ? j->procs[j->nprocs - 1].pid : 0;
}

int job_last_status(const Job *j)
{
    return j->nprocs > 0 ? j->procs[j->nprocs - 1].status : 0;
}

int job_exit_code(const Job *j)
{
    int st = job_last_status(j);
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 1;
}

void job_free(Job *j)
{
    job_close_output(j);
    close_fd(&j->in_fd);
    close_fd(&j->in_rd);
    free(j->procs);
    j->procs  = NULL;
    j->nprocs = 0;
}

void job_kill(Job *j)
{
    /* A job in its own process group is signalled as a group, which also
     * stops anything its commands started. The group still exists while one
     * of our children is unreaped, so its id cannot have been reused yet;
     * Terminator's own group is never signalled. */
    if (j->pgid > 0 && j->pgid != getpgrp() && !job_done(j))
        kill(-j->pgid, SIGKILL);

    /* Signal them all first, so no child waits on one not yet killed. */
    for (int i = 0; i < j->nprocs; i++)
        if (!j->procs[i].reaped)
            kill(j->procs[i].pid, SIGKILL);
    for (int i = 0; i < j->nprocs; i++) {
        Process *p = &j->procs[i];
        if (p->reaped)
            continue;
        pid_t r;
        do
            r = waitpid(p->pid, &p->status, 0);
        while (r < 0 && errno == EINTR);
        p->reaped = 1;
    }
    job_free(j);
}
