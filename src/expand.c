/*
 * expand.c - Tilde, parameter and command substitution expansion, splitting
 * of unquoted results into words, and pathname (glob) expansion.
 */
#include "expand.h"

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strbuf.h"

/* The words produced by one Word, built up one character at a time. Next
 * to each word its glob pattern is built: the same characters, with quoted
 * ones backslash-escaped so only unquoted * ? [ act as wildcards. */
typedef struct {
    char  **v;
    int     n, cap;
    StrBuf  cur;       /* the word being built */
    StrBuf  pat;       /* its glob pattern */
    int     glob;      /* `cur` has an unquoted * ? or [ */
    int     started;   /* `cur` is a word even if empty (it had quotes) */
} Fields;

/* Appends `s` (taking ownership) as the next word. */
static int fields_push(Fields *f, char *s)
{
    if (f->n + 1 >= f->cap) {
        int cap = f->cap ? f->cap * 2 : 8;
        char **grown = realloc(f->v, (size_t)cap * sizeof *grown);
        if (!grown) {
            free(s);
            return -1;
        }
        f->v = grown;
        f->cap = cap;
    }
    f->v[f->n++] = s;
    f->v[f->n] = NULL;
    return 0;
}

/* Ends the current word. One with unquoted wildcards is replaced by the
 * matching paths, sorted; if nothing matches it stays as typed, as in bash
 * (so `ls *.none` reports the name). Names starting with `.` only match a
 * pattern that starts with `.`. */
static int fields_emit(Fields *f)
{
    int rc = 0;
    int globbed = 0;
    if (f->glob) {
        glob_t g;
        int r = glob(sb_str(&f->pat), 0, NULL, &g);
        if (r == 0) {
            for (size_t i = 0; i < g.gl_pathc && rc == 0; i++) {
                char *s = strdup(g.gl_pathv[i]);
                rc = s ? fields_push(f, s) : -1;
            }
            globbed = 1;
        } else if (r == GLOB_NOSPACE) {
            rc = -1;
        }
        /* GLOB_NOMATCH, or an unreadable directory: keep the word. */
        if (r != GLOB_NOMATCH)
            globfree(&g);
    }
    if (rc == 0 && !globbed) {
        char *s = sb_take(&f->cur);
        rc = s ? fields_push(f, s) : -1;
    }
    sb_clear(&f->cur);
    sb_clear(&f->pat);
    f->glob = 0;
    f->started = 0;
    return rc;
}

static void fields_free(Fields *f)
{
    for (int i = 0; i < f->n; i++)
        free(f->v[i]);
    free(f->v);
    sb_free(&f->cur);
    sb_free(&f->pat);
    memset(f, 0, sizeof *f);
}

/* Adds one character to the current word and to its pattern. */
static int fields_putc(Fields *f, char c, int quoted)
{
    int rc = sb_putc(&f->cur, c);
    /* A backslash from an expansion is always literal. */
    if (c == '\\' || (quoted && strchr("*?[]", c))) {
        rc |= sb_putc(&f->pat, '\\');
    } else if (!quoted && (c == '*' || c == '?' || c == '[')) {
        f->glob = 1;
    }
    rc |= sb_putc(&f->pat, c);
    f->started = 1;
    return rc;
}

static int fields_puts(Fields *f, const char *s, int quoted)
{
    for (; *s; s++)
        if (fields_putc(f, *s, quoted) != 0)
            return -1;
    f->started = 1;
    return 0;
}

