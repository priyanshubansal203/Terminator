/*
 * keymap.c - Default key bindings, the ~/.terminatorrc parser and the
 * mapping from KeyPress events to actions.
 */
#include "keymap.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "parser.h"

enum {
    KM_CTRL  = 1,
    KM_SHIFT = 2,
    KM_ALT   = 4,
};

typedef struct {
    KeySym   sym;    /* normalised: letters lower case, keypad as main keys */
    unsigned mods;   /* KM_* */
} KeyCombo;

typedef struct {
    KeyCombo keys[KEYMAP_MAX_KEYS];
    int      n;
} KeyList;

static const struct {
    const char *name;
    const char *defaults;
    const char *help;
} ACTIONS[ACT_COUNT] = {
    [ACT_BACKWARD_CHAR]     = { "backward-char",     "Left",         "move the cursor one character left" },
    [ACT_FORWARD_CHAR]      = { "forward-char",      "Right",        "move the cursor one character right" },
    [ACT_BEGINNING_OF_LINE] = { "beginning-of-line", "Ctrl+A, Home", "move to the start of the line" },
    [ACT_END_OF_LINE]       = { "end-of-line",       "Ctrl+E, End",  "move to the end of the line" },
    [ACT_PREVIOUS_HISTORY]  = { "previous-history",  "Up",           "previous command in history" },
    [ACT_NEXT_HISTORY]      = { "next-history",      "Down",         "next command in history" },
    [ACT_KILL_TO_START]     = { "unix-line-discard", "Ctrl+U",       "delete from the cursor to the start of the line" },
    [ACT_KILL_TO_END]       = { "kill-line",         "Ctrl+K",       "delete from the cursor to the end of the line" },
    [ACT_KILL_WORD]         = { "unix-word-rubout",  "Ctrl+W",       "delete the word before the cursor" },
    [ACT_DELETE_CHAR]       = { "delete-char",       "Delete",       "delete the character under the cursor" },
    [ACT_CLEAR_SCREEN]      = { "clear-screen",      "Ctrl+L",       "clear the screen" },
    [ACT_EOF]               = { "end-of-file",       "Ctrl+D",       "exit on an empty line, end of input on a continuation line, "
                                                                     "else delete the character under the cursor" },
    [ACT_COPY]              = { "copy",              "Ctrl+Shift+C", "copy the mouse selection (or the input line) to the clipboard" },
    [ACT_PASTE]             = { "paste",             "Ctrl+Shift+V", "paste the clipboard" },
    [ACT_INTERRUPT]         = { "interrupt",         "Ctrl+C",       "send SIGINT to the foreground job" },
    [ACT_SUSPEND]           = { "suspend",           "Ctrl+Z",       "send SIGTSTP to the foreground job" },
    [ACT_SCROLL_PAGE_UP]    = { "scroll-page-up",    "Shift+Page_Up",   "scroll back one page of output" },
    [ACT_SCROLL_PAGE_DOWN]  = { "scroll-page-down",  "Shift+Page_Down", "scroll forward one page" },
    [ACT_SCROLL_LINE_UP]    = { "scroll-line-up",    "Shift+Up",        "scroll back one line" },
    [ACT_SCROLL_LINE_DOWN]  = { "scroll-line-down",  "Shift+Down",      "scroll forward one line" },
    [ACT_SCROLL_TOP]        = { "scroll-to-top",     "Shift+Home",      "scroll to the oldest output kept" },
    [ACT_SCROLL_BOTTOM]     = { "scroll-to-bottom",  "Shift+End",       "back to the newest output" },
    [ACT_NEW_TAB]           = { "new-tab",           "Ctrl+Shift+T",    "open a new tab (in this tab's directory)" },
    [ACT_CLOSE_TAB]         = { "close-tab",         "Ctrl+Shift+W",    "close this tab (its jobs are killed)" },
    [ACT_NEXT_TAB]          = { "next-tab",          "Ctrl+Page_Down, Ctrl+Tab", "switch to the next tab" },
    [ACT_PREV_TAB]          = { "previous-tab",      "Ctrl+Page_Up, Ctrl+Shift+Tab", "switch to the previous tab" },
    [ACT_HISTORY_SEARCH]    = { "reverse-search-history", "Ctrl+R",   "search the command history" },
};

