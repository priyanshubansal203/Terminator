/*
 * eval.c - Running a parsed command line: command substitutions innermost
 * first, then the line itself, without ever blocking the event loop.
 */
#include "eval.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtins.h"
#include "expand.h"
#include "redirect.h"
#include "terminal.h"

int eval_busy(const Eval *ev)
{
    return ev->nframes > 0;
}

int eval_in_subst(const Eval *ev)
{
    return ev->nframes > 1;
}

/* Points `f` at `node`, with room for the results of its substitutions. */
static int frame_set_node(EvalFrame *f, const RawPipeline *node, int base_status)
{
    f->node = node;
    f->base_status = base_status;
    f->next = 0;
    f->outputs = NULL;
    f->statuses = NULL;
    if (node->nsubsts > 0) {
        f->outputs  = calloc((size_t)node->nsubsts, sizeof *f->outputs);
        f->statuses = calloc((size_t)node->nsubsts, sizeof *f->statuses);
        if (!f->outputs || !f->statuses) {
            free(f->outputs);
            free(f->statuses);
            f->outputs = NULL;
            f->statuses = NULL;
            return -1;
        }
    }
    return 0;
}

static void frame_clear_node(EvalFrame *f)
{
    for (int i = 0; i < f->next; i++)
        free(f->outputs[i]);
    free(f->outputs);
    free(f->statuses);
    f->outputs = NULL;
    f->statuses = NULL;
    f->next = 0;
}

static int push_frame(Eval *ev, const RawPipeline *node, int base_status)
{
    if (ev->nframes == ev->cap) {
        int cap = ev->cap ? ev->cap * 2 : 4;
        EvalFrame *grown = realloc(ev->frames, (size_t)cap * sizeof *grown);
        if (!grown)
            return -1;
        ev->frames = grown;
        ev->cap = cap;
    }
    EvalFrame *f = &ev->frames[ev->nframes];
    memset(f, 0, sizeof *f);
    sb_init(&f->done_output);
    f->saved_dir = -1;
    if (frame_set_node(f, node, base_status) != 0)
        return -1;
    ev->nframes++;
    return 0;
}

static void restore_env(const char *name, char *value)
{
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
    free(value);
}

static void pop_frame(Eval *ev)
{
    EvalFrame *f = &ev->frames[--ev->nframes];
    frame_clear_node(f);
    sb_free(&f->done_output);
    /* A substitution runs in a subshell in bash: its cd ends with it. */
    if (f->saved_dir >= 0) {
        if (fchdir(f->saved_dir) != 0)
            perror("terminator: cannot return to the directory before $(...)");
        close(f->saved_dir);
        restore_env("PWD", f->saved_pwd);
        restore_env("OLDPWD", f->saved_oldpwd);
        f->saved_dir = -1;
    }
}

static char *dup_env(const char *name)
{
    const char *v = getenv(name);
    return v ? strdup(v) : NULL;
}

/* Before the first cd in a substitution: remembers where to come back to.
 * Returns 0 or -1 (errno set). */
static int save_dir(EvalFrame *f)
{
    if (f->saved_dir >= 0)
        return 0;
    int fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    f->saved_dir    = fd;
    f->saved_pwd    = dup_env("PWD");
    f->saved_oldpwd = dup_env("OLDPWD");
    return 0;
}

void eval_free(Eval *ev)
{
    while (ev->nframes > 0)
        pop_frame(ev);
    free(ev->frames);
    raw_pipeline_free(ev->line);
    free(ev->text);
    memset(ev, 0, sizeof *ev);
}

/* Ends the top frame, a substitution, handing its output (owned, may be
 * NULL for empty) to the frame below. Its status is the new $?. */
static void deliver(struct Terminal *t, char *output, int status)
{
    Eval *ev = &t->eval;
    pop_frame(ev);
    EvalFrame *f = &ev->frames[ev->nframes - 1];
    f->outputs[f->next]  = output;
    f->statuses[f->next] = status;
    f->next++;
    t->last_status = status;
}

