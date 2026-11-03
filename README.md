# Terminator

Terminator is a terminal emulator and shell for Linux, written in C. It draws its own X11 window
with tabs. Each tab is an independent shell that supports pipes, I/O redirection, job control,
multi-line input, a shared searchable history, file-name completion and the `multiWatch`
command.

It is the term project for Computing Lab (CS69201). It uses only Xlib, POSIX threads, POSIX
semaphores and the standard C library: no toolkit and no pseudo-terminal library.

How it works inside (event loop, thread and process modes, IPC, signals, data structures) is
described in the design document, [DESIGN.md](DESIGN.md). Demo commands and screenshots for each
task are in [tests/](tests/README.md).

---

## 1. Dependencies

| Needed for | Package (Debian / Ubuntu) | Notes |
|---|---|---|
| Compiling | `build-essential` (gcc, make) | Any C11 compiler works (`make CC=clang`) |
| Xlib headers and library | `libx11-dev` | Pulls in `libx11-6` at run time |
| X11 compiler flags | `pkg-config` | Optional. Without it the Makefile falls back to `-lX11` |
| A display | an X server (Xorg, or XWayland on a Wayland desktop) | Any normal Linux desktop session has one |
| The window font | `xfonts-base` | Usually already installed. Needed only if Terminator says `cannot load a monospace font` |

Install everything on Debian / Ubuntu:

```sh
sudo apt update
sudo apt install build-essential libx11-dev pkg-config
```

On other distributions: `sudo dnf install gcc make libX11-devel pkgconf-pkg-config` (Fedora) or
`sudo pacman -S base-devel libx11` (Arch).

Tested on Ubuntu 26.04 with gcc 15.2 and libX11 1.8.13. It builds there with no warnings under
`-Wall -Wextra -Wpedantic`.

---

## 2. Building

```sh
make            # builds ./terminator (object files go in build/)
make run        # builds, then starts it in thread mode
make clean      # removes build/ and ./terminator
```

The Makefile compiles with
`-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -pthread` and
`pkg-config --cflags x11`. It links with `pkg-config --libs x11` (or `-lX11`) and `-pthread`.

---

## 3. Running

Start Terminator from a desktop session so that `$DISPLAY` is set:

```sh
./terminator                     # thread mode (default)
./terminator useProcess=True     # process mode
```

`useProcess` also accepts `true`/`1` and `False`/`false`/`0`. Any other argument prints a usage
line and exits with status 2.

Both modes look and behave the same to the user. They differ only in how the tabs run:

| | Thread mode (default) | Process mode (`useProcess=True`) |
|---|---|---|
| A tab is… | a thread of the Terminator process | a process forked by the GUI |
| GUI ↔ tab communication | a pair of pipes per tab | a pair of pipes per tab (same messages) |
| Shared history lives in… | process memory, guarded by a `pthread` mutex | an anonymous `MAP_SHARED` mapping, guarded by a process-shared POSIX semaphore |
| A tab crashing… | takes the whole process down | closes only that tab |

In both modes:

- only the GUI talks to the X server;
- every command typed runs as a child process (`fork()` + `execvp()`);
- each tab has its own working directory, environment, scrollback and jobs. `cd`, Ctrl+C and
  `jobs` in one tab never affect another tab;
- the command history and key bindings are shared by all tabs.

The prompt is `user@terminator> ` and the continuation prompt is `> `. `exit` or Ctrl+D closes
the current tab. Terminator quits when its last tab closes, or when the window is closed.

---

## 4. Implemented features

Features are grouped by the tasks in the project specification.

### Task 1: X11 GUI and tabs

- The window is drawn with Xlib (`XOpenDisplay`, `XCreateSimpleWindow`, `XNextEvent`,
  `XDrawString`) into an off-screen pixmap, then copied on screen, which avoids flicker. It
  resizes with the window.
- Each tab keeps its own scrollback buffer (the last 5,000 lines) and the screen is always
  redrawn from it. Command output is read from pipes and shown as it arrives.
