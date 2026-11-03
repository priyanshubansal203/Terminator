/*
 * eval.h - Running a parsed command line in a terminal session.
 *
 * Command substitutions must run before the command around them can be
 * expanded, innermost first, and each one is an ordinary Job whose stdout is
 * captured. To keep the GUI responsive nothing here waits for a child:
 * evaluation is a stack of frames, one per pipeline being prepared (the line
 * itself at the bottom, the substitution currently needed on top). A frame
 * whose substitutions have all produced their output is expanded and run;
 * when its Job finishes, the event loop calls eval_job_finished(), which
 * hands the output to the frame below and continues.
 *
 * Job control: every Job runs in its own process group. A line ending in
 * `&` is started as a background job (any substitutions in it still run
 * first, in the foreground) and the line is done at once. `fg` does not
 * finish at once either: the job it resumes becomes the line's Job.
 */
#ifndef TERMINATOR_EVAL_H
#define TERMINATOR_EVAL_H

#include "parser.h"
#include "strbuf.h"

typedef struct {
    const RawPipeline *node;
    char **outputs;      /* captured stdout of node->substs[0..next) */
    int   *statuses;     /* and their exit statuses */
    int    next;         /* substitutions done so far */
    int    base_status;  /* $? when this frame started */
    StrBuf done_output;  /* a substitution's list: stdout of the commands
                          * before `node` (see RawPipeline.next) */
    int    saved_dir;    /* a `cd` ran in this substitution: the directory to
                          * go back to when it is done, else -1 */
    char  *saved_pwd, *saved_oldpwd;   /* and PWD / OLDPWD (NULL: unset) */
} EvalFrame;

typedef struct {
    RawPipeline *line;   /* owned; NULL when idle */
    char        *text;   /* the line as typed, for job listings */
    EvalFrame   *frames;
    int          nframes, cap;
} Eval;

struct Terminal;

/* Starts running `line` (taking ownership; `text` is copied). May finish at
 * once (built-ins, errors, background jobs) or leave a Job running in
 * t->job. */
void eval_start(struct Terminal *t, RawPipeline *line, const char *text);

/* Call once t->job has finished (all children reaped, pipes closed) with its
 * exit status; for a substitution, t->captured holds its stdout. */
void eval_job_finished(struct Terminal *t, int status);

/* Abandons the line: kills a running Job and sets $? to `status`. */
void eval_abort(struct Terminal *t, int status);

/* 1 while a line is being evaluated. */
int  eval_busy(const Eval *ev);

/* 1 if the running Job is a command substitution. */
int  eval_in_subst(const Eval *ev);

void eval_free(Eval *ev);

#endif
