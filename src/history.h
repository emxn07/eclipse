/* history.h - command history with persistent storage. */
#ifndef ECLIPSE_HISTORY_H
#define ECLIPSE_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

/* Loads the history file. With persist == false nothing is read or
 * written; history lives in memory only. */
void history_init(bool persist);
void history_free(void);

/* Remembers a command. Blank lines, lines starting with a space and
 * immediate repeats are ignored. */
void history_add(const char *line);
size_t history_count(void);
/* Entry i, where 0 is the oldest. */
const char *history_get(size_t i);
/* Forgets everything, in memory and on disk. */
void history_clear(void);
/* Path of the history file, or NULL when not persisting. */
const char *history_file(void);

/* Expands "!!" (the previous command) and "!$" (its last word) outside
 * single quotes. Returns a new string and sets *changed if anything was
 * replaced; returns NULL if there is no previous command to refer to. */
char *history_expand(const char *line, bool *changed);

/* One history entry <-> one line of the file (newlines are escaped).
 * Both return newly allocated strings. */
char *history_encode(const char *entry);
char *history_decode(const char *line);

#endif
