/* builtins.h - commands implemented inside the shell. */
#ifndef ECLIPSE_BUILTINS_H
#define ECLIPSE_BUILTINS_H

#include <stdbool.h>
#include <stddef.h>

typedef int (*BuiltinFn)(int argc, char **argv);

typedef enum { GROUP_FRIENDLY, GROUP_CORE, GROUP_JOBS } BuiltinGroup;

typedef struct {
    const char *name;
    BuiltinFn fn;
    BuiltinGroup group;
    const char *usage;    /* argument synopsis, may be "" */
    const char *summary;  /* one line */
} Builtin;

const Builtin *builtin_find(const char *name);
size_t builtin_count(void);
const Builtin *builtin_at(size_t i);

/* Changes directory, maintaining PWD and OLDPWD. With `remember` the
 * directory being left is pushed on the stack used by `back`. `cmd`
 * names the calling command in error messages. Returns 0 or 1. */
int shell_chdir(const char *target, const char *cmd, bool remember);
/* Pops the directory stack used by `back`; NULL when it is empty.
 * The caller owns the result. */
char *dirstack_pop(void);
void dirstack_free(void);

/* The human-friendly commands, defined in friendly.c. */
int builtin_go(int argc, char **argv);
int builtin_whereami(int argc, char **argv);
int builtin_up(int argc, char **argv);
int builtin_back(int argc, char **argv);
int builtin_mkcd(int argc, char **argv);
int builtin_findfile(int argc, char **argv);
int builtin_sysinfo(int argc, char **argv);
int builtin_recent(int argc, char **argv);
int builtin_extract(int argc, char **argv);

#endif
