/* exec.h - runs a parsed command list. */
#ifndef ECLIPSE_EXEC_H
#define ECLIPSE_EXEC_H

#include "parser.h"

/* Executes every and-or list in l and returns the last exit status.
 * Also keeps shell.last_status up to date. */
int exec_list(List *l);

#endif
