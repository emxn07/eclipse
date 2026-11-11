/* vars.c - shell variables (local to the shell) and environment variables. */
#include "vars.h"

#include <stdlib.h>
#include <string.h>

extern char **environ;

typedef struct {
    char *name;
    char *value;
} Var;

static Var *locals;
static size_t nlocals, caplocals;

static Var *local_find(const char *name)
{
    for (size_t i = 0; i < nlocals; i++)
        if (strcmp(locals[i].name, name) == 0)
            return &locals[i];
    return NULL;
}

static void local_remove(const char *name)
{
    Var *v = local_find(name);
    if (!v)
        return;
    free(v->name);
    free(v->value);
    *v = locals[--nlocals];
}

const char *var_get(const char *name)
{
    Var *v = local_find(name);
    if (v)
        return v->value;
    return getenv(name);
}

bool var_is_exported(const char *name)
{
    return getenv(name) != NULL;
}

void var_set(const char *name, const char *value)
{
    if (var_is_exported(name)) {
        setenv(name, value, 1);
        return;
    }
    Var *v = local_find(name);
    if (v) {
        free(v->value);
        v->value = xstrdup(value);
        return;
    }
    if (nlocals == caplocals) {
        caplocals = caplocals ? caplocals * 2 : 16;
        locals = xrealloc(locals, caplocals * sizeof *locals);
    }
    locals[nlocals].name = xstrdup(name);
    locals[nlocals].value = xstrdup(value);
    nlocals++;
}

void var_export(const char *name, const char *value)
{
    if (!value) {
        Var *v = local_find(name);
        if (v)
            setenv(name, v->value, 1);
        else if (!getenv(name))
            setenv(name, "", 1);
    } else {
        setenv(name, value, 1);
    }
    local_remove(name);
}

void var_unset(const char *name)
{
    local_remove(name);
    unsetenv(name);
}

void var_names(StrVec *out)
{
    for (size_t i = 0; i < nlocals; i++)
        sv_push_copy(out, locals[i].name);
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (eq && eq != *e)
            sv_push(out, xstrndup(*e, (size_t)(eq - *e)));
    }
}

void vars_free(void)
{
    for (size_t i = 0; i < nlocals; i++) {
        free(locals[i].name);
        free(locals[i].value);
    }
    free(locals);
    locals = NULL;
    nlocals = caplocals = 0;
}
