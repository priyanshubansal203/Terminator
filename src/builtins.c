/*
 * builtins.c - cd, exit, history and bind, plus dispatch to jobs / fg / bg
 * (jobs.c) and source (source.c). multiWatch lives in multiwatch.c.
 */
#include "builtins.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "keymap.h"
#include "parser.h"

/* cd [DIR] - with no argument goes to $HOME. `~` was already expanded by the
 * parser. Updates PWD/OLDPWD so child processes see the new directory. */
static int builtin_cd(Terminal *t, int argc, char **argv)
{
    if (argc > 2) {
        terminal_printf(t, "terminator: cd: too many arguments");
        return 1;
    }
    const char *dir = argc == 2 ? argv[1] : home_dir();

    char old[PATH_MAX];
    int have_old = getcwd(old, sizeof old) != NULL;

    if (chdir(dir) != 0) {
        terminal_printf(t, "terminator: cd: %s: %s", dir, strerror(errno));
        return 1;
    }

    char now[PATH_MAX];
    if (have_old)
        setenv("OLDPWD", old, 1);
    if (getcwd(now, sizeof now))
        setenv("PWD", now, 1);
    return 0;
}

/* Saved PWD/OLDPWD (NULL = unset), restored after a subshell's cd. */
static char *dup_env(const char *name)
{
    const char *v = getenv(name);
    return v ? strdup(v) : NULL;
}

static void restore_env(const char *name, char *value)
{
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
    free(value);
}

/* cd as a subshell runs it: same output and status, but the shell's
 * directory and PWD/OLDPWD are put back afterwards. */
static int builtin_cd_subshell(Terminal *t, int argc, char **argv)
{
    int here = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (here < 0) {
        terminal_printf(t, "terminator: cd: cannot save current directory: %s", strerror(errno));
        return 1;
    }
    char *pwd = dup_env("PWD");
    char *oldpwd = dup_env("OLDPWD");

    int status = builtin_cd(t, argc, argv);

    if (fchdir(here) != 0)
        terminal_printf(t, "terminator: cd: cannot restore directory: %s", strerror(errno));
    close(here);
    restore_env("PWD", pwd);
    restore_env("OLDPWD", oldpwd);
    return status;
}

/* exit [N] - closes Terminator; N (default: last status) is its exit code.
 * In a subshell only the status is produced. */
static int builtin_exit(Terminal *t, int argc, char **argv, int subshell)
{
    if (argc > 2) {
        terminal_printf(t, "terminator: exit: too many arguments");
        return 1;
    }
    int code = t->last_status;
    if (argc == 2) {
        char *end;
        errno = 0;
        long v = strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0') {
            terminal_printf(t, "terminator: exit: %s: numeric argument required", argv[1]);
            code = 2;
        } else {
            code = (int)(v & 0xff);
        }
    }
    if (!subshell && !terminal_request_exit(t, code))
        return 1;
    return code;
}

/* history [N] - the last N entries (default HISTORY_SHOW), numbered;
 * history -c - clears the list. */
#define HISTORY_SHOW 1000

static int builtin_history(Terminal *t, int argc, char **argv)
{
    History *h = t->history;   /* shared by all tabs */
    size_t show = HISTORY_SHOW;
    if (argc > 2) {
        terminal_print(t, "terminator: history: too many arguments");
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        history_clear(h);
        t->hist_browsing = 0;
        free(t->hist_draft);
        t->hist_draft = NULL;
        return 0;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        long n = strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || n < 0) {
            terminal_printf(t, "terminator: history: %s: numeric argument required", argv[1]);
            terminal_print(t, "history: usage: history [-c] [n]");
            return 2;
        }
        show = (size_t)n;
    }
    /* Copy the lines out first: printing can block (a full pipe, as in
     * `history | sleep 9`), and the lock is shared with every tab. */
    history_lock(h);
    size_t count = history_count_locked(h);
    size_t first = count > show ? count - show : 0;
    size_t n = count - first;
    char **lines = calloc(n ? n : 1, sizeof *lines);
    size_t number = history_number_locked(h, first);
    for (size_t i = 0; lines && i < n; i++)
        lines[i] = strdup(history_get_locked(h, first + i));
    history_unlock(h);
    if (!lines) {
        terminal_print(t, "terminator: history: out of memory");
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        if (lines[i])
            terminal_outf(t, "%5zu  %s", number + i, lines[i]);
        free(lines[i]);
    }
    free(lines);
    return 0;
}

static void bind_list(Terminal *t)
{
    for (int a = 0; a < ACT_COUNT; a++) {
        char keys[256];
        keymap_describe((KeyAction)a, keys, sizeof keys);
        terminal_outf(t, "%-18s = %-22s # %s", keymap_action_name((KeyAction)a), keys,
                        keymap_action_help((KeyAction)a));
    }
}

/* Joins argv[from..] with spaces, so `bind copy Ctrl+Y, Alt+W` works
 * unquoted. */
