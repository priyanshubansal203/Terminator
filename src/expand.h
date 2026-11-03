/*
 * expand.h - Turns a parsed (RawPipeline) command into argv strings.
 *
 * Expansion follows bash for the features Terminator has: a leading `~`,
 * parameters ($NAME and ${NAME} from the environment, $?, $0 (empty), and
 * $# / $1... from ctx->params: none at the prompt, a sourced script's
 * arguments while it runs), and command substitution output.
 * Results of unquoted parameters and substitutions are split into separate
 * words at spaces, tabs and newlines; quoted ones never are. A word that
 * expands to nothing unquoted disappears, while "" stays an empty argument.
 * Finally each word with an unquoted `*`, `?` or `[...]` (typed, or from an
 * unquoted expansion) is replaced by the matching paths, sorted; with no
 * match it is kept as it is. Quoted or backslash-escaped wildcards are
 * literal. A redirection target must still match exactly one path.
 */
#ifndef TERMINATOR_EXPAND_H
#define TERMINATOR_EXPAND_H

#include <stddef.h>

#include "command.h"
#include "parser.h"

/* What has been learnt by running the pipeline's command substitutions. */
typedef struct {
    char *const *outputs;      /* stdout of substs[i], trailing newlines removed;
                                * NULL entries count as empty */
    const int   *statuses;     /* exit status of substs[i] */
    int          base_status;  /* $? before any of them ran */
    char *const *params;       /* $1, $2, ... (a sourced script's arguments) */
    int          nparams;      /* $# */
} ExpandContext;

/* Expands every command of `raw`. Returns 0, or -1 with a message in `err`
 * (an ambiguous redirection, or out of memory); `out` is then empty. */
int expand_pipeline(const RawPipeline *raw, const ExpandContext *ctx,
                    Pipeline *out, char *err, size_t errsize);

#endif