static KeyList g_bindings[ACT_COUNT];
static KeyList g_defaults[ACT_COUNT];
static int     g_defaults_ready;

/* Recursive, as the public functions call each other. Held across fork()
 * (pthread_atfork), so a forked child never inherits it locked. */
static pthread_mutex_t g_lock;
static pthread_once_t  g_lock_once = PTHREAD_ONCE_INIT;

static void lock_before_fork(void);
static void unlock_after_fork(void);
static void reinit_after_fork(void);

static void make_lock(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void lock_init(void)
{
    make_lock();
    pthread_atfork(lock_before_fork, unlock_after_fork, reinit_after_fork);
}

static void lock(void)
{
    pthread_once(&g_lock_once, lock_init);
    pthread_mutex_lock(&g_lock);
}

static void unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static void lock_before_fork(void)
{
    pthread_mutex_lock(&g_lock);
}

static void unlock_after_fork(void)
{
    pthread_mutex_unlock(&g_lock);
}

/* In the child the lock belongs to a thread that does not exist there (its
 * only thread has another id), so it cannot be unlocked: make a new one. */
static void reinit_after_fork(void)
{
    make_lock();
}

/* ---- Key names ----------------------------------------------------------- */

static KeySym normalise(KeySym sym)
{
    switch (sym) {
    case XK_KP_Left:   return XK_Left;
    case XK_KP_Right:  return XK_Right;
    case XK_KP_Up:     return XK_Up;
    case XK_KP_Down:   return XK_Down;
    case XK_KP_Home:   return XK_Home;
    case XK_KP_End:    return XK_End;
    case XK_KP_Delete: return XK_Delete;
    case XK_KP_Insert: return XK_Insert;
    case XK_KP_Prior:  return XK_Prior;
    case XK_KP_Next:   return XK_Next;
    default: break;
    }
    KeySym lower, upper;
    XConvertCase(sym, &lower, &upper);
    return lower;
}

/* Spellings XStringToKeysym() does not know. */
static const struct {
    const char *name;
    KeySym      sym;
} ALIASES[] = {
    { "del", XK_Delete }, { "ins", XK_Insert }, { "esc", XK_Escape },
    { "pageup", XK_Prior }, { "pgup", XK_Prior }, { "page_up", XK_Prior },
    { "pagedown", XK_Next }, { "pgdn", XK_Next }, { "page_down", XK_Next },
    { "enter", XK_Return }, { "backspace", XK_BackSpace },
};

static KeySym keysym_by_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 1 && (unsigned char)name[0] > 0x20 && (unsigned char)name[0] < 0x7f)
        return (KeySym)(unsigned char)name[0];   /* Latin-1 keysyms are the codes */

    KeySym sym = XStringToKeysym(name);
    if (sym != NoSymbol)
        return sym;
    /* "left" -> "Left", "f5" -> "F5", "page_up" -> "Page_up" */
    char buf[64];
    if (len == 0 || len >= sizeof buf)
        return NoSymbol;
    for (size_t i = 0; i <= len; i++)
        buf[i] = (char)tolower((unsigned char)name[i]);
    buf[0] = (char)toupper((unsigned char)buf[0]);
    if ((sym = XStringToKeysym(buf)) != NoSymbol)
        return sym;
    for (size_t i = 0; i < sizeof ALIASES / sizeof ALIASES[0]; i++)
        if (strcasecmp(name, ALIASES[i].name) == 0)
            return ALIASES[i].sym;
    return NoSymbol;
}

static void combo_name(const KeyCombo *k, char *buf, size_t size)
{
    char key[64];
    if (k->sym > 0x20 && k->sym < 0x7f)
        snprintf(key, sizeof key, "%c", toupper((int)k->sym));
    else if (k->sym == XK_Prior || k->sym == XK_Next)   /* X's names for them */
        snprintf(key, sizeof key, "%s", k->sym == XK_Prior ? "Page_Up" : "Page_Down");
    else {
        const char *s = XKeysymToString(k->sym);
        snprintf(key, sizeof key, "%s", s ? s : "?");
    }
    snprintf(buf, size, "%s%s%s%s",
             k->mods & KM_CTRL ? "Ctrl+" : "", k->mods & KM_ALT ? "Alt+" : "",
             k->mods & KM_SHIFT ? "Shift+" : "", key);
}

