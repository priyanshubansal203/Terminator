/*
 * command.c - Lifetime of parsed commands and pipelines.
 */
#include "command.h"

#include <stdlib.h>

void command_init(Command *cmd)
{
    cmd->argv    = NULL;
    cmd->argc    = 0;
    cmd->redirs  = NULL;
    cmd->nredirs = 0;
}

void command_free(Command *cmd)
{
    if (cmd->argv) {
        for (int i = 0; i < cmd->argc; i++)
            free(cmd->argv[i]);
        free(cmd->argv);
    }
    for (int i = 0; i < cmd->nredirs; i++)
        free(cmd->redirs[i].path);
    free(cmd->redirs);
    command_init(cmd);
}

void pipeline_init(Pipeline *pl)
{
    pl->cmds  = NULL;
    pl->ncmds = 0;
}

void pipeline_free(Pipeline *pl)
{
    for (int i = 0; i < pl->ncmds; i++)
        command_free(&pl->cmds[i]);
    free(pl->cmds);
    pipeline_init(pl);
}
