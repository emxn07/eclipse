/* arith.h - integer arithmetic for $((...)). */
#ifndef ECLIPSE_ARITH_H
#define ECLIPSE_ARITH_H

#include <stdbool.h>

/* Evaluates a C-style integer expression. Bare names are read as shell
 * variables (unset or non-numeric ones count as 0). On failure returns
 * false and stores a newly allocated message in *err. */
bool arith_eval(const char *expr, long long *result, char **err);

#endif
