/* parser.c - alias expansion and a recursive-descent parser. */
#include "parser.h"

#include <stdlib.h>
#include <string.h>

#include "alias.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* Alias expansion (on the token stream)                               */
/* ------------------------------------------------------------------ */

#define MAX_ALIAS_EXPANSIONS 256

/* Replaces token `at` with the tokens of `sub` (minus its T_EOF). */
static void splice(TokVec *tv, size_t at, TokVec *sub)
{
    size_t add = sub->len - 1; /* drop T_EOF */
    size_t newlen = tv->len - 1 + add;
    free(tv->items[at].text);
    if (newlen > tv->cap) {
        tv->cap = newlen * 2;
        tv->items = xrealloc(tv->items, tv->cap * sizeof *tv->items);
    }
    memmove(&tv->items[at + add], &tv->items[at + 1],
            (tv->len - at - 1) * sizeof *tv->items);
    memcpy(&tv->items[at], sub->items, add * sizeof *tv->items);
    tv->len = newlen;
    /* the texts now belong to tv */
    free(sub->items);
    tokvec_init(sub);
}

static bool expand_aliases(TokVec *tv, char **err)
{
    StrVec active; /* aliases already expanded at this command position */
    sv_init(&active);
    bool cmd_pos = true;
    int expansions = 0;
    size_t i = 0;

    while (i < tv->len) {
        Token *t = &tv->items[i];
        if (t->type == T_REDIR) {
            i++;
            if (i < tv->len && tv->items[i].type == T_WORD)
                i++; /* the redirection target is never a command */
            continue;
        }
        if (t->type != T_WORD) {
            cmd_pos = true;
            sv_free(&active);
            i++;
            continue;
        }
        if (!cmd_pos || is_assignment(t->text) || strcmp(t->text, "!") == 0) {
            i++;
            continue;
        }
        const char *value = alias_get(t->text);
        if (!value || sv_contains(&active, t->text)) {
            cmd_pos = false;
            sv_free(&active);
            i++;
            continue;
        }
        if (++expansions > MAX_ALIAS_EXPANSIONS) {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "alias '%s' expands forever (alias loop)",
                      t->text);
            *err = sb_take(&sb);
            sv_free(&active);
            return false;
        }
        TokVec sub;
        char *suberr = NULL;
        if (lex(value, &sub, &suberr) != LEX_OK) {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "alias '%s' has an invalid definition%s%s",
                      t->text, suberr ? ": " : "", suberr ? suberr : "");
            free(suberr);
            *err = sb_take(&sb);
            sv_free(&active);
            return false;
        }
        sv_push_copy(&active, t->text);
        splice(tv, i, &sub);
        /* stay at i: the first substituted word may itself be an alias */
    }
    sv_free(&active);
    return true;
}

/* ------------------------------------------------------------------ */
/* Tree construction                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    TokVec *tv;
    size_t pos;
    char *err;
    bool incomplete;
} Parser;

static Token *peek(Parser *p)
{
    return &p->tv->items[p->pos];
}

static void skip_newlines(Parser *p)
{
    while (peek(p)->type == T_NEWLINE)
        p->pos++;
}

static void syntax_error(Parser *p, const char *what)
{
    if (p->err || p->incomplete)
        return;
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "syntax error near '%s'", what);
    p->err = sb_take(&sb);
}

static void command_free(Command *c)
{
    if (!c)
        return;
    for (size_t i = 0; i < c->nassigns; i++)
        free(c->assigns[i]);
    for (size_t i = 0; i < c->nwords; i++)
        free(c->words[i]);
    for (size_t i = 0; i < c->nredirs; i++)
        free(c->redirs[i].target);
    free(c->assigns);
    free(c->words);
    free(c->redirs);
    free(c);
}

static void pipeline_free(Pipeline *pl)
{
    if (!pl)
        return;
    for (size_t i = 0; i < pl->ncmds; i++)
        command_free(pl->cmds[i]);
    free(pl->cmds);
    free(pl);
}

static void andor_free(AndOr *ao)
{
    if (!ao)
        return;
    for (size_t i = 0; i < ao->npipes; i++)
        pipeline_free(ao->pipes[i]);
    free(ao->pipes);
    free(ao->conns);
    free(ao);
}

void list_free(List *l)
{
    if (!l)
        return;
    for (size_t i = 0; i < l->n; i++)
        andor_free(l->items[i]);
    free(l->items);
    free(l);
}

static void push_str(char ***arr, size_t *n, char *s)
{
    *arr = xrealloc(*arr, (*n + 2) * sizeof **arr);
    (*arr)[(*n)++] = s;
    (*arr)[*n] = NULL;
}

/* Words that begin scripting constructs in sh and bash. Eclipse has no
 * such constructs, and treating these as ordinary command names would
 * be dangerous: `if rm -rf build; then ...` would run the rm and only
 * then fail on "then". Reject the whole line before anything runs. */
