/*
 * terminal.h - State of one terminal session, i.e. one tab (see tabs.h):
 * scrollback, the line being edited, the foreground job and the job table.
 * Nothing here is shared with other tabs except the command history:
 * Ctrl+C / Ctrl+Z and jobs / fg / bg only ever touch this session's jobs.
 * The session uses the process's working directory and environment as its
 * own; tabs.c switches them to the right tab's before calling in here.
 */
#ifndef TERMINATOR_TERMINAL_H
#define TERMINATOR_TERMINAL_H

#include <poll.h>
#include <X11/X.h>

#include "complete.h"
#include "eval.h"
#include "history.h"
#include "input.h"
#include "jobs.h"
#include "multiwatch.h"
#include "process.h"
#include "screen.h"
#include "source.h"
#include "strbuf.h"
#include "termkey.h"
#include "textbuf.h"

#define TERMINAL_PROMPT "user@terminator> "
#define CONTINUATION_PROMPT "> "
#define SEARCH_PROMPT "Enter search term: "

/* Keys typed while the shell is busy but no command reads input (e.g. while
 * a command substitution runs) are kept and replayed afterwards. */
#define TYPEAHEAD_MAX 256

/* Most file descriptors terminal_poll_fds() asks to watch: the foreground
 * job's two pipes, multiWatch, and every job in the table. */
#define TERMINAL_MAX_POLLFDS (2 + MULTIWATCH_MAX_COMMANDS + JOBS_MAX + 1)

/* A position in the text: line number counted since the session started
 * (so it stays valid as the scrollback grows), and offset in that line. */
typedef struct {
    unsigned long long line;
    size_t             off;
} TextPos;

/* Requests to the GUI, which owns the X selections (see xsel.h). */
typedef enum {
    TERM_SEL_PRIMARY,
    TERM_SEL_CLIPBOARD,
    TERM_SEL_COUNT
} TermSel;

typedef struct Terminal {
    TextBuffer scrollback;
    InputLine  input;
    const char *prompt;
    StrBuf     continued;   /* earlier lines of a command that is not
                             * complete yet (unclosed quote, trailing \ ...) */

    History   *history;     /* shared by all tabs (not owned) */
    int        hist_browsing;/* Up/Down is showing a history entry */
    size_t     hist_pos;    /* the entry shown while browsing */
    char      *hist_draft;  /* the line being typed, kept while browsing */

    /* Ctrl+R: the search term is typed on the input line, after
     * SEARCH_PROMPT; the line and prompt from before come back after. */
    int        searching;
    const char *search_saved_prompt;
    char      *search_saved_input;

    /* Tab: a numbered list of completions is shown and typing its number
     * picks one; any other key carries on as usual. */
    int        picking;
    Completion pick;
    int        pick_number; /* digits typed so far */
    int        pick_shown;  /* how many of the matches are numbered */

    /* The line being run (see eval.h) and its current Job, the foreground
     * job: the command itself or one of its command substitutions. While
     * it runs, the prompt is hidden. */
    Eval       eval;
    Job        job;
    int        job_active;
    char      *fg_cmd;      /* the foreground job's command text, NULL while
                             * a command substitution runs */
    int        fg_id;       /* its job number if `fg` brought it, else 0 */
    int        interrupted; /* Ctrl+C was pressed during this line */
    StrBuf     captured;    /* stdout of a running command substitution */
    MultiWatch watch;       /* a running multiWatch (instead of a line) */
    InputLine  pending;     /* job output received after the last newline */
    InputLine  job_input;   /* line being typed to the foreground job, shown
                             * after `pending` and sent on Enter */
    StrBuf     typeahead_text; /* typed lines a job left unread: the shell
                                * reads them at the prompt, as with a tty */
    TermKey    typeahead[TYPEAHEAD_MAX];
    int        typeahead_len;
    JobTable   jobs;        /* background and stopped jobs */
    SourceStack sources;    /* scripts being run by `source` */
    int        script_abort;/* Ctrl+C: abandon the running scripts */

    /* Lines ever added to the scrollback; the oldest still kept is number
     * lines_total - textbuf_count(). The current line is lines_total. */
    unsigned long long lines_total;

    /* Scrolling back: while `scrolled`, the view's top row is row
     * view_row of line view_line (a TextPos line), so it stays on the same
     * text as output arrives. view_cols/rows is the window's text grid. */
    int        scrolled;
    unsigned long long view_line;
    int        view_row;
    int        view_cols, view_rows;

    /* Mouse selection, as text positions (inclusive), and its text. */
    int        sel_state;   /* 0 none, 1 being dragged, 2 made */
    TextPos    sel_anchor, sel_end;
    char      *sel_text;

    char      *own_request[TERM_SEL_COUNT]; /* text to put on a selection */
    int        paste_request;               /* 1 + TermSel to paste, or 0 */

    /* Where a built-in's output (terminal_out) goes: the screen, or */
    int        out_fd;      /* this descriptor (`> file`, or a pipe), or */
    int        out_capture; /* t->captured (inside $(...)) */
    int        in_child;    /* in a forked pipeline child: messages go to
                             * stderr, as there is no screen here */

    int        keymap_changed; /* `bind` changed the key bindings */

    int        last_status; /* value of $? */
    unsigned long lines_run;      /* command lines submitted so far */
    unsigned long exit_warned_at; /* lines_run when `exit` warned about jobs */
    int        quit_requested;
    int        exit_code;   /* Terminator's exit code once quit_requested */
} Terminal;

