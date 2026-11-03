/*
 * source.h - The source (and `.`) built-in: running the commands of a file
 * in the current shell.
 *
 *     source FILE [ARG...]
 *
 * The file is read whole and pushed on a stack of running scripts (a script
 * may source another). Its commands are not run inside the built-in: like a
 * typed line, each one may start a Job that the event loop waits for without
 * blocking. Whenever a line finishes, the terminal calls source_step() to
 * start the next command of the innermost script, and returns to the prompt
 * once no script is left.
 *
 * Commands are read the way typed lines are: a command whose quotes,
 * substitutions or trailing backslash continue on the next lines takes
 * those lines too, and `#` starts a comment. With ARGs, $1, $2, ... and $#
 * are the arguments while the script runs; without, the caller's stay.
 * $? afterwards is the status of the script's last command. A syntax error
 * ends the script (status 2), and Ctrl+C ends every running script.
 */
#ifndef TERMINATOR_SOURCE_H
#define TERMINATOR_SOURCE_H

#include <stddef.h>

/* Nested source calls deeper than this are refused. */
#define SOURCE_MAX_DEPTH 64

typedef struct {
    char  *path;      /* as given, for error messages */
    char  *text;      /* the whole file */
    size_t pos;       /* start of the next unread line */
    int    lineno;    /* lines read so far */
    char **params;    /* $1..$n while it runs, or NULL to keep the caller's */
    int    nparams;
} SourceFrame;

typedef struct {
    SourceFrame *v;
    int          n, cap;
} SourceStack;

struct Terminal;

void source_init(SourceStack *ss);
void source_free(SourceStack *ss);

/* 1 while a script still has commands to run. */
int  source_active(const SourceStack *ss);

/* source FILE [ARG...]: loads the file and queues it. Returns 0, or an error
 * status (message printed). `subshell` (a substitution or `&`) is refused:
 * the script could not run there without blocking the shell. */
int  source_builtin(struct Terminal *t, int argc, char **argv, int subshell);

/* Starts the next command of the innermost script (or finishes a script
 * that has none left). Call only while no line is running. */
void source_step(struct Terminal *t);

/* Abandons every running script (Ctrl+C). */
void source_abort(struct Terminal *t);

/* The positional parameters in effect: $1 is (*params)[0]. */
void source_params(const SourceStack *ss, char *const **params, int *nparams);

#endif
