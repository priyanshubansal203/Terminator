/*
 * tabhost.h - The shell side of a tab: one Terminal running in an event loop
 * of its own, talking to the GUI only through messages (proto.h). It never
 * touches X11. In thread mode each tab's loop runs in a thread; in process
 * mode, in a process forked for it. The loop is the same in both.
 *
 * The loop sleeps in poll() on the messages from the GUI, the tab's SIGCHLD
 * wakeup pipe and the tab's command pipes, and the nearest multiWatch
 * deadline. It sends the GUI what it shows (a Screen, while the tab is
 * shown, at most about 60 times a second), its directory and whether it
 * printed something while hidden, copy / paste requests, and its exit.
 *
 * Process-wide state in thread mode: the working directory and the
 * environment belong to the whole process, but each tab has its own (a cd
 * in one tab must not move another). So a tab thread does its work holding
 * one lock, between switching the process to its directory and environment
 * and recording what it changed; tabs therefore never run shell code at the
 * same time, but each waits for its own events in its own thread. In
 * process mode every tab has a process, and so a directory, of its own.
 */
#ifndef TERMINATOR_TABHOST_H
#define TERMINATOR_TABHOST_H

#include "history.h"

typedef struct TabHost TabHost;

/* Thread mode: tabs share the process, so their work is serialised. */
void     tabhost_set_threaded(int threaded);

/* A tab starting in directory `dir` (with OLDPWD `oldpwd`, NULL for unset),
 * reading the GUI's messages from `cmd_fd` (non-blocking) and writing its
 * own to `ev_fd`. Returns NULL on OOM. */
TabHost *tabhost_new(History *history, const char *dir, const char *oldpwd,
                     int cmd_fd, int ev_fd, int cols, int rows);

/* Runs the tab until its shell ends (`exit`, Ctrl+D) or the GUI closes its
 * end of cmd_fd; then kills its jobs and frees it (closing both fds).
 * Returns the shell's exit code. */
int      tabhost_run(TabHost *h);

/* Frees a tab that was never run (closing both fds). */
void     tabhost_destroy(TabHost *h);

/* Thread mode: runs tabhost_run() in a new thread. Returns 0 or -1. */
int      tabhost_start_thread(TabHost *h);

/* Waits (up to `ms`) for every tab thread to finish. Returns 0 if they did. */
int      tabhost_wait_threads(int ms);

#endif
