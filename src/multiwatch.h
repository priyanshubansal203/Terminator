/*
 * multiwatch.h - The multiWatch built-in.
 *
 *     multiWatch ["cmd1", "cmd2", ...]
 *
 * runs every command at the same time, each as its own Job with its own
 * output pipe, and prints each command's output as it arrives, in the format
 * from the specification:
 *
 *     "cmd1", <unix_timestamp>:
 *     ----------------------------------------------------
 *     <output of cmd1>
 *     ----------------------------------------------------
 *
 * Like watch, a command is run again after it finishes, on a 2-second grid
 * measured from its first start (a run that takes longer skips to the next
 * slot, it never overlaps itself). It all ends when the user presses Ctrl+C,
 * which kills every running command (and anything they started).
 *
 * Nothing here blocks: the pipes are watched by the main poll() loop
 * (multiwatch_poll_fds), exits arrive through the SIGCHLD self-pipe, and the
 * re-runs and output flushes are deadlines the loop sleeps until
 * (multiwatch_timeout_ms).
 *
 * Output that arrives close together (within MULTIWATCH_GROUP_MS) is printed
 * as one block, so a quick command gives one block per run while a long one
 * shows its output as it comes. A run that prints nothing gives an empty
 * block, so every run is visible.
 *
 * The command list is taken from the words as typed, before the shell expands
 * them (quote removal would lose the item boundaries); each item is then
 * parsed and expanded as a command line of its own.
 */
#ifndef TERMINATOR_MULTIWATCH_H
#define TERMINATOR_MULTIWATCH_H

#include <poll.h>

#include "command.h"
#include "parser.h"
#include "process.h"
#include "strbuf.h"

#define MULTIWATCH_INTERVAL_MS 2000
#define MULTIWATCH_GROUP_MS    200
#define MULTIWATCH_MAX_COMMANDS 32

typedef struct {
    char      *command;      /* as given; shown in every header */
    Pipeline   pl;           /* expanded once: nothing it uses can change */
    Job        job;
    int        running;
    long long  started;      /* monotonic ms: start of the current/last run */
    long long  next_start;   /* when to run again, while not running */
    StrBuf     pending;      /* output read but not printed yet */
    long long  pending_since;/* when `pending` got its first byte, -1 if empty */
    int        printed;      /* blocks printed for the current run */
} WatchSlot;

typedef struct {
    WatchSlot    *slots;     /* NULL while multiWatch is not running */
    int           nslots;
    unsigned long blocks;    /* blocks printed so far (to detect redraws) */
} MultiWatch;

struct Terminal;

/* 1 if the parsed line is a multiWatch command (its first word, unquoted). */
int  multiwatch_is_invocation(const RawPipeline *line);

/* Starts multiWatch for `line`. Returns 1 if it is running; otherwise an
 * error was printed and *status is the value for $?. */
int  multiwatch_start(struct Terminal *t, const RawPipeline *line, int *status);

int  multiwatch_active(const MultiWatch *mw);

/* Ctrl+C: kills all commands, prints ^C and sets $? to 130. */
void multiwatch_interrupt(struct Terminal *t);

/* Adds the output pipes to watch; returns how many were added. */
int  multiwatch_poll_fds(const MultiWatch *mw, struct pollfd *pfd, int max);

/* Call when `fd` (from multiwatch_poll_fds) is ready. Returns 1 if redraw
 * needed. */
int  multiwatch_on_fd(struct Terminal *t, int fd);

/* Call after a SIGCHLD. Returns 1 if redraw needed. */
int  multiwatch_on_child_exit(struct Terminal *t);

/* Milliseconds until the next re-run or output flush, or -1 for none. */
int  multiwatch_timeout_ms(const MultiWatch *mw);

/* Runs whatever is due. Returns 1 if redraw needed. */
int  multiwatch_on_timer(struct Terminal *t);

/* Stops silently (kills and reaps all commands) and frees everything. */
void multiwatch_free(MultiWatch *mw);

#endif