static bool is_script_keyword(const char *word)
{
    static const char *const keywords[] = {
        "if",   "then", "else", "elif",  "fi",   "for",      "while",
        "until", "do",  "done", "case",  "esac", "function", "select",
    };
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
        if (strcmp(word, keywords[i]) == 0)
            return true;
    return false;
}

static Command *parse_command(Parser *p)
{
    Command *c = xcalloc(1, sizeof *c);
    for (;;) {
        Token *t = peek(p);
        if (t->type == T_WORD) {
            if (c->nwords == 0 && is_script_keyword(t->text)) {
                if (!p->err) {
                    StrBuf sb;
                    sb_init(&sb);
                    sb_printf(&sb,
                              "'%s' is scripting syntax, which Eclipse does "
                              "not have; nothing on this line was run (use: bash -c "
                              "'...' or a bash script)",
                              t->text);
                    p->err = sb_take(&sb);
                }
                command_free(c);
                return NULL;
            }
            if (c->nwords == 0 && is_assignment(t->text))
                push_str(&c->assigns, &c->nassigns, xstrdup(t->text));
            else
                push_str(&c->words, &c->nwords, xstrdup(t->text));
            p->pos++;
        } else if (t->type == T_REDIR) {
            Token *target = &p->tv->items[p->pos + 1];
            if (target->type != T_WORD) {
                if (!p->err) {
                    StrBuf sb;
                    sb_init(&sb);
                    sb_printf(&sb, "expected a %s after '%s'",
                              t->rtype == R_HERESTR ? "word" : "file name",
                              redir_symbol(t->rtype));
                    p->err = sb_take(&sb);
                }
                command_free(c);
                return NULL;
            }
            c->redirs =
                xrealloc(c->redirs, (c->nredirs + 1) * sizeof *c->redirs);
            Redir *r = &c->redirs[c->nredirs++];
            r->type = t->rtype;
            r->fd = t->fd;
            if (r->fd < 0)
                r->fd = (t->rtype == R_IN || t->rtype == R_DUP_IN ||
                         t->rtype == R_HERESTR)
                            ? 0
                            : 1;
            r->target = xstrdup(target->text);
            p->pos += 2;
        } else {
            break;
        }
    }
    if (c->nwords == 0 && c->nassigns == 0 && c->nredirs == 0) {
        syntax_error(p, token_describe(peek(p)));
        command_free(c);
        return NULL;
    }
    return c;
}

/* After an operator that needs a right-hand side: skip newlines and
 * report "incomplete" if the input simply ends there. */
static bool expect_more(Parser *p)
{
    skip_newlines(p);
    if (peek(p)->type == T_EOF) {
        p->incomplete = true;
        return false;
    }
    return true;
}

static Pipeline *parse_pipeline(Parser *p)
{
    Pipeline *pl = xcalloc(1, sizeof *pl);
    while (peek(p)->type == T_WORD && strcmp(peek(p)->text, "!") == 0) {
        pl->negate = !pl->negate;
        p->pos++;
    }
    for (;;) {
        Command *c = parse_command(p);
        if (!c) {
            pipeline_free(pl);
            return NULL;
        }
        pl->cmds = xrealloc(pl->cmds, (pl->ncmds + 1) * sizeof *pl->cmds);
        pl->cmds[pl->ncmds++] = c;
        if (peek(p)->type != T_PIPE)
            break;
        p->pos++;
        if (!expect_more(p)) {
            pipeline_free(pl);
            return NULL;
        }
    }
    return pl;
}