/* Returns the first built-in named in a multi-command pipeline, or NULL. */
static const char *pipeline_builtin(const Pipeline *pl)
{
    for (int i = 0; i < pl->ncmds; i++) {
        const Command *cmd = &pl->cmds[i];
        if (cmd->argc > 0 && is_builtin(cmd->argv[0]))
            return cmd->argv[0];
    }
    return NULL;
}

/* Marks the commands that are built-ins (see Command.builtin). */
static void mark_builtins(Pipeline *pl)
{
    for (int i = 0; i < pl->ncmds; i++) {
        Command *cmd = &pl->cmds[i];
        cmd->builtin = cmd->argc > 0 && is_builtin(cmd->argv[0]);
    }
}

/* A built-in of a pipeline, in its forked child (see job_spawn): the
 * child's copy of the shell runs it as a subshell would, its output going
 * to the pipe and its messages to stderr. */
static int run_builtin_in_child(void *ctx, const Command *cmd)
{
    struct Terminal *t = ctx;
    t->in_child    = 1;
    t->out_fd      = STDOUT_FILENO;
    t->out_capture = 0;
    return builtin_run(t, cmd->argc, cmd->argv, 1);
}

static int run_in_shell(struct Terminal *t, const EvalFrame *f, const Command *cmd, int subshell);

static void spawn_failed(struct Terminal *t, const Pipeline *pl, int *status)
{
    const Command *first = &pl->cmds[0];
    terminal_printf(t, "terminator: %s: %s",
                    first->argc > 0 ? first->argv[0] : "fork", strerror(errno));
    *status = 126;
}

/* Starts `pl` as t->job, the foreground job, in a process group of its own
 * so Ctrl+C / Ctrl+Z reach all of it. Returns 1 if it is running, or 0 with
 * *status set if it could not start. */
static int start_job(struct Terminal *t, const Pipeline *pl, int capture, int *status)
{
    char *cmd = NULL;
    if (!capture && !(cmd = jobs_command_text(t->eval.text ? t->eval.text : "", 0))) {
        terminal_print(t, "terminator: out of memory");
        *status = 1;
        return 0;
    }
    /* The command itself reads what the user types; a substitution, like
     * in bash, would too, but here it reads /dev/null. */
    if (job_spawn(&t->job, pl, JOB_OWN_PGRP | (capture ? JOB_CAPTURE : JOB_STDIN),
                  run_builtin_in_child, t) != 0) {
        spawn_failed(t, pl, status);
        free(cmd);
        return 0;
    }
    sb_clear(&t->captured);
    t->job_active = 1;
    /* Only the command itself (not a substitution) can be suspended. */
    free(t->fg_cmd);
    t->fg_cmd = cmd;
    t->fg_id  = 0;
    return 1;
}

/* `cmd &`: starts the pipeline as a background job. Built-ins have no
 * process to put in the background; like bash's subshell they run without
 * lasting effect. Always finishes the line (returns 0, *status set). */
static int start_background(struct Terminal *t, const EvalFrame *f, const Pipeline *pl, int *status)
{
    const char *builtin = pipeline_builtin(pl);
    if (pl->ncmds == 1 && (pl->cmds[0].argc == 0 || builtin)) {
        *status = run_in_shell(t, f, &pl->cmds[0], 1);
        return 0;
    }

    char *cmd = jobs_command_text(t->eval.text ? t->eval.text : "", 1);
    if (!cmd) {
        terminal_print(t, "terminator: out of memory");
        *status = 1;
        return 0;
    }
    Job job;
    /* Its input pipe stays unused until `fg` brings it to the foreground;
     * reading before that just waits, where bash would stop the job. */
    if (job_spawn(&job, pl, JOB_OWN_PGRP | JOB_STDIN, run_builtin_in_child, t) != 0) {
        spawn_failed(t, pl, status);
        free(cmd);
        return 0;
    }
    *status = jobs_add_background(t, &job, cmd) == 0 ? 0 : 1;
    free(cmd);
    return 0;
}

