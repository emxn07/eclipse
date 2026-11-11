/* settings.h - user-tunable options (the `set` builtin) and colours. */
#ifndef ECLIPSE_SETTINGS_H
#define ECLIPSE_SETTINGS_H

#include <stdbool.h>

#include "util.h"

typedef struct {
    /* prompt */
    char *prompt;            /* format string, see prompt.h */
    char *prompt2;           /* continuation prompt format */
    char *symbol;            /* what %s expands to */
    char *git_prefix;        /* printed before the branch name */
    char *git_suffix;        /* printed after the branch name */
    /* colours (specs such as "bold cyan" or "#ffaa00") */
    char *color_user;
    char *color_host;
    char *color_path;
    char *color_git;
    char *color_symbol;
    char *color_error;
    char *color_bracket;
    char *color_suggestion;
    /* behaviour */
    bool color;              /* master switch for all colour output */
    bool autosuggest;        /* inline history suggestions while typing */
    bool suggest_commands;   /* "did you mean" for mistyped commands */
    long history_size;       /* maximum number of remembered commands */
} Settings;

extern Settings settings;

void settings_init(void);
void settings_free(void);

/* Sets one option. On failure returns false and stores a newly
 * allocated message in *err. */
bool settings_set(const char *key, const char *value, char **err);
/* Returns a newly allocated printable value, or NULL for unknown keys. */
char *settings_get(const char *key);
/* Calls fn for every option, in display order. */
void settings_each(void (*fn)(const char *key, const char *value,
                              const char *help, void *ctx),
                   void *ctx);
/* Appends every option name to out (for completion). */
void settings_names(StrVec *out);

/* True if colour should be written to fd (tty, enabled, NO_COLOR unset). */
bool color_enabled(int fd);
/* Validates a colour spec and appends its SGR escape sequence to out.
 * Returns false (appending nothing) if the spec is not understood. */
bool color_parse(const char *spec, StrBuf *out);

#endif
