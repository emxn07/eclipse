/* parser.h - the syntax tree and the parser that builds it.
 *
 *   list      := and_or { (';' | '&' | NEWLINE) and_or } [';' | '&']
 *   and_or    := pipeline { ('&&' | '||') pipeline }
 *   pipeline  := ['!'] command { '|' command }
 *   command   := { NAME=value } { word | redirection }
 */
#ifndef ECLIPSE_PARSER_H
#define ECLIPSE_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "lexer.h"

typedef struct {
    RedirType type;
    int fd;        /* descriptor being redirected */
    char *target;  /* unexpanded file name, descriptor number or text */
} Redir;

typedef struct {
    char **assigns;  /* unexpanded NAME=value words */
    size_t nassigns;
    char **words;    /* unexpanded command name and arguments */
    size_t nwords;
    Redir *redirs;
    size_t nredirs;
} Command;

typedef struct {
    Command **cmds;
    size_t ncmds;
    bool negate;     /* leading '!' */
} Pipeline;

typedef enum { CONN_AND, CONN_OR } Conn;

typedef struct {
    Pipeline **pipes;
    Conn *conns;     /* conns[i] joins pipes[i] and pipes[i + 1] */
    size_t npipes;
    bool background; /* terminated by '&' */
} AndOr;

typedef struct {
    AndOr **items;
    size_t n;
} List;

typedef enum { PARSE_OK, PARSE_INCOMPLETE, PARSE_ERROR } ParseStatus;

/* Parses src (expanding aliases). On PARSE_OK *out is a list (possibly
 * empty) to release with list_free(). PARSE_INCOMPLETE means the input
 * ends mid-command and more lines should be read. On PARSE_ERROR *err
 * receives a newly allocated message. */
ParseStatus parse(const char *src, List **out, char **err);
void list_free(List *l);

/* Reconstructs printable source for job listings (new strings). */
char *pipeline_text(const Pipeline *p);
char *andor_text(const AndOr *ao);

#endif
