/*
 * history.c - The shared, locked command history.
 */
#define _DEFAULT_SOURCE   /* MAP_ANONYMOUS */

#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct History {
    int             between_processes;
    pthread_mutex_t mutex;          /* thread mode */
    sem_t           sem;            /* process mode */
    _Atomic pid_t   holder;         /* process holding `sem`, 0 if none */
    size_t          start;          /* ring index of the oldest line */
    size_t          count;
    size_t          dropped;        /* lines dropped so far: numbers stay stable */
    char            path[4096];     /* the history file, "" for none */
    size_t          file_lines;     /* lines in it (as far as we know) */
    char            lines[HISTORY_MAX][HISTORY_LINE_MAX];
};

/* The file is rewritten once it has this many lines more than it keeps. */
#define HISTORY_FILE_SLACK 1000

/* Thread-mode histories, for the fork handlers below. */
static History *g_thread_history;

static void before_fork(void)
{
    if (g_thread_history)
        pthread_mutex_lock(&g_thread_history->mutex);
}

static void after_fork_parent(void)
{
    if (g_thread_history)
        pthread_mutex_unlock(&g_thread_history->mutex);
}

/* The child's only thread is not the one that locked it: start afresh. */
static void after_fork_child(void)
{
    if (g_thread_history)
        pthread_mutex_init(&g_thread_history->mutex, NULL);
}

static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;

static void register_fork_handlers(void)
{
    pthread_atfork(before_fork, after_fork_parent, after_fork_child);
}

History *history_create(int between_processes)
{
    /* Shared or private, the mapping is zero-filled and only the pages
     * actually used take memory. */
    History *h = mmap(NULL, sizeof *h, PROT_READ | PROT_WRITE,
                      (between_processes ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS, -1, 0);
    if (h == MAP_FAILED)
        return NULL;
    h->between_processes = between_processes;
    if (between_processes) {
        if (sem_init(&h->sem, 1, 1) != 0) {
            munmap(h, sizeof *h);
            return NULL;
        }
    } else {
        if (pthread_mutex_init(&h->mutex, NULL) != 0) {
            munmap(h, sizeof *h);
            return NULL;
        }
        /* A forked child (a built-in in a pipeline) must not inherit the
         * mutex locked by another thread. */
        g_thread_history = h;
        pthread_once(&g_atfork_once, register_fork_handlers);
    }
    atomic_store(&h->holder, 0);
    return h;
}

void history_destroy(History *h)
{
    if (!h)
        return;
    if (h->between_processes)
        sem_destroy(&h->sem);
    else
        pthread_mutex_destroy(&h->mutex);
    if (g_thread_history == h)
        g_thread_history = NULL;
    munmap(h, sizeof *h);
}

void history_lock(History *h)
{
    if (!h->between_processes) {
        pthread_mutex_lock(&h->mutex);
        return;
    }
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        if (sem_timedwait(&h->sem, &ts) == 0)
            break;
        if (errno != ETIMEDOUT)
            continue;   /* EINTR */
        /* Held for a whole second: the critical sections are tiny, so the
         * holder has probably died. If so, take the lock over (only one
         * waiter wins the exchange). */
        pid_t holder = atomic_load(&h->holder);
        if (holder > 0 && kill(holder, 0) != 0 && errno == ESRCH &&
            atomic_compare_exchange_strong(&h->holder, &holder, getpid()))
            return;
    }
    atomic_store(&h->holder, getpid());
}

void history_unlock(History *h)
{
    if (!h->between_processes) {
        pthread_mutex_unlock(&h->mutex);
        return;
    }
    atomic_store(&h->holder, 0);
    sem_post(&h->sem);
}

size_t history_count_locked(const History *h)
{
    return h->count;
}

const char *history_get_locked(const History *h, size_t i)
{
    return i < h->count ? h->lines[(h->start + i) % HISTORY_MAX] : NULL;
}

