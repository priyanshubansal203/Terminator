/*
 * parser.c - Tokenizer (bash quoting, `$` expansions, command substitution)
 * and the builder that groups tokens into commands and pipelines.
 *
 * Errors do not stop the tokenizer: the first syntax error is recorded and
 * scanning goes on, so input that is also incomplete (an unclosed quote
 * later on) still asks for more lines first, as bash does.
 */
#include "parser.h"

#include <ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "strbuf.h"

/* Inside double quotes bash removes a backslash-newline, joining the lines.
 * The project specification's example instead keeps the line break:
 *     echo "Isn't process creation exciting?\
 *     > Fork, exec, repeat!"
 * prints two lines, so that is what Terminator does. Set to 0 for bash's
 * behaviour. Outside quotes a backslash-newline is always removed. */
#define DQUOTE_KEEPS_ESCAPED_NEWLINE 1

const char *home_dir(void)
{
    const char *home = getenv("HOME");
    if (home && *home)
        return home;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *pw->pw_dir)
        return pw->pw_dir;
    return "/";
}

/* ---- Syntax tree lifetime ---------------------------------------------- */

static void word_free(Word *w)
{
    for (int i = 0; i < w->nparts; i++)
        free(w->parts[i].text);
    free(w->parts);
    free(w->source);
    memset(w, 0, sizeof *w);
}

static void raw_command_free(RawCommand *cmd)
{
    for (int i = 0; i < cmd->nwords; i++)
        word_free(&cmd->words[i]);
    free(cmd->words);
    for (int i = 0; i < cmd->nredirs; i++)
        word_free(&cmd->redirs[i].target);
    free(cmd->redirs);
}

void raw_pipeline_free(RawPipeline *pl)
{
    while (pl) {
        RawPipeline *next = pl->next;
        for (int i = 0; i < pl->ncmds; i++)
            raw_command_free(&pl->cmds[i]);
        free(pl->cmds);
        for (int i = 0; i < pl->nsubsts; i++)
            raw_pipeline_free(pl->substs[i]);
        free(pl->substs);
        free(pl);
        pl = next;
    }
}

int raw_command_is_blank(const RawCommand *cmd)
{
    return cmd->nwords == 0 && cmd->nredirs == 0;
}

/* ---- Stage 1: tokenizer ---------------------------------------------- */

typedef enum {
    TOK_WORD,
    TOK_LESS,      /* < */
    TOK_GREATER,   /* > */
    TOK_PIPE,      /* | */
    TOK_AMP,       /* & */
    TOK_NEWLINE,   /* a command separator inside a substitution */
} TokenType;

typedef struct {
    TokenType type;
    Word      word;      /* TOK_WORD only */
    int       nsubsts;   /* TOK_NEWLINE: substitutions found before it */
} Token;

/* The tokens of one pipeline (the whole line, or the inside of a command
 * substitution) and the substitutions found in its words. */
typedef struct {
    Token        *toks;
    size_t        ntoks, cap;
    RawPipeline **substs;
    int           nsubsts, substs_cap;
    const char   *end_name;   /* how the end of input is named in errors */
    int           newline_sep;/* a substitution: newlines separate commands */
} Level;

typedef struct {
    const char *p;            /* next character */
    int         incomplete;   /* the text ended inside a construct */
    int         failed;       /* a syntax error is recorded in `err` */
    int         nomem;
    char       *err;
    size_t      errsize;
} Lexer;

/* Records the first syntax error; later ones are usually consequences. */
static void syntax_error(Lexer *lx, const char *fmt, ...)
{
    if (lx->failed)
        return;
    lx->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lx->err, lx->errsize, fmt, ap);
    va_end(ap);
}

static int oom(Lexer *lx)
{
    lx->nomem = 1;
    return -1;
}

