/* suggest.h - "did you mean" suggestions for mistyped names. */
#ifndef ECLIPSE_SUGGEST_H
#define ECLIPSE_SUGGEST_H

#include <stddef.h>

#include "util.h"

/* Optimal string alignment distance: the number of single-character
 * insertions, deletions, substitutions and adjacent transpositions
 * needed to turn a into b. */
size_t edit_distance(const char *a, const char *b);

/* The largest distance at which a candidate still counts as "close"
 * to a name of the given length. */
size_t suggest_threshold(size_t len);

/* Appends up to max command names (builtins, aliases, programs in
 * $PATH) that closely resemble name, best first. */
void suggest_commands(const char *name, StrVec *out, size_t max);

/* Returns the candidate closest to name (a new string), or NULL if
 * none is close enough. */
char *suggest_closest(const char *name, char *const *candidates, size_t n);

/* Appends the names of executables in $PATH that start with prefix. */
void path_commands(const char *prefix, StrVec *out);

#endif
