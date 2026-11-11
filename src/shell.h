/* shell.h - global shell state and top-level entry points. */
#ifndef ECLIPSE_SHELL_H
#define ECLIPSE_SHELL_H

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <termios.h>

#define ECLIPSE_VERSION "0.1.0"

typedef struct {
    bool interactive;        /* reading commands from a terminal */
    bool job_control;        /* we own the terminal and manage process groups */
    int tty;                 /* private descriptor for the terminal, or -1 */
    pid_t pid;               /* the main shell's pid ($$), even in subshells */
    pid_t pgid;              /* the shell's own process group */
    struct termios tmodes;   /* terminal modes to restore after each job */

    int last_status;         /* $? */
    pid_t last_bg_pid;       /* $! */

    bool should_exit;        /* set by `exit`; unwinds every loop */
    int exit_status;
    bool exit_warned;        /* already warned about stopped jobs */
    bool abort_line;         /* a foreground job died from Ctrl+C */

    const char *arg0;        /* $0 */
    char **pos_args;         /* $1.. */
    int pos_argc;

    int source_depth;        /* nesting of `source` */
} Shell;

extern Shell shell;

/* Set from the SIGINT handler; long-running builtins poll it. */
extern volatile sig_atomic_t shell_interrupted;
/* Set from the SIGWINCH handler. */
extern volatile sig_atomic_t shell_winch;

/* Claims the terminal and installs signal handling for interactive use. */
void shell_init_interactive(void);
/* Restores default signal dispositions in a forked child. */
void shell_child_signals(void);
/* Marks this (forked) process as a non-interactive subshell. */
void shell_become_subshell(void);

/* Parses and runs a complete piece of source. Returns its exit status.
 * `origin` (may be NULL) and `lineno` are used in syntax error messages. */
int shell_run_string(const char *src, const char *origin, int lineno);
/* Runs commands from a stream, joining lines until they parse. */
int shell_run_stream(FILE *fp, const char *origin);
/* Sources a file in the current shell. Returns its status, or -1 if the
 * file could not be opened (errno is set). */
int shell_source_file(const char *path);
/* Runs `cmd` in a subshell and returns its output with trailing newlines
 * removed. Updates shell.last_status. Never returns NULL. */
char *shell_capture(const char *cmd);

/* Location of the config file (new string). */
char *shell_config_path(void);
/* Loads the config file if it exists and is safe to read. */
void shell_load_config(const char *override_path);

#endif