- A tab bar at the top shows each tab's directory: `[ 1: ~/Downloads ] [ 2: /tmp ] [ + ]`. A
  hidden tab that printed something is marked `[*2: /tmp ]`. The window title follows the shown
  tab's directory.
- Tabs keep running while hidden. A new tab starts in the shown tab's directory.

| Tab action | Keyboard | Mouse |
|---|---|---|
| New tab | Ctrl+Shift+T | click `[ + ]` |
| Close tab | Ctrl+Shift+W, or `exit` / Ctrl+D in it | middle-click the tab |
| Next / previous tab | Ctrl+Page_Down / Ctrl+Page_Up (also Ctrl+Tab / Ctrl+Shift+Tab) | mouse wheel over the tab bar |
| Go to tab *n* | Alt+1 … Alt+9 | click the tab |

- **Scrolling back:** Shift+Page_Up / Shift+Page_Down by a page, Shift+Up / Shift+Down by a line,
  Shift+Home / Shift+End to the top or bottom, or use the mouse wheel. While scrolled back, a
  note at the top right says that there are more lines below.
- **Mouse selection:** drag with the left button to select text. The selection becomes the X
  PRIMARY selection, and a middle-click pastes PRIMARY, as in xterm.
- **No busy-waiting:** the GUI sleeps in `poll()` on the X connection and the tab pipes. Each
  tab sleeps in its own `poll()` on GUI messages, its command pipes, a SIGCHLD self-pipe and the
  next multiWatch deadline. An idle Terminator uses no CPU.

### Task 2: Built-in and external commands

External commands are searched in `$PATH` and run with `fork()` + `execvp()`. An unknown command
prints `terminator: NAME: command not found` (status 127) and the tab stays usable. `$?` holds
the last exit status (128 + *signal* for a command killed by a signal).

| Built-in | What it does |
|---|---|
| `cd [DIR]` | Changes this tab's directory (`$HOME` without an argument) and updates `PWD` / `OLDPWD` |
| `exit [N]` | Closes the tab with status N (default `$?`). With running or stopped jobs it warns once; a second `exit` in a row closes the tab anyway |
| `history [N]` / `history -c` | Shows the last 1,000 (or N) commands, numbered / clears the history |
| `multiWatch ["cmd1", "cmd2", ...]` | See Task 6 |
| `jobs [-l\|-p] [%job...]`, `fg [%job]`, `bg [%job...]` | Job control, see Task 8 |
| `source FILE [ARG...]` (also `.`) | Runs FILE's commands in this tab, one line at a time, with `$1`… and `$#` set to the ARGs |
| `bind ...` | Shows or changes key bindings, see Task 7 |

Built-ins work with redirection and in pipelines (`history | grep gcc`, `jobs > jobs.txt`). In a
pipeline they run in a forked child, as bash runs them in a subshell. A `cd` inside `$(...)`
does not change the tab's directory.

### Task 3: Multi-line input and quoting

- If the line is incomplete when Enter is pressed, Terminator shows the `> ` prompt and keeps
  reading. A line is incomplete when it has a trailing `\`, an unclosed `'` or `"` quote, an
  unclosed `$(`, an unclosed backtick or an unclosed `${`.
- Bash quoting rules apply. `'...'` is literal. `"..."` still expands `$VAR`, `${VAR}`, `$?`,
  `$(...)` and backticks. A backslash quotes the next character.
- Command substitution with `$(...)` and backticks can be nested and span several lines. The
  innermost one runs first. A substitution may hold several commands, one per line. Its trailing
  newlines are removed and, when it is unquoted, its output is split into words.
- Expansions also include a leading `~`, glob patterns (`*`, `?`, `[...]`, matched against file
  names and sorted), and `#` comments.
- Operators do not need spaces around them, so `sort<in.txt>out.txt` works.
- Syntax errors are reported before anything runs, for example
  `terminator: syntax error near unexpected token '|'`.

```
user@terminator> echo "Files in $(basename $(pwd)): $(ls | \
> wc -l)"
Files in mydir: 3
```