static int incomplete(Lexer *lx)
{
    lx->incomplete = 1;
    return -1;
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* Characters that form an operator token on their own. */
static int is_operator(char c)
{
    return c == '<' || c == '>' || c == '|' || c == '&';
}

static TokenType operator_type(char c)
{
    return c == '<' ? TOK_LESS : c == '>' ? TOK_GREATER : c == '|' ? TOK_PIPE : TOK_AMP;
}

static int ends_word(char c)
{
    return c == '\0' || is_blank(c) || is_operator(c) || c == '(' || c == ')';
}

static void level_init(Level *lv, const char *end_name)
{
    memset(lv, 0, sizeof *lv);
    lv->end_name = end_name;
}

static void level_free(Level *lv)
{
    for (size_t i = 0; i < lv->ntoks; i++)
        if (lv->toks[i].type == TOK_WORD)
            word_free(&lv->toks[i].word);
    free(lv->toks);
    for (int i = 0; i < lv->nsubsts; i++)
        raw_pipeline_free(lv->substs[i]);
    free(lv->substs);
    int sep = lv->newline_sep;
    level_init(lv, lv->end_name);
    lv->newline_sep = sep;
}

/* Appends a token, taking ownership of `*word` (TOK_WORD only). */
static int level_push(Level *lv, TokenType type, Word *word)
{
    if (lv->ntoks == lv->cap) {
        size_t cap = lv->cap ? lv->cap * 2 : 16;
        Token *grown = realloc(lv->toks, cap * sizeof *grown);
        if (!grown)
            return -1;
        lv->toks = grown;
        lv->cap  = cap;
    }
    Token *tok = &lv->toks[lv->ntoks++];
    tok->type = type;
    tok->nsubsts = lv->nsubsts;
    if (word)
        tok->word = *word;
    else
        memset(&tok->word, 0, sizeof tok->word);
    return 0;
}

/* Returns the new substitution's index, or -1 on OOM. */
static int level_add_subst(Level *lv, RawPipeline *sub)
{
    if (lv->nsubsts == lv->substs_cap) {
        int cap = lv->substs_cap ? lv->substs_cap * 2 : 4;
        RawPipeline **grown = realloc(lv->substs, (size_t)cap * sizeof *grown);
        if (!grown)
            return -1;
        lv->substs = grown;
        lv->substs_cap = cap;
    }
    lv->substs[lv->nsubsts] = sub;
    return lv->nsubsts++;
}

/* A word under construction. Literal characters collect in `text` until a
 * part of another kind (or quoting) starts. */
typedef struct {
    Word   w;
    int    parts_cap;
    StrBuf text;
    int    text_quoted;
    int    have_text;   /* a literal run is open; it may be empty, as in "" */
} WordBuilder;

/* Adds a part, taking ownership of `text` (freed on failure). */
static int wb_push_part(WordBuilder *wb, WordPartType type, int quoted, char *text, int index)
{
    if (wb->w.nparts == wb->parts_cap) {
        int cap = wb->parts_cap ? wb->parts_cap * 2 : 4;
        WordPart *grown = realloc(wb->w.parts, (size_t)cap * sizeof *grown);
        if (!grown) {
            free(text);
            return -1;
        }
        wb->w.parts = grown;
        wb->parts_cap = cap;
    }
    WordPart *part = &wb->w.parts[wb->w.nparts++];
    part->type   = type;
    part->quoted = quoted;
    part->text   = text;
    part->index  = index;
    return 0;
}

/* Ends the current literal run, adding it as a part. */
static int wb_flush(WordBuilder *wb)
{
    if (!wb->have_text)
        return 0;
    wb->have_text = 0;
    char *s = sb_take(&wb->text);
    if (!s)
        return -1;
    return wb_push_part(wb, PART_TEXT, wb->text_quoted, s, 0);
}

/* Appends literal characters (n may be 0: an empty quoted string still
 * makes a word). */
static int wb_text(WordBuilder *wb, const char *s, size_t n, int quoted)
{
    if (wb->have_text && wb->text_quoted != quoted && wb_flush(wb) != 0)
        return -1;
    wb->have_text   = 1;
    wb->text_quoted = quoted;
    return sb_append(&wb->text, s, n);
}

static int lex_level(Lexer *lx, Level *lv, int in_subst);
static RawPipeline *build_list(Lexer *lx, Level *lv);

/* Builds `inner` (consuming it) and adds it to the word as a substitution. */
static int add_subst(Lexer *lx, Level *lv, WordBuilder *wb, Level *inner, int quoted)
{
    RawPipeline *sub = build_list(lx, inner);
    if (!sub)   /* a syntax error is recorded; keep scanning */
        return lx->nomem ? -1 : 0;
    int idx = level_add_subst(lv, sub);
    if (idx < 0) {
        raw_pipeline_free(sub);
        return oom(lx);
    }
    if (wb_flush(wb) != 0 || wb_push_part(wb, PART_SUBST, quoted, NULL, idx) != 0)
        return oom(lx);
    return 0;
}

/* `...` with lx->p at the opening backquote. As in bash, the body ends at
 * the next unescaped backquote; inside it `\$`, `\``, `\\` (and `\"` within
 * double quotes) lose their backslash, then the body is parsed as a command
 * line of its own. Nested backquotes therefore need escaping. */
static int lex_backquote(Lexer *lx, Level *lv, WordBuilder *wb, int quoted)
{
    StrBuf body;
    sb_init(&body);
    const char *p = lx->p + 1;
    int rc = 0;
    for (;;) {
        if (*p == '\0') {
            sb_free(&body);
            return incomplete(lx);
        }
        if (*p == '`') {
            p++;
            break;
        }
        if (p[0] == '\\' && p[1] != '\0' &&
            (strchr("$`\\", p[1]) || (quoted && p[1] == '"'))) {
            rc |= sb_putc(&body, p[1]);
            p += 2;
        } else {
            rc |= sb_putc(&body, *p);
            p++;
        }
    }
    lx->p = p;
    if (rc != 0) {
        sb_free(&body);
        return oom(lx);
    }

    char suberr[256];
    Lexer sub = { .p = sb_str(&body), .err = suberr, .errsize = sizeof suberr };
    Level inner;
    level_init(&inner, "newline");
    inner.newline_sep = 1;
    lex_level(&sub, &inner, 0);
    sb_free(&body);   /* the tokens hold copies */
    if (sub.nomem || sub.incomplete || sub.failed) {
        level_free(&inner);
        if (sub.nomem)
            return oom(lx);
        if (sub.incomplete)
            syntax_error(lx, "terminator: syntax error: unexpected end of input inside `...`");
        else
            syntax_error(lx, "%s", suberr);
        return 0;
    }
    return add_subst(lx, lv, wb, &inner, quoted);
}

static int is_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static int is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* A name usable in ${...}: a variable name, all digits, `?` or `#`. */
static int valid_param(const char *s, size_t len)
{
    if (len == 1 && (s[0] == '?' || s[0] == '#'))
        return 1;
    if (len == 0)
        return 0;
    int digits = isdigit((unsigned char)s[0]);
    if (!digits && !is_name_start(s[0]))
        return 0;
    for (size_t i = 1; i < len; i++)
        if (digits ? !isdigit((unsigned char)s[i]) : !is_name_char(s[i]))
            return 0;
    return 1;
}

/* `$` at lx->p: a command substitution, a parameter, or a literal `$`. */
static int lex_dollar(Lexer *lx, Level *lv, WordBuilder *wb, int quoted)
{
    const char *p = lx->p + 1;
    if (*p == '(') {
        lx->p = p + 1;
        Level inner;
        level_init(&inner, ")");
        inner.newline_sep = 1;
        if (lex_level(lx, &inner, 1) != 0) {
            level_free(&inner);
            return -1;
        }
        return add_subst(lx, lv, wb, &inner, quoted);
    }

    const char *name;
    size_t len;
    if (*p == '{') {
        const char *end = strchr(p + 1, '}');
        if (!end)
            return incomplete(lx);
        name = p + 1;
        len  = (size_t)(end - name);
        if (!valid_param(name, len))
            syntax_error(lx, "terminator: ${%.*s}: bad substitution", (int)len, name);
        lx->p = end + 1;
    } else if (*p == '?' || *p == '#' || isdigit((unsigned char)*p)) {
        name  = p;
        len   = 1;
        lx->p = p + 1;
    } else if (is_name_start(*p)) {
        name = p;
        len  = 1;
        while (is_name_char(p[len]))
            len++;
        lx->p = p + len;
    } else {
        lx->p++;
        return wb_text(wb, "$", 1, quoted) != 0 ? oom(lx) : 0;
    }

    if (wb_flush(wb) != 0)
        return oom(lx);
    char *s = strndup(name, len);
    if (!s || wb_push_part(wb, PART_PARAM, quoted, s, lv->nsubsts) != 0)
        return oom(lx);
    return 0;
}

static int lex_squote(Lexer *lx, WordBuilder *wb)
{
    const char *start = lx->p + 1;
    const char *end = strchr(start, '\'');
    if (!end)
        return incomplete(lx);
    if (wb_text(wb, start, (size_t)(end - start), 1) != 0)
        return oom(lx);
    lx->p = end + 1;
    return 0;
}

static int lex_dquote(Lexer *lx, Level *lv, WordBuilder *wb)
{
    lx->p++;
    if (wb_text(wb, "", 0, 1) != 0)
        return oom(lx);
    for (;;) {
        const char *p = lx->p;
        int rc = 0;
        switch (*p) {
        case '\0':
            return incomplete(lx);
        case '"':
            lx->p++;
            return 0;
        case '\\':
            /* Only $ ` " \ and newline are special after a backslash. */
            if (p[1] == '\0')
                return incomplete(lx);
            if (strchr("$`\"\\", p[1])) {
                rc = wb_text(wb, p + 1, 1, 1);
                lx->p += 2;
            } else if (p[1] == '\n') {
                if (DQUOTE_KEEPS_ESCAPED_NEWLINE)
                    rc = wb_text(wb, "\n", 1, 1);
                lx->p += 2;
            } else {
                rc = wb_text(wb, p, 1, 1);
                lx->p++;
            }
            break;
        case '$':
            if (lex_dollar(lx, lv, wb, 1) != 0)
                return -1;
            break;
        case '`':
            if (lex_backquote(lx, lv, wb, 1) != 0)
                return -1;
            break;
        default:
            rc = wb_text(wb, p, 1, 1);
            lx->p++;
            break;
        }
        if (rc != 0)
            return oom(lx);
    }
}

/* One word, made of unquoted characters, quoted strings, escapes and `$`
 * and backquote expansions, up to a blank, an operator or a parenthesis. */
static int lex_word(Lexer *lx, Level *lv, Word *out)
{
    const char *start = lx->p;
    WordBuilder wb;
    memset(&wb, 0, sizeof wb);
    sb_init(&wb.text);

    int rc = 0;
    while (rc == 0 && !ends_word(*lx->p)) {
        const char *p = lx->p;
        switch (*p) {
        case '\\':
            if (p[1] == '\0') {
                rc = incomplete(lx);
            } else if (p[1] == '\n') {
                lx->p += 2;   /* line continuation */
            } else {
                rc = wb_text(&wb, p + 1, 1, 1) != 0 ? oom(lx) : 0;
                lx->p += 2;
            }
            break;
        case '\'':
            rc = lex_squote(lx, &wb);
            break;
        case '"':
            rc = lex_dquote(lx, lv, &wb);
            break;
        case '`':
            rc = lex_backquote(lx, lv, &wb, 0);
            break;
        case '$':
            rc = lex_dollar(lx, lv, &wb, 0);
            break;
        default:
            rc = wb_text(&wb, p, 1, 0) != 0 ? oom(lx) : 0;
            lx->p++;
            break;
        }
    }
    if (rc == 0 && wb_flush(&wb) != 0)
        rc = oom(lx);
    if (rc == 0 && !(wb.w.source = strndup(start, (size_t)(lx->p - start))))
        rc = oom(lx);
    sb_free(&wb.text);
    if (rc != 0) {
        word_free(&wb.w);
        return -1;
    }
    *out = wb.w;
    return 0;
}

/* Tokenizes up to the end of the text or, inside $(...) (in_subst), up to
 * and including the matching `)`. Returns -1 when the text is incomplete or
 * memory ran out; syntax errors are only recorded. */
static int lex_level(Lexer *lx, Level *lv, int in_subst)
{
    for (;;) {
        /* Blanks and backslash-newlines separate words. */
        while (is_blank(*lx->p) || (lx->p[0] == '\\' && lx->p[1] == '\n')) {
            /* In a substitution a newline also ends a command, except
             * after `|` (the pipeline goes on) or where none has started. */
            if (*lx->p == '\n' && lv->newline_sep && lv->ntoks > 0) {
                TokenType last = lv->toks[lv->ntoks - 1].type;
                if (last != TOK_PIPE && last != TOK_NEWLINE &&
                    level_push(lv, TOK_NEWLINE, NULL) != 0)
                    return oom(lx);
            }
            lx->p += *lx->p == '\\' ? 2 : 1;
        }

        char c = *lx->p;
        /* `#` where a word could start begins a comment, up to the end of
         * the line (inside a word, as in a#b, it is an ordinary character). */
        if (c == '#') {
            while (*lx->p != '\0' && *lx->p != '\n')
                lx->p++;
            continue;
        }
        if (c == '\0')
            return in_subst ? incomplete(lx) : 0;
        if (c == ')' && in_subst) {
            lx->p++;
            return 0;
        }
        if (c == '(' || c == ')') {
            /* Subshells and other ( ) syntax are not supported. */
            syntax_error(lx, "terminator: syntax error near unexpected token `%c'", c);
            lx->p++;
            continue;
        }
        if (is_operator(c)) {
            if (level_push(lv, operator_type(c), NULL) != 0)
                return oom(lx);
            lx->p++;
            continue;
        }

        Word w;
        if (lex_word(lx, lv, &w) != 0)
            return -1;
        if (level_push(lv, TOK_WORD, &w) != 0) {
            word_free(&w);
            return oom(lx);
        }
    }
}

static const char *token_name(const Token *tok)
{
    switch (tok->type) {
    case TOK_LESS:    return "<";
    case TOK_GREATER: return ">";
    case TOK_PIPE:    return "|";
    case TOK_AMP:     return "&";
    case TOK_NEWLINE: return "newline";
    default:          return tok->word.source;
    }
}

/* ---- Stage 2: build a pipeline from tokens ----------------------------- */

/* Builds `cmd` from the tokens lv->toks[start..end), which contain no `|`,
 * moving their words into it. */
static int build_command(Lexer *lx, Level *lv, size_t start, size_t end, RawCommand *cmd)
{
    for (size_t i = start; i < end; i++) {
        Token *tok = &lv->toks[i];
        if (tok->type == TOK_WORD) {
            Word *grown = realloc(cmd->words, (size_t)(cmd->nwords + 1) * sizeof *grown);
            if (!grown)
                return oom(lx);
            cmd->words = grown;
            cmd->words[cmd->nwords++] = tok->word;
            memset(&tok->word, 0, sizeof tok->word);
            continue;
        }

        /* A redirection must be followed by a file name. */
        if (i + 1 >= end || lv->toks[i + 1].type != TOK_WORD) {
            syntax_error(lx, "terminator: syntax error near unexpected token `%s'",
                         i + 1 < lv->ntoks ? token_name(&lv->toks[i + 1]) : lv->end_name);
            return -1;
        }
        Token *file = &lv->toks[++i];
        WordRedirect *grown = realloc(cmd->redirs, (size_t)(cmd->nredirs + 1) * sizeof *grown);
        if (!grown)
            return oom(lx);
        cmd->redirs = grown;
        cmd->redirs[cmd->nredirs].type   = tok->type == TOK_LESS ? REDIR_IN : REDIR_OUT;
        cmd->redirs[cmd->nredirs].target = file->word;
        cmd->nredirs++;
        memset(&file->word, 0, sizeof file->word);
    }
    return 0;
}

/* One command per `|`-separated segment of the tokens. Consumes `lv`.
 * Returns NULL on a syntax error (recorded) or OOM (lx->nomem). */
static RawPipeline *build_pipeline(Lexer *lx, Level *lv)
{
    RawPipeline *pl = calloc(1, sizeof *pl);
    if (!pl) {
        oom(lx);
        level_free(lv);
        return NULL;
    }
    /* The substitutions found while tokenizing belong to this pipeline. */
    pl->substs  = lv->substs;
    pl->nsubsts = lv->nsubsts;
    lv->substs  = NULL;
    lv->nsubsts = 0;

    /* parse_line() has already taken a `&` that ends the line. */
    for (size_t i = 0; i < lv->ntoks; i++) {
        if (lv->toks[i].type == TOK_AMP) {
            syntax_error(lx, "terminator: syntax error: `&' is only supported at the "
                             "end of a command line");
            level_free(lv);
            raw_pipeline_free(pl);
            return NULL;
        }
    }

    size_t nsegs = 1;
    for (size_t i = 0; i < lv->ntoks; i++)
        if (lv->toks[i].type == TOK_PIPE)
            nsegs++;

    int ok = (pl->cmds = calloc(nsegs, sizeof *pl->cmds)) != NULL;
    if (!ok)
        oom(lx);
    size_t start = 0;
    for (size_t s = 0; ok && s < nsegs; s++) {
        size_t end = start;
        while (end < lv->ntoks && lv->toks[end].type != TOK_PIPE)
            end++;

        /* `| ls`, `ls |` and `ls || wc` all leave a segment empty. A blank
         * line (one empty segment, no `|`) is fine. */
        if (nsegs > 1 && start == end) {
            syntax_error(lx, "terminator: syntax error near unexpected token `%s'",
                         end < lv->ntoks ? "|" : lv->end_name);
            ok = 0;
            break;
        }
        pl->ncmds++;
        if (build_command(lx, lv, start, end, &pl->cmds[s]) != 0)
            ok = 0;
        start = end + 1;
    }
    level_free(lv);
    if (!ok) {
        raw_pipeline_free(pl);
        return NULL;
    }
    return pl;
}

/* Shifts the substitution numbers in a word's parts down by `by`. */
static void renumber_word(Word *w, int by)
{
    for (int i = 0; i < w->nparts; i++)
        if (w->parts[i].type == PART_SUBST || w->parts[i].type == PART_PARAM)
            w->parts[i].index -= by;
}

/* A substitution's tokens: one pipeline per newline-separated command,
 * linked by `next`. Each takes the tokens and substitutions between its
 * separators. Consumes `lv`; returns NULL on error, like build_pipeline(). */
static RawPipeline *build_list(Lexer *lx, Level *lv)
{
    size_t nseps = 0;
    for (size_t i = 0; i < lv->ntoks; i++)
        nseps += lv->toks[i].type == TOK_NEWLINE;
    if (nseps == 0)
        return build_pipeline(lx, lv);

    RawPipeline *head = NULL, **tail = &head;
    size_t start = 0;
    int sub_lo = 0;
    for (size_t i = 0; i <= lv->ntoks; i++) {
        if (i < lv->ntoks && lv->toks[i].type != TOK_NEWLINE)
            continue;
        int sub_hi = i < lv->ntoks ? lv->toks[i].nsubsts : lv->nsubsts;
        size_t n = i - start;
        if (n > 0) {
            Level seg;
            level_init(&seg, i < lv->ntoks ? "newline" : lv->end_name);
            seg.toks = malloc(n * sizeof *seg.toks);
            seg.substs = sub_hi > sub_lo ? malloc((size_t)(sub_hi - sub_lo) * sizeof *seg.substs)
                                         : NULL;
            if (!seg.toks || (sub_hi > sub_lo && !seg.substs)) {
                free(seg.toks);
                free(seg.substs);
                oom(lx);
                goto fail;
            }
            /* Move the tokens and substitutions over; `lv` keeps empty
             * husks, which level_free() handles. */
            for (size_t k = 0; k < n; k++) {
                Token *tok = &lv->toks[start + k];
                seg.toks[k] = *tok;
                if (tok->type == TOK_WORD)
                    renumber_word(&seg.toks[k].word, sub_lo);
                memset(&tok->word, 0, sizeof tok->word);
            }
            seg.ntoks = seg.cap = n;
            for (int k = sub_lo; k < sub_hi; k++) {
                seg.substs[k - sub_lo] = lv->substs[k];
                lv->substs[k] = NULL;
            }
            seg.nsubsts = seg.substs_cap = sub_hi - sub_lo;

            RawPipeline *pl = build_pipeline(lx, &seg);
            if (!pl)
                goto fail;
            *tail = pl;
            tail = &pl->next;
        }
        start = i + 1;
        sub_lo = sub_hi;
    }
    level_free(lv);
    if (!head) {
        /* Only newlines: a blank command, as for `$()`. */
        Level empty;
        level_init(&empty, lv->end_name);
        head = build_pipeline(lx, &empty);
    }
    return head;

fail:
    raw_pipeline_free(head);
    level_free(lv);
    return NULL;
}

ParseResult parse_line(const char *text, RawPipeline **out, char *err, size_t errsize)
{
    *out = NULL;
    Lexer lx = { .p = text, .err = err, .errsize = errsize };
    Level lv;
    level_init(&lv, "newline");
    lex_level(&lx, &lv, 0);

    /* `cmd &`: a background job. A lone `&` has no command to run. */
    int background = 0;
    if (!lx.nomem && !lx.incomplete && !lx.failed &&
        lv.ntoks > 0 && lv.toks[lv.ntoks - 1].type == TOK_AMP) {
        lv.ntoks--;
        background = 1;
        if (lv.ntoks == 0)
            syntax_error(&lx, "terminator: syntax error near unexpected token `&'");
    }

    RawPipeline *pl = NULL;
    if (!lx.nomem && !lx.incomplete && !lx.failed)
        pl = build_pipeline(&lx, &lv);
    else
        level_free(&lv);
    if (pl)
        pl->background = background;

    if (lx.nomem) {
        raw_pipeline_free(pl);
        return PARSE_NO_MEMORY;
    }
    if (lx.incomplete)
        return PARSE_INCOMPLETE;
    if (lx.failed)
        return PARSE_SYNTAX_ERROR;
    *out = pl;
    return PARSE_OK;
}
