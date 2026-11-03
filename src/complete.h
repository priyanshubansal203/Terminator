/*
 * complete.h - File-name completion (Tab).
 *
 * The word before the cursor is completed with the names of the files in
 * the tab's current directory (or, if the word contains a `/`, in the
 * directory it names: `src/ma`, `../`, `~/Do`). The word is found the way
 * the parser would split the line: it starts after the last blank or
 * operator (| < > &) that is not quoted or backslash-escaped, and quotes and
 * backslashes in it are taken into account. Names starting with `.` only
 * match a word starting with `.`; directories are given a trailing `/`.
 *
 * Completion only ever appends to the word: the characters still missing,
 * quoted so the line keeps meaning the same file (`a\ b.txt`, or as typed
 * inside an open "..." or '...'; a quote left open is closed once the word
 * is a complete file name). The text after the cursor is untouched.
 */
#ifndef TERMINATOR_COMPLETE_H
#define TERMINATOR_COMPLETE_H

#include <stddef.h>

/* At most this many matches are collected. */
#define COMPLETE_MAX 5000

typedef struct {
    char  **names;    /* the matching names, sorted; directories end in '/' */
    int     n;
    size_t  typed;    /* characters of them the word already has */
    int     quote;    /* the word is inside an open ' or " (else 0) */
} Completion;

/* Finds the completions of the word before `cursor` in `line`. Returns how
 * many there are (0 if none, or the word cannot be completed, e.g. it has
 * a $ expansion). Free with complete_free(). */
int   complete_find(const char *line, size_t cursor, Completion *c);

/* The longest prefix all matches share. Returns a malloc'd string. */
char *complete_common_prefix(const Completion *c);

/* What to insert at the cursor to turn the word into `name` (one of the
 * matches, or their common prefix), quoted as needed. Returns a malloc'd
 * string, empty if nothing is missing. */
char *complete_insertion(const Completion *c, const char *name);

void  complete_free(Completion *c);

#endif
