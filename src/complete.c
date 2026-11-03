/*
 * complete.c - Finding the word to complete and the files that match it.
 */
#include "complete.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "parser.h"
#include "strbuf.h"

void complete_free(Completion *c)
{
    for (int i = 0; i < c->n; i++)
        free(c->names[i]);
    free(c->names);
    memset(c, 0, sizeof *c);
}

static int is_separator(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '|' || ch == '<' ||
           ch == '>' || ch == '&' || ch == '(' || ch == ')';
}

/* The literal text of the word before `cursor` (quotes and backslashes
 * removed), and whether it ends inside a quote. Returns -1 for a word that
 * cannot be completed (it has a $ or backquote expansion). */
static int word_before(const char *line, size_t cursor, StrBuf *lit, int *open_quote)
{
    /* First find where the word starts, scanning from the line's start so
     * quotes are known. */
    size_t start = 0;
    int quote = 0;
    for (size_t i = 0; i < cursor; i++) {
        char ch = line[i];
        if (quote == '\'') {
            if (ch == '\'')
                quote = 0;
        } else if (quote == '"') {
            if (ch == '\\' && i + 1 < cursor)
                i++;
            else if (ch == '"')
                quote = 0;
        } else if (ch == '\\') {
            i++;
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (is_separator(ch)) {
            start = i + 1;
        }
    }

    /* Then its literal value. */
    quote = 0;
    for (size_t i = start; i < cursor; i++) {
        char ch = line[i];
        if (quote == '\'') {
            if (ch == '\'')
                quote = 0;
            else
                sb_putc(lit, ch);
            continue;
        }
        if (ch == '`' || (ch == '$' && quote != '\''))
            return -1;
        if (quote == '"') {
            if (ch == '\\' && i + 1 < cursor && strchr("$`\"\\", line[i + 1]))
                sb_putc(lit, line[++i]);
            else if (ch == '"')
                quote = 0;
            else
                sb_putc(lit, ch);
            continue;
        }
        if (ch == '\\' && i + 1 < cursor)
            sb_putc(lit, line[++i]);
        else if (ch == '\'' || ch == '"')
            quote = ch;
        else
            sb_putc(lit, ch);
    }
    *open_quote = quote;
    return 0;
}

/* Plain byte order, as in the specification's example (abc.txt before
 * abcd.txt; the locale's collation would ignore the dot). */
static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int complete_find(const char *line, size_t cursor, Completion *c)
{
    memset(c, 0, sizeof *c);
    StrBuf lit;
    sb_init(&lit);
    if (word_before(line, cursor, &lit, &c->quote) != 0) {
        sb_free(&lit);
        return 0;
    }
    const char *word = sb_str(&lit);

    /* Split at the last '/': the directory to look in, and the start of
     * the name. A leading ~ means the home directory. */
    const char *slash = strrchr(word, '/');
    const char *prefix = slash ? slash + 1 : word;
    StrBuf dir;
    sb_init(&dir);
    if (slash) {
        if (word[0] == '~' && (word[1] == '/' || word + 1 == slash)) {
            sb_append(&dir, home_dir(), strlen(home_dir()));
            sb_append(&dir, word + 1, (size_t)(slash - word));
        } else {
            sb_append(&dir, word, (size_t)(slash - word) + 1);
        }
    } else {
        sb_append(&dir, ".", 1);
    }

    DIR *d = opendir(sb_str(&dir));
    size_t plen = strlen(prefix);
    int cap = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && c->n < COMPLETE_MAX) {
            const char *name = e->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (name[0] == '.' && prefix[0] != '.')
                continue;
            if (strncmp(name, prefix, plen) != 0)
                continue;

            /* Directories get a '/'. */
            StrBuf path;
            sb_init(&path);
            sb_append(&path, sb_str(&dir), dir.len);
            sb_putc(&path, '/');
            sb_append(&path, name, strlen(name));
            struct stat st;
            int is_dir = stat(sb_str(&path), &st) == 0 && S_ISDIR(st.st_mode);
            sb_free(&path);

            if (c->n == cap) {
                cap = cap ? cap * 2 : 16;
                char **grown = realloc(c->names, (size_t)cap * sizeof *grown);
                if (!grown)
                    break;
                c->names = grown;
            }
            size_t len = strlen(name);
            char *s = malloc(len + 2);
            if (!s)
                break;
            memcpy(s, name, len);
            s[len] = is_dir ? '/' : '\0';
            s[len + 1] = '\0';
            c->names[c->n++] = s;
        }
        closedir(d);
    }
    c->typed = plen;
    sb_free(&dir);
    sb_free(&lit);
    if (c->n > 1)
        qsort(c->names, (size_t)c->n, sizeof *c->names, by_name);
    return c->n;
}

char *complete_common_prefix(const Completion *c)
{
    if (c->n == 0)
        return strdup("");
    size_t len = strlen(c->names[0]);
    for (int i = 1; i < c->n; i++) {
        size_t k = 0;
        while (k < len && c->names[i][k] == c->names[0][k])
            k++;
        len = k;
    }
    char *s = malloc(len + 1);
    if (s) {
        memcpy(s, c->names[0], len);
        s[len] = '\0';
    }
    return s;
}

char *complete_insertion(const Completion *c, const char *name)
{
    StrBuf out;
    sb_init(&out);
    size_t len = strlen(name);
    for (size_t i = c->typed; i < len; i++) {
        char ch = name[i];
        if (c->quote == '\'') {
            /* Nothing can be escaped in '...': close, escape, reopen. */
            if (ch == '\'')
                sb_append(&out, "'\\''", 4);
            else
                sb_putc(&out, ch);
        } else if (c->quote == '"') {
            if (strchr("$`\"\\", ch))
                sb_putc(&out, '\\');
            sb_putc(&out, ch);
        } else {
            /* Characters the shell would read as something else. A `~` or
             * `#` only matters at the start of the word. */
            if (strchr(" \t\\'\"$`|&;<>()*?[]", ch) ||
                ((ch == '~' || ch == '#') && i == 0))
                sb_putc(&out, '\\');
            sb_putc(&out, ch);
        }
    }
    char *s = sb_take(&out);
    return s;
}