size_t history_number_locked(const History *h, size_t i)
{
    return h->dropped + i + 1;
}

static int is_blank_line(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t')
            return 0;
    return 1;
}

/* Adds a line to the ring (lock held). Returns 1 if added, 0 if it repeats
 * the newest entry. */
static int add_locked(History *h, const char *line)
{
    const char *newest = history_get_locked(h, h->count - 1);
    if (h->count > 0 && strncmp(newest, line, HISTORY_LINE_MAX - 1) == 0)
        return 0;
    if (h->count == HISTORY_MAX) {
        h->start = (h->start + 1) % HISTORY_MAX;
        h->count--;
        h->dropped++;
    }
    char *slot = h->lines[(h->start + h->count) % HISTORY_MAX];
    strncpy(slot, line, HISTORY_LINE_MAX - 1);
    slot[HISTORY_LINE_MAX - 1] = '\0';
    h->count++;
    return 1;
}

/* Rewrites the file with the lines in memory (lock held): to a temporary
 * file renamed over it, so it is never left half-written. */
static void rewrite_file_locked(History *h)
{
    char tmp[sizeof h->path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", h->path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return;
    }
    for (size_t i = 0; i < h->count; i++)
        fprintf(f, "%s\n", history_get_locked(h, i));
    if (fclose(f) == 0 && rename(tmp, h->path) == 0)
        h->file_lines = h->count;
    else
        unlink(tmp);
}

/* Appends one line to the file (lock held). */
static void append_file_locked(History *h, const char *line)
{
    if (h->path[0] == '\0')
        return;
    int fd = open(h->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    size_t len = strnlen(line, HISTORY_LINE_MAX - 1);
    char buf[HISTORY_LINE_MAX + 1];
    memcpy(buf, line, len);
    buf[len] = '\n';
    /* One write: O_APPEND keeps lines from different writers whole. */
    ssize_t w = write(fd, buf, len + 1);
    (void)w;
    close(fd);
    if (++h->file_lines > HISTORY_MAX + HISTORY_FILE_SLACK)
        rewrite_file_locked(h);
}

void history_add(History *h, const char *line)
{
    if (is_blank_line(line))
        return;
    history_lock(h);
    if (add_locked(h, line))
        append_file_locked(h, line);
    history_unlock(h);
}

int history_set_file(History *h, const char *path)
{
    history_lock(h);
    snprintf(h->path, sizeof h->path, "%s", path ? path : "");
    int rc = 0;
    if (h->path[0] != '\0') {
        FILE *f = fopen(h->path, "r");
        if (f) {
            char *line = NULL;
            size_t cap = 0;
            ssize_t n;
            size_t lines = 0;
            while ((n = getline(&line, &cap, f)) >= 0) {
                while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                    line[--n] = '\0';
                lines++;
                if (!is_blank_line(line))
                    add_locked(h, line);
            }
            free(line);
            fclose(f);
            h->dropped = 0;   /* numbering starts with what was loaded */
            h->file_lines = lines;
            /* Keep only the last HISTORY_MAX in the file too. */
            if (lines > h->count)
                rewrite_file_locked(h);
        } else if (errno != ENOENT) {
            rc = -1;
        }
    }
    history_unlock(h);
    return rc;
}

size_t history_count(History *h)
{
    history_lock(h);
    size_t n = h->count;
    history_unlock(h);
    return n;
}

char *history_dup(History *h, size_t i)
{
    history_lock(h);
    const char *s = history_get_locked(h, i);
    char *copy = s ? strdup(s) : NULL;
    history_unlock(h);
    return copy;
}

void history_clear(History *h)
{
    history_lock(h);
    h->start = h->count = h->dropped = 0;
    if (h->path[0] != '\0') {
        int fd = open(h->path, O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd >= 0)
            close(fd);
        h->file_lines = 0;
    }
    history_unlock(h);
}