/* Runs a built-in, or a command that has no words left after expansion.
 * These execute in the shell itself, so their redirections are only checked
 * (files created/truncated, inputs verified), never applied to the shell's
 * own descriptors. A failed redirection skips the command, like bash. */
/* Runs a built-in in the shell with its output where it belongs: the file
 * of its last `>` (redirect_check() has created them all), else, inside a
 * substitution, the captured output; messages still go to the screen. */
static int run_builtin_with_output(struct Terminal *t, const Command *cmd, int subshell)
{
    const char *out_path = NULL;
    for (int i = 0; i < cmd->nredirs; i++)
        if (cmd->redirs[i].type == REDIR_OUT)
            out_path = cmd->redirs[i].path;
    int fd = -1;
    if (out_path) {
        fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) {
            terminal_printf(t, "terminator: %s: %s", out_path, strerror(errno));
            return 1;
        }
    }
    int saved_fd = t->out_fd, saved_capture = t->out_capture;
    t->out_fd = fd;
    t->out_capture = fd < 0 && eval_in_subst(&t->eval);
    int status = builtin_run(t, cmd->argc, cmd->argv, subshell);
    t->out_fd = saved_fd;
    t->out_capture = saved_capture;
    if (fd >= 0)
        close(fd);
    return status;
}

static int run_in_shell(struct Terminal *t, const EvalFrame *f, const Command *cmd, int subshell)
{
    char err[512];
    if (redirect_check(cmd->redirs, cmd->nredirs, err, sizeof err) != 0) {
        terminal_print(t, err);
        return 1;
    }
    /* cd in a substitution changes the directory for the rest of it (the
     * next commands of its list), and pop_frame() undoes it afterwards. */
    if (cmd->argc > 0 && subshell && eval_in_subst(&t->eval) &&
        strcmp(cmd->argv[0], "cd") == 0) {
        if (save_dir(&t->eval.frames[t->eval.nframes - 1]) != 0) {
            terminal_printf(t, "terminator: cd: cannot save current directory: %s",
                            strerror(errno));
            return 1;
        }
        return builtin_run(t, cmd->argc, cmd->argv, 0);
    }
    if (cmd->argc > 0)
        return run_builtin_with_output(t, cmd, subshell);

    /* Nothing to run. A blank line leaves $? alone; otherwise, as in bash,
     * the status is the last substitution's (or 0 if there was none). */
    if (f->node->nsubsts > 0)
        return f->statuses[f->node->nsubsts - 1];
    if (f->node->ncmds == 1 && raw_command_is_blank(&f->node->cmds[0]))
        return t->last_status;
    return 0;
}

/* Runs the expanded top frame. Returns 1 if a Job was started, else 0 with
 * *status set. */
static int run_frame(struct Terminal *t, const EvalFrame *f, const Pipeline *pl, int *status)
{
    int subst = eval_in_subst(&t->eval);
    if (!subst && t->eval.line->background)
        return start_background(t, f, pl, status);
    /* A pipeline runs its built-ins in forked children (run_builtin_in_child). */
    if (pl->ncmds > 1)
        return start_job(t, pl, subst, status);
    const Command *cmd = &pl->cmds[0];
    /* fg makes a job from the table this line's Job, so the line ends when
     * that job ends (or stops again). In a substitution it has no effect. */
    if (cmd->argc > 0 && strcmp(cmd->argv[0], "fg") == 0 && !subst) {
        char err[512];
        if (redirect_check(cmd->redirs, cmd->nredirs, err, sizeof err) != 0) {
            terminal_print(t, err);
            *status = 1;
            return 0;
        }
        return jobs_fg(t, cmd->argc, cmd->argv, status);
    }
    if (cmd->argc == 0 || is_builtin(cmd->argv[0])) {
        *status = run_in_shell(t, f, cmd, subst);
        return 0;
    }
    return start_job(t, pl, subst, status);
}

static void finish_line(struct Terminal *t, int status)
{
    t->last_status = status;
    free(t->fg_cmd);
    t->fg_cmd = NULL;
    t->fg_id = 0;
    t->interrupted = 0;
    eval_free(&t->eval);
}