### Task 4: I/O redirection

- `<` and `>` are supported, alone or together, anywhere in a command. They are applied with
  `open()` + `dup2()` in the child.
- A missing or unreadable input file is reported (`terminator: nothere.txt: No such file or
  directory`) and the command does not run.
- Each background job opens its own copy of the file, so jobs that read the same file at the
  same time do not share an offset and do not interfere with each other.

### Task 5: Pipes

- Pipelines can have any number of commands: N commands use N−1 pipes and N children. Pipes can
  be combined with redirection, for example `cat < a.txt | sort | head -5 > top.txt`.
- All commands of a pipeline run in one process group, so Ctrl+C and Ctrl+Z reach every command.

### Task 6: multiWatch

```
multiWatch ["date +%T", "ls | wc -l"]
```

- Each command runs in its own process, with its own output pipe. All pipes are watched with
  `poll()`.
- Each command is re-run 2 s after its previous start, as `watch` does. A run that takes longer
  than 2 s is never overlapped with the next one.
- Each command's output is printed in the required format:

  ```
  "date +%T", 1790860020:
  ----------------------------------------------------
  18:37:00
  ----------------------------------------------------
  ```

- Each item is a full command line, so it can use pipes, redirection and substitution. Up to
  32 commands are accepted.
- Ctrl+C is caught in the X11 event loop and kills every command and anything they started,
  using `kill()` on their process groups. It prints `^C` and returns to the prompt with
  `$?` = 130.

### Task 7: Hotkeys (configurable)

Default bindings:

| Key | Action (`bind` name) |
|---|---|
| ← / → | Move within the line (`backward-char` / `forward-char`) |
| Ctrl+A or Home / Ctrl+E or End | Start / end of line (`beginning-of-line` / `end-of-line`) |
| ↑ / ↓ | Browse the history (`previous-history` / `next-history`) |
| Ctrl+U / Ctrl+K | Delete to start / end of line (`unix-line-discard` / `kill-line`) |
| Ctrl+W | Delete the previous word (`unix-word-rubout`) |
| Delete | Delete the character under the cursor (`delete-char`) |
| Ctrl+L | Clear the screen (`clear-screen`) |
| Ctrl+D | Close the tab on an empty line; otherwise send EOF to a running command, or delete a character (`end-of-file`) |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste with the X CLIPBOARD selection (`copy` / `paste`) |
| Ctrl+C / Ctrl+Z | Interrupt / suspend (`interrupt` / `suspend`) |
| Ctrl+R | Search the history (`reverse-search-history`) |
| Shift+Page_Up … Shift+End, Ctrl+Shift+T … | Scrolling and tab keys, listed under Task 1 |

Ctrl+Shift+C copies the mouse selection, or the line being typed if nothing is selected.

**Changing bindings.** Bindings are read from `~/.terminatorrc`, or from the file named by
`$TERMINATORRC`, when Terminator starts. [`terminatorrc.example`](terminatorrc.example) lists
every action with its default keys:

```ini
# action = key[, key...]   or   action = none
beginning-of-line = Ctrl+A, Home
copy              = Alt+C
```

A key is written as optional `Ctrl+`, `Alt+` and `Shift+` modifiers followed by one character or
an X key name (`Left`, `Home`, `Delete`, `F5`, `Page_Up`, …).

Bindings can also be changed at run time with the `bind` built-in:

```sh
bind                       # list every action and its keys
bind copy Alt+C, Ctrl+Y    # rebind; takes effect at once and is saved to the config file
bind -d copy               # back to the default (also saved)
bind -r                    # reload the config file
```

Changes need no recompiling and are still in place after a restart. Invalid bindings are
reported in the first tab at startup, or by `bind`, and that action keeps its default. A binding
is invalid if it names an unknown action or key, or uses a printable key without Ctrl or Alt,
or uses Enter, Backspace or Tab without Ctrl or Alt. A key bound to two actions is a conflict: it is reported, and
the customised actions go back to their defaults.

