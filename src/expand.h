/* expand.h - word expansion: braces, tilde, variables, command and
 * arithmetic substitution, field splitting, globbing, quote removal. */
#ifndef ECLIPSE_EXPAND_H
#define ECLIPSE_EXPAND_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

/* Expands n raw words into zero or more fields appended to out.
 * Returns false (after printing a message) if an expansion fails. */
bool expand_words(char *const *raw, size_t n, StrVec *out);

/* Expands one raw word into exactly one string: no field splitting and
 * no globbing. Used for assignments and redirection targets. With
 * `assign` set, a tilde is also expanded after '=' and ':'. Returns a
 * new string, or NULL (after printing a message) on failure. */
char *expand_single(const char *raw, bool assign);

/* Brace expansion only ({a,b}, {1..3}); exposed for testing. */
void expand_braces(const char *word, StrVec *out);

#endif
