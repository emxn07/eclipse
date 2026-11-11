/* alias.h - the alias table. */
#ifndef ECLIPSE_ALIAS_H
#define ECLIPSE_ALIAS_H

#include <stdbool.h>
#include <stddef.h>

/* Returns the replacement text for an alias, or NULL. */
const char *alias_get(const char *name);
void alias_set(const char *name, const char *value);
bool alias_unset(const char *name);
void alias_clear(void);
/* Aliases are kept sorted by name; iterate with index 0..alias_count()-1. */
size_t alias_count(void);
const char *alias_name(size_t i);
const char *alias_value(size_t i);
/* A valid alias name is non-empty and free of shell metacharacters. */
bool alias_valid_name(const char *name);

#endif
