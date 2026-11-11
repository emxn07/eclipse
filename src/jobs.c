/* jobs.c - the job table and foreground/background control.
 *
 * With job control (an interactive shell that owns the terminal) every
 * pipeline runs in its own process group, and the terminal is handed to
 * whichever group is in the foreground. Status changes are collected
 * synchronously with waitpid(): while waiting for a foreground job, and
 * once before each prompt. No work is done in signal handlers. */
#include "jobs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shell.h"
#include "util.h"

static Job *job_list;
static unsigned long seq_counter;

/* ------------------------------------------------------------------ */
/* Table management                                                    */
/* ------------------------------------------------------------------ */

Job *job_new(const char *text)
{
    Job *j = xcalloc(1, sizeof *j);
    j->text = xstrdup(text);
    return j;
}

void job_add_process(Job *j, pid_t pid)
{
    j->procs = xrealloc(j->procs, (j->nprocs + 1) * sizeof *j->procs);
    Process *p = &j->procs[j->nprocs++];
    p->pid = pid;
    p->status = 0;
    p->completed = false;
    p->stopped = false;
}

void job_register(Job *j)
{
    int max = 0;
    Job **tail = &job_list;
    for (Job *k = job_list; k; k = k->next) {
        if (k->id > max)
            max = k->id;
        tail = &k->next;
    }
    j->id = max + 1;
    j->next = NULL;
    *tail = j;
}

void job_discard(Job *j)
{
    if (!j)
        return;
    free(j->procs);
    free(j->text);
    free(j);
}

static void job_remove(Job *j)
{
    for (Job **pp = &job_list; *pp; pp = &(*pp)->next) {
        if (*pp == j) {
            *pp = j->next;
            break;
        }
    }
    job_discard(j);
}

void jobs_free(void)
{
    while (job_list) {
        Job *next = job_list->next;
        job_discard(job_list);
        job_list = next;
    }
}

bool job_is_completed(const Job *j)
{
    for (size_t i = 0; i < j->nprocs; i++)
        if (!j->procs[i].completed)
            return false;
    return true;
}

bool job_is_stopped(const Job *j)
{
    bool any = false;
    for (size_t i = 0; i < j->nprocs; i++) {
        if (j->procs[i].completed)
            continue;
        if (!j->procs[i].stopped)
            return false;
        any = true;
    }
    return any;
}

size_t jobs_count(void)
{
    size_t n = 0;
    for (Job *j = job_list; j; j = j->next)
        n++;
    return n;
}

size_t jobs_stopped_count(void)
{
    size_t n = 0;
    for (Job *j = job_list; j; j = j->next)
        if (job_is_stopped(j))
            n++;
    return n;
}

/* Records a status reported by waitpid(). */
static void mark_status(pid_t pid, int status)
{
    for (Job *j = job_list; j; j = j->next) {
        for (size_t i = 0; i < j->nprocs; i++) {
            Process *p = &j->procs[i];
            if (p->pid != pid)
                continue;
            if (WIFSTOPPED(status)) {
                p->stopped = true;
                p->status = status;
            } else if (WIFCONTINUED(status)) {
                p->stopped = false;
                j->stop_notified = false;
            } else {
                p->completed = true;
                p->stopped = false;
                p->status = status;
            }
            return;
        }
    }
}

/* Exit status of a job: that of its last process. */
static int job_status(const Job *j)
{
    if (j->nprocs == 0)
        return 0;
    int st = j->procs[j->nprocs - 1].status;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    if (WIFSTOPPED(st))
        return 128 + WSTOPSIG(st);
    return 0;
}

/* Current ('+') and previous ('-') jobs, by recency. */
static void current_jobs(Job **cur, Job **prev)
{
    *cur = *prev = NULL;
    for (Job *j = job_list; j; j = j->next) {
        if (!*cur || j->seq > (*cur)->seq) {
            *prev = *cur;
            *cur = j;
        } else if (!*prev || j->seq > (*prev)->seq) {
            *prev = j;
        }
    }
}

