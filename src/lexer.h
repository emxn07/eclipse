/* lexer.h - turns a command line into tokens. */
#ifndef ECLIPSE_LEXER_H
#define ECLIPSE_LEXER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    T_WORD,     /* a word, still carrying its quotes and $-expressions */
    T_PIPE,     /* |  */
    T_AND,      /* && */
    T_OR,       /* || */
    T_SEMI,     /* ;  */
    T_AMP,      /* &  */
    T_NEWLINE,
    T_REDIR,    /* any redirection operator; see RedirType */
    T_EOF
} TokType;

typedef enum {
    R_IN,         /* <      */
    R_OUT,        /* > , >| */
    R_APPEND,     /* >>     */
    R_DUP_OUT,    /* >&N    */
    R_DUP_IN,     /* <&N    */
    R_OUT_ERR,    /* &>     */
    R_APPEND_ERR, /* &>>    */
    R_HERESTR     /* <<<    */
} RedirType;

typedef struct {
    TokType type;
    char *text;       /* T_WORD only */
    RedirType rtype;  /* T_REDIR only */
    int fd;           /* T_REDIR only: explicit descriptor, or -1 */
} Token;

typedef struct {
    Token *items;
    size_t len;
    size_t cap;
} TokVec;

typedef enum { LEX_OK, LEX_INCOMPLETE, LEX_ERROR } LexStatus;

/* Tokenises src. The vector always ends with T_EOF on LEX_OK.
 * LEX_INCOMPLETE means more input is needed (open quote, trailing
 * backslash). On LEX_ERROR *err receives a newly allocated message. */
LexStatus lex(const char *src, TokVec *out, char **err);
void tokvec_init(TokVec *tv);
void tokvec_free(TokVec *tv);

/* Scanners for nested constructs, shared with the expander. Each takes
 * the index of the opening delimiter and returns the index just past
 * the matching closing delimiter, or (size_t)-1 if it is unterminated. */
size_t lex_skip_single(const char *s, size_t i); /* at ' */
size_t lex_skip_double(const char *s, size_t i); /* at " */
size_t lex_skip_paren(const char *s, size_t i);  /* at ( */
size_t lex_skip_brace(const char *s, size_t i);  /* at { */
size_t lex_skip_btick(const char *s, size_t i);  /* at ` */

/* Human-readable spelling of a token, for error messages. */
const char *token_describe(const Token *t);
const char *redir_symbol(RedirType t);

#endif