static void list_name(const KeyList *l, char *buf, size_t size)
{
    if (l->n == 0) {
        snprintf(buf, size, "none");
        return;
    }
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < l->n && used < size; i++) {
        char one[80];
        combo_name(&l->keys[i], one, sizeof one);
        int w = snprintf(buf + used, size - used, "%s%s", i ? ", " : "", one);
        if (w < 0)
            break;
        used += (size_t)w;
    }
}

static int has_prefix(const char *s, const char *prefix)
{
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

/* Parses one key such as "Ctrl+Shift+C". Returns 0, or -1 with the reason. */
static int parse_combo(const char *text, KeyCombo *out, char *err, size_t errsize)
{
    static const struct {
        const char *prefix;
        unsigned    mod;
    } MODS[] = {
        { "ctrl+", KM_CTRL }, { "control+", KM_CTRL }, { "c-", KM_CTRL },
        { "shift+", KM_SHIFT }, { "s-", KM_SHIFT },
        { "alt+", KM_ALT }, { "meta+", KM_ALT }, { "m-", KM_ALT },
    };
    const char *s = text;
    unsigned mods = 0;
    for (;;) {
        size_t i;
        for (i = 0; i < sizeof MODS / sizeof MODS[0]; i++) {
            size_t len = strlen(MODS[i].prefix);
            /* The key itself may be "+" or "-", as in "Ctrl++". */
            if (has_prefix(s, MODS[i].prefix) && s[len] != '\0') {
                mods |= MODS[i].mod;
                s += len;
                break;
            }
        }
        if (i == sizeof MODS / sizeof MODS[0])
            break;
    }
    if (*s == '\0') {
        snprintf(err, errsize, "`%s': no key name", text);
        return -1;
    }
    KeySym sym = keysym_by_name(s);
    if (sym == NoSymbol) {
        snprintf(err, errsize, "`%s': unknown key name `%s'", text, s);
        return -1;
    }
    if (IsModifierKey(sym)) {
        snprintf(err, errsize, "`%s': a modifier key cannot be bound on its own", text);
        return -1;
    }
    if ((sym == XK_Return || sym == XK_KP_Enter || sym == XK_BackSpace ||
         sym == XK_Tab || sym == XK_ISO_Left_Tab) && !(mods & (KM_CTRL | KM_ALT))) {
        snprintf(err, errsize, "`%s': Enter, Backspace and Tab are reserved "
                 "(unless with Ctrl or Alt)", text);
        return -1;
    }
    if (sym >= 0x20 && sym <= 0xff && !(mods & (KM_CTRL | KM_ALT))) {
        snprintf(err, errsize, "`%s': a printable key needs Ctrl or Alt, or it could "
                 "no longer be typed", text);
        return -1;
    }
    out->sym  = normalise(sym);
    out->mods = mods;
    return 0;
}

static int combo_equal(const KeyCombo *a, const KeyCombo *b)
{
    return a->sym == b->sym && a->mods == b->mods;
}

static int list_has(const KeyList *l, const KeyCombo *k)
{
    for (int i = 0; i < l->n; i++)
        if (combo_equal(&l->keys[i], k))
            return 1;
    return 0;
}

/* Parses "key, key, ..." or "none". Duplicates are dropped. */
static int parse_list(const char *spec, KeyList *out, char *err, size_t errsize)
{
    out->n = 0;
    char *copy = strdup(spec);
    if (!copy) {
        snprintf(err, errsize, "out of memory");
        return -1;
    }
    int rc = 0;
    int items = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*tok))
            tok++;
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1]))
            *--end = '\0';
        if (*tok == '\0')
            continue;
        items++;
        if (strcasecmp(tok, "none") == 0)
            continue;
        KeyCombo k;
        if (parse_combo(tok, &k, err, errsize) != 0) {
            rc = -1;
            break;
        }
        if (list_has(out, &k))
            continue;
        if (out->n == KEYMAP_MAX_KEYS) {
            snprintf(err, errsize, "at most %d keys per action", KEYMAP_MAX_KEYS);
            rc = -1;
            break;
        }
        out->keys[out->n++] = k;
    }
    free(copy);
    if (rc == 0 && items == 0) {
        snprintf(err, errsize, "no keys given (use `none' to unbind)");
        rc = -1;
    }
    return rc;
}

