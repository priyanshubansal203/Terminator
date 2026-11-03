/*
 * redirect.c - open()/dup2() for file redirections.
 */
#include "redirect.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Output files get rw-rw-rw- minus the umask, like bash. */
#define REDIR_OUT_MODE 0666

static int open_redirect(const Redirect *r)
{
    int flags = r->type == REDIR_IN ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
    int fd;
    do
        fd = open(r->path, flags, REDIR_OUT_MODE);
    while (fd < 0 && errno == EINTR);
    return fd;
}

static int target_fd(const Redirect *r)
{
    return r->type == REDIR_IN ? STDIN_FILENO : STDOUT_FILENO;
}

int redirect_apply(const Redirect *redirs, int n)
{
    for (int i = 0; i < n; i++) {
        const Redirect *r = &redirs[i];
        int fd = open_redirect(r);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "terminator: %s: %s\n", r->path, strerror(errno));
            return -1;
        }
        int target = target_fd(r);
        if (fd != target) {
            if (dup2(fd, target) < 0) {
                dprintf(STDERR_FILENO, "terminator: %s: %s\n", r->path, strerror(errno));
                close(fd);
                return -1;
            }
            close(fd);
        }
    }
    return 0;
}

int redirect_check(const Redirect *redirs, int n, char *err, size_t errsize)
{
    for (int i = 0; i < n; i++) {
        int fd = open_redirect(&redirs[i]);
        if (fd < 0) {
            snprintf(err, errsize, "terminator: %s: %s", redirs[i].path, strerror(errno));
            return -1;
        }
        close(fd);
    }
    return 0;
}
