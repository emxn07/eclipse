/* suggest.c - "did you mean" suggestions for mistyped names. */
#include "suggest.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alias.h"
#include "builtins.h"

#define MAX_NAME 128 /* longer names are never suggested */

size_t edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0)
        return lb;
    if (lb == 0)
        return la;

    /* three rolling rows: i-2, i-1 and i */
    size_t *rows = xmalloc(3 * (lb + 1) * sizeof *rows);
    size_t *prev2 = rows, *prev = rows + (lb + 1), *cur = rows + 2 * (lb + 1);

    for (size_t j = 0; j <= lb; j++)
        prev[j] = j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = i;
        for (size_t j = 1; j <= lb; j++) {
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            size_t best = prev[j] + 1;          /* deletion */
            if (cur[j - 1] + 1 < best)
                best = cur[j - 1] + 1;          /* insertion */
            if (prev[j - 1] + cost < best)
                best = prev[j - 1] + cost;      /* substitution */
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] &&
                a[i - 2] == b[j - 1] && prev2[j - 2] + 1 < best)
                best = prev2[j - 2] + 1;        /* transposition */
            cur[j] = best;
        }
        size_t *tmp = prev2;
        prev2 = prev;
        prev = cur;
        cur = tmp;
    }
    size_t d = prev[lb];
    free(rows);
    return d;
}

size_t suggest_threshold(size_t len)
{
    if (len <= 3)
        return 1;
    if (len <= 6)
        return 2;
    return 3;
}

/* Calls fn for every executable name in $PATH. */
static void each_path_command(void (*fn)(const char *name, void *ctx),
                              void *ctx)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        return;
    char *copy = xstrdup(path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        DIR *d = opendir(dir);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            if (e->d_type == DT_DIR)
                continue;
            if (faccessat(dirfd(d), e->d_name, X_OK, 0) != 0)
                continue;
            if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
                struct stat st;
                if (fstatat(dirfd(d), e->d_name, &st, 0) != 0 ||
                    !S_ISREG(st.st_mode))
                    continue;
            }
            fn(e->d_name, ctx);
        }
        closedir(d);
    }
    free(copy);
}

/* ---- prefix listing (used by completion) ---- */

typedef struct {
    const char *prefix;
    size_t plen;
    StrVec *out;
} PrefixCtx;

static void prefix_visit(const char *name, void *ctx)
{
    PrefixCtx *p = ctx;
    if (strncmp(name, p->prefix, p->plen) == 0)
        sv_push_copy(p->out, name);
}

void path_commands(const char *prefix, StrVec *out)
{
    PrefixCtx ctx = { prefix, strlen(prefix), out };
    each_path_command(prefix_visit, &ctx);
}

/* ---- closest matches ---- */

typedef struct {
    char *name;
    size_t dist;
    int scrambled; /* 0 if it is the same letters in another order */
    int bonus;     /* 0 if it shares the first letter, else 1 */
} Match;

/* True if a and b consist of exactly the same characters. Swapping two
 * letters is the most common typing mistake, so such candidates are by
 * far the likeliest intent. */
static bool same_letters(const char *a, const char *b)
{
    int counts[256] = { 0 };
    for (; *a; a++)
        counts[(unsigned char)*a]++;
    for (; *b; b++)
        counts[(unsigned char)*b]--;
    for (size_t i = 0; i < 256; i++)
        if (counts[i] != 0)
            return false;
    return true;
}

typedef struct {
    const char *name;
    size_t len;
    size_t threshold;
    Match *matches;
    size_t n, cap;
} MatchCtx;

static void match_visit(const char *cand, void *vctx)
{
    MatchCtx *c = vctx;
    size_t cl = strlen(cand);
    if (cl > MAX_NAME)
        return;
    size_t diff = cl > c->len ? cl - c->len : c->len - cl;
    if (diff > c->threshold || strcmp(cand, c->name) == 0)
        return;
    size_t d = edit_distance(c->name, cand);
    if (d > c->threshold || d >= cl)
        return; /* too different, or nothing in common */
    for (size_t i = 0; i < c->n; i++)
        if (strcmp(c->matches[i].name, cand) == 0)
            return;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->matches = xrealloc(c->matches, c->cap * sizeof *c->matches);
    }
    c->matches[c->n].name = xstrdup(cand);
    c->matches[c->n].dist = d;
    c->matches[c->n].scrambled = same_letters(c->name, cand) ? 0 : 1;
    c->matches[c->n].bonus = cand[0] == c->name[0] ? 0 : 1;
    c->n++;
}

static int cmp_match(const void *a, const void *b)
{
    const Match *x = a, *y = b;
    if (x->dist != y->dist)
        return x->dist < y->dist ? -1 : 1;
    if (x->scrambled != y->scrambled)
        return x->scrambled - y->scrambled;
    if (x->bonus != y->bonus)
        return x->bonus - y->bonus;
    return strcmp(x->name, y->name);
}

static void match_finish(MatchCtx *c, StrVec *out, size_t max)
{
    if (c->n > 1)
        qsort(c->matches, c->n, sizeof *c->matches, cmp_match);
    for (size_t i = 0; i < c->n; i++) {
        /* only the best tier: a worse guess next to a good one is noise */
        bool best_tier = c->matches[i].dist == c->matches[0].dist &&
                         c->matches[i].scrambled == c->matches[0].scrambled;
        if (i < max && best_tier)
            sv_push(out, c->matches[i].name);
        else
            free(c->matches[i].name);
    }
    free(c->matches);
}

void suggest_commands(const char *name, StrVec *out, size_t max)
{
    size_t len = strlen(name);
    if (len == 0 || len > MAX_NAME)
        return;
    MatchCtx c = { name, len, suggest_threshold(len), NULL, 0, 0 };
    for (size_t i = 0; i < builtin_count(); i++)
        match_visit(builtin_at(i)->name, &c);
    for (size_t i = 0; i < alias_count(); i++)
        match_visit(alias_name(i), &c);
    each_path_command(match_visit, &c);
    match_finish(&c, out, max);
}

char *suggest_closest(const char *name, char *const *candidates, size_t n)
{
    size_t len = strlen(name);
    if (len == 0 || len > MAX_NAME)
        return NULL;
    MatchCtx c = { name, len, suggest_threshold(len), NULL, 0, 0 };
    for (size_t i = 0; i < n; i++)
        match_visit(candidates[i], &c);
    StrVec best;
    sv_init(&best);
    match_finish(&c, &best, 1);
    char *result = best.len ? xstrdup(best.items[0]) : NULL;
    sv_free(&best);
    return result;
}