/* ---- Bindings -------------------------------------------------------------- */

static void init_defaults(void)
{
    if (g_defaults_ready)
        return;
    for (int a = 0; a < ACT_COUNT; a++) {
        char err[128];
        if (parse_list(ACTIONS[a].defaults, &g_defaults[a], err, sizeof err) != 0)
            g_defaults[a].n = 0;   /* cannot happen: the table is valid */
    }
    g_defaults_ready = 1;
}

static void reset_unlocked(void)
{
    init_defaults();
    memcpy(g_bindings, g_defaults, sizeof g_bindings);
}

const char *keymap_action_name(KeyAction a)
{
    return a >= 0 && a < ACT_COUNT ? ACTIONS[a].name : "none";
}

const char *keymap_action_help(KeyAction a)
{
    return a >= 0 && a < ACT_COUNT ? ACTIONS[a].help : "";
}

KeyAction keymap_action_by_name(const char *name)
{
    for (int a = 0; a < ACT_COUNT; a++)
        if (strcmp(name, ACTIONS[a].name) == 0)
            return (KeyAction)a;
    return ACT_NONE;
}

static void describe_unlocked(KeyAction a, char *buf, size_t size)
{
    init_defaults();
    list_name(&g_bindings[a], buf, size);
}

static void describe_default_unlocked(KeyAction a, char *buf, size_t size)
{
    init_defaults();
    list_name(&g_defaults[a], buf, size);
}

/* The action other than `self` that `k` is bound to in `map`, or ACT_NONE. */
static KeyAction bound_elsewhere(const KeyList *map, KeyAction self, const KeyCombo *k)
{
    for (int a = 0; a < ACT_COUNT; a++)
        if (a != (int)self && list_has(&map[a], k))
            return (KeyAction)a;
    return ACT_NONE;
}

static KeyAction lookup_unlocked(XKeyEvent *ev)
{
    init_defaults();
    KeySym sym = XLookupKeysym(ev, 0);
    if (IsKeypadKey(sym)) {
        /* Keypad keys depend on NumLock, which XLookupString applies. */
        char buf[8];
        XLookupString(ev, buf, sizeof buf, &sym, NULL);
    }
    if (sym == NoSymbol)
        return ACT_NONE;
    KeyCombo k = { normalise(sym), 0 };
    if (ev->state & ControlMask)
        k.mods |= KM_CTRL;
    if (ev->state & ShiftMask)
        k.mods |= KM_SHIFT;
    if (ev->state & Mod1Mask)
        k.mods |= KM_ALT;
    for (int a = 0; a < ACT_COUNT; a++)
        if (list_has(&g_bindings[a], &k))
            return (KeyAction)a;
    return ACT_NONE;
}

static int bind_unlocked(KeyAction a, const char *spec, char *err, size_t errsize)
{
    init_defaults();
    KeyList l;
    char why[160];
    if (parse_list(spec, &l, why, sizeof why) != 0) {
        snprintf(err, errsize, "%s: %s", ACTIONS[a].name, why);
        return -1;
    }
    for (int i = 0; i < l.n; i++) {
        KeyAction other = bound_elsewhere(g_bindings, a, &l.keys[i]);
        if (other != ACT_NONE) {
            char name[80];
            combo_name(&l.keys[i], name, sizeof name);
            snprintf(err, errsize, "%s: %s is already bound to %s", ACTIONS[a].name,
                     name, ACTIONS[other].name);
            return -1;
        }
    }
    g_bindings[a] = l;
    return 0;
}

static int bind_default_unlocked(KeyAction a, char *err, size_t errsize)
{
    char spec[256];
    describe_default_unlocked(a, spec, sizeof spec);
    return bind_unlocked(a, spec, err, errsize);
}

/* ---- Config file ------------------------------------------------------------ */

const char *keymap_path(void)
{
    static char path[4096];
    lock();
    if (path[0] == '\0') {
        const char *env = getenv("TERMINATORRC");
        if (env && *env)
            snprintf(path, sizeof path, "%s", env);
        else
            snprintf(path, sizeof path, "%s/.terminatorrc", home_dir());
    }
    unlock();
    return path;
}

