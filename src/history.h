/*
 * history.h - The command history, shared by all tabs.
 *
 * It lives in one place, under a lock, in both execution modes:
 *   - thread mode: in the process's memory, locked with a pthread mutex;
 *   - process mode: in an anonymous shared mapping created before the tab
 *     processes are forked, locked with a process-shared POSIX semaphore.
 * The layout is fixed-size (no pointers), so the same code serves both: a
 * ring of HISTORY_MAX lines of up to HISTORY_LINE_MAX - 1 bytes (longer
 * ones are cut). A tab process that dies while holding the semaphore does
 * not block the others: a waiter that times out checks whether the holder
 * still exists and takes the lock over if it does not.
 *
 * It is also kept in a file (history_set_file), as bash keeps
 * ~/.bash_history: loaded when Terminator starts, and every line added is
 * appended at once, under the same lock, so the file follows the shared
 * history whichever tab (thread or process) adds to it. When the file has
 * grown well past HISTORY_MAX lines it is rewritten with the newest ones.
 */
#ifndef TERMINATOR_HISTORY_H
#define TERMINATOR_HISTORY_H

#include <stddef.h>

#define HISTORY_MAX      10000
#define HISTORY_LINE_MAX 1024

typedef struct History History;

/* Creates an empty history; `between_processes` for process mode. Returns
 * NULL on failure. */
History    *history_create(int between_processes);
void        history_destroy(History *h);

/* Keeps the history in the file at `path` (NULL or "": no file): loads its
 * last HISTORY_MAX lines and appends every line added from now on. Call
 * before the tabs start. Returns 0, or -1 if it could not be read (the
 * history then starts empty, and is still written to it). */
int         history_set_file(History *h, const char *path);

/* Adds a line, unless it is blank or repeats the newest entry. The oldest
 * entry is dropped beyond HISTORY_MAX. */
void        history_add(History *h, const char *line);

size_t      history_count(History *h);

/* A malloc'd copy of entry `i` (0 = oldest), or NULL. */
char       *history_dup(History *h, size_t i);

/* Empties the history (and its file). */
void        history_clear(History *h);

/* For going through the entries consistently: between lock and unlock, the
 * _locked functions may be used (and nothing else of this API). */
void        history_lock(History *h);
void        history_unlock(History *h);
size_t      history_count_locked(const History *h);
const char *history_get_locked(const History *h, size_t i);
/* The number `history` shows for entry `i`, counting from 1. */
size_t      history_number_locked(const History *h, size_t i);

#endif