/* One command of the substitution on top has finished with `status` (its
 * captured stdout, if any, is in t->captured). Runs on with the next
 * command of the list, or, after the last, hands the output of all of them
 * to the frame below. Returns 0, or -1 if the line had to be abandoned. */
static int subst_command_done(struct Terminal *t, int status)
{
    Eval *ev = &t->eval;
    EvalFrame *f = &ev->frames[ev->nframes - 1];
    if (t->captured.len > 0 &&
        sb_append(&f->done_output, t->captured.data, t->captured.len) != 0) {
        terminal_print(t, "terminator: out of memory in command substitution");
        finish_line(t, 1);
        return -1;
    }
    sb_clear(&t->captured);
    t->last_status = status;   /* $? for the next command of the list */

    if (f->node->next) {
        frame_clear_node(f);
        if (frame_set_node(f, f->node->next, status) != 0) {
            terminal_print(t, "terminator: out of memory");
            finish_line(t, 1);
            return -1;
        }
        return 0;
    }

    /* As in bash, trailing newlines of the output are removed. */
    char *out = f->done_output.len > 0 ? sb_take(&f->done_output) : NULL;
    if (out) {
        size_t len = strlen(out);
        while (len > 0 && out[len - 1] == '\n')
            out[--len] = '\0';
    }
    deliver(t, out, status);
    return 0;
}

/* Moves evaluation forward until a Job is running or the line is done. */
static void advance(struct Terminal *t)
{
    Eval *ev = &t->eval;
    while (ev->nframes > 0) {
        EvalFrame *f = &ev->frames[ev->nframes - 1];

        /* Run the next substitution this frame needs, as a frame of its own. */
        if (f->next < f->node->nsubsts) {
            if (push_frame(ev, f->node->substs[f->next], t->last_status) != 0) {
                terminal_print(t, "terminator: out of memory");
                finish_line(t, 1);
                return;
            }
            continue;
        }

        ExpandContext ctx = { f->outputs, f->statuses, f->base_status, NULL, 0 };
        source_params(&t->sources, &ctx.params, &ctx.nparams);
        Pipeline pl;
        char err[512];
        int status;
        if (expand_pipeline(f->node, &ctx, &pl, err, sizeof err) != 0) {
            terminal_print(t, err);
            status = 1;
        } else {
            mark_builtins(&pl);
            int started = run_frame(t, f, &pl, &status);
            pipeline_free(&pl);
            if (started)
                return;
        }

        if (!eval_in_subst(ev)) {
            finish_line(t, status);
            return;
        }
        /* A command that ran nothing (or failed to) outputs nothing. */
        if (subst_command_done(t, status) != 0)
            return;
    }
}

void eval_start(struct Terminal *t, RawPipeline *line, const char *text)
{
    eval_free(&t->eval);

    /* multiWatch takes its commands as typed (see multiwatch.h), so the
     * line is not expanded and its substitutions do not run here. */
    if (multiwatch_is_invocation(line)) {
        int status;
        if (line->background) {
            terminal_print(t, "terminator: multiWatch: cannot run in the background");
            t->last_status = 1;
        } else if (!multiwatch_start(t, line, &status)) {
            t->last_status = status;
        }
        raw_pipeline_free(line);
        return;
    }

    t->eval.line = line;
    t->eval.text = strdup(text);
    if (push_frame(&t->eval, line, t->last_status) != 0) {
        terminal_print(t, "terminator: out of memory");
        finish_line(t, 1);
        return;
    }
    advance(t);
}

void eval_job_finished(struct Terminal *t, int status)
{
    Eval *ev = &t->eval;
    if (!eval_busy(ev))
        return;
    if (!eval_in_subst(ev)) {
        finish_line(t, status);
        return;
    }

    if (subst_command_done(t, status) == 0)
        advance(t);
}

void eval_abort(struct Terminal *t, int status)
{
    if (t->job_active) {
        job_kill(&t->job);
        t->job_active = 0;
    }
    sb_clear(&t->captured);
    finish_line(t, status);
}