static int is_ifs(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* Adds expansion output. Unquoted, every run of blanks ends the current
 * word, so `a$(echo " b ")c` gives a, b and c. */
static int fields_add(Fields *f, const char *s, int quoted)
{
    if (quoted)
        return fields_puts(f, s, 1);
    for (; *s; s++) {
        if (is_ifs(*s)) {
            if (f->started && fields_emit(f) != 0)
                return -1;
        } else if (fields_putc(f, *s, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

/* $? as seen by a part: the status of the last substitution before it, or
 * the status from before the command if none ran yet. */
static int status_at(const ExpandContext *ctx, int nsubsts_before)
{
    return nsubsts_before > 0 ? ctx->statuses[nsubsts_before - 1] : ctx->base_status;
}

static const char *param_value(const WordPart *part, const ExpandContext *ctx,
                               char *buf, size_t bufsize)
{
    const char *name = part->text;
    if (strcmp(name, "?") == 0) {
        snprintf(buf, bufsize, "%d", status_at(ctx, part->index));
        return buf;
    }
    if (strcmp(name, "#") == 0) {
        snprintf(buf, bufsize, "%d", ctx->nparams);
        return buf;
    }
    if (name[0] >= '0' && name[0] <= '9') {
        long n = strtol(name, NULL, 10);
        return n >= 1 && n <= ctx->nparams ? ctx->params[n - 1] : "";
    }
    const char *v = getenv(name);
    return v ? v : "";
}

/* Expands one word into zero or more fields appended to `f`. */
static int expand_word(const Word *w, const ExpandContext *ctx, Fields *f)
{
    for (int i = 0; i < w->nparts; i++) {
        const WordPart *part = &w->parts[i];
        char num[16];
        int rc = 0;
        switch (part->type) {
        case PART_TEXT: {
            const char *s = part->text;
            /* `~` or `~/...` at the start of a word, unquoted. */
            if (i == 0 && !part->quoted && s[0] == '~' &&
                (s[1] == '/' || (s[1] == '\0' && w->nparts == 1))) {
                /* The home directory is a literal path, never a pattern. */
                rc = fields_puts(f, home_dir(), 1);
                s++;
            }
            if (rc == 0)
                rc = fields_puts(f, s, part->quoted);
            /* Unquoted text cannot be empty, so this only marks "" and ''. */
            f->started = 1;
            break;
        }
        case PART_PARAM:
            rc = fields_add(f, param_value(part, ctx, num, sizeof num), part->quoted);
            break;
        case PART_SUBST: {
            const char *out = ctx->outputs[part->index];
            rc = fields_add(f, out ? out : "", part->quoted);
            break;
        }
        }
        if (rc != 0)
            return -1;
    }
    return f->started ? fields_emit(f) : 0;
}

static int expand_command(const RawCommand *raw, const ExpandContext *ctx,
                          Command *cmd, char *err, size_t errsize)
{
    Fields f;
    memset(&f, 0, sizeof f);
    for (int i = 0; i < raw->nwords; i++) {
        if (expand_word(&raw->words[i], ctx, &f) != 0)
            goto nomem;
    }
    if (!f.v && !(f.v = calloc(1, sizeof *f.v)))
        goto nomem;
    cmd->argv = f.v;
    cmd->argc = f.n;
    f.v = NULL;
    f.n = 0;
    sb_free(&f.cur);
    sb_free(&f.pat);

    if (raw->nredirs > 0 && !(cmd->redirs = calloc((size_t)raw->nredirs, sizeof *cmd->redirs)))
        goto nomem;
    for (int i = 0; i < raw->nredirs; i++) {
        const Word *target = &raw->redirs[i].target;
        Fields t;
        memset(&t, 0, sizeof t);
        if (expand_word(target, ctx, &t) != 0) {
            fields_free(&t);
            goto nomem;
        }
        /* A file name must expand to exactly one word, as in bash. */
        if (t.n != 1) {
            fields_free(&t);
            snprintf(err, errsize, "terminator: %s: ambiguous redirect", target->source);
            return -1;
        }
        cmd->redirs[i].type = raw->redirs[i].type;
        cmd->redirs[i].path = t.v[0];
        cmd->nredirs++;
        free(t.v);
        sb_free(&t.cur);
        sb_free(&t.pat);
    }
    return 0;

nomem:
    fields_free(&f);
    snprintf(err, errsize, "terminator: out of memory");
    return -1;
}

int expand_pipeline(const RawPipeline *raw, const ExpandContext *ctx,
                    Pipeline *out, char *err, size_t errsize)
{
    pipeline_init(out);
    out->cmds = calloc((size_t)raw->ncmds, sizeof *out->cmds);
    if (!out->cmds) {
        snprintf(err, errsize, "terminator: out of memory");
        return -1;
    }
    for (int i = 0; i < raw->ncmds; i++) {
        command_init(&out->cmds[i]);
        out->ncmds++;
        if (expand_command(&raw->cmds[i], ctx, &out->cmds[i], err, errsize) != 0) {
            pipeline_free(out);
            return -1;
        }
    }
    return 0;
}