### Task 8: Signals and job control

- Keys arrive as X events, so Terminator sends signals itself. Ctrl+C sends `SIGINT` and Ctrl+Z
  sends `SIGTSTP`, with `kill(-pgid, …)` to the foreground job's process group. Each job runs in
  its own process group.
- Terminator itself ignores `SIGTSTP`, `SIGTTIN`, `SIGTTOU` and `SIGPIPE`, so it never stops.
  Commands get the default signal handlers back before `exec`.
- `cmd &` starts a background job and prints `[n] pid`. Its output keeps appearing in the tab.
  A job that finishes or stops in the background is reported before the next prompt, as in
  bash.
- Ctrl+Z prints `[n]+  Stopped  cmd` and returns to the prompt.
- `jobs` lists jobs; `-l` adds the PIDs and `-p` shows only the PIDs. `fg` brings a job to the
  foreground, continuing it if it was stopped, and `bg` continues a stopped job in the
  background. Jobs can be named as `%n` or `n`, `%%` / `%+` (current), `%-` (previous),
  `%text` (command starts with *text*) or `%?text` (command contains *text*).
- Children are reaped through a SIGCHLD self-pipe, so no zombies are left behind.
- Text typed while a foreground command runs is echoed and sent to the command's standard input
  when Enter is pressed, so `cat`, `sort` and `./a.out` that read input work. Ctrl+D on an empty
  line sends EOF. Keys typed while a command runs that the command does not read are kept and
  used at the next prompt.

```
user@terminator> sleep 100
^Z
[1]+  Stopped                 sleep 100
user@terminator> bg %1
[1]+ sleep 100 &
user@terminator> fg %1
sleep 100
^C
```

### Task 9: Searchable history

- History is shared by all tabs in both modes and holds the last 10,000 commands. Consecutive
  duplicates and blank lines are skipped.
- It is saved in `~/.terminator_history`, or in the file named by `$TERMINATOR_HISTFILE` (an
  empty value turns the file off). The file is loaded at startup, and each new command is
  appended at once under the shared lock. When the file grows well past 10,000 lines, it is
  rewritten with only the newest ones.
- `history` shows the most recent 1,000 commands. ↑ / ↓ browse the history, and an entry added
  meanwhile by another tab shows up.
- Ctrl+R shows `Enter search term: `. When Enter is pressed, it prints one of the following:
  1. the most recent command that exactly matches the search term;
  2. otherwise, every distinct command sharing the longest common substring with the term, newest
     first, when that substring is longer than 2 characters. This uses dynamic programming;
  3. otherwise, `No match for search term in history`.

### Task 10: File-name completion (Tab)

- Tab completes the word before the cursor with file names from the tab's current directory. If
  the word contains `/`, it completes from that directory instead, for example `src/ma`, `../`
  or `~/Do`. The rest of the line is never changed, whether or not the command exists.
- **One match:** it is completed. Directories get a trailing `/`.
- **Several matches:** the word is extended to the matches' longest common prefix, then a
  numbered list such as `1. abc.txt 2. abcd.txt` is shown. Typing a number picks a match. Any
  other key dismisses the list and then does what it normally does.
- **No match:** nothing happens.
- Spaces and special characters in completed names are quoted (`my\ file.txt`). Completion works
  inside an open quote and closes the quote once the name is complete. Hidden files are offered
  only when the word starts with `.`.

---

## 5. Configuration files and environment variables

| What | Default | Override |
|---|---|---|
| Key bindings | `~/.terminatorrc` (a sample is in [`terminatorrc.example`](terminatorrc.example)) | `$TERMINATORRC` |
| History file | `~/.terminator_history` | `$TERMINATOR_HISTFILE` (empty: no file) |

```sh
cp terminatorrc.example ~/.terminatorrc     # then uncomment and edit lines
TERMINATOR_HISTFILE=/tmp/h ./terminator     # e.g. start with a fresh history for testing
```

---

## 6. Known limitations