static void report(StrBuf *msgs, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void report(StrBuf *msgs, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (msgs->len > 0)
        sb_putc(msgs, '\n');
    sb_append(msgs, line, strlen(line));
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* Splits a config line into action name and value. Returns 0 for a blank or
 * comment line, 1 for "name = value", -1 if malformed. Modifies `line`. */
static int split_line(char *line, char **name, char **value)
{
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
    char *s = trim(line);
    if (*s == '\0')
        return 0;
    char *eq = strchr(s, '=');
    if (!eq)
        return -1;
    *eq = '\0';
    *name  = trim(s);
    *value = trim(eq + 1);
    return 1;
}

static int load_unlocked(const char *path, StrBuf *msgs)
{
    reset_unlocked();
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        report(msgs, "terminator: %s: %s; using the default key bindings", path, strerror(errno));
        return 1;
    }

    KeyList custom[ACT_COUNT];
    int     is_custom[ACT_COUNT] = { 0 };
    int     lineno_of[ACT_COUNT] = { 0 };
    int     problems = 0;
    char   *line = NULL;
    size_t  cap = 0;
    int     lineno = 0;
    while (getline(&line, &cap, f) >= 0) {
        lineno++;
        char *name, *value;
        int kind = split_line(line, &name, &value);
        if (kind == 0)
            continue;
        if (kind < 0) {
            report(msgs, "terminator: %s:%d: expected `action = key[, key...]'", path, lineno);
            problems++;
            continue;
        }
        KeyAction a = keymap_action_by_name(name);
        if (a == ACT_NONE) {
            report(msgs, "terminator: %s:%d: unknown action `%s' (see `bind -l')", path, lineno, name);
            problems++;
            continue;
        }
        char why[160];
        if (parse_list(value, &custom[a], why, sizeof why) != 0) {
            char def[256];
            list_name(&g_defaults[a], def, sizeof def);
            report(msgs, "terminator: %s:%d: %s: %s; using the default (%s)",
                   path, lineno, name, why, def);
            problems++;
            is_custom[a] = 0;   /* a later bad line undoes an earlier good one */
            continue;
        }
        is_custom[a] = 1;
        lineno_of[a] = lineno;
    }
    free(line);
    fclose(f);

    /* Conflicts: until no key is bound twice, send every customised action
     * that shares a key back to its default. Defaults never conflict with
     * each other, so this ends. */
    for (;;) {
        KeyList final[ACT_COUNT];
        for (int a = 0; a < ACT_COUNT; a++)
            final[a] = is_custom[a] ? custom[a] : g_defaults[a];

        int reverted = 0;
        for (int a = 0; a < ACT_COUNT && !reverted; a++) {
            if (!is_custom[a])
                continue;
            for (int i = 0; i < final[a].n && !reverted; i++) {
                KeyAction b = bound_elsewhere(final, (KeyAction)a, &final[a].keys[i]);
                if (b == ACT_NONE)
                    continue;
                char key[80];
                combo_name(&final[a].keys[i], key, sizeof key);
                int involved[2] = { a, b };
                for (int j = 0; j < 2; j++) {
                    int x = involved[j];
                    if (!is_custom[x])
                        continue;
                    char def[256];
                    list_name(&g_defaults[x], def, sizeof def);
                    report(msgs, "terminator: %s:%d: %s: %s conflicts with %s; using the default (%s)",
                           path, lineno_of[x], ACTIONS[x].name, key,
                           ACTIONS[involved[1 - j]].name, def);
                    problems++;
                    is_custom[x] = 0;
                }
                reverted = 1;
            }
        }
        if (!reverted) {
            memcpy(g_bindings, final, sizeof g_bindings);
            break;
        }
    }
    return problems;
}

static int same_list(const KeyList *a, const KeyList *b)
{
    if (a->n != b->n)
        return 0;
    for (int i = 0; i < a->n; i++)
        if (!list_has(b, &a->keys[i]))
            return 0;
    return 1;
}

static int save_unlocked(const char *path, KeyAction a, char *err, size_t errsize)
{
    init_defaults();
    /* Only customisations are written: a default binding removes the line. */
    char newline[512] = "";
    if (!same_list(&g_bindings[a], &g_defaults[a])) {
        char keys[256];
        list_name(&g_bindings[a], keys, sizeof keys);
        snprintf(newline, sizeof newline, "%s = %s\n", ACTIONS[a].name, keys);
    }

    StrBuf out;
    sb_init(&out);
    int rc = 0;
    FILE *in = fopen(path, "r");
    if (!in && errno != ENOENT) {
        snprintf(err, errsize, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (!in) {
        static const char HEADER[] =
            "# Terminator key bindings: action = key[, key...]   (or: action = none)\n"
            "# Run `bind -l' in Terminator for the list of actions.\n";
        rc |= sb_append(&out, HEADER, sizeof HEADER - 1);
    } else {
        char  *line = NULL;
        size_t cap = 0;
        ssize_t n;
        int replaced = 0;
        while ((n = getline(&line, &cap, in)) >= 0) {
            char *copy = strdup(line);
            char *name, *value;
            int is_ours = copy && split_line(copy, &name, &value) == 1 &&
                          strcmp(name, ACTIONS[a].name) == 0;
            free(copy);
            if (is_ours) {
                /* The first line for the action becomes the new one; any
                 * later ones would override it, so they go. */
                if (!replaced)
                    rc |= sb_append(&out, newline, strlen(newline));
                replaced = 1;
                continue;
            }
            rc |= sb_append(&out, line, (size_t)n);
            if (n > 0 && line[n - 1] != '\n')
                rc |= sb_putc(&out, '\n');
        }
        free(line);
        fclose(in);
        if (replaced)
            newline[0] = '\0';
    }
    rc |= sb_append(&out, newline, strlen(newline));
    if (rc != 0) {
        sb_free(&out);
        snprintf(err, errsize, "out of memory");
        return -1;
    }

    /* Write a temporary file and rename it over the old one, so a failure
     * never leaves a half-written config behind. */
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errsize, "%s: %s", tmp, strerror(errno));
        sb_free(&out);
        return -1;
    }
    size_t len = out.len;
    int ok = fwrite(sb_str(&out), 1, len, f) == len;
    ok &= fclose(f) == 0;
    sb_free(&out);
    if (!ok || rename(tmp, path) != 0) {
        snprintf(err, errsize, "%s: %s", path, strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

/* ---- Locked entry points ------------------------------------------------------ */

void keymap_reset(void)
{
    lock();
    reset_unlocked();
    unlock();
}

void keymap_describe(KeyAction a, char *buf, size_t size)
{
    lock();
    describe_unlocked(a, buf, size);
    unlock();
}

void keymap_describe_default(KeyAction a, char *buf, size_t size)
{
    lock();
    describe_default_unlocked(a, buf, size);
    unlock();
}

KeyAction keymap_lookup(XKeyEvent *ev)
{
    lock();
    KeyAction a = lookup_unlocked(ev);
    unlock();
    return a;
}

void keymap_translate(XKeyEvent *ev, TermKey *key)
{
    memset(key, 0, sizeof *key);
    key->action = keymap_lookup(ev);
    KeySym sym = NoSymbol;
    int n = XLookupString(ev, key->text, (int)sizeof key->text - 1, &sym, NULL);
    key->len = n > 0 ? n : 0;
    key->sym = sym;
    key->state = ev->state;
    key->modifier = IsModifierKey(XLookupKeysym(ev, 0));
}

int keymap_bind(KeyAction a, const char *spec, char *err, size_t errsize)
{
    lock();
    int rc = bind_unlocked(a, spec, err, errsize);
    unlock();
    return rc;
}

int keymap_bind_default(KeyAction a, char *err, size_t errsize)
{
    lock();
    int rc = bind_default_unlocked(a, err, errsize);
    unlock();
    return rc;
}

int keymap_load(const char *path, StrBuf *msgs)
{
    lock();
    int rc = load_unlocked(path, msgs);
    unlock();
    return rc;
}

int keymap_save(const char *path, KeyAction a, char *err, size_t errsize)
{
    lock();
    int rc = save_unlocked(path, a, err, errsize);
    unlock();
    return rc;
}
