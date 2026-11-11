/* complete.h - Tab completion. */
#ifndef ECLIPSE_COMPLETE_H
#define ECLIPSE_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

typedef struct {
    StrVec items;  /* candidate words, unescaped, sorted and unique */
    char *word;    /* the (unescaped) word being completed */
    size_t start;  /* byte offset in the line where that word begins */
    char quote;    /* quote character the word is inside, or 0 */
    bool verbatim; /* insert candidates as-is (e.g. $VARIABLES) */
} Completion;

/* Works out what could complete the word ending at byte `pos`. */
void complete_line(const char *line, size_t pos, Completion *out);
void completion_free(Completion *c);

/* Escapes text for insertion into a command line: inside the given
 * quote character, or with backslashes when quote is 0. New string. */
char *complete_escape(const char *text, char quote);
/* Longest common prefix of all candidates (never splits a UTF-8
 * character). New string. */
char *complete_common_prefix(const StrVec *items);

#endif
