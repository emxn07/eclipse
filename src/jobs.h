/* jobs.h - the job table and foreground/background control. */
#ifndef ECLIPSE_JOBS_H
#define ECLIPSE_JOBS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

typedef struct {
    pid_t pid;
    int status;      /* raw wait status */
    bool completed;
    bool stopped;
} Process;

typedef struct Job {
    int id;                 /* the number shown as [id] */
    pid_t pgid;             /* process group, or 0 without job control */
    Process *procs;
    size_t nprocs;
    char *text;             /* command line, for messages */
    bool background;
    bool stop_notified;     /* "Stopped" has been reported */
    bool have_tmodes;
    struct termios tmodes;  /* terminal modes saved when it stopped */
    unsigned long seq;      /* recency; the highest is the current job */
    struct Job *next;
} Job;

/* Building a job: create it, add processes as they are forked, then
 * register it in the table. An unregistered job is released with
 * job_discard(). */
Job *job_new(const char *text);
void job_add_process(Job *j, pid_t pid);
void job_register(Job *j);
void job_discard(Job *j);

/* Waits for a registered foreground job to finish or stop, then takes
 * the terminal back. Returns the job's exit status. A finished job is
 * removed from the table; a stopped one stays. */
int job_wait_foreground(Job *j);
/* Marks a registered job as running in the background and announces it. */
void job_started_background(Job *j);

/* Collects status changes of background jobs without blocking. */
void jobs_reap(void);
/* Reports finished and newly stopped background jobs, dropping the
 * finished ones. Called before each prompt. */
void jobs_notify(void);

/* Resolves "%1", "1", "%+", "%-", "%name" or NULL (the current job).
 * Returns NULL and sets *err to a static message when there is none. */
Job *job_lookup(const char *spec, const char **err);
int job_foreground(Job *j);
int job_background(Job *j);
/* Sends a signal to every process of the job. */
int job_signal(Job *j, int sig);
void jobs_list(void);
size_t jobs_count(void);
size_t jobs_stopped_count(void);
bool job_is_stopped(const Job *j);
bool job_is_completed(const Job *j);
/* Blocks until every background job has finished. */
int jobs_wait_all(void);
void jobs_free(void);

#endif
