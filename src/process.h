/*
 * process.h - Running external commands and pipelines as child processes.
 *
 * A Job is one pipeline: a child per command, with each command's stdout
 * piped into the next one's stdin. The last command's stdout and every
 * command's stderr go to one shared output pipe, so their output stays in the
 * order it was written; the parent reads the other end without blocking.
 * For a command substitution the last command's stdout goes to a separate
 * capture pipe instead, while stderr still goes to the output pipe.
 * An interactive job's first command reads its stdin from an input pipe
 * whose write end Terminator holds, so what the user types can be sent to
 * it (and closing that end is end of input); other jobs read /dev/null.
 * Child exits, stops and continues are reported through a SIGCHLD self-pipe,
 * so the event loop can wait for them with poll() alongside the X connection.
 *
 * For job control every interactive Job runs in a process group of its own
 * (JOB_OWN_PGRP), led by its first command: Ctrl+C / Ctrl+Z are X key events,
 * so the shell delivers SIGINT / SIGTSTP itself with kill(-pgid, ...), which
 * reaches every process of the pipeline (and anything they started).
 */
#ifndef TERMINATOR_PROCESS_H
#define TERMINATOR_PROCESS_H

#include <stddef.h>
#include <sys/types.h>

#include "command.h"

typedef struct {
    pid_t pid;
    int   reaped;   /* 1 once waitpid() has collected the child */
    int   status;   /* raw wait status, valid when reaped */
    int   stopped;  /* 1 while stopped (WIFSTOPPED seen, no WIFCONTINUED since) */
    int   stop_sig; /* the signal that stopped it, valid while stopped */
} Process;

typedef struct {
    Process *procs;   /* one per pipeline command, in order */
    int      nprocs;
    int      out_fd;  /* read end of the output pipe, -1 once closed */
    int      cap_fd;  /* read end of the capture pipe, -1 if none or closed */
    int      in_fd;   /* write end of the input pipe (JOB_STDIN), -1 if none
                       * or closed (end of input) */
    int      in_rd;   /* Terminator's copy of its read end, to take back what
                       * the job did not read; -1 if none */
    pid_t    pgid;    /* the job's own process group (JOB_OWN_PGRP), else 0 */
} Job;

/* job_spawn() flags (with job_spawn's `builtin`, commands marked builtin
 * run that function in their child, as bash runs built-ins of a pipeline in
 * a subshell). */
enum {
    JOB_CAPTURE  = 1,   /* last stdout to the capture pipe (command substitution) */
    JOB_OWN_PGRP = 2,   /* run in a new process group, killed as a whole */
    JOB_STDIN    = 4,   /* first command reads from an input pipe (in_fd) */
};

/* Installs the SIGCHLD handler. Returns 0 or -1. */
int     process_init(void);
void    process_cleanup(void);

/* A descriptor that becomes readable whenever a SIGCHLD arrives, for one
 * event loop (each tab has its own); *slot identifies it for closing.
 * Returns -1 if none is left. */
int     process_sigchld_open(int *slot);
void    process_sigchld_close(int slot);

/* Empties it after it became readable. */
void    process_sigchld_drain(int fd);

/* In a child forked to run a tab (process mode): drops the wakeup pipes
 * shared with the parent. */
void    process_after_fork(void);

/* Runs built-in `cmd` in a forked pipeline child, whose stdin, stdout and
 * stderr are already connected, and returns its exit status. */
typedef int (*JobBuiltinFn)(void *ctx, const Command *cmd);

/* Forks and execs every command of `pl` (searched in PATH), connected by
 * pipes. The first command's stdin is the input pipe with JOB_STDIN, else
 * /dev/null; a command's own
 * redirections replace its pipe connections. If a redirection fails that
 * child exits 1 without running its command; if exec fails it exits 127/126.
 * Either way the error is written to the output pipe. A command with no words
 * (only redirections) just applies them and exits 0. `flags` is a mix of
 * JOB_CAPTURE and JOB_OWN_PGRP.
 * Returns 0, or -1 with errno set if pipe()/fork() failed (any children
 * already started are then killed and reaped). */
int     job_spawn(Job *j, const Pipeline *pl, int flags, JobBuiltinFn builtin, void *ctx);

/* In a forked child: closes every descriptor above 2 (but those in `keep`). */
void    process_close_other_fds(void);
void    process_close_fds_except(const int *keep, int nkeep);

/* Non-blocking read from the output (or capture) pipe: returns bytes read
 * (>0), 0 at end of file (the pipe is then closed), or -1 if no data is
 * available now. */
ssize_t job_read(Job *j, char *buf, size_t size);
ssize_t job_read_capture(Job *j, char *buf, size_t size);

/* Closes both output pipes. */
void    job_close_output(Job *j);

/* Writes typed input to the job (non-blocking). Returns bytes written, or -1
 * if the job has no input pipe or it is full. */
ssize_t job_write_input(Job *j, const char *buf, size_t n);

/* End of input: closes the write end of the input pipe. */
void    job_close_input(Job *j);

/* Takes input the job has not read yet back out of its pipe, without
 * blocking. Returns bytes read (0 if none). */
ssize_t job_take_unread_input(Job *j, char *buf, size_t size);

/* Collects every child that has exited and notes those that stopped or
 * continued (waitpid with WNOHANG | WUNTRACED | WCONTINUED). Returns 1 if any
 * child changed state. */
int     job_try_reap(Job *j);

/* 1 once every child has been reaped. */
int     job_done(const Job *j);

/* 1 if no child is running: every one not yet reaped is stopped (and at
 * least one is). */
int     job_stopped(const Job *j);

/* Signal that stopped the job (valid when job_stopped()). */
int     job_stop_signal(const Job *j);

/* Sends `sig` to the job: to its process group with JOB_OWN_PGRP, else to
 * each child not yet reaped. */
void    job_signal(Job *j, int sig);

/* Resumes a stopped job with SIGCONT. */
void    job_continue(Job *j);

/* PID of the last command (what bash prints for `cmd &`). */
pid_t   job_last_pid(const Job *j);

/* Raw wait status of the last command, valid once job_done(). */
int     job_last_status(const Job *j);

/* The pipeline's exit status in shell terms, which is the last command's:
 * its exit code, or 128 + signal number. */
int     job_exit_code(const Job *j);

/* Closes all pipes and frees the children; call once job_done(). */
void    job_free(Job *j);

/* Kills every child still running (and, with JOB_OWN_PGRP, the rest of its
 * process group), reaps them all, then job_free()s. */
void    job_kill(Job *j);

#endif
