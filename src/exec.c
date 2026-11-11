/* exec.c - runs a parsed command list: redirections, pipelines,
 * builtins, external programs and command substitution. */
#include "exec.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "expand.h"
#include "jobs.h"
#include "settings.h"
#include "shell.h"
#include "suggest.h"
#include "util.h"
#include "vars.h"

/* Set whenever a command substitution runs, so that `x=$(cmd)` can
 * report the status of cmd. */
static bool capture_ran;

static int exec_andor(AndOr *ao);

/* ------------------------------------------------------------------ */
/* Saving and restoring descriptors (for builtins run in the shell)    */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    int saved; /* duplicate of the original, or -1 if fd was closed */
} SavedFd;

typedef struct {
    SavedFd *items;
    size_t n;
} FdSave;

static bool save_fd(FdSave *s, int fd)
{
    if (!s)
        return true;
    for (size_t i = 0; i < s->n; i++)
        if (s->items[i].fd == fd)
            return true; /* the original is already saved */
    int saved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    if (saved < 0 && errno != EBADF) {
        msg_error("cannot save descriptor %d: %s", fd, strerror(errno));
        return false;
    }
    s->items = xrealloc(s->items, (s->n + 1) * sizeof *s->items);
    s->items[s->n].fd = fd;
    s->items[s->n].saved = saved;
    s->n++;
    return true;
}

static void restore_fds(FdSave *s)
{
    for (size_t i = s->n; i-- > 0;) {
        if (s->items[i].saved >= 0) {
            dup2(s->items[i].saved, s->items[i].fd);
            close(s->items[i].saved);
        } else {
            close(s->items[i].fd);
        }
    }
    free(s->items);
    s->items = NULL;
    s->n = 0;
}

/* Keeps the redirections in place (used by `exec > file`). */
static void forget_fds(FdSave *s)
{
    for (size_t i = 0; i < s->n; i++)
        if (s->items[i].saved >= 0)
            close(s->items[i].saved);
    free(s->items);
    s->items = NULL;
    s->n = 0;
}

/* ------------------------------------------------------------------ */
/* Redirections                                                        */
/* ------------------------------------------------------------------ */

