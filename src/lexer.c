/* lexer.c - turns a command line into tokens.
 *
 * Words keep their quoting and $-expressions verbatim; the expander
 * (expand.c) interprets them later. The lexer only needs to know where a
 * word ends, which means understanding how quotes and $(...) nest. */
#include "lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define UNTERMINATED ((size_t)-1)

/* ------------------------------------------------------------------ */
/* Nested-construct scanners                                           */
/* ------------------------------------------------------------------ */

size_t lex_skip_single(const char *s, size_t i)
{
    i++;
    while (s[i] && s[i] != '\'')
        i++;
    return s[i] ? i + 1 : UNTERMINATED;
}

size_t lex_skip_btick(const char *s, size_t i)
{
    i++;
    while (s[i]) {
        if (s[i] == '\\') {
            if (!s[i + 1])
                return UNTERMINATED;
            i += 2;
        } else if (s[i] == '`') {
            return i + 1;
        } else {
            i++;
        }
    }
    return UNTERMINATED;
}

/* Skips one quoted or nested region starting at s[i] if there is one.
 * Returns the new index, i itself if s[i] starts nothing special, or
 * UNTERMINATED. `in_dquote` limits which constructs are recognised. */
static size_t skip_nested(const char *s, size_t i, bool in_dquote)
{
    switch (s[i]) {
    case '\\':
        return s[i + 1] ? i + 2 : UNTERMINATED;
    case '\'':
        return in_dquote ? i : lex_skip_single(s, i);
    case '"':
        return in_dquote ? i : lex_skip_double(s, i);
    case '`':
        return lex_skip_btick(s, i);
    case '$':
        if (s[i + 1] == '(')
            return lex_skip_paren(s, i + 1);
        if (s[i + 1] == '{')
            return lex_skip_brace(s, i + 1);
        return i;
    default:
        return i;
    }
}

size_t lex_skip_double(const char *s, size_t i)
{
    i++;
    while (s[i]) {
        if (s[i] == '"')
            return i + 1;
        size_t j = skip_nested(s, i, true);
        if (j == UNTERMINATED)
            return UNTERMINATED;
        i = (j == i) ? i + 1 : j;
    }
    return UNTERMINATED;
}

size_t lex_skip_paren(const char *s, size_t i)
{
    i++;
    while (s[i]) {
        if (s[i] == ')')
            return i + 1;
        size_t j;
        if (s[i] == '(')
            j = lex_skip_paren(s, i);
        else
            j = skip_nested(s, i, false);
        if (j == UNTERMINATED)
            return UNTERMINATED;
        i = (j == i) ? i + 1 : j;
    }
    return UNTERMINATED;
}

size_t lex_skip_brace(const char *s, size_t i)
{
    i++;
    while (s[i]) {
        if (s[i] == '}')
            return i + 1;
        size_t j = skip_nested(s, i, false);
        if (j == UNTERMINATED)
            return UNTERMINATED;
        i = (j == i) ? i + 1 : j;
    }
    return UNTERMINATED;
}

/* ------------------------------------------------------------------ */
/* Token vector                                                        */
/* ------------------------------------------------------------------ */

void tokvec_init(TokVec *tv)
{
    tv->items = NULL;
    tv->len = 0;
    tv->cap = 0;
}

void tokvec_free(TokVec *tv)
{
    for (size_t i = 0; i < tv->len; i++)
        free(tv->items[i].text);
    free(tv->items);
    tokvec_init(tv);
}

static void push(TokVec *tv, TokType type, char *text, RedirType rt, int fd)
{
    if (tv->len == tv->cap) {
        tv->cap = tv->cap ? tv->cap * 2 : 16;
        tv->items = xrealloc(tv->items, tv->cap * sizeof *tv->items);
    }
    Token *t = &tv->items[tv->len++];
    t->type = type;
    t->text = text;
    t->rtype = rt;
    t->fd = fd;
}

const char *redir_symbol(RedirType t)
{
    switch (t) {
    case R_IN:         return "<";
    case R_OUT:        return ">";
    case R_APPEND:     return ">>";
    case R_DUP_OUT:    return ">&";
    case R_DUP_IN:     return "<&";
    case R_OUT_ERR:    return "&>";
    case R_APPEND_ERR: return "&>>";
    case R_HERESTR:    return "<<<";
    }
    return "?";
}

