/*
 * redirect.h - Applying `<` and `>` redirections.
 */
#ifndef TERMINATOR_REDIRECT_H
#define TERMINATOR_REDIRECT_H

#include <stddef.h>

#include "command.h"

/* In a forked child: opens each file and dup2()s it onto stdin/stdout, in
 * order. On failure writes "terminator: PATH: reason" to stderr and returns
 * -1. Never call this in the shell process itself. */
int redirect_apply(const Redirect *redirs, int n);

/* In the shell process, for built-ins and redirection-only lines: opens and
 * immediately closes each file, so output files are created/truncated and
 * missing input files are detected, without touching the shell's own fds.
 * Returns 0, or -1 with a message in `err`. */
int redirect_check(const Redirect *redirs, int n, char *err, size_t errsize);

#endif
