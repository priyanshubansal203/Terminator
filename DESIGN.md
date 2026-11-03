# Terminator: Design Document

This document explains how Terminator is built and why. It covers the X11 GUI and event loop,
the two execution modes and how shared state is synchronised, process and job management, IPC,
the data structures behind history and completion, signal handling, and other design
decisions. For building, running and the feature list, see [README.md](README.md).

## Contents

1. [Architecture at a glance](#1-architecture-at-a-glance)
2. [X11 GUI and event loop](#2-x11-gui-and-event-loop)
3. [Thread mode vs process mode, and shared state](#3-thread-mode-vs-process-mode-and-shared-state)
4. [Process and job management](#4-process-and-job-management)
5. [IPC: pipes, `dup2` and redirection](#5-ipc-pipes-dup2-and-redirection)
6. [Data structures: history, Ctrl+R, completion, scrollback](#6-data-structures-history-ctrlr-completion-scrollback)
7. [Signal handling](#7-signal-handling)
8. [Other design decisions](#8-other-design-decisions)

---

## 1. Architecture at a glance

```
                              X server
                                 ▲ │   Xlib: drawing out, events in
                                 │ ▼
 ┌─────────────────────────────────────────────────────────────┐
 │ GUI   main.c · xwin.c · xsel.c · tabs.c                     │
 │ sleeps in poll() on: X socket, tab pipes, SIGCHLD wake pipe │
 └───────┬──────────────────────▲──────────────────────────────┘
         │ KEY, MOUSE, PASTE,   │ SCREEN, STATUS, OWN,
         │ RESIZE, SHOW, PRINT  │ PASTE_REQ, KEYMAP, EXIT
         ▼    two pipes per tab │
 ┌──────────────────────┐  ┌──────────────────────┐
 │ Tab 1                │  │ Tab 2                │  thread mode:  threads of the GUI process
 │ tabhost.c (loop)     │  │ ...                  │  process mode: processes forked by the GUI
 │ terminal.c (session) │  │                      │
 │ poll(): GUI pipe,    │  │                      │
 │ SIGCHLD pipe, jobs   │  │                      │
 └──────┬───────────────┘  └──────────────────────┘
        │ fork + execvp, pipe, dup2
        ▼
  ┌──────────────── process group (one per job) ─────────────────┐
  │  cmd1  ──pipe──▶  cmd2  ──pipe──▶  cmd3  ──▶ output pipe ──▶ tab │
  └─────────────────────────────────────────────────────────────────┘

  Shared by all tabs: command history (locked), key bindings.
```

Terminator has three layers:

| Layer | Code | Job |
|---|---|---|
| GUI | `main.c`, `xwin.c`, `xsel.c`, `tabs.c`, `screen.c` (drawing) | Owns the X connection. Turns X events into messages for the shown tab and draws the screens the tabs send back. Manages the tab bar and X selections. |
| Tab (one per tab) | `tabhost.c` around a `Terminal` (`terminal.c`) | An independent shell: line editing, scrollback, parsing, expansion, jobs, history and completion. It never touches X. |
| Commands | children forked by a tab | One process per pipeline command, in one process group per job. |

Three rules shape the design:

1. **Only the GUI talks to the X server.** Tabs get keys as plain structs and send back plain
   screen snapshots.
2. **Nothing blocks.** The GUI and every tab sleep only in `poll()`. Child exits arrive through a
   SIGCHLD self-pipe. Timed work, such as multiWatch re-runs, is a `poll()` timeout. No component
   ever calls a blocking `waitpid()` while a command runs, and nothing busy-waits.
3. **One code path for both modes.** Thread mode and process mode run the same tab loop and
   exchange the same messages over the same pipes. The only differences are how a tab is
   started and which lock protects the shared history.

---

## 2. X11 GUI and event loop

### 2.1 Window and drawing (`xwin.c`)

- `XOpenDisplay` connects to the display, and `XCreateSimpleWindow` creates an 800×500 window.
  It selects `Exposure`, `KeyPress`, `StructureNotify`, `ButtonPress`, `ButtonRelease` and
  `Button1Motion` events, and registers for `WM_DELETE_WINDOW` so that closing the window is a
  normal event.
- The font is a monospace X core font. It tries
  `-misc-fixed-medium-r-normal--15-*-*-*-c-*-iso8859-1`, then `9x15`, then `fixed`. The cell
  size comes from `max_bounds.width` and the font's ascent and descent.
- **Double buffering:** everything is drawn with `XDrawString` into a `Pixmap` the size of the
  window, then copied on screen with one `XCopyArea` (`xwin_present`). This prevents flicker
  while output streams in. The pixmap is recreated on `ConfigureNotify` when the size changes.
- The cursor and the mouse selection are drawn in reverse video: a rectangle is filled in the
  foreground colour (`XFillRectangle`), then the text is drawn on it in the background colour.
- The window is split into a one-row tab bar at the top and the text grid below it.
  `xwin_cols()` and `xwin_rows()` give the grid size.

### 2.2 The event loop (`main.c`)

```
loop:
    while XPending():            XNextEvent() → handle_event()
    close tabs that exited or crashed
    if dirty:                    render(); XFlush()
    poll([ X socket,
           SIGCHLD wake pipe              (process mode: tab processes ending),
           each tab: from_tab  POLLIN,
                     to_tab    POLLOUT    (only while messages are queued for it) ],
         timeout = -1)
    read and handle tab messages; write queued messages that now fit
```

- **Draining `XPending()` before `poll()`:** Xlib reads events from the socket into its own
  queue. Calling `poll()` on the socket alone could therefore sleep while events are already
  waiting in that queue.
- **Infinite timeout:** the GUI has no timers. All timed work lives in the tabs. An idle
  Terminator uses no CPU.
- **The GUI never blocks on a tab:** its pipe ends are non-blocking. Messages for a tab go into
  that tab's `outbox`, and the GUI asks for `POLLOUT` only while the outbox is non-empty. A busy
  or stuck tab therefore never freezes the window or the other tabs.
- **Coalescing:** the window is redrawn at most once per loop iteration, only when something
  changed (`dirty`). Pointer motion during a drag is compressed with
  `XCheckTypedWindowEvent`, so only the latest position is sent.

### 2.3 Input path

```
KeyPress ─▶ keymap_translate(): XLookupString() → text + keysym; modifiers; keymap_lookup() → action
         ─▶ new / close / next / previous tab, Alt+1…9?  handled in the GUI
         ─▶ otherwise MSG_KEY(TermKey) to the shown tab
```

- `TermKey` (`termkey.h`) holds the action, keysym, modifier mask and typed text. It contains no
  X types, so it can be copied through a pipe.
- Keys are translated in the GUI because only the GUI has the X keyboard mapping. In process
  mode, a tab process has no X connection at all.
- Clicks on the tab bar are handled in the GUI. Mouse events over the text area are converted to
  grid cells (`xwin_cell_at`) and sent to the tab as `MSG_MOUSE`.

### 2.4 Output path: screen snapshots

A tab does not send its output text to the GUI. It sends what it **shows**: a `TermScreen`
(`screen.h`). That is the `cols × rows` character grid, each row's length, the cursor cell, the
reverse-video span of each row (the selection) and the "more lines below" note shown while
scrolled back. The snapshot is serialised flat (ints and bytes) into a `MSG_SCREEN` message.

- A tab sends screens only while it is shown, at most every 16 ms (about 60 per second). If
  output keeps arriving, a `dirty` flag and a short `poll()` timeout make sure the latest state
  is sent once the interval has passed. A command that prints megabytes therefore does not
  flood the GUI pipe.
- The GUI keeps the last screen of every tab and draws the shown one. Switching tabs sends
  `MSG_SHOW 0` to the old tab and `MSG_SHOW 1` to the new one, which replies with a fresh
  screen.
- A hidden tab that produces output sends `MSG_STATUS` with `activity = 1` once, and the bar
  shows `[*2: …]`. `MSG_STATUS` also carries the tab's directory, used for its label and the
  window title.

**Why snapshots instead of forwarding raw output:**

- The GUI stays a thin X layer.
- Scrollback, line wrapping, selection and scrolling live in one place, the `Terminal`.
- The GUI's cost per tab is bounded by the screen size, not by how much a command prints.

### 2.5 Copy and paste (`xsel.c`)

- **Copy:** the tab sends `MSG_OWN(which, text)`. The GUI calls `XSetSelectionOwner` and keeps
  the text. When another client asks for it with a `SelectionRequest`, the GUI answers
  `TARGETS`, `UTF8_STRING` or `STRING` by writing the property on the requestor and sending
  `SelectionNotify`. A mouse selection sets PRIMARY. Ctrl+Shift+C sets CLIPBOARD.
- **Paste:** the tab sends `MSG_PASTE_REQ(which)`. If Terminator owns that selection, the GUI
  uses its own copy of the text at once. Otherwise it calls `XConvertSelection` and remembers
  which tab asked. The text arrives later as a `SelectionNotify` and is sent to that tab as
  `MSG_PASTE`. The GUI never waits for another X client.
- The timestamp of the latest user input event is used for selection requests, as ICCCM
  requires.

### 2.6 Resizing and closing

`ConfigureNotify` recreates the back buffer and sends `MSG_RESIZE(cols, rows)` to every tab, and
each tab re-wraps its scrollback. `WM_DELETE_WINDOW` ends the loop, and `tabs_free()` shuts the
tabs down (see §7.4).

---

## 3. Thread mode vs process mode, and shared state

### 3.1 What both modes share

`tabs_open()` is the only place where the two modes differ:

```c
if (process_mode) {
    pid = fork();
    if (pid == 0) {                         /* the tab's own process */
        process_after_fork();               /* drop the GUI's SIGCHLD wake pipes */
        signal(SIGINT, SIG_IGN);            /* console Ctrl+C belongs to the GUI */
        process_close_fds_except(keep, 2);  /* everything except its two pipes, incl. the X socket */
        _exit(tabhost_run(tabhost_new(...)));
    }
} else {
    tabhost_start_thread(tabhost_new(...)); /* a detached pthread */
}
```

Everything after that is the same in both modes:

- the tab loop (`tabhost_run`) and the `Terminal` it drives;
- the message protocol over two pipes per tab;
- how commands are forked, and how jobs are tracked and signalled.

### 3.2 Thread mode

Each tab is a detached pthread running `tabhost_run()`. Each tab's state (its `Terminal`,
scrollback, jobs and pipes) belongs to its own thread. Three process-wide resources need care.

**The working directory and environment.** These belong to the whole process, but each tab needs
its own: `cd` in tab 1 must not move tab 2. A forked command also inherits the process's
directory, so whenever a tab forks, the process must be in *that tab's* directory.

The solution is one "big lock" (`g_big` in `tabhost.c`):

```
tab thread:
    loop:
        poll(GUI pipe, SIGCHLD pipe, job pipes, timeout)      ← without the lock
        lock(g_big)
            tab_enter():  fchdir(tab's saved dir fd); set PWD / OLDPWD
            handle messages, job output, child exits, timers   (may fork commands)
            tab_leave():  if "." changed (compared by st_dev, st_ino): reopen the dir fd, getcwd();
                          record PWD / OLDPWD
        unlock(g_big)
        send updates to the GUI                                ← without the lock
```

- Tabs therefore never run shell code at the same time, but each tab waits for its own events
  in its own thread. The lock is held only while there is work to do, never while waiting.
  Commands run in parallel because they are separate processes.
- The directory is kept as an open directory fd, so `fchdir()` still works if the path is
  renamed or longer than `PATH_MAX`. The path is a fallback for directories that can be entered
  but not opened.
- `PWD` and `OLDPWD` are the only environment variables the shell changes, so those two are
  swapped.
- Messages to the GUI are written after releasing the lock. A GUI that is slow to read can make
  this tab wait, but never the other tabs.

**Signal masks.** Each tab thread blocks `SIGINT` and `SIGTERM`, so those signals, for example
Ctrl+C in the console that launched Terminator, are delivered to the GUI thread, which shuts down
cleanly. `SIGCHLD` may arrive in any thread; its handler wakes every tab (§7.2).

**Forking in a multi-threaded process.** A child gets a copy of every mutex in whatever state it
was at fork time. Most children `exec` at once and are unaffected. However, a built-in inside a
pipeline (`history | grep x`) runs shell code in the child. The history mutex and the keymap lock
therefore have `pthread_atfork` handlers that lock before `fork()`, unlock in the parent and
re-initialise in the child. That way a child never inherits a lock held by another thread.

### 3.3 Process mode

The GUI forks one process per tab (see the snippet in §3.1).

- The tab process closes every inherited descriptor except its two pipes, listing them from
  `/proc/self/fd`. Above all, this closes the X socket: the tab must never use the parent's Xlib
  connection.
- It gives up the GUI's SIGCHLD wake pipes and opens its own.
- It ignores `SIGINT`, because console Ctrl+C is the GUI's business.
- The GUI keeps a list of tab PIDs. Its own SIGCHLD wake pipe makes it call
  `waitpid(pid, WNOHANG)` for each one (`tabs_reap`), so dead tab processes never linger as
  zombies.
- **Crash isolation:** if a tab process dies (a segfault or `kill -9`), the kernel closes its
  pipe ends. The GUI's `read()` sees end of file, marks the tab as `gone`, closes it, and prints
  `terminator: tab N ended unexpectedly` in the tab that is shown. The other tabs have their own
  address spaces. The only memory they share is the history, which is protected against a holder
  that dies while holding the lock (§3.4).

In thread mode, a crash in one tab thread takes the whole process down. Threads cannot be
isolated from each other, which is why the specification asks for process mode.

### 3.4 Shared state and how it is synchronised

| State | Thread mode | Process mode |
|---|---|---|
| Command history | `struct History` in an anonymous `MAP_PRIVATE` mapping, locked by a `pthread_mutex_t` | the same struct in an anonymous `MAP_SHARED` mapping created **before** any tab is forked, locked by an unnamed POSIX semaphore stored inside it (`sem_init(&sem, pshared=1, 1)`) |
| History file | appended under the same lock | appended under the same lock |
| Key bindings | one table in memory, locked by a recursive `pthread_mutex_t` | one table per process. `~/.terminatorrc` is the single source of truth (below) |
| SIGCHLD wake pipes | a pool with `_Atomic` write ends, allocated under a mutex (§7.2) | each process has its own pool |
| Directory, environment, scrollback, jobs | per tab; directory and environment swapped under `g_big` | per process |

**History locking details:**

- `struct History` has a fixed size and contains no pointers: it is a ring of
  `char[10000][1024]` plus indices. The same code therefore works in a private mapping or in a
  shared one. A pointer into one process's `malloc` heap would mean nothing in another tab
  process, so the entries are stored inline. (The mapping itself is at the same address in every
  forked tab, because it is created before the fork.)
- Critical sections are short. `history`, Ctrl+R and ↑ / ↓ copy what they need under the lock
  and then work without it. Nothing that could block, such as printing into a full pipe, is ever
  done while holding the lock.
- **Recovering from a dead lock holder:** unlike a robust `pthread` mutex, a semaphore is not
  released automatically when its holder dies. So the holder's PID is stored atomically next to
  the semaphore, and waiters use `sem_timedwait()` with a 1-second timeout. After a timeout, if
  `kill(holder, 0)` fails with `ESRCH`, the holder is dead. A compare-and-swap on the holder
  field lets exactly one waiter take the lock over. Its later `sem_post()` restores the count. A
  tab that crashes while holding the history lock therefore cannot freeze the others.

**Key bindings:** all key lookups happen in the GUI (§2.3), so the GUI's table is the one that
matters. Running `bind` in a tab does the following:

1. Reloads `~/.terminatorrc`, so it starts from what any other tab last saved.
2. Changes and validates the binding.
3. Rewrites only that action's line in the file.
4. Sends `MSG_KEYMAP`, and the GUI reloads the file.

A new binding therefore takes effect in every tab at once, in both modes, and survives a
restart. In thread mode the table is also shared in memory, under a recursive mutex, because the
public keymap functions call each other.

---

## 4. Process and job management

### 4.1 From typed text to processes

```
Enter ─▶ terminal_submit()
          parse_line()        text → RawPipeline tree  (PARSE_INCOMPLETE → "> " prompt, keep reading)
          eval_start()        runs the tree without blocking (§4.2)
            expand_pipeline() RawPipeline → Pipeline { Command { argv[], redirs[] } … }
            job_spawn()       fork / execvp, pipes, process group (§4.3)
```

- **`RawPipeline`** (`parser.h`) is the unexpanded tree. It holds the commands (each with words
  and redirections), the list of command substitutions in this pipeline (each a `RawPipeline`
  of its own), a `background` flag for a final `&`, and `next` for the commands of a multi-line
  `$(...)`.
- A **`Word`** is a list of parts: literal text, `$NAME`, or substitution #*i*. Each part
  records whether it was quoted. Words stay unexpanded because a substitution's output is only
  known after it has run.
- Syntax errors anywhere in the line, including inside nested substitutions, are found while
  parsing, before anything runs.
- **`Pipeline` / `Command`** (`command.h`) is the expanded form: a `NULL`-terminated `argv`
  ready for `execvp`, plus `<` / `>` redirections in order.

### 4.2 Evaluation without blocking (`eval.c`)

A tab must keep handling events while a command runs: keys (Ctrl+C, Ctrl+Z, input typed for the
command), GUI messages, and output from background jobs. In thread mode, a blocking `waitpid()`
would also hold `g_big` and freeze every tab. So the shell **never waits for a child**.

Evaluation is a stack of frames, one per pipeline being prepared. For
`echo "Files in $(basename $(pwd))"`:

```
frame 0  echo "Files in $(…)"        needs subst #0 → push
frame 1    basename $(pwd)           needs subst #0 → push
frame 2      pwd                     no substitutions → expand, spawn with JOB_CAPTURE, return to poll()
             … SIGCHLD → reaped, pipes drained → eval_job_finished(): output "/home/u/mydir" → frame 1, pop
frame 1    basename /home/u/mydir    complete → expand, spawn with JOB_CAPTURE, return to poll()
             … finished → output "mydir" → frame 0, pop
frame 0  echo "Files in mydir"       complete → expand, spawn (foreground job)
```

- Substitutions run innermost first and left to right. Each frame records the exit status of
  each substitution, so `$?` after a substitution sees its status.
- A `cd` inside `$(...)` is undone when its frame is popped. The frame saved a directory fd and
  `PWD` / `OLDPWD`. This matches bash, which runs substitutions in a subshell.
- `fg` and `multiWatch` also return to the loop rather than finishing at once. The resumed job
  becomes the line's job, or multiWatch takes over the tab until Ctrl+C.

### 4.3 Spawning a pipeline (`process.c: job_spawn`)

For *N* commands, `job_spawn` forks *N* children connected by *N − 1* pipes:

```
                  input pipe          link pipe            link pipe
Terminator ──in_fd──▶ [cmd1] ──stdout──▶ [cmd2] ──stdout──▶ [cmd3] ──stdout──┐
   ▲                    │stderr            │stderr            │stderr       │
   └──── out_fd ◀───────┴──────────────────┴──────────────────┴─────────────┘
                     one shared output pipe   (with JOB_CAPTURE, cmd3's stdout
                                               goes to a separate capture pipe)
```

- **Few descriptors held:** each link pipe is created just before forking its writer. The
  parent closes the write end right after forking the writer and the read end right after
  forking the reader. So the parent holds at most one link plus the output pipe at a time, and
  end of file reaches each reader correctly.
- **No leaked descriptors:** every pipe is created `FD_CLOEXEC`. The child `dup2()`s the ends it
  needs onto 0, 1 and 2, and the copies are not close-on-exec. After `exec`, only 0–2 survive:
  no other tab's pipes, no X socket. `dup_onto()` handles the case where an fd already has the
  target number (`dup2` would then do nothing): it clears `FD_CLOEXEC` instead.
- **One output pipe for stdout and stderr:** output and errors appear in the tab in the order
  they were written, as on a terminal.
- **Input:** an interactive job's first command reads from an input pipe. Terminator keeps the
  write end, to send lines the user types, and a copy of the read end. When the job ends,
  Terminator uses that copy to take back input the job did not read, so it reaches the shell
  instead, as with a tty. Command substitutions read `/dev/null`.
- **Process groups:** with `JOB_OWN_PGRP`, the first child's PID becomes the group ID. Both the
  parent and the child call `setpgid()`, so the group is correct whichever runs first. Then
  `kill(-pgid, sig)` reaches every command of the pipeline and anything they started.
  Terminator's own group is never signalled (an explicit `pgid != getpgrp()` check).
- **In the child before `exec`:** reset the job-control and other signals to `SIG_DFL` (ignored
  dispositions would survive `exec`), empty the signal mask, wire up the descriptors, apply
  redirections (§5.2), then `execvp()`. On failure the child prints
  `terminator: X: command not found` and exits with 127, or prints the error and exits with 126.
- If `pipe()` or `fork()` fails part-way, the children already started are killed and reaped.

### 4.4 Built-ins

| Where | How it runs | Why |
|---|---|---|
| Alone on the line | Inside the tab (`builtin_run`) | It must change the shell's own state (`cd`, `exit`, jobs). Its redirections are only *checked* (`redirect_check`: open, create or truncate, close). The shell's own descriptors are never `dup2`'d; output goes to the file through `t->out_fd`. |
| In a pipeline or with `&` | In the forked child, like bash's subshell | It gets pipe ends like any command. The child first closes every fd above 2, as `exec` would have. |
| Inside `$(...)` | In the tab, with `subshell = 1` | Output goes to the capture buffer. `cd` is undone and `exit` only sets the status. |

### 4.5 The foreground job and the job table (`jobs.c`)

- `Terminal.job` is the **foreground job**: the command itself, or one of its substitutions
  while they run. Its output pipe is polled, and lines typed meanwhile go to its input pipe.
- `JobTable` is a growable array of `JobEntry`: job number, the `Job` (PIDs, pgid, pipes), the
  command text, state (`RUNNING`, `STOPPED` or `DONE`), a `changed` flag, a `seq` stamp, and the
  job's unfinished output line.

| Event | What happens |
|---|---|
| `cmd &` | Spawned in its own group, moved into the table, `[n] pid` printed. The line finishes at once. |
| Ctrl+Z | `kill(-pgid, SIGTSTP)`. When SIGCHLD reports (via `WUNTRACED`) that every live child has stopped, `jobs_stop_foreground` moves the `Job` into the table as the current job, prints `[n]+  Stopped  cmd`, and the prompt returns with `$?` = 148 (128 + SIGTSTP). |
| `fg [%n]` | The entry moves back into `Terminal.job` (`SIGCONT` if stopped) and becomes the line's job. The prompt returns when it finishes or stops again. |
| `bg [%n]` | `SIGCONT` to the group. The state becomes `RUNNING`. |
| Background output | Every job's output pipe is in the tab's `poll()` set. Complete lines are printed as they arrive, and a partial line waits in the entry. |
| Job finishes or stops in the background | Its `changed` flag is set. `jobs_notify()` reports it just before the next prompt, as bash does, then removes `DONE` entries. |
| `exit` / closing the tab | With live jobs, `exit` warns once. Closing kills every job (`SIGKILL` to each group) and reaps them. |

- **Current and previous job (`+` / `-`):** the job with the highest rank is current. A stopped
  job ranks above a running one, and among equals the most recent `seq` wins. `seq` is stamped
  when a job is stopped or put in the background.
- **Reaping:** `job_try_reap()` calls `waitpid(pid, WNOHANG | WUNTRACED | WCONTINUED)` for each of
  the job's own PIDs. It **never calls `waitpid(-1)`**. In thread mode, every tab's children
  belong to the same process, and a wildcard wait in one tab would steal another tab's child.
- When the foreground pipeline has been reaped, its pipes are drained and closed even if a
  background grandchild still holds the write end open. The prompt returns when the pipeline
  itself exits.

### 4.6 multiWatch (`multiwatch.c`)

- The command list is taken from the **raw** words of the line, before quote removal, which
  would lose the boundaries between items. Each item is then parsed and expanded once into a
  `Pipeline` of its own, so items may contain pipes and redirections.
- Each command has its own `WatchSlot`: its own `Job` (own process group, own output pipe), its
  run times and its unprinted output. All the slots' pipes are in the tab's `poll()` set. This
  is the "one pipe per command, wait on all read ends" design the specification asks for.
- **Schedule:** runs are placed on a 2-second grid from the first start:
  `next = started + ⌈(finished − started) / 2000⌉ · 2000` ms. A run that overruns skips to the
  next slot and never overlaps itself. The earliest deadline becomes the `poll()` timeout
  (`terminal_timeout_ms`), so there is no `sleep()` and no busy-wait.
- **Output:** bytes that arrive within 200 ms of each other are printed as one block in the
  required format: `"cmd", <time(NULL)>:`, a dashed line, the output, a dashed line. A run that
  prints nothing still produces an (empty) block.
- **Ctrl+C:** the key reaches the tab as an X event (§7.1). `multiwatch_interrupt()` prints any
  output already received, `SIGKILL`s every slot's process group and reaps it, prints `^C`, and
  sets `$?` = 130. The tab carries on.

### 4.7 `source` (`source.c`)

A stack of `SourceFrame`s holds each file's text, read position, line number and positional
parameters. After each line finishes, `source_step()` passes the next complete command to
`eval_start()`. A complete command may span several lines if quotes or a trailing backslash
continue it, exactly as at the prompt. A script's commands therefore run under the same
non-blocking model as typed ones, and Ctrl+C abandons every running script.

---

## 5. IPC: pipes, `dup2` and redirection

### 5.1 Every channel at a glance

| Channel | From → to | Mechanism | Blocking behaviour |
|---|---|---|---|
| Commands to a tab | GUI → tab | pipe, framed messages | GUI: non-blocking write plus an outbox. Tab: non-blocking read. |
| Updates from a tab | tab → GUI | pipe, framed messages | Tab: blocking write of whole messages, never under `g_big`. GUI: non-blocking read. |
| Pipeline links | cmd *i* → cmd *i + 1* | `pipe()` + `dup2()` | ordinary blocking pipes, as the commands expect |
| Output pipe | every command's stderr, last command's stdout → tab | `pipe()` + `dup2()` | tab reads it non-blocking |
| Capture pipe | last command's stdout → tab (`$(...)`) | `pipe()` + `dup2()` | tab reads it non-blocking |
| Input pipe | tab → first command's stdin | `pipe()` + `dup2()` | tab writes it non-blocking |
| SIGCHLD wakeups | signal handler → each event loop | self-pipe per loop | both ends non-blocking |
| History | all tabs | shared mapping + semaphore (process mode) | short critical sections |

### 5.2 GUI ↔ tab protocol (`proto.c`)

A message is an 8-byte header `{ uint32 type; uint32 len; }` followed by `len` bytes of payload
(at most 16 MiB).

| GUI → tab | Payload | | Tab → GUI | Payload |
|---|---|---|---|---|
| `MSG_KEY` | `TermKey` | | `MSG_SCREEN` | encoded `TermScreen` |
| `MSG_MOUSE` | `TermMouse` | | `MSG_STATUS` | activity flag + directory |
| `MSG_PASTE` | pasted text | | `MSG_OWN` | selection + text to copy |
| `MSG_RESIZE` | cols, rows | | `MSG_PASTE_REQ` | which selection to paste |
| `MSG_SHOW` | shown? | | `MSG_KEYMAP` | (bindings changed) |
| `MSG_PRINT` | text to print | | `MSG_EXIT` | exit code |

- Payloads are raw structs and ints. That is safe because both ends are the same binary on the
  same machine.
- A `MsgReader` gathers bytes from a non-blocking descriptor and returns complete messages.
- A malformed stream, or end of file, means the other side is gone. The GUI then closes the tab,
  and the tab shuts down.
- Thread mode uses pipes too, although the threads share memory. This gives one code path and
  identical behaviour in both modes. It also lets the GUI `poll()` on tabs and the X socket
  together, and keeps each tab's state owned by one thread.

### 5.3 Redirection (`redirect.c`)

- **Parsing:** `<` and `>` are operators whether or not they touch the words around them, so
  `sort<in>out` works. Each operator takes the next word as its target, and redirections keep
  their left-to-right order. A target must expand to exactly one word; otherwise the error is
  `ambiguous redirect`.
- **Applying (`redirect_apply`)** happens in the child, after the pipe ends are wired up, so a
  file redirection overrides a pipe connection, as in bash. Each file is opened with `O_RDONLY`,
  or `O_WRONLY | O_CREAT | O_TRUNC` with mode `0666 & ~umask`. It is then `dup2()`'d onto 0 or 1
  and the original descriptor is closed.
- **Failure:** the child writes `terminator: FILE: No such file or directory` (or the relevant
  error) to stderr, which is the tab's output pipe. It then exits with 1 **without** calling
  `exec`, so a missing input file is reported and the command never runs.
- **Background jobs reading the same file:** each child calls `open()` itself, so each has its
  own open file description and file offset. Two `sort < data.txt &` jobs do not affect each
  other. If the shell opened the file once and shared the descriptor, they would share one
  offset and each would read only part of the file.
- The shell process never `dup2()`s its own 0, 1 and 2. Built-ins that run in the shell only
  check their redirections (§4.4).

---

## 6. Data structures: history, Ctrl+R, completion, scrollback

### 6.1 History (`history.c`)

```c
struct History {
    int             between_processes;
    pthread_mutex_t mutex;                    /* thread mode  */
    sem_t           sem;                      /* process mode */
    _Atomic pid_t   holder;                   /* who holds sem, for dead-holder recovery */
    size_t          start, count;             /* ring: oldest entry, number of entries   */
    size_t          dropped;                  /* entries dropped so far                  */
    char            path[4096];               /* the history file                        */
    size_t          file_lines;
    char            lines[HISTORY_MAX][HISTORY_LINE_MAX];   /* 10,000 × 1,024 bytes      */
};
```

- **Ring buffer:** entry *i* (0 = oldest) is `lines[(start + i) % 10000]`. Adding is O(1). When
  the ring is full, `start` advances and `dropped` increases, so the numbers `history` prints stay
  stable as old entries fall off. A blank line, or a repeat of the newest entry, is not added.
- **Memory:** about 10 MB of address space, but the anonymous mapping is zero-filled on demand,
  so only the pages actually used take memory.
- **File (`~/.terminator_history`):**
  - At startup, it is read with `getline()` and each line is added through the same function, so
    de-duplication and the 10,000 cap apply.
  - Every new command is appended under the lock with a single `write()` on an `O_APPEND`
    descriptor, so lines from different tabs or processes never interleave.
  - When the file has grown past 11,000 lines, it is rewritten from the ring: a temporary file is
    written and then `rename()`d over it, so the file is never left half-written. This keeps the
    file near 10,000 lines while rewriting only about once every 1,000 commands.
- **↑ / ↓ browsing:** each tab keeps its own position (`hist_pos`) and a copy of the line being
  typed (`hist_draft`). It re-reads the entry count on every key, because other tabs may add
  entries or clear the history while this tab is browsing.

### 6.2 Ctrl+R search (`terminal.c: search_history`)

1. The entries are copied out under the lock, and the search runs without it.
2. **Exact match:** scan from newest to oldest and print the first entry equal to the term.
3. **Longest common substring:** otherwise, for each entry compute the length of the longest
   substring it shares with the term (classic dynamic programming):

   ```
   L[i][j] = L[i-1][j-1] + 1   if line[i] == term[j]
           = 0                 otherwise         answer = max L[i][j]
   ```

   Only one row of `|term| + 1` integers is kept, updated from right to left so that `L[i-1][j-1]`
   is still the previous row's value. That is **O(|term|) memory** and
   **O(Σ|line| · |term|) time** over the whole history. The indices of all entries with the best
   length are collected newest first. If that length is more than 2, each distinct entry is
   printed once.
4. Otherwise it prints `No match for search term in history`.

### 6.3 File-name completion (`complete.c`)

```c
typedef struct {
    char  **names;   /* matching names, sorted; directories end in '/' */
    int     n;       /* at most COMPLETE_MAX = 5000 */
    size_t  typed;   /* how many characters of them the word already has */
    int     quote;   /* the word is inside an open ' or " (else 0) */
} Completion;
```

1. **Find the word:** `word_before()` scans the line up to the cursor the way the tokenizer
   would, honouring quotes, backslashes and the operators `| < > &`. It returns the word's
   literal text and whether a quote is open. A word containing `$` is not completed.
2. **Pick the directory:** split the word at its last `/`. With no slash, use the tab's current
   directory. A leading `~` is expanded.
3. **List matches:** read the directory with `opendir()` / `readdir()` and keep names that start
   with the prefix. Dot-files are skipped unless the prefix starts with `.`. Each name is
   checked with `stat()`, and directories get a trailing `/`. The list is sorted with `qsort()`.
4. **Act:**
   - One match: insert its missing characters.
   - Several matches: insert their longest common prefix (one linear pass), print the numbered
     list, and enter *pick* mode.
   - No match: do nothing.
5. **Insert:** `complete_insertion()` only ever **appends** at the cursor. Characters are escaped
   for their context (`\ ` outside quotes, literal inside them), so the rest of the line is never
   changed.
6. **Pick mode** is state in the `Terminal`: `picking`, the kept `Completion`, and the digits
   typed so far. A number is chosen as soon as no further digit could make a valid choice, or
   when Enter is pressed. Any other key leaves pick mode and is then handled normally.

The directory is read fresh on every Tab press rather than cached in a trie. Files change between
presses, a sorted array directly gives both the numbered list and the common prefix, and
directories of a few thousand entries are listed instantly.

### 6.4 Scrollback and the line editor

- **`TextBuffer`** is a ring of up to 5,000 heap-allocated output lines per tab. Positions in the
  text are `TextPos { line number since the session started, offset }`, so a selection or a
  scrolled-back view stays on the same text while old lines drop off and new ones arrive.
- Long lines are wrapped when the snapshot is made, for the current width, so resizing re-wraps
  everything for free.
- **`InputLine`** (a growable buffer plus a cursor) is used for the line being edited, the
  foreground job's unfinished output line, and the line being typed to a running command.

---

## 7. Signal handling

### 7.1 Keyboard signals: Ctrl+C and Ctrl+Z

There is no tty and so no line discipline to turn ^C into `SIGINT`. Keys arrive as X `KeyPress`
events, so Terminator delivers signals itself:

```
X KeyPress (Ctrl+C) ─▶ GUI: keymap → ACT_INTERRUPT ─▶ MSG_KEY ─▶ tab: key_interrupt()
    foreground job:   kill(-pgid, SIGINT)   (+ SIGCONT if it was stopped, so it can act on it)
                      print ^C; the prompt returns when SIGCHLD reports the exit
    multiWatch:       SIGKILL every command's group, reap them, print ^C, $? = 130
    sourced script:   abandon the remaining commands too
    at the prompt:    discard the line being typed, $? = 130
Ctrl+Z ─▶ kill(-pgid, SIGTSTP) ─▶ SIGCHLD (WUNTRACED) ─▶ job moves to the table as Stopped
```

- Every job has its own process group and Terminator is not in it, so these signals can never
  hit Terminator itself.
- A tab only ever signals the groups of its own jobs, so Ctrl+C in one tab cannot touch another
  tab's jobs.

### 7.2 SIGCHLD self-pipe (`process.c`)

Waiting for children with `poll()` needs SIGCHLD turned into something readable.

- The handler is installed with `SA_RESTART` and **without** `SA_NOCLDSTOP`, so stops (Ctrl+Z)
  and continues also wake the loops.
- It is async-signal-safe: it only calls `write()` of one byte to every registered wake pipe,
  and saves and restores `errno`.
- The wake pipes come from a fixed pool of 64 slots. Write ends are stored in `_Atomic int`s,
  which are lock-free and therefore safe to read in a handler. Slots are handed out under a
  mutex.
- Pipes are **never closed** while Terminator runs, only marked free and reused. The handler may
  run at any moment in any thread, and must never write to a descriptor number that has been
  closed and reused for something else.
- Each event loop (every tab, and the GUI in process mode) has its own slot and polls its read
  end. When it becomes readable, the loop drains it and calls `waitpid(pid, WNOHANG | …)` for
  its own children only (§4.5).
- In thread mode, one SIGCHLD wakes every tab. A tab with nothing to reap pays only a few
  non-blocking `waitpid` calls.
- If a wake pipe is full, a wakeup is already pending, so `EAGAIN` from the handler is harmless.

### 7.3 Signal dispositions

| Signal | GUI process | Tab thread (thread mode) | Tab process (process mode) | Commands |
|---|---|---|---|---|
| `SIGINT` | handler sets `g_quit` → clean shutdown | blocked (delivered to the GUI thread) | ignored | default |
| `SIGTERM` | handler sets `g_quit` → clean shutdown | blocked | default | default |
| `SIGTSTP`, `SIGTTIN`, `SIGTTOU` | ignored: **Terminator never stops** | (process-wide: ignored) | ignored (inherited) | default |
| `SIGPIPE` | ignored: writing to a closed tab pipe returns `EPIPE` instead | (ignored) | ignored | default |
| `SIGCHLD` | self-pipe handler | (process-wide handler) | self-pipe handler (own pool) | default |

The `SIGINT` handler is installed without `SA_RESTART`, so `poll()` returns `EINTR` and the loop
sees `g_quit` at once. Before `exec`, every child resets these signals to `SIG_DFL` and clears
its signal mask, because ignored dispositions and blocked masks are inherited across `exec`.

### 7.4 Clean shutdown

Closing the window, closing the last tab, or a `SIGINT` / `SIGTERM` to Terminator leads to
`tabs_free()`:

1. It closes the GUI's ends of every tab's pipes.
2. Each tab sees end of file, `SIGKILL`s and reaps its jobs, and ends.
3. The GUI waits up to 3 s. In thread mode it waits on a condition variable over a thread
   counter. In process mode it runs a `waitpid` loop, then `SIGKILL`s any tab process still
   running.
4. Only then is the history mapping destroyed, when no tab can still be using it.

---

## 8. Other design decisions

- **Pipes, not a pseudo-terminal.** The specification asks for command output to be captured
  through pipes, and Terminator does exactly that. As a result, programs see a pipe rather than
  a terminal: no full-screen programs, no escape sequences, and input is sent one line at a
  time. The README lists these limitations.
- **Keys typed ahead are not lost.** Keys typed while a command substitution runs, when nothing
  reads input, are buffered (up to 256) and replayed at the prompt. Input that a finished
  command did not read is taken back out of its pipe and given to the shell, as a tty would do.
- **Key-binding validation.** A binding with an unknown action or key name is rejected, and so
  is a printable key without Ctrl or Alt (it could no longer be typed), or Enter, Backspace or
  Tab without Ctrl or Alt. The action then keeps its default. A key bound to two actions is a conflict:
  every *customised* action involved goes back to its default. Swapping two keys in the config
  file still works, but taking another action's default key does not.
- **Bash-compatible details where they are cheap:**
  - error messages look like `terminator: X: …`;
  - the exit status is 127 for "not found", 126 for "cannot execute", 128 + *n* for a signal;
  - glob results are sorted using the locale's collation (only `LC_COLLATE` is set, so character
    handling stays in the C locale);
  - jobs are reported just before the next prompt;
  - `exit` warns once when jobs are running or stopped.
- **Fixed limits as named constants:** `TABS_MAX`, `JOBS_MAX`, `MULTIWATCH_MAX_COMMANDS`,
  `TEXTBUF_MAX_LINES`, `HISTORY_MAX`, `HISTORY_LINE_MAX`, `COMPLETE_MAX` and others. Each is a
  single `#define` in the header of the module it belongs to.
- **Documented interfaces:** every module's header starts with a comment describing what it
  does, how, and the reasoning behind it. Those comments are the detailed companion to this
  document.