/* `history` is the command history, shared with the other tabs. */
int  terminal_init(Terminal *t, History *history);

/* Frees everything; a still-running command is killed and reaped. */
void terminal_free(Terminal *t);

/* Appends a line of output to the scrollback: messages and errors (a
 * command's stderr, in bash terms). */
void terminal_print(Terminal *t, const char *text);
void terminal_printf(Terminal *t, const char *fmt, ...);

/* A line of a built-in's output proper (its stdout): it follows `>` and
 * pipes and is captured by $(...); otherwise it is shown like
 * terminal_print. */
void terminal_out(Terminal *t, const char *text);
void terminal_outf(Terminal *t, const char *fmt, ...);

/* Processes a key press. Returns 1 if the screen needs a redraw. */
int  terminal_handle_key(Terminal *t, const TermKey *key);

/* Appends command output (split into lines, tabs expanded, other control
 * characters dropped), ending with a complete line. */
void terminal_write_output(Terminal *t, const char *buf, size_t n);

/* Adds raw output to the scrollback through `line`, which holds the text
 * after the last newline (t->pending for the foreground job, or a background
 * job's own line); terminal_flush_line() commits that unfinished line. */
void terminal_feed(Terminal *t, InputLine *line, const char *buf, size_t n);
void terminal_flush_line(Terminal *t, InputLine *line);

/* Asks to quit Terminator with `code`, as `exit` and Ctrl+D do. With
 * running or stopped jobs the first request only warns (they would be
 * killed); asking again straight away quits. Returns 1 if quitting. */
int  terminal_request_exit(Terminal *t, int code);

/* Call with the window's text grid size at startup and on every resize;
 * scrolling moves by these rows. */
void terminal_resize(Terminal *t, int cols, int rows);

/* Mouse button press/release/motion (and the wheel, which scrolls).
 * Returns 1 if redraw needed. */
int  terminal_mouse(Terminal *t, const TermMouse *m);

/* Inserts pasted text at the cursor (newlines become spaces, other control
 * characters are dropped). Returns 1 if redraw needed. */
int  terminal_paste(Terminal *t, const char *text, size_t len);

/* 1 while a command line (or one of its substitutions), multiWatch or a
 * sourced script runs. */
int  terminal_busy(const Terminal *t);

/* Fills `pfd` with the pipes to watch for input (at most `max`, which
 * TERMINAL_MAX_POLLFDS always covers); returns how many. */
int  terminal_poll_fds(const Terminal *t, struct pollfd *pfd, int max);

/* Call after poll() with the entries terminal_poll_fds() filled in.
 * Returns 1 if redraw needed. */
int  terminal_on_poll(Terminal *t, const struct pollfd *pfd, int n);

/* Milliseconds until terminal_on_timer() has work to do, or -1 for never. */
int  terminal_timeout_ms(const Terminal *t);

/* Runs timed work (multiWatch re-runs). Returns 1 if redraw needed. */
int  terminal_on_timer(Terminal *t);

/* Call after a SIGCHLD. Returns 1 if redraw needed. */
int  terminal_on_child_exit(Terminal *t);

/* What the terminal shows in its view_cols x view_rows grid. Returns 0 or
 * -1 on OOM. */
int  terminal_snapshot(const Terminal *t, TermScreen *s);

#endif