**It is not a full terminal emulator.** Commands write to pipes, not to a pseudo-terminal.

- Full-screen and terminal-control programs (`vim`, `nano`, `less`, `top`, `htop`, password
  prompts) do not work.
- Terminal escape sequences are not interpreted. Control characters other than newline and tab
  are dropped, so a program that forces colour (`grep --color=always`) shows stray text such as
  `[01;31m`.
- Programs that check for a terminal behave as they do with output to a pipe. For example, `ls`
  prints one name per line.
- Programs that buffer their output when it is not a terminal show it late, in chunks. Use
  `python3 -u` or `stdbuf -oL` to get output line by line.
- Typed input is sent to a running command one line at a time, when Enter is pressed. There is
  no raw, key-by-key mode.

**Shell grammar is limited to what the specification asks for.**

- Only `|`, `<`, `>` and a final `&` are supported. There is no `;`, `&&`, `||` or `( ... )`,
  and `&` in the middle of a line is a syntax error.
- There is no `>>`, `2>`, `2>&1` or here-document. A command's standard error is shown in the tab
  together with its output, in the order they were written.
- `source` runs at the prompt only. It cannot run in a pipeline, inside `$(...)` or in the
  background.

**Display**

- Text is drawn with X core bitmap fonts in ISO-8859-1. UTF-8 output, such as accented letters
  or emoji, is not shown correctly. There is no font or colour configuration.
- Ctrl+L clears the tab's whole scrollback rather than only scrolling it out of view.

**Fixed limits**

- 32 tabs.
- 64 background jobs per tab.
- 32 commands per `multiWatch`.
- 5,000 scrollback lines per tab.
- History entries are cut at 1,023 bytes.
- Tab completion lists at most 100 matches; type more of the name to narrow them down.

**Behaviour that differs from bash**

- Each line of a multi-line command is stored as its own history entry.
- Ctrl+Z suspends only the command itself. A running command substitution or `multiWatch` cannot
  be suspended (Ctrl+C still stops them).
- In thread mode, each tab's shell work, such as parsing, expansion and starting commands, runs
  under one lock. This is needed because the working directory and environment belong to the
  whole process. The commands themselves run in parallel as separate processes, so a long
  command in one tab does not block the others.

---

## 7. Source layout

| File(s) | Responsibility |
|---|---|
| `src/main.c` | Entry point, argument parsing and the GUI's X11 event loop |
| `src/xwin.c`, `src/xsel.c` | Window, font and drawing; X selections for copy and paste (the only code that calls Xlib) |
| `src/tabs.c` | GUI side of tabs: starts threads or processes, tab bar, message pipes |
| `src/tabhost.c` | Shell side of a tab: its own `poll()` loop (same code in a thread or a process) |
| `src/proto.c`, `src/screen.c` | GUI ↔ tab message format; screen snapshots sent to the GUI |
| `src/terminal.c` | One tab's session: line editing, scrollback, scrolling, selection, Ctrl+R, completion |
| `src/parser.c`, `src/expand.c` | Tokenising and parsing (quotes, substitutions, continuation); expansion and globbing |
| `src/eval.c` | Runs a parsed line without blocking, including nested command substitutions |
| `src/process.c`, `src/redirect.c` | `fork`/`execvp`, pipes, `dup2`, process groups, SIGCHLD self-pipe; `<` and `>` |
| `src/jobs.c` | Job table, `jobs` / `fg` / `bg`, background output and notifications |
| `src/builtins.c`, `src/source.c`, `src/multiwatch.c` | `cd`, `exit`, `history`, `bind`; `source`; `multiWatch` |
| `src/history.c` | Shared history ring (mutex or process-shared semaphore) and the history file |
| `src/keymap.c` | Key bindings, config file parsing, conflict checking |
| `src/complete.c` | Tab completion |
| `src/input.c`, `src/textbuf.c`, `src/strbuf.c` | Editable line, scrollback ring buffer, growable string |

Each header (`src/*.h`) starts with a comment describing what its module does and how.
# Terminator