/* Returns a readable descriptor positioned at the start of `text`. */
static int herestring_fd(const char *text)
{
    size_t len = strlen(text);
    int fd = memfd_create("eclipse-herestring", MFD_CLOEXEC);
    if (fd < 0) {
        const char *tmp = getenv("TMPDIR");
        char *path = path_join(tmp && *tmp ? tmp : "/tmp", "eclipse-XXXXXX");
        fd = mkostemp(path, O_CLOEXEC);
        if (fd >= 0)
            unlink(path);
        free(path);
        if (fd < 0)
            return -1;
    }
    if (!write_all(fd, text, len) || !write_all(fd, "\n", 1) ||
        lseek(fd, 0, SEEK_SET) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* Points descriptor `to` at the open file `from` (which is not closed). */
static bool point_fd(int from, int to, FdSave *save)
{
    if (from == to)
        return true;
    if (!save_fd(save, to))
        return false;
    if (dup2(from, to) < 0) {
        msg_error("cannot redirect descriptor %d: %s", to, strerror(errno));
        return false;
    }
    return true;
}

static bool apply_one_redir(const Redir *r, const char *target, FdSave *save)
{
    int flags = 0;
    bool both = false;

    switch (r->type) {
    case R_IN:
        flags = O_RDONLY;
        break;
    case R_OUT:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case R_APPEND:
        flags = O_WRONLY | O_CREAT | O_APPEND;
        break;
    case R_OUT_ERR:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        both = true;
        break;
    case R_APPEND_ERR:
        flags = O_WRONLY | O_CREAT | O_APPEND;
        both = true;
        break;
    case R_HERESTR: {
        int fd = herestring_fd(target);
        if (fd < 0) {
            msg_error("cannot create here-string: %s", strerror(errno));
            return false;
        }
        bool ok = point_fd(fd, r->fd, save);
        if (fd != r->fd)
            close(fd);
        return ok;
    }
    case R_DUP_IN:
    case R_DUP_OUT: {
        long n;
        if (strcmp(target, "-") == 0) {
            if (!save_fd(save, r->fd))
                return false;
            close(r->fd);
            return true;
        }
        if (parse_long(target, &n) && n >= 0 && n < 1024) {
            if (fcntl((int)n, F_GETFD) < 0) {
                msg_error("%ld: bad file descriptor", n);
                return false;
            }
            return point_fd((int)n, r->fd, save);
        }
        if (r->type == R_DUP_OUT && r->fd == 1) {
            /* ">& file" is an old spelling of "&> file" */
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            both = true;
            break;
        }
        msg_error("'%s%s': expected a descriptor number after '%s'",
                  redir_symbol(r->type), target, redir_symbol(r->type));
        return false;
    }
    }

    int fd = open(target, flags, 0666);
    if (fd < 0) {
        msg_error("cannot open '%s': %s", target, strerror(errno));
        return false;
    }
    bool ok = both ? (point_fd(fd, 1, save) && point_fd(fd, 2, save))
                   : point_fd(fd, r->fd, save);
    if (both ? (fd != 1 && fd != 2) : fd != r->fd)
        close(fd);
    return ok;
}

/* Applies a command's redirections in order. With `save` the original
 * descriptors are remembered so restore_fds() can undo everything. */
static bool apply_redirs(const Command *c, FdSave *save)
{
    for (size_t i = 0; i < c->nredirs; i++) {
        const Redir *r = &c->redirs[i];
        char *target = expand_single(r->target, false);
        if (!target)
            return false;
        bool ok = apply_one_redir(r, target, save);
        free(target);
        if (!ok)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Expanded commands                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char **argv;
    int argc;
    char **names;   /* NAME of each leading NAME=value */
    char **values;  /* its expanded value */
    size_t nassigns;
} XCmd;

static void xcmd_free(XCmd *x)
{
    strv_free(x->argv);
    for (size_t i = 0; i < x->nassigns; i++) {
        free(x->names[i]);
        free(x->values[i]);
    }
    free(x->names);
    free(x->values);
    memset(x, 0, sizeof *x);
}

static bool xcmd_expand(const Command *c, XCmd *x)
{
    memset(x, 0, sizeof *x);
    x->names = xcalloc(c->nassigns + 1, sizeof *x->names);
    x->values = xcalloc(c->nassigns + 1, sizeof *x->values);
    for (size_t i = 0; i < c->nassigns; i++) {
        char *full = expand_single(c->assigns[i], true);
        if (!full) {
            xcmd_free(x);
            return false;
        }
        char *eq = strchr(full, '=');
        x->names[i] = xstrndup(full, (size_t)(eq - full));
        x->values[i] = xstrdup(eq + 1);
        x->nassigns++;
        free(full);
    }
    StrVec words;
    sv_init(&words);
    if (!expand_words(c->words, c->nwords, &words)) {
        sv_free(&words);
        xcmd_free(x);
        return false;
    }
    x->argc = (int)words.len;
    x->argv = sv_take(&words);
    return true;
}

/* ------------------------------------------------------------------ */
/* Running programs                                                    */
/* ------------------------------------------------------------------ */

/* Explains a failed execvp() and returns the conventional exit status. */
static int report_exec_failure(const char *name, int err)
{
    bool has_slash = strchr(name, '/') != NULL;

    if (is_directory(name) && (has_slash || err == ENOENT)) {
        msg_error("'%s' is a directory, not a command", name);
        msg_hint("use 'go %s' to enter it", name);
        return 126;
    }
    if (err == ENOENT && !has_slash) {
        msg_error("command not found: %s", name);
        if (settings.suggest_commands && *name) {
            StrVec s;
            sv_init(&s);
            suggest_commands(name, &s, 3);
            if (s.len > 0) {
                StrBuf sb;
                sb_init(&sb);
                for (size_t i = 0; i < s.len; i++) {
                    if (i)
                        sb_puts(&sb, i + 1 == s.len ? " or " : ", ");
                    sb_puts(&sb, s.items[i]);
                }
                msg_hint("did you mean %s?", sb_cstr(&sb));
                sb_free(&sb);
            }
            sv_free(&s);
        }
        return 127;
    }
    if (err == ENOENT) {
        msg_error("no such file or directory: %s", name);
        return 127;
    }
    if (err == EACCES) {
        msg_error("permission denied: %s", name);
        if (has_slash && access(name, F_OK) == 0)
            msg_hint("if it is a program, make it executable with: "
                     "chmod +x %s", name);
        return 126;
    }
    msg_error("cannot run '%s': %s", name, strerror(err));
    return 126;
}

/* Body of a forked child: never returns. */
static void child_exec(const Command *c, XCmd *x)
{
    if (!apply_redirs(c, NULL))
        _exit(1);
    for (size_t i = 0; i < x->nassigns; i++)
        var_export(x->names[i], x->values[i]);
    if (x->argc == 0)
        _exit(0);

    const Builtin *b = builtin_find(x->argv[0]);
    if (b) {
        int st = b->fn(x->argc, x->argv);
        fflush(NULL);
        _exit(st & 0xFF);
    }
    execvp(x->argv[0], x->argv);
    int st = report_exec_failure(x->argv[0], errno);
    fflush(NULL);
    _exit(st);
}

/* A background job without job control must not read the terminal or
 * be killed by the keyboard. */
static void detach_background_child(bool stdin_is_pipe)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    if (!stdin_is_pipe) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            if (fd != STDIN_FILENO)
                close(fd);
        }
    }
}