static char *join_args(int argc, char **argv, int from)
{
    StrBuf sb;
    sb_init(&sb);
    for (int i = from; i < argc; i++) {
        if (i > from)
            sb_putc(&sb, ' ');
        sb_append(&sb, argv[i], strlen(argv[i]));
    }
    return sb_take(&sb);
}

/*   bind [-l]                  list the bindings
 *   bind ACTION KEY[, KEY...]  bind (or `none`), saved to the config file
 *   bind -d ACTION             back to the default, saved
 *   bind -r                    reload the config file */
static int builtin_bind(Terminal *t, int argc, char **argv, int subshell)
{
    /* The config file is where all tabs (and, in process mode, their
     * processes) meet: start from what it says now. */
    StrBuf ignored;
    sb_init(&ignored);
    keymap_load(keymap_path(), &ignored);
    sb_free(&ignored);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "-l") == 0)) {
        bind_list(t);
        return 0;
    }
    if (subshell) {
        terminal_print(t, "terminator: bind: bindings cannot be changed in a subshell");
        return 1;
    }
    const char *path = keymap_path();
    if (argc == 2 && strcmp(argv[1], "-r") == 0) {
        StrBuf msgs;
        sb_init(&msgs);
        int problems = keymap_load(path, &msgs);
        if (msgs.len > 0)
            terminal_print(t, sb_str(&msgs));
        sb_free(&msgs);
        terminal_printf(t, "bind: reloaded %s%s", path, problems ? " (with problems, see above)" : "");
        t->keymap_changed = 1;
        return problems ? 1 : 0;
    }

    int reset = strcmp(argv[1], "-d") == 0;
    if ((reset && argc != 3) || (!reset && argv[1][0] == '-') || (!reset && argc < 3)) {
        terminal_print(t, "bind: usage: bind [-l] | bind ACTION KEY[, KEY...] | "
                          "bind -d ACTION | bind -r");
        return 2;
    }
    const char *name = argv[reset ? 2 : 1];
    KeyAction a = keymap_action_by_name(name);
    if (a == ACT_NONE) {
        terminal_printf(t, "terminator: bind: %s: unknown action (see `bind -l')", name);
        return 1;
    }

    char err[512];
    int rc;
    if (reset) {
        rc = keymap_bind_default(a, err, sizeof err);
    } else {
        char *spec = join_args(argc, argv, 2);
        rc = spec ? keymap_bind(a, spec, err, sizeof err) : -1;
        if (!spec)
            snprintf(err, sizeof err, "out of memory");
        free(spec);
    }
    if (rc != 0) {
        char keys[256];
        keymap_describe(a, keys, sizeof keys);
        terminal_printf(t, "terminator: bind: %s; %s stays %s", err, name, keys);
        return 1;
    }
    char keys[256];
    keymap_describe(a, keys, sizeof keys);
    t->keymap_changed = 1;   /* the GUI reloads the file */
    if (keymap_save(path, a, err, sizeof err) != 0) {
        terminal_printf(t, "terminator: bind: %s = %s for now, but not saved: %s", name, keys, err);
        return 1;
    }
    terminal_outf(t, "%s = %s (saved in %s)", name, keys, path);
    return 0;
}

int is_builtin(const char *name)
{
    static const char *const NAMES[] = {
        "cd", "exit", "multiWatch", "jobs", "fg", "bg", "bind", "history",
        "source", ".",
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (strcmp(name, NAMES[i]) == 0)
            return 1;
    return 0;
}

int builtin_run(Terminal *t, int argc, char **argv, int subshell)
{
    if (strcmp(argv[0], "cd") == 0)
        return subshell ? builtin_cd_subshell(t, argc, argv) : builtin_cd(t, argc, argv);
    if (strcmp(argv[0], "exit") == 0)
        return builtin_exit(t, argc, argv, subshell);
    if (strcmp(argv[0], "multiWatch") == 0) {
        /* A typed multiWatch line is started by eval.c before expansion;
         * reaching here means it came from an expansion or a $(...). */
        terminal_print(t, "terminator: multiWatch: must be typed as the command itself");
        return 2;
    }
    if (strcmp(argv[0], "jobs") == 0)
        return jobs_builtin_jobs(t, argc, argv);
    /* A typed fg is run by eval.c; here it is in a subshell or background,
     * which has no jobs to control. */
    if (strcmp(argv[0], "fg") == 0 || (strcmp(argv[0], "bg") == 0 && subshell)) {
        terminal_printf(t, "terminator: %s: no job control", argv[0]);
        return 1;
    }
    if (strcmp(argv[0], "bg") == 0)
        return jobs_builtin_bg(t, argc, argv);
    if (strcmp(argv[0], "bind") == 0)
        return builtin_bind(t, argc, argv, subshell);
    if (strcmp(argv[0], "history") == 0)
        return builtin_history(t, argc, argv);
    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0)
        return source_builtin(t, argc, argv, subshell);
    return 127;
}
