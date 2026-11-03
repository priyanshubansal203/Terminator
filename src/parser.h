/*
 * parser.h - Turns command-line text into an unexpanded syntax tree.
 *
 * Parsing happens in two stages:
 *   1. tokenize: split the text into words and operator tokens (`<`, `>`,
 *      `|`). Operators are recognised whether or not they touch adjacent
 *      words, so `sort<in.txt>out.txt` gives: sort < in.txt > out.txt.
 *      Quoting follows bash: '...' is literal, "..." still allows `$` and
 *      backquote expansions, a backslash quotes the next character, and a
 *      backslash-newline is removed. A `#` at the start of a word begins a
 *      comment that runs to the end of the line. `$(...)` and `...` (backquotes) are
 *      parsed recursively into their own trees, so syntax errors anywhere in
 *      the line are found before anything runs.
 *   2. build: split the tokens at each `|`, then for every segment collect
 *      the words and attach each redirection operator to the word that
 *      follows it. A `|` with nothing on one side is a syntax error.
 *      A `&` ending the line runs it as a background job; `&` anywhere else
 *      (lists such as `a & b`, `&&`, or inside a substitution) is an error.
 *      Inside `$(...)` and backquotes an unquoted newline separates
 *      commands, as in bash: the substitution is a list of pipelines run one
 *      after the other (linked by `next`), and its output is all of theirs.
 *      Blank lines are skipped, and a newline after `|` continues the
 *      pipeline.
 *
 * Words are kept unexpanded, as a list of parts (literal text, `$NAME`,
 * command substitution), because a substitution's output is only known once
 * it has run. expand.h turns a tree into argv strings.
 */
#ifndef TERMINATOR_PARSER_H
#define TERMINATOR_PARSER_H

#include <stddef.h>

#include "command.h"

typedef enum {
    PART_TEXT,    /* literal characters */
    PART_PARAM,   /* $NAME, ${NAME}, $?, $#, $0-$9 */
    PART_SUBST,   /* $(...) or `...` */
} WordPartType;

typedef struct {
    WordPartType type;
    int   quoted;  /* inside quotes or backslash-escaped: never split into
                    * several words, and no `~` expansion */
    char *text;    /* PART_TEXT: the characters; PART_PARAM: the name */
    int   index;   /* PART_SUBST: index into the pipeline's substs.
                    * PART_PARAM: number of substs before this part, so `$?`
                    * can see the status of those that already ran. */
} WordPart;

typedef struct {
    WordPart *parts;
    int       nparts;
    char     *source;  /* the word as typed, for error messages */
} Word;

typedef struct {
    RedirType type;
    Word      target;
} WordRedirect;

typedef struct {
    Word         *words;
    int           nwords;
    WordRedirect *redirs;   /* applied left to right, as in bash */
    int           nredirs;
} RawCommand;

typedef struct RawPipeline RawPipeline;
struct RawPipeline {
    RawCommand   *cmds;
    int           ncmds;
    /* Command substitutions in this pipeline's words, in the order they run
     * (left to right). Substitutions nested inside them belong to their own
     * RawPipeline. */
    RawPipeline **substs;
    int           nsubsts;
    int           background;   /* the line ended with `&` */
    RawPipeline  *next;         /* next command of a substitution's list */
};

typedef enum {
    PARSE_OK,
    PARSE_INCOMPLETE,     /* more lines needed: unclosed quote, `$(`,
                           * backquote or `${`, or a trailing backslash */
    PARSE_SYNTAX_ERROR,   /* message written to `err` */
    PARSE_NO_MEMORY,
} ParseResult;

/* Parses `text` (possibly several lines joined by '\n'). On PARSE_OK `*out`
 * is a tree with at least one command (a blank line gives one command with
 * no words and no redirections); free it with raw_pipeline_free(). */
ParseResult parse_line(const char *text, RawPipeline **out, char *err, size_t errsize);

void raw_pipeline_free(RawPipeline *pl);

/* 1 if the command has no words and no redirections (a blank line). */
int raw_command_is_blank(const RawCommand *cmd);

/* $HOME, or the passwd entry's home directory, or "/" as a last resort. */
const char *home_dir(void);

#endif
