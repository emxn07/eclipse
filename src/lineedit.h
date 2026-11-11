/* lineedit.h - the interactive line editor. */
#ifndef ECLIPSE_LINEEDIT_H
#define ECLIPSE_LINEEDIT_H

#include <stdbool.h>

typedef enum {
    LE_LINE,      /* *line holds what the user entered */
    LE_EOF,       /* Ctrl+D on an empty line, or the terminal went away */
    LE_INTERRUPT  /* Ctrl+C: the line was abandoned */
} LeResult;

/* True if the terminal can be driven with escape sequences. When it
 * cannot (TERM=dumb), lineedit_read falls back to plain line input. */
bool lineedit_supported(void);

/* Shows the prompt and reads one line with editing, history, Tab
 * completion and inline suggestions. On LE_LINE the caller owns *line
 * (which has no trailing newline). */
LeResult lineedit_read(const char *prompt, char **line);

void lineedit_free(void);

#endif
