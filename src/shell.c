/* shell.c - global state, signal setup, and running source text. */
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "exec.h"
#include "parser.h"
#include "util.h"

#define MAX_SOURCE_DEPTH 32

Shell shell = { .tty = -1 };
volatile sig_atomic_t shell_interrupted;
volatile sig_atomic_t shell_winch;

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

static void on_sigint(int sig)
{
    (void)sig;
    shell_interrupted = 1;
}

static void on_sigwinch(int sig)
{
    (void)sig;
    shell_winch = 1;
}

/* SIGTERM/SIGHUP while the line editor has the terminal in raw mode:
 * put it back before dying. Only async-signal-safe calls are used. */
static void on_fatal(int sig)
{
    if (shell.tty >= 0)
        tcsetattr(shell.tty, TCSANOW, &shell.tmodes);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: blocking reads must notice */
    sigaction(sig, &sa, NULL);
}

void shell_init_interactive(void)
{
    shell.interactive = true;
    shell.tty = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
    if (shell.tty < 0)
        return;

    /* If we were started in the background, wait to be foregrounded. */
    pid_t fg;
    while ((fg = tcgetpgrp(shell.tty)) != -1 && fg != getpgrp())
        kill(0, SIGTTIN);

    install(SIGINT, on_sigint);
    install(SIGWINCH, on_sigwinch);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    if (fg == -1)
        return; /* no controlling terminal: no job control */

    /* Lead our own process group and take the terminal. */
    shell.pgid = getpid();
    if (setpgid(shell.pgid, shell.pgid) < 0 && getpgrp() != shell.pgid)
        shell.pgid = getpgrp();
    tcsetpgrp(shell.tty, shell.pgid);
    if (tcgetattr(shell.tty, &shell.tmodes) != 0)
        return;
    shell.job_control = true;

    install(SIGTERM, on_fatal);
    install(SIGHUP, on_fatal);
}

void shell_child_signals(void)
{
    static const int sigs[] = { SIGINT,  SIGQUIT, SIGTSTP,  SIGTTIN,
                                SIGTTOU, SIGTERM, SIGHUP,   SIGWINCH,
                                SIGCHLD, SIGPIPE };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        signal(sigs[i], SIG_DFL);
}

void shell_become_subshell(void)
{
    if (shell.interactive)
        shell_child_signals();
    shell.interactive = false;
    shell.job_control = false;
}

/* ------------------------------------------------------------------ */
/* Running source text                                                 */
/* ------------------------------------------------------------------ */

static void report_syntax(const char *origin, int lineno, const char *msg)
{
    if (origin && lineno > 0)
        msg_error("%s:%d: %s", origin, lineno, msg);
    else if (origin)
        msg_error("%s: %s", origin, msg);
    else
        msg_error("%s", msg);
}

static const char incomplete_msg[] =
    "syntax error: unexpected end of input "
    "(unclosed quote, or a line ending in |, && or \\)";

/* Source text is run one complete command at a time: lines are fed in
 * and executed as soon as they parse. That way a line can depend on the
 * ones before it (an alias defined on line 1 works on line 2), and a
 * mistake on one line does not stop the rest of a config file. */
typedef struct {
    StrBuf pending;     /* lines read but not yet a complete command */
    const char *origin; /* file name for error messages, or NULL */
    int start;          /* line number where `pending` began */
    int status;
} Runner;

static void runner_init(Runner *r, const char *origin)
{
    sb_init(&r->pending);
    r->origin = origin;
    r->start = 0;
    r->status = shell.last_status;
}

static bool runner_stopped(void)
{
    return shell.should_exit || shell.abort_line || shell_interrupted;
}

/* Adds one line (with or without its newline). */
static void runner_feed(Runner *r, const char *line, size_t len, int lineno)
{
    if (r->pending.len == 0)
        r->start = lineno;
    sb_putn(&r->pending, line, len);

    List *list;
    char *err;
    switch (parse(sb_cstr(&r->pending), &list, &err)) {
    case PARSE_INCOMPLETE:
        return; /* wait for the next line */
    case PARSE_ERROR:
        report_syntax(r->origin, r->start, err);
        free(err);
        r->status = shell.last_status = 2;
        break;
    case PARSE_OK:
        r->status = exec_list(list);
        list_free(list);
        break;
    }
    sb_clear(&r->pending);
}

static int runner_finish(Runner *r)
{
    if (r->pending.len > 0 && !runner_stopped()) {
        report_syntax(r->origin, r->start, incomplete_msg);
        r->status = shell.last_status = 2;
    }
    sb_free(&r->pending);
    return shell.should_exit ? shell.exit_status : r->status;
}

int shell_run_string(const char *src, const char *origin, int lineno)
{
    Runner r;
    runner_init(&r, origin);
    const char *p = src;
    while (*p && !runner_stopped()) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        runner_feed(&r, p, len, lineno);
        p += len;
        if (lineno > 0)
            lineno++;
    }
    return runner_finish(&r);
}

int shell_run_stream(FILE *fp, const char *origin)
{
    Runner r;
    runner_init(&r, origin);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int lineno = 0;
    while (!runner_stopped() && (n = getline(&line, &cap, fp)) >= 0)
        runner_feed(&r, line, (size_t)n, ++lineno);
    free(line);
    return runner_finish(&r);
}

int shell_source_file(const char *path)
{
    FILE *fp = fopen(path, "re");
    if (!fp)
        return -1;
    struct stat st;
    if (fstat(fileno(fp), &st) == 0 && S_ISDIR(st.st_mode)) {
        fclose(fp);
        errno = EISDIR;
        return -1;
    }
    if (shell.source_depth >= MAX_SOURCE_DEPTH) {
        fclose(fp);
        msg_error("source: '%s': files are nested too deeply (a file that "
                  "sources itself?)", path);
        return 1;
    }
    shell.source_depth++;
    int status = shell_run_stream(fp, path);
    shell.source_depth--;
    fclose(fp);
    return status;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

char *shell_config_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *base = (xdg && xdg[0] == '/') ? xstrdup(xdg)
                                        : path_join(home_dir(), ".config");
    char *path = path_join(base, "eclipse/config");
    free(base);
    return path;
}

void shell_load_config(const char *override_path)
{
    char *path = override_path ? xstrdup(override_path) : shell_config_path();
    struct stat st;
    if (stat(path, &st) != 0) {
        if (override_path)
            msg_error("cannot read config '%s': %s", path, strerror(errno));
        free(path);
        return;
    }
    /* The config runs commands, so refuse one that somebody else could
     * have written. */
    if (st.st_uid != geteuid() && st.st_uid != 0) {
        msg_error("ignoring config '%s': it belongs to another user", path);
    } else if (st.st_mode & S_IWOTH) {
        msg_error("ignoring config '%s': it is writable by everyone", path);
        msg_hint("fix it with: chmod o-w %s", path);
    } else if (shell_source_file(path) < 0) {
        msg_error("cannot read config '%s': %s", path, strerror(errno));
    }
    /* `exit` or Ctrl+C inside the config must not end the session */
    shell.should_exit = false;
    shell.abort_line = false;
    shell_interrupted = 0;
    free(path);
}
