/*
 * builtins.h - Commands that change the shell's own state and therefore run
 * inside Terminator instead of in a child process.
 */
#ifndef TERMINATOR_BUILTINS_H
#define TERMINATOR_BUILTINS_H

#include "terminal.h"

/* Returns 1 if `name` is a built-in command. */
int is_builtin(const char *name);

/* Runs built-in argv[0] and returns its exit status. With `subshell` (inside
 * a command substitution, which bash runs in a subshell) it has no lasting
 * effect: cd returns to the original directory, exit does not quit. */
int builtin_run(Terminal *t, int argc, char **argv, int subshell);

#endif