const char *token_describe(const Token *t)
{
    switch (t->type) {
    case T_WORD:    return t->text;
    case T_PIPE:    return "|";
    case T_AND:     return "&&";
    case T_OR:      return "||";
    case T_SEMI:    return ";";
    case T_AMP:     return "&";
    case T_NEWLINE: return "end of line";
    case T_REDIR:   return redir_symbol(t->rtype);
    case T_EOF:     return "end of input";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */

static bool is_meta(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '|' || c == '&' ||
           c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

static LexStatus fail(TokVec *out, char **err, const char *msg)
{
    tokvec_free(out);
    if (err)
        *err = xstrdup(msg);
    return LEX_ERROR;
}

/* Lexes a redirection operator at s[*i] ('<' or '>'). */
static bool lex_redir(const char *s, size_t *i, int fd, TokVec *out,
                      const char **errmsg)
{
    size_t p = *i;
    if (s[p] == '<') {
        if (s[p + 1] == '<' && s[p + 2] == '<') {
            push(out, T_REDIR, NULL, R_HERESTR, fd);
            p += 3;
        } else if (s[p + 1] == '<') {
            *errmsg = "here-documents (<<) are not supported; "
                      "use a here-string (<<<) or a file";
            return false;
        } else if (s[p + 1] == '&') {
            push(out, T_REDIR, NULL, R_DUP_IN, fd);
            p += 2;
        } else if (s[p + 1] == '>') {
            *errmsg = "the <> redirection is not supported";
            return false;
        } else {
            push(out, T_REDIR, NULL, R_IN, fd);
            p += 1;
        }
    } else {
        if (s[p + 1] == '>') {
            push(out, T_REDIR, NULL, R_APPEND, fd);
            p += 2;
        } else if (s[p + 1] == '&') {
            push(out, T_REDIR, NULL, R_DUP_OUT, fd);
            p += 2;
        } else if (s[p + 1] == '|') {
            push(out, T_REDIR, NULL, R_OUT, fd);
            p += 2;
        } else {
            push(out, T_REDIR, NULL, R_OUT, fd);
            p += 1;
        }
    }
    *i = p;
    return true;
}

LexStatus lex(const char *s, TokVec *out, char **err)
{
    size_t i = 0;
    tokvec_init(out);
    if (err)
        *err = NULL;

    for (;;) {
        /* blanks and line continuations between tokens */
        for (;;) {
            if (s[i] == ' ' || s[i] == '\t') {
                i++;
            } else if (s[i] == '\\' && s[i + 1] == '\n') {
                i += 2;
                if (s[i] == '\0') { /* the continued line is still to come */
                    tokvec_free(out);
                    return LEX_INCOMPLETE;
                }
            } else {
                break;
            }
        }
        char c = s[i];
        if (c == '\0') {
            push(out, T_EOF, NULL, R_IN, -1);
            return LEX_OK;
        }
        if (c == '#') {
            while (s[i] && s[i] != '\n')
                i++;
            continue;
        }
        if (c == '\n') {
            push(out, T_NEWLINE, NULL, R_IN, -1);
            i++;
            continue;
        }
        if (c == '|') {
            if (s[i + 1] == '|') {
                push(out, T_OR, NULL, R_IN, -1);
                i += 2;
            } else {
                push(out, T_PIPE, NULL, R_IN, -1);
                i++;
            }
            continue;
        }
        if (c == '&') {
            if (s[i + 1] == '&') {
                push(out, T_AND, NULL, R_IN, -1);
                i += 2;
            } else if (s[i + 1] == '>' && s[i + 2] == '>') {
                push(out, T_REDIR, NULL, R_APPEND_ERR, -1);
                i += 3;
            } else if (s[i + 1] == '>') {
                push(out, T_REDIR, NULL, R_OUT_ERR, -1);
                i += 2;
            } else {
                push(out, T_AMP, NULL, R_IN, -1);
                i++;
            }
            continue;
        }
        if (c == ';') {
            if (s[i + 1] == ';')
                return fail(out, err, "unexpected ';;'");
            push(out, T_SEMI, NULL, R_IN, -1);
            i++;
            continue;
        }
        if (c == '(' || c == ')') {
            return fail(out, err,
                        c == '(' ? "unexpected '(' (subshell groups are not "
                                   "supported; quote it if it is text)"
                                 : "unexpected ')'");
        }
        if (c == '<' || c == '>') {
            const char *msg = NULL;
            if (!lex_redir(s, &i, -1, out, &msg))
                return fail(out, err, msg);
            continue;
        }
        /* a descriptor number glued to a redirection, as in 2> */
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            long fd = 0;
            while (isdigit((unsigned char)s[j]) && j - i < 4) {
                fd = fd * 10 + (s[j] - '0');
                j++;
            }
            if (s[j] == '<' || s[j] == '>') {
                const char *msg = NULL;
                i = j;
                if (!lex_redir(s, &i, (int)fd, out, &msg))
                    return fail(out, err, msg);
                continue;
            }
        }

        /* a word */
        StrBuf w;
        sb_init(&w);
        while (s[i] && !is_meta(s[i])) {
            if (s[i] == '\\' && s[i + 1] == '\n') {
                i += 2; /* line continuation inside a word */
                if (s[i] == '\0') {
                    sb_free(&w);
                    tokvec_free(out);
                    return LEX_INCOMPLETE;
                }
                continue;
            }
            size_t j;
            if (s[i] == '\\' || s[i] == '\'' || s[i] == '"' || s[i] == '`' ||
                s[i] == '$') {
                j = s[i] == '\\'
                        ? (s[i + 1] ? i + 2 : UNTERMINATED)
                        : (s[i] == '\'' ? lex_skip_single(s, i)
                                        : s[i] == '"' ? lex_skip_double(s, i)
                                        : s[i] == '`' ? lex_skip_btick(s, i)
                                        : s[i + 1] == '('
                                              ? lex_skip_paren(s, i + 1)
                                        : s[i + 1] == '{'
                                              ? lex_skip_brace(s, i + 1)
                                              : i + 1);
                if (j == UNTERMINATED) {
                    sb_free(&w);
                    tokvec_free(out);
                    return LEX_INCOMPLETE;
                }
            } else {
                j = i + 1;
            }
            sb_putn(&w, s + i, j - i);
            i = j;
        }
        push(out, T_WORD, sb_take(&w), R_IN, -1);
    }
}