static AndOr *parse_andor(Parser *p)
{
    AndOr *ao = xcalloc(1, sizeof *ao);
    for (;;) {
        Pipeline *pl = parse_pipeline(p);
        if (!pl) {
            andor_free(ao);
            return NULL;
        }
        ao->pipes = xrealloc(ao->pipes, (ao->npipes + 1) * sizeof *ao->pipes);
        ao->conns = xrealloc(ao->conns, (ao->npipes + 1) * sizeof *ao->conns);
        ao->pipes[ao->npipes++] = pl;
        TokType tt = peek(p)->type;
        if (tt != T_AND && tt != T_OR)
            break;
        ao->conns[ao->npipes - 1] = (tt == T_AND) ? CONN_AND : CONN_OR;
        p->pos++;
        if (!expect_more(p)) {
            andor_free(ao);
            return NULL;
        }
    }
    return ao;
}

static List *parse_list(Parser *p)
{
    List *l = xcalloc(1, sizeof *l);
    skip_newlines(p);
    while (peek(p)->type != T_EOF) {
        AndOr *ao = parse_andor(p);
        if (!ao) {
            list_free(l);
            return NULL;
        }
        l->items = xrealloc(l->items, (l->n + 1) * sizeof *l->items);
        l->items[l->n++] = ao;
        switch (peek(p)->type) {
        case T_AMP:
            ao->background = true;
            p->pos++;
            break;
        case T_SEMI:
        case T_NEWLINE:
            p->pos++;
            break;
        case T_EOF:
            break;
        default:
            syntax_error(p, token_describe(peek(p)));
            list_free(l);
            return NULL;
        }
        skip_newlines(p);
    }
    return l;
}

ParseStatus parse(const char *src, List **out, char **err)
{
    TokVec tv;
    *out = NULL;
    *err = NULL;

    LexStatus ls = lex(src, &tv, err);
    if (ls == LEX_INCOMPLETE)
        return PARSE_INCOMPLETE;
    if (ls == LEX_ERROR) {
        char *msg = *err;
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "syntax error: %s", msg ? msg : "invalid input");
        free(msg);
        *err = sb_take(&sb);
        return PARSE_ERROR;
    }
    if (!expand_aliases(&tv, err)) {
        tokvec_free(&tv);
        return PARSE_ERROR;
    }

    Parser p = { .tv = &tv, .pos = 0, .err = NULL, .incomplete = false };
    List *l = parse_list(&p);
    tokvec_free(&tv);
    if (!l) {
        if (p.incomplete) {
            free(p.err);
            return PARSE_INCOMPLETE;
        }
        *err = p.err ? p.err : xstrdup("syntax error");
        return PARSE_ERROR;
    }
    free(p.err);
    *out = l;
    return PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

static void command_text(const Command *c, StrBuf *sb)
{
    bool first = true;
    for (size_t i = 0; i < c->nassigns; i++) {
        if (!first)
            sb_putc(sb, ' ');
        sb_puts(sb, c->assigns[i]);
        first = false;
    }
    for (size_t i = 0; i < c->nwords; i++) {
        if (!first)
            sb_putc(sb, ' ');
        sb_puts(sb, c->words[i]);
        first = false;
    }
    for (size_t i = 0; i < c->nredirs; i++) {
        const Redir *r = &c->redirs[i];
        if (!first)
            sb_putc(sb, ' ');
        bool is_input =
            r->type == R_IN || r->type == R_DUP_IN || r->type == R_HERESTR;
        bool both = r->type == R_OUT_ERR || r->type == R_APPEND_ERR;
        if (!both && r->fd != (is_input ? 0 : 1))
            sb_printf(sb, "%d", r->fd);
        sb_puts(sb, redir_symbol(r->type));
        if (r->type != R_DUP_OUT && r->type != R_DUP_IN)
            sb_putc(sb, ' ');
        sb_puts(sb, r->target);
        first = false;
    }
}

static void pipeline_text_into(const Pipeline *p, StrBuf *sb)
{
    if (p->negate)
        sb_puts(sb, "! ");
    for (size_t i = 0; i < p->ncmds; i++) {
        if (i)
            sb_puts(sb, " | ");
        command_text(p->cmds[i], sb);
    }
}

char *pipeline_text(const Pipeline *p)
{
    StrBuf sb;
    sb_init(&sb);
    pipeline_text_into(p, &sb);
    return sb_take(&sb);
}

char *andor_text(const AndOr *ao)
{
    StrBuf sb;
    sb_init(&sb);
    for (size_t i = 0; i < ao->npipes; i++) {
        if (i)
            sb_puts(&sb, ao->conns[i - 1] == CONN_AND ? " && " : " || ");
        pipeline_text_into(ao->pipes[i], &sb);
    }
    return sb_take(&sb);
}