/* Forks every command of a pipeline and either waits for it or leaves
 * it running in the background. */
static int launch_pipeline(const Pipeline *p, XCmd *xs, bool background)
{
    char *text = pipeline_text(p);
    Job *job = job_new(text);
    free(text);

    int prev_read = -1;
    fflush(stdout);
    fflush(stderr);

    for (size_t i = 0; i < p->ncmds; i++) {
        int pfd[2] = { -1, -1 };
        if (i + 1 < p->ncmds && pipe2(pfd, O_CLOEXEC) < 0) {
            msg_error("cannot create pipe: %s", strerror(errno));
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            msg_error("cannot start a new process: %s", strerror(errno));
            if (pfd[0] >= 0) {
                close(pfd[0]);
                close(pfd[1]);
            }
            break;
        }
        if (pid == 0) {
            bool jc = shell.job_control;
            if (jc) {
                pid_t me = getpid();
                pid_t pg = job->pgid ? job->pgid : me;
                setpgid(me, pg);
                if (!background)
                    tcsetpgrp(shell.tty, pg);
            }
            shell_become_subshell();
            if (background && !jc)
                detach_background_child(prev_read >= 0);
            if (prev_read >= 0)
                dup2(prev_read, STDIN_FILENO);
            if (pfd[1] >= 0)
                dup2(pfd[1], STDOUT_FILENO);
            if (prev_read >= 0)
                close(prev_read);
            if (pfd[0] >= 0) {
                close(pfd[0]);
                close(pfd[1]);
            }
            child_exec(p->cmds[i], &xs[i]);
        }
        if (shell.job_control) {
            if (!job->pgid)
                job->pgid = pid;
            setpgid(pid, job->pgid); /* also done in the child: no race */
        }
        job_add_process(job, pid);
        if (prev_read >= 0)
            close(prev_read);
        if (pfd[1] >= 0)
            close(pfd[1]);
        prev_read = pfd[0];
    }
    if (prev_read >= 0)
        close(prev_read);

    if (job->nprocs == 0) {
        job_discard(job);
        return 1;
    }
    job_register(job);
    if (background) {
        job_started_background(job);
        return 0;
    }
    if (shell.job_control)
        tcsetpgrp(shell.tty, job->pgid);
    return job_wait_foreground(job);
}

/* Runs a builtin inside the shell process, with temporary redirections
 * and temporary NAME=value assignments. */
static int run_builtin(const Builtin *b, const Command *c, XCmd *x)
{
    FdSave save = { NULL, 0 };
    if (!apply_redirs(c, &save)) {
        restore_fds(&save);
        return 1;
    }

    char **old = xcalloc(x->nassigns + 1, sizeof *old);
    bool *was_exported = xcalloc(x->nassigns + 1, sizeof *was_exported);
    for (size_t i = 0; i < x->nassigns; i++) {
        const char *v = var_get(x->names[i]);
        old[i] = v ? xstrdup(v) : NULL;
        was_exported[i] = var_is_exported(x->names[i]);
        var_export(x->names[i], x->values[i]);
    }

    int status = b->fn(x->argc, x->argv);
    if (fflush(stdout) != 0) {
        /* e.g. `echo hi >&-`: drop the text instead of letting it leak
         * into the next command's output */
        __fpurge(stdout);
        clearerr(stdout);
        if (status == 0)
            status = 1;
    }
    fflush(stderr);

    for (size_t i = x->nassigns; i-- > 0;) {
        var_unset(x->names[i]);
        if (old[i]) {
            if (was_exported[i])
                var_export(x->names[i], old[i]);
            else
                var_set(x->names[i], old[i]);
        }
        free(old[i]);
    }
    free(old);
    free(was_exported);

    if (strcmp(b->name, "exec") == 0 && x->argc == 1 && status == 0)
        forget_fds(&save);
    else
        restore_fds(&save);
    return status;
}

/* A single foreground command: the only case where builtins run in the
 * shell itself (so that cd, export and friends can change its state). */
