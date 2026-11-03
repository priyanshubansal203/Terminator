/*
 * command.h - Parsed form of a command line.
 *
 * A Command is one program invocation: its arguments plus the redirections to
 * apply before it runs. A Pipeline is a sequence of Commands joined by `|`.
 */
#ifndef TERMINATOR_COMMAND_H
#define TERMINATOR_COMMAND_H

typedef enum {
    REDIR_IN,    /* < path : stdin from file */
    REDIR_OUT,   /* > path : stdout to file, created/truncated */
} RedirType;

typedef struct {
    RedirType type;
    char     *path;
} Redirect;

typedef struct {
    char     **argv;     /* NULL-terminated, ready for execvp() */
    int        argc;
    Redirect  *redirs;   /* applied left to right, as in bash */
    int        nredirs;
    int        builtin;  /* argv[0] is a built-in: in a pipeline it runs in
                          * the forked child instead of being exec'd */
} Command;

/* `a | b | c`: each command's stdout feeds the next one's stdin. A plain
 * command line is a pipeline of one. */
typedef struct {
    Command *cmds;
    int      ncmds;
} Pipeline;

void command_init(Command *cmd);
void command_free(Command *cmd);

void pipeline_init(Pipeline *pl);
void pipeline_free(Pipeline *pl);

#endif
