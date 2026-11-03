/*
 * source.c - Loading script files and feeding their commands to eval.c one
 * command line at a time.
 */
#include "source.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parser.h"
#include "terminal.h"

/* Larger files are refused; a script this big is almost certainly not one. */
#define SOURCE_MAX_BYTES (16 << 20)

void source_init(SourceStack *ss)
{
    memset(ss, 0, sizeof *ss);
}

static void frame_free(SourceFrame *f)
{
    free(f->path);
    free(f->text);
    for (int i = 0; i < f->nparams; i++)
        free(f->params[i]);
    free(f->params);
}

static void pop(SourceStack *ss)
{
    frame_free(&ss->v[--ss->n]);
}

void source_free(SourceStack *ss)
{
    while (ss->n > 0)
        pop(ss);
    free(ss->v);
    source_init(ss);
}

int source_active(const SourceStack *ss)
{
    return ss->n > 0;
}

void source_abort(Terminal *t)
{
    while (t->sources.n > 0)
        pop(&t->sources);
}

void source_params(const SourceStack *ss, char *const **params, int *nparams)
{
    for (int i = ss->n - 1; i >= 0; i--) {
        if (ss->v[i].params) {
            *params  = ss->v[i].params;
            *nparams = ss->v[i].nparams;
            return;
        }
    }
    *params  = NULL;
    *nparams = 0;
}

/* As bash does, a name without a slash is looked for in $PATH first, then
 * in the current directory. Returns a malloc'd path. */
static char *find_file(const char *name)
{
    const char *path = getenv("PATH");
    if (strchr(name, '/') || !path)
        return strdup(name);
    size_t nlen = strlen(name);
    while (*path) {
        size_t dlen = strcspn(path, ":");
        char *full = malloc(dlen + nlen + 3);
        if (!full)
            return NULL;
        if (dlen == 0)
            snprintf(full, dlen + nlen + 3, "./%s", name);
        else
            snprintf(full, dlen + nlen + 3, "%.*s/%s", (int)dlen, path, name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, R_OK) == 0)
            return full;
        free(full);
        path += dlen;
        if (*path == ':')
            path++;
    }
    return strdup(name);
}

/* Reads the whole file into a malloc'd string. Returns NULL with a message
 * in `err` on failure. */
static char *read_script(const char *path, char *err, size_t errsize)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errsize, "%s", strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        snprintf(err, errsize, "%s", S_ISDIR(st.st_mode) ? "is a directory" : strerror(errno));
        close(fd);
        return NULL;
    }

    StrBuf sb;
    sb_init(&sb);
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            snprintf(err, errsize, "%s", strerror(errno));
            break;
        }
        if (n == 0) {
            close(fd);
            if (memchr(sb_str(&sb), '\0', sb.len)) {
                snprintf(err, errsize, "cannot execute binary file");
                sb_free(&sb);
                return NULL;
            }
            char *text = sb_take(&sb);
            if (!text)
                snprintf(err, errsize, "out of memory");
            return text;
        }
        if (sb.len + (size_t)n > SOURCE_MAX_BYTES) {
            snprintf(err, errsize, "file too large");
            break;
        }
        if (sb_append(&sb, buf, (size_t)n) != 0) {
            snprintf(err, errsize, "out of memory");
            break;
        }
    }
    close(fd);
    sb_free(&sb);
    return NULL;
}

int source_builtin(Terminal *t, int argc, char **argv, int subshell)
{
    const char *who = argv[0];
    if (argc < 2) {
        terminal_printf(t, "terminator: %s: filename argument required", who);
        terminal_printf(t, "%s: usage: %s filename [arguments]", who, who);
        return 2;
    }
    if (subshell) {
        terminal_printf(t, "terminator: %s: cannot run a script inside a command "
                        "substitution or in the background", who);
        return 1;
    }
    SourceStack *ss = &t->sources;
    if (ss->n >= SOURCE_MAX_DEPTH) {
        terminal_printf(t, "terminator: %s: %s: scripts nested too deeply (at most %d)",
                        who, argv[1], SOURCE_MAX_DEPTH);
        return 1;
    }

    char err[256];
    char *path = find_file(argv[1]);
    if (!path) {
        terminal_print(t, "terminator: out of memory");
        return 1;
    }
    char *text = read_script(path, err, sizeof err);
    free(path);
    if (!text) {
        terminal_printf(t, "terminator: %s: %s", argv[1], err);
        return 1;
    }

    SourceFrame f;
    memset(&f, 0, sizeof f);
    f.text = text;
    f.path = strdup(argv[1]);
    int ok = f.path != NULL;
    if (ok && argc > 2) {
        f.params = calloc((size_t)(argc - 2), sizeof *f.params);
        ok = f.params != NULL;
        for (int i = 2; ok && i < argc; i++) {
            f.params[f.nparams] = strdup(argv[i]);
            ok = f.params[f.nparams] != NULL;
            f.nparams += ok;
        }
    }
    if (ok && ss->n == ss->cap) {
        int cap = ss->cap ? ss->cap * 2 : 4;
        SourceFrame *grown = realloc(ss->v, (size_t)cap * sizeof *grown);
        ok = grown != NULL;
        if (ok) {
            ss->v = grown;
            ss->cap = cap;
        }
    }
    if (!ok) {
        frame_free(&f);
        terminal_print(t, "terminator: out of memory");
        return 1;
    }
    ss->v[ss->n++] = f;
    return 0;
}

/* Parser messages start with "terminator: "; the script's name and line
 * number go in front instead. */
static void script_error(Terminal *t, const SourceFrame *f, const char *msg)
{
    static const char prefix[] = "terminator: ";
    if (strncmp(msg, prefix, sizeof prefix - 1) == 0)
        msg += sizeof prefix - 1;
    terminal_printf(t, "terminator: %s: line %d: %s", f->path, f->lineno, msg);
}

void source_step(Terminal *t)
{
    SourceStack *ss = &t->sources;
    if (ss->n == 0)
        return;
    SourceFrame *f = &ss->v[ss->n - 1];

    StrBuf acc;
    sb_init(&acc);
    int nlines = 0;
    for (;;) {
        if (f->text[f->pos] == '\0') {
            /* Out of lines: the script is finished, unless a command was
             * still open (an unclosed quote, a trailing backslash ...). */
            if (nlines > 0) {
                script_error(t, f, "syntax error: unexpected end of file");
                t->last_status = 2;
            }
            break;
        }
        const char *start = f->text + f->pos;
        const char *nl = strchr(start, '\n');
        size_t len = nl ? (size_t)(nl - start) : strlen(start);
        f->pos += len + (nl ? 1 : 0);
        f->lineno++;
        int rc = nlines++ > 0 ? sb_putc(&acc, '\n') : 0;
        if (rc != 0 || sb_append(&acc, start, len) != 0) {
            terminal_print(t, "terminator: out of memory");
            t->last_status = 1;
            break;
        }

        RawPipeline *line = NULL;
        char err[256];
        ParseResult r = parse_line(sb_str(&acc), &line, err, sizeof err);
        if (r == PARSE_INCOMPLETE)
            continue;
        if (r == PARSE_OK) {
            char *text = sb_take(&acc);
            t->interrupted = 0;
            eval_start(t, line, text ? text : "");
            free(text);
            return;   /* the frame stays until its last command is done */
        }
        if (r == PARSE_SYNTAX_ERROR)
            script_error(t, f, err);
        else
            terminal_print(t, "terminator: out of memory");
        t->last_status = r == PARSE_SYNTAX_ERROR ? 2 : 1;
        break;
    }
    sb_free(&acc);
    pop(ss);
}
