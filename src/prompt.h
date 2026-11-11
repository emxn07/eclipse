/* prompt.h - renders the prompt from a format string. */
#ifndef ECLIPSE_PROMPT_H
#define ECLIPSE_PROMPT_H

#include <stdbool.h>

/* Expands a prompt format (see `help set` for the escapes) into text
 * ready to print. `color` controls whether escape sequences are
 * emitted. Returns a new string. */
char *prompt_render(const char *format, int last_status, bool color);

/* Name of the git branch that `dir` is on, a short commit id for a
 * detached HEAD, or NULL outside a repository. Reads .git directly;
 * no process is started. Returns a new string. */
char *git_branch(const char *dir);

#endif
