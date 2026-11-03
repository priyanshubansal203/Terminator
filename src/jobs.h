/*
 * jobs.h - Job control: the table of background and stopped jobs of one
 * terminal session, and the jobs / fg / bg built-ins.
 *
 * The foreground job is t->job (see terminal.h); it moves into this table
 * when it stops (Ctrl+Z), and `fg` moves a job from here back into t->job.
 * A job started with `cmd &` goes straight into the table. Each job keeps
 * its Job (one process per pipeline command, all in one process group whose
 * id is the first command's PID), its number, its command text and state.
 *
 * Background jobs keep running while the shell does other things: their
 * output pipes are watched by the main poll() loop and printed as complete
 * lines arrive, and SIGCHLD reaps them (no zombies). As in bash, a job that
 * finishes or stops in the background is reported just before the next
 * prompt (after the next command line, or by `jobs`), and is then removed
 * from the table if it has finished.
 */
#ifndef TERMINATOR_JOBS_H
#define TERMINATOR_JOBS_H

#include <poll.h>

#include "input.h"
#include "process.h"

/* Jobs `cmd &` may create; one more can appear when a foreground job stops. */
#define JOBS_MAX 64

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE,
} JobState;

typedef struct {
    int           id;        /* job number, as in %1 */
    Job           job;
    char         *cmd;       /* command text as typed, without the `&` */
    JobState      state;
    int           changed;   /* finished or stopped, not yet reported */
    unsigned long seq;       /* when it last became stopped/backgrounded,
                              * to pick the current (+) and previous (-) job */
    InputLine     out;       /* output received after its last newline */
} JobEntry;

typedef struct {
    JobEntry     *v;
    int           n, cap;
    unsigned long seq;
} JobTable;

struct Terminal;

int  jobs_init(JobTable *jt);

/* Kills (SIGKILL to each process group) and reaps every job. */
void jobs_free(JobTable *jt);

/* The text a job is listed with: `line` with newlines turned into spaces
 * and, for a `background` line, without its final `&`. Returns a malloc'd
 * string, or NULL on OOM. */
char *jobs_command_text(const char *line, int background);

/* Takes a just-spawned `cmd &` Job (which is reset) into the table and
 * prints "[n] pid". Returns 0, or -1 (job killed, message printed). */
int  jobs_add_background(struct Terminal *t, Job *job, const char *cmd);

/* Call when the foreground job t->job has stopped: moves it into the table
 * as the current job (keeping its number if it came from `fg`), prints
 * "[n]+  Stopped  cmd" and returns the status for $? (128 + signal). */
int  jobs_stop_foreground(struct Terminal *t);

/* jobs [-l | -p] [%job ...] */
int  jobs_builtin_jobs(struct Terminal *t, int argc, char **argv);

/* bg [%job ...] */
int  jobs_builtin_bg(struct Terminal *t, int argc, char **argv);

/* fg [%job]: moves the job into t->job (sending SIGCONT if stopped) and
 * returns 1; or prints an error, sets *status and returns 0. */
int  jobs_fg(struct Terminal *t, int argc, char **argv, int *status);

/* Prints jobs that finished or stopped since last reported and drops the
 * finished ones. Call before showing a new prompt. */
void jobs_notify(struct Terminal *t);

/* Number of jobs still running or stopped. */
int  jobs_live(const JobTable *jt, int *stopped);

/* Adds the output pipes of all jobs (at most `max`); returns how many. */
int  jobs_poll_fds(const JobTable *jt, struct pollfd *pfd, int max);

/* Call when `fd` (from jobs_poll_fds) is readable. Returns 1 if the fd
 * belonged to a job (and output may have been printed). */
int  jobs_on_fd(struct Terminal *t, int fd);

/* Call after a SIGCHLD: reaps children, notes stops/continues and finished
 * jobs. Returns 1 if something was printed. */
int  jobs_on_child_exit(struct Terminal *t);

#endif