static char job_marker(const Job *j)
{
    Job *cur, *prev;
    current_jobs(&cur, &prev);
    return j == cur ? '+' : j == prev ? '-' : ' ';
}

/* "Running", "Stopped", "Done", "Exit 3", "Killed", ... */
static void job_state(const Job *j, char *buf, size_t size)
{
    if (job_is_completed(j)) {
        int st = j->nprocs ? j->procs[j->nprocs - 1].status : 0;
        if (WIFSIGNALED(st))
            snprintf(buf, size, "%s", strsignal(WTERMSIG(st)));
        else if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
            snprintf(buf, size, "Exit %d", WEXITSTATUS(st));
        else
            snprintf(buf, size, "Done");
    } else if (job_is_stopped(j)) {
        snprintf(buf, size, "Stopped");
    } else {
        snprintf(buf, size, "Running");
    }
}

static void job_print(const Job *j, FILE *fp)
{
    char state[64];
    job_state(j, state, sizeof state);
    fprintf(fp, "[%d]%c  %-10s %s%s\n", j->id, job_marker(j), state, j->text,
            (j->background && !job_is_completed(j) && !job_is_stopped(j))
                ? " &"
                : "");
}

/* ------------------------------------------------------------------ */
/* Waiting                                                             */
/* ------------------------------------------------------------------ */

/* Explains why a foreground job died, unless it is obvious. */
static void report_signals(const Job *j)
{
    for (size_t i = 0; i < j->nprocs; i++) {
        int st = j->procs[i].status;
        if (!WIFSIGNALED(st))
            continue;
        int sig = WTERMSIG(st);
        if (sig == SIGINT) {
            shell.abort_line = true;
            continue;
        }
        if (sig == SIGPIPE)
            continue;
        bool core = false;
#ifdef WCOREDUMP
        core = WCOREDUMP(st);
#endif
        msg_error("%s: %s%s", j->text, strsignal(sig),
                  core ? " (core dumped)" : "");
        return;
    }
}

int job_wait_foreground(Job *j)
{
    while (!job_is_completed(j) && !job_is_stopped(j)) {
        int st;
        pid_t pid = waitpid(-1, &st, shell.job_control ? WUNTRACED : 0);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            /* ECHILD: nothing left to wait for */
            for (size_t i = 0; i < j->nprocs; i++)
                j->procs[i].completed = true;
            break;
        }
        mark_status(pid, st);
    }

    bool stopped = !job_is_completed(j);
    if (shell.job_control) {
        tcsetpgrp(shell.tty, shell.pgid);
        bool signaled = false;
        for (size_t i = 0; i < j->nprocs; i++)
            if (WIFSIGNALED(j->procs[i].status))
                signaled = true;
        if (stopped)
            j->have_tmodes = tcgetattr(shell.tty, &j->tmodes) == 0;
        if (stopped || signaled) {
            /* the job may have left the terminal in any state */
            tcsetattr(shell.tty, TCSADRAIN, &shell.tmodes);
        } else {
            /* a clean exit: keep deliberate changes such as `stty` */
            tcgetattr(shell.tty, &shell.tmodes);
        }
    }

    int status = job_status(j);
    if (stopped) {
        j->background = true;
        j->stop_notified = true;
        j->seq = ++seq_counter;
        fputc('\n', stderr);
        job_print(j, stderr);
        fflush(stderr);
    } else {
        report_signals(j);
        job_remove(j);
    }
    return status;
}

void job_started_background(Job *j)
{
    j->background = true;
    j->seq = ++seq_counter;
    if (j->nprocs)
        shell.last_bg_pid = j->procs[j->nprocs - 1].pid;
    if (shell.interactive) {
        fprintf(stderr, "[%d] %ld\n", j->id, (long)shell.last_bg_pid);
        fflush(stderr);
    }
}

void jobs_reap(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        mark_status(pid, st);
}

