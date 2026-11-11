/* arith.c - integer arithmetic for $((...)), by precedence climbing. */
#include "arith.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "vars.h"

#define MAX_DEPTH 64

typedef struct {
    const char *s;
    size_t i;
    char *err;
    int depth;
} Arith;

static long long parse_binary(Arith *a, int min_prec);

static void fail(Arith *a, const char *msg)
{
    if (!a->err)
        a->err = xstrdup(msg);
}

static void skip_ws(Arith *a)
{
    while (isspace((unsigned char)a->s[a->i]))
        a->i++;
}

static long long variable_value(const char *name)
{
    const char *v = var_get(name);
    if (!v)
        return 0;
    char *end;
    errno = 0;
    long long n = strtoll(v, &end, 0);
    while (isspace((unsigned char)*end))
        end++;
    return (errno == 0 && end != v && *end == '\0') ? n : 0;
}

static long long parse_primary(Arith *a)
{
    skip_ws(a);
    char c = a->s[a->i];
    if (c == '(') {
        a->i++;
        if (++a->depth > MAX_DEPTH) {
            fail(a, "expression is nested too deeply");
            return 0;
        }
        long long v = parse_binary(a, 1);
        a->depth--;
        skip_ws(a);
        if (a->s[a->i] != ')') {
            fail(a, "missing ')'");
            return 0;
        }
        a->i++;
        return v;
    }
    if (isdigit((unsigned char)c)) {
        char *end;
        errno = 0;
        long long v = strtoll(a->s + a->i, &end, 0);
        if (errno != 0) {
            fail(a, "number is too large");
            return 0;
        }
        a->i = (size_t)(end - a->s);
        if (isalnum((unsigned char)a->s[a->i]) || a->s[a->i] == '_') {
            fail(a, "malformed number");
            return 0;
        }
        return v;
    }
    size_t n = name_len(a->s + a->i);
    if (n > 0) {
        char *name = xstrndup(a->s + a->i, n);
        long long v = variable_value(name);
        free(name);
        a->i += n;
        return v;
    }
    fail(a, c ? "unexpected character in expression"
              : "expression ends unexpectedly");
    return 0;
}

static long long parse_unary(Arith *a)
{
    skip_ws(a);
    char c = a->s[a->i];
    if (c == '+' || c == '-' || c == '!' || c == '~') {
        a->i++;
        if (++a->depth > MAX_DEPTH) {
            fail(a, "expression is nested too deeply");
            return 0;
        }
        long long v = parse_unary(a);
        a->depth--;
        switch (c) {
        case '-':
            return (long long)(0ULL - (unsigned long long)v);
        case '!':
            return !v;
        case '~':
            return ~v;
        default:
            return v;
        }
    }
    return parse_primary(a);
}

typedef enum {
    OP_NONE, OP_LOR, OP_LAND, OP_BOR, OP_BXOR, OP_BAND, OP_EQ, OP_NE,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_SHL, OP_SHR, OP_ADD, OP_SUB,
    OP_MUL, OP_DIV, OP_MOD
} Op;

/* Recognises a binary operator at the cursor without consuming it. */
static Op peek_op(Arith *a, int *prec, size_t *len)
{
    skip_ws(a);
    const char *p = a->s + a->i;
    static const struct {
        const char *text;
        Op op;
        int prec;
    } table[] = {
        { "||", OP_LOR, 1 },  { "&&", OP_LAND, 2 }, { "==", OP_EQ, 6 },
        { "!=", OP_NE, 6 },   { "<=", OP_LE, 7 },   { ">=", OP_GE, 7 },
        { "<<", OP_SHL, 8 },  { ">>", OP_SHR, 8 },  { "|", OP_BOR, 3 },
        { "^", OP_BXOR, 4 },  { "&", OP_BAND, 5 },  { "<", OP_LT, 7 },
        { ">", OP_GT, 7 },    { "+", OP_ADD, 9 },   { "-", OP_SUB, 9 },
        { "*", OP_MUL, 10 },  { "/", OP_DIV, 10 },  { "%", OP_MOD, 10 },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        size_t n = strlen(table[i].text);
        if (strncmp(p, table[i].text, n) == 0) {
            *prec = table[i].prec;
            *len = n;
            return table[i].op;
        }
    }
    return OP_NONE;
}

static long long apply(Arith *a, Op op, long long l, long long r)
{
    unsigned long long ul = (unsigned long long)l, ur = (unsigned long long)r;
    switch (op) {
    case OP_LOR:  return l || r;
    case OP_LAND: return l && r;
    case OP_BOR:  return l | r;
    case OP_BXOR: return l ^ r;
    case OP_BAND: return l & r;
    case OP_EQ:   return l == r;
    case OP_NE:   return l != r;
    case OP_LT:   return l < r;
    case OP_LE:   return l <= r;
    case OP_GT:   return l > r;
    case OP_GE:   return l >= r;
    case OP_SHL:  return (long long)(ul << (ur & 63));
    case OP_SHR:  return l >> (ur & 63);
    case OP_ADD:  return (long long)(ul + ur);
    case OP_SUB:  return (long long)(ul - ur);
    case OP_MUL:  return (long long)(ul * ur);
    case OP_DIV:
    case OP_MOD:
        if (r == 0) {
            fail(a, "division by zero");
            return 0;
        }
        if (r == -1) /* avoids overflow for LLONG_MIN / -1 */
            return op == OP_DIV ? (long long)(0ULL - ul) : 0;
        return op == OP_DIV ? l / r : l % r;
    case OP_NONE:
        break;
    }
    return 0;
}

static long long parse_binary(Arith *a, int min_prec)
{
    long long left = parse_unary(a);
    while (!a->err) {
        int prec;
        size_t len;
        Op op = peek_op(a, &prec, &len);
        if (op == OP_NONE || prec < min_prec)
            break;
        a->i += len;
        long long right = parse_binary(a, prec + 1);
        if (a->err)
            break;
        left = apply(a, op, left, right);
    }
    return left;
}

bool arith_eval(const char *expr, long long *result, char **err)
{
    Arith a = { .s = expr, .i = 0, .err = NULL, .depth = 0 };
    skip_ws(&a);
    if (a.s[a.i] == '\0') { /* $(( )) is 0, as in other shells */
        *result = 0;
        return true;
    }
    long long v = parse_binary(&a, 1);
    skip_ws(&a);
    if (!a.err && a.s[a.i] != '\0')
        fail(&a, "unexpected text after expression");
    if (a.err) {
        *err = a.err;
        return false;
    }
    *result = v;
    return true;
}
