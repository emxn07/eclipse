/* vars.h - shell variables (local to the shell) and environment variables. */
#ifndef ECLIPSE_VARS_H
#define ECLIPSE_VARS_H

#include <stdbool.h>

#include "util.h"

/* Looks a variable up: shell variables first, then the environment.
 * Returns NULL if unset. The pointer is valid until the next change. */
const char *var_get(const char *name);
/* Assigns a variable. If it is already exported the environment is
 * updated, otherwise it becomes (or stays) a shell variable. */
void var_set(const char *name, const char *value);
/* Exports a variable; value may be NULL to export the current value. */
void var_export(const char *name, const char *value);
void var_unset(const char *name);
bool var_is_exported(const char *name);
/* Appends the names of all variables (shell and environment) to out. */
void var_names(StrVec *out);
void vars_free(void);

#endif