void jobs_notify(void)
{
    Job *j = job_list;
    while (j) {
        Job *next = j->next;
        if (job_is_completed(j)) {
            if (j->background && shell.interactive)
                job_print(j, stderr);
            job_remove(j);
        } else if (job_is_stopped(j) && !j->stop_notified) {
            j->stop_notified = true;
            j->seq = ++seq_counter;
            if (shell.interactive)
                job_print(j, stderr);
        }
        j = next;
    }
    fflush(stderr);
}

int jobs_wait_all(void)
{
    for (;;) {
        bool running = false;
        for (Job *j = job_list; j; j = j->next)
            if (!job_is_completed(j) && !job_is_stopped(j))
                running = true;
        if (!running)
            break;
        int st;
        pid_t pid = waitpid(-1, &st, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) {
                if (shell_interrupted)
                    return 130;
                continue;
            }
            break;
        }
        mark_status(pid, st);
    }
    jobs_notify();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Job control commands                                                */
/* ------------------------------------------------------------------ */

Job *job_lookup(const char *spec, const char **err)
{
    Job *cur, *prev;
    current_jobs(&cur, &prev);
    *err = NULL;

    if (!spec || !*spec || strcmp(spec, "%") == 0 || strcmp(spec, "%+") == 0 ||
        strcmp(spec, "%%") == 0) {
        if (!cur)
            *err = "there are no jobs";
        return cur;
    }
    if (strcmp(spec, "%-") == 0) {
        if (!prev)
            *err = "there is no previous job";
        return prev;
    }
    const char *body = spec[0] == '%' ? spec + 1 : spec;
    long id;
    if (parse_long(body, &id)) {
        for (Job *j = job_list; j; j = j->next)
            if (j->id == id)
                return j;
        *err = "no such job";
        return NULL;
    }
    if (spec[0] == '%') {
        Job *found = NULL;
        size_t n = strlen(body);
        for (Job *j = job_list; j; j = j->next) {
            if (strncmp(j->text, body, n) == 0) {
                if (found) {
                    *err = "more than one job matches";
                    return NULL;
                }
                found = j;
            }
        }
        if (found)
            return found;
    }
    *err = "no such job";
    return NULL;
}

int job_signal(Job *j, int sig)
{
    if (j->pgid > 0)
        return kill(-j->pgid, sig);
    int rc = 0;
    for (size_t i = 0; i < j->nprocs; i++)
        if (!j->procs[i].completed && kill(j->procs[i].pid, sig) != 0)
            rc = -1;
    return rc;
}

static void mark_running(Job *j)
{
    for (size_t i = 0; i < j->nprocs; i++)
        j->procs[i].stopped = false;
    j->stop_notified = false;
}

int job_foreground(Job *j)
{
    fprintf(stderr, "%s\n", j->text);
    fflush(stderr);
    j->background = false;
    if (shell.job_control) {
        tcsetpgrp(shell.tty, j->pgid);
        if (j->have_tmodes)
            tcsetattr(shell.tty, TCSADRAIN, &j->tmodes);
    }
    mark_running(j);
    job_signal(j, SIGCONT);
    return job_wait_foreground(j);
}

int job_background(Job *j)
{
    mark_running(j);
    j->background = true;
    j->seq = ++seq_counter;
    if (job_signal(j, SIGCONT) != 0) {
        msg_error("bg: cannot resume job %d: %s", j->id, strerror(errno));
        return 1;
    }
    fprintf(stderr, "[%d]%c %s &\n", j->id, job_marker(j), j->text);
    fflush(stderr);
    return 0;
}

void jobs_list(void)
{
    jobs_reap();
    for (Job *j = job_list; j; j = j->next)
        job_print(j, stdout);
    /* finished jobs have now been shown; forget them */
    Job *j = job_list;
    while (j) {
        Job *next = j->next;
        if (job_is_completed(j))
            job_remove(j);
        j = next;
    }
}
