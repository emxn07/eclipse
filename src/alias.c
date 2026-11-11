/* alias.c - the alias table (a small sorted array). */
#include "alias.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    char *name;
    char *value;
} Alias;

static Alias *aliases;
static size_t naliases, capaliases;

/* Binary search; returns the index where name is or should be. */
static size_t alias_slot(const char *name, bool *found)
{
    size_t lo = 0, hi = naliases;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(aliases[mid].name, name);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = false;
    return lo;
}

const char *alias_get(const char *name)
{
    bool found;
    size_t i = alias_slot(name, &found);
    return found ? aliases[i].value : NULL;
}

void alias_set(const char *name, const char *value)
{
    bool found;
    size_t i = alias_slot(name, &found);
    if (found) {
        free(aliases[i].value);
        aliases[i].value = xstrdup(value);
        return;
    }
    if (naliases == capaliases) {
        capaliases = capaliases ? capaliases * 2 : 16;
        aliases = xrealloc(aliases, capaliases * sizeof *aliases);
    }
    memmove(&aliases[i + 1], &aliases[i], (naliases - i) * sizeof *aliases);
    aliases[i].name = xstrdup(name);
    aliases[i].value = xstrdup(value);
    naliases++;
}

bool alias_unset(const char *name)
{
    bool found;
    size_t i = alias_slot(name, &found);
    if (!found)
        return false;
    free(aliases[i].name);
    free(aliases[i].value);
    memmove(&aliases[i], &aliases[i + 1],
            (naliases - i - 1) * sizeof *aliases);
    naliases--;
    return true;
}

void alias_clear(void)
{
    for (size_t i = 0; i < naliases; i++) {
        free(aliases[i].name);
        free(aliases[i].value);
    }
    free(aliases);
    aliases = NULL;
    naliases = capaliases = 0;
}

size_t alias_count(void)
{
    return naliases;
}

const char *alias_name(size_t i)
{
    return aliases[i].name;
}

const char *alias_value(size_t i)
{
    return aliases[i].value;
}

bool alias_valid_name(const char *name)
{
    if (!*name)
        return false;
    return name[strcspn(name, " \t\n=|&;<>()$`\\\"'/")] == '\0';
}