static int exec_simple(const Pipeline *p)
{
    const Command *c = p->cmds[0];
    XCmd x;
    capture_ran = false;
    if (!xcmd_expand(c, &x))
        return 1;
    if (shell_interrupted) {
        xcmd_free(&x);
        return 130;
    }

    int status;
    const Builtin *b;
    if (x.argc == 0) {
        FdSave save = { NULL, 0 };
        bool ok = apply_redirs(c, &save);
        restore_fds(&save);
        if (ok) {
            for (size_t i = 0; i < x.nassigns; i++)
                var_set(x.names[i], x.values[i]);
            status = capture_ran ? shell.last_status : 0;
        } else {
            status = 1;
        }
    } else if ((b = builtin_find(x.argv[0])) != NULL) {
        status = run_builtin(b, c, &x);
    } else {
        status = launch_pipeline(p, &x, false);
    }
    xcmd_free(&x);
    return status;
}

static int exec_pipeline(const Pipeline *p, bool background)
{
    int status;
    if (p->ncmds == 1 && !background) {
        status = exec_simple(p);
    } else {
        XCmd *xs = xcalloc(p->ncmds, sizeof *xs);
        size_t done = 0;
        bool ok = true;
        for (; done < p->ncmds; done++) {
            if (!xcmd_expand(p->cmds[done], &xs[done])) {
                ok = false;
                break;
            }
        }
        if (ok && shell_interrupted)
            status = 130;
        else
            status = ok ? launch_pipeline(p, xs, background) : 1;
        for (size_t i = 0; i < done; i++)
            xcmd_free(&xs[i]);
        free(xs);
    }
    if (p->negate)
        status = status == 0 ? 1 : 0;
    return status;
}

/* `a && b &` or `! a &`: the whole list runs in one background subshell. */
static int launch_background_list(AndOr *ao)
{
    char *text = andor_text(ao);
    Job *job = job_new(text);
    free(text);

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        msg_error("cannot start a new process: %s", strerror(errno));
        job_discard(job);
        return 1;
    }
    if (pid == 0) {
        bool jc = shell.job_control;
        if (jc)
            setpgid(0, 0);
        shell_become_subshell();
        if (!jc)
            detach_background_child(false);
        ao->background = false;
        int st = exec_andor(ao);
        fflush(NULL);
        _exit(st & 0xFF);
    }
    if (shell.job_control) {
        job->pgid = pid;
        setpgid(pid, pid);
    }
    job_add_process(job, pid);
    job_register(job);
    job_started_background(job);
    return 0;
}

static bool should_stop(void)
{
    return shell.should_exit || shell.abort_line || shell_interrupted;
}

static int exec_andor(AndOr *ao)
{
    int status = 0;
    for (size_t i = 0; i < ao->npipes; i++) {
        if (i > 0) {
            Conn conn = ao->conns[i - 1];
            if ((conn == CONN_AND && status != 0) ||
                (conn == CONN_OR && status == 0))
                continue;
        }
        status = exec_pipeline(ao->pipes[i], false);
        shell.last_status = status;
        if (should_stop())
            break;
    }
    return status;
}

int exec_list(List *l)
{
    int status = shell.last_status;
    for (size_t i = 0; i < l->n; i++) {
        AndOr *ao = l->items[i];
        if (ao->background) {
            if (ao->npipes == 1 && !ao->pipes[0]->negate)
                status = exec_pipeline(ao->pipes[0], true);
            else
                status = launch_background_list(ao);
        } else {
            status = exec_andor(ao);
        }
        shell.last_status = status;
        if (should_stop())
            break;
    }
    if (shell_interrupted && !shell.should_exit)
        status = shell.last_status = 130;
    return status;
}

/* ------------------------------------------------------------------ */
/* Command substitution                                                */
/* ------------------------------------------------------------------ */

char *shell_capture(const char *cmd)
{
    StrBuf out;
    sb_init(&out);
    capture_ran = true;

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) {
        msg_error("cannot create pipe: %s", strerror(errno));
        shell.last_status = 1;
        return sb_take(&out);
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        msg_error("cannot start a new process: %s", strerror(errno));
        close(pfd[0]);
        close(pfd[1]);
        shell.last_status = 1;
        return sb_take(&out);
    }
    if (pid == 0) {
        shell_become_subshell();
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int st = shell_run_string(cmd, NULL, 0);
        fflush(NULL);
        _exit(st & 0xFF);
    }
    close(pfd[1]);
    char buf[4096];
    for (;;) {
        ssize_t n = read(pfd[0], buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++)
                if (buf[i] != '\0') /* strings cannot hold NUL bytes */
                    sb_putc(&out, buf[i]);
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
    close(pfd[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        continue;
    if (WIFEXITED(st))
        shell.last_status = WEXITSTATUS(st);
    else if (WIFSIGNALED(st))
        shell.last_status = 128 + WTERMSIG(st);

    while (out.len > 0 && out.data[out.len - 1] == '\n')
        out.data[--out.len] = '\0';
    return sb_take(&out);
}
