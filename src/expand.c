/* expand.c - word expansion.
 *
 * A raw word is expanded into a "marked buffer": every output character
 * carries a flag saying where it came from. The flags drive the last two
 * steps, because only unquoted expansion results are split into fields
 * and only unquoted characters act as glob wildcards. */
#include "expand.h"

#include <ctype.h>
#include <glob.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arith.h"
#include "lexer.h"
#include "shell.h"
#include "vars.h"

#define UNTERMINATED ((size_t)-1)
#define MAX_RANGE_ITEMS 100000

enum {
    F_LIT,    /* unquoted text typed by the user: globs, never splits */
    F_QUOTED, /* quoted: taken literally */
    F_EXP,    /* unquoted expansion result: splits and globs */
    F_BREAK,  /* forced field boundary (between the words of "$@") */
    F_QNULL   /* an empty quoted string was here; keeps the field alive */
};

typedef struct {
    StrBuf ch;
    StrBuf fl;
    bool at_empty; /* "$@" just expanded to nothing */
} MBuf;

static bool expand_into(const char *w, MBuf *m, bool assign);

static void mb_init(MBuf *m)
{
    sb_init(&m->ch);
    sb_init(&m->fl);
    m->at_empty = false;
}

static void mb_free(MBuf *m)
{
    sb_free(&m->ch);
    sb_free(&m->fl);
}

static void mb_put(MBuf *m, char c, char flag)
{
    sb_putc(&m->ch, c);
    sb_putc(&m->fl, flag);
}

static void mb_puts(MBuf *m, const char *s, char flag)
{
    for (; *s; s++)
        mb_put(m, *s, flag);
}

static void mb_append(MBuf *dst, const MBuf *src, bool force_quoted)
{
    for (size_t i = 0; i < src->ch.len; i++) {
        char f = src->fl.data[i];
        if (force_quoted && (f == F_LIT || f == F_EXP))
            f = F_QUOTED;
        mb_put(dst, src->ch.data[i], f);
    }
}

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

static char *fmt_long(long v)
{
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%ld", v);
    return sb_take(&sb);
}

/* Length of a parameter name at s: NAME, digits, or one special char. */
static size_t param_len(const char *s)
{
    size_t n = name_len(s);
    if (n)
        return n;
    while (isdigit((unsigned char)s[n]))
        n++;
    if (n)
        return n;
    return (s[0] && strchr("?$!#@*", s[0])) ? 1 : 0;
}

/* Value of a parameter as a new string, or NULL if it is unset. */
static char *param_value(const char *name)
{
    if (isdigit((unsigned char)name[0])) {
        long idx;
        if (!parse_long(name, &idx))
            return NULL;
        if (idx == 0)
            return xstrdup(shell.arg0 ? shell.arg0 : "eclipse");
        if (idx <= shell.pos_argc)
            return xstrdup(shell.pos_args[idx - 1]);
        return NULL;
    }
    if (name[0] && !name[1]) {
        switch (name[0]) {
        case '?':
            return fmt_long(shell.last_status);
        case '$':
            return fmt_long((long)shell.pid);
        case '!':
            return shell.last_bg_pid ? fmt_long((long)shell.last_bg_pid)
                                     : NULL;
        case '#':
            return fmt_long(shell.pos_argc);
        case '@':
        case '*': {
            StrBuf sb;
            sb_init(&sb);
            for (int i = 0; i < shell.pos_argc; i++) {
                if (i)
                    sb_putc(&sb, ' ');
                sb_puts(&sb, shell.pos_args[i]);
            }
            return sb_take(&sb);
        }
        default:
            break;
        }
    }
    const char *v = var_get(name);
    return v ? xstrdup(v) : NULL;
}

/* $@ and $*: each positional argument becomes its own field, except
 * for "$*" which joins them with spaces. */
static void put_args(MBuf *m, bool quoted, bool star)
{
    if (shell.pos_argc == 0) {
        if (quoted && !star)
            m->at_empty = true;
        return;
    }
    for (int k = 0; k < shell.pos_argc; k++) {
        if (k) {
            if (quoted && star)
                mb_put(m, ' ', F_QUOTED);
            else
                mb_put(m, '\0', F_BREAK);
        }
        mb_puts(m, shell.pos_args[k], quoted ? F_QUOTED : F_EXP);
        if (quoted && !star)
            mb_put(m, '\0', F_QNULL);
    }
}

/* ${...}: `inner` is the text between the braces. */
static bool expand_braced(const char *inner, MBuf *m, bool quoted)
{
    char flag = quoted ? F_QUOTED : F_EXP;

    if (inner[0] == '#' && inner[1]) { /* ${#NAME}: length */
        size_t plen = param_len(inner + 1);
        if (plen > 0 && inner[1 + plen] == '\0') {
            char *v = param_value(inner + 1);
            char *n = fmt_long(v ? (long)strlen(v) : 0);
            mb_puts(m, n, flag);
            free(n);
            free(v);
            return true;
        }
    }

    size_t plen = param_len(inner);
    if (plen == 0) {
        msg_error("${%s}: bad substitution", inner);
        return false;
    }
    char *name = xstrndup(inner, plen);
    const char *rest = inner + plen;
    char *val = param_value(name);
    bool ok = true;

    if (!*rest) {
        if (name[0] == '@' || name[0] == '*')
            put_args(m, quoted, name[0] == '*');
        else if (val)
            mb_puts(m, val, flag);
        goto done;
    }

    bool colon = rest[0] == ':';
    char op = rest[colon ? 1 : 0];
    if (!op || !strchr("-+=?", op)) {
        msg_error("${%s}: this form of parameter expansion is not supported",
                  inner);
        msg_hint("supported forms: ${VAR} ${#VAR} ${VAR:-default} "
                 "${VAR:=default} ${VAR:+alternative} ${VAR:?message}");
        ok = false;
        goto done;
    }
    const char *word = rest + (colon ? 2 : 1);
    bool missing = !val || (colon && !*val);

    if ((op == '-' && missing) || (op == '+' && !missing)) {
        MBuf sub;
        mb_init(&sub);
        ok = expand_into(word, &sub, false);
        if (ok)
            mb_append(m, &sub, quoted);
        mb_free(&sub);
    } else if (op == '=' && missing) {
        char *wv = expand_single(word, false);
        if (!wv) {
            ok = false;
        } else if (!is_valid_name(name)) {
            msg_error("${%s}: cannot assign to this parameter", inner);
            ok = false;
        } else {
            var_set(name, wv);
            mb_puts(m, wv, flag);
        }
        free(wv);
    } else if (op == '?' && missing) {
        char *wv = *word ? expand_single(word, false) : NULL;
        msg_error("%s: %s", name, wv ? wv : "parameter is not set");
        free(wv);
        ok = false;
    } else if (op != '+' && val) {
        mb_puts(m, val, flag);
    }

done:
    free(name);
    free(val);
    return ok;
}

/* $((...)) once the inner "(...)" has been isolated. */
static bool expand_arith(const char *expr, MBuf *m, char flag)
{
    char *expanded = expand_single(expr, false);
    if (!expanded)
        return false;
    long long result;
    char *err = NULL;
    bool ok = arith_eval(expanded, &result, &err);
    if (ok) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", result);
        mb_puts(m, buf, flag);
    } else {
        msg_error("arithmetic: %s: '%s'", err, expanded);
        free(err);
    }
    free(expanded);
    return ok;
}

/* Handles everything that starts with '$'. *ip points at the '$'. */
static bool expand_dollar(const char *w, size_t *ip, MBuf *m, bool quoted)
{
    size_t i = *ip;
    char flag = quoted ? F_QUOTED : F_EXP;
    char n = w[i + 1];

    if (n == '(') {
        size_t end = lex_skip_paren(w, i + 1);
        if (end != UNTERMINATED) {
            char *inner = xstrndup(w + i + 2, end - 1 - (i + 2));
            size_t ilen = strlen(inner);
            bool ok = true;
            if (inner[0] == '(' && lex_skip_paren(inner, 0) == ilen) {
                inner[ilen - 1] = '\0';
                ok = expand_arith(inner + 1, m, flag);
            } else {
                char *out = shell_capture(inner);
                mb_puts(m, out, flag);
                free(out);
            }
            free(inner);
            *ip = end;
            return ok;
        }
    } else if (n == '{') {
        size_t end = lex_skip_brace(w, i + 1);
        if (end != UNTERMINATED) {
            char *inner = xstrndup(w + i + 2, end - 1 - (i + 2));
            bool ok = expand_braced(inner, m, quoted);
            free(inner);
            *ip = end;
            return ok;
        }
    } else if (n == '@' || n == '*') {
        put_args(m, quoted, n == '*');
        *ip = i + 2;
        return true;
    } else if (n && (isdigit((unsigned char)n) || strchr("?$!#", n))) {
        char name[2] = { n, '\0' };
        char *v = param_value(name);
        if (v)
            mb_puts(m, v, flag);
        free(v);
        *ip = i + 2;
        return true;
    } else {
        size_t len = name_len(w + i + 1);
        if (len > 0) {
            char *name = xstrndup(w + i + 1, len);
            const char *v = var_get(name);
            if (v)
                mb_puts(m, v, flag);
            free(name);
            *ip = i + 1 + len;
            return true;
        }
    }
    /* a lone '$' is just a dollar sign */
    mb_put(m, '$', quoted ? F_QUOTED : F_LIT);
    *ip = i + 1;
    return true;
}

/* `command`: *ip points at the opening backtick. */
static bool expand_btick(const char *w, size_t *ip, MBuf *m, bool quoted)
{
    size_t i = *ip;
    size_t end = lex_skip_btick(w, i);
    if (end == UNTERMINATED) {
        mb_put(m, '`', quoted ? F_QUOTED : F_LIT);
        *ip = i + 1;
        return true;
    }
    StrBuf cmd;
    sb_init(&cmd);
    for (size_t k = i + 1; k < end - 1; k++) {
        if (w[k] == '\\' && (w[k + 1] == '`' || w[k + 1] == '\\' ||
                             w[k + 1] == '$'))
            k++;
        sb_putc(&cmd, w[k]);
    }
    char *out = shell_capture(sb_cstr(&cmd));
    mb_puts(m, out, quoted ? F_QUOTED : F_EXP);
    free(out);
    sb_free(&cmd);
    *ip = end;
    return true;
}

/* Tilde prefix at w[i]. Returns the index after the prefix, or i if it
 * is not a tilde expansion (the '~' then stays literal). */
static size_t try_tilde(const char *w, size_t i, MBuf *m, bool assign)
{
    size_t j = i + 1;
    while (w[j] && w[j] != '/' && !(assign && w[j] == ':')) {
        if (strchr("\\'\"$`", w[j]))
            return i;
        j++;
    }
    char *user = xstrndup(w + i + 1, j - i - 1);
    const char *dir = NULL;
    if (!*user) {
        dir = var_get("HOME");
        if (!dir || !*dir)
            dir = home_dir();
    } else if (strcmp(user, "+") == 0) {
        dir = var_get("PWD");
    } else if (strcmp(user, "-") == 0) {
        dir = var_get("OLDPWD");
    } else {
        struct passwd *pw = getpwnam(user);
        if (pw)
            dir = pw->pw_dir;
    }
    free(user);
    if (!dir)
        return i;
    mb_puts(m, dir, F_QUOTED);
    return j;
}

static bool expand_into(const char *w, MBuf *m, bool assign)
{
    size_t i = 0;
    bool seen_eq = false;

    if (w[0] == '~')
        i = try_tilde(w, 0, m, assign);
    if (i == 0 && w[0] == '~') {
        mb_put(m, '~', F_LIT);
        i = 1;
    }

    while (w[i]) {
        char c = w[i];
        if (c == '\\') {
            if (w[i + 1]) {
                mb_put(m, w[i + 1], F_QUOTED);
                i += 2;
            } else {
                mb_put(m, '\\', F_LIT);
                i++;
            }
        } else if (c == '\'') {
            size_t end = lex_skip_single(w, i);
            size_t stop = (end == UNTERMINATED) ? strlen(w) : end - 1;
            for (size_t k = i + 1; k < stop; k++)
                mb_put(m, w[k], F_QUOTED);
            mb_put(m, '\0', F_QNULL);
            i = (end == UNTERMINATED) ? stop : end;
        } else if (c == '"') {
            i++;
            size_t mark = m->ch.len;
            m->at_empty = false;
            while (w[i] && w[i] != '"') {
                if (w[i] == '\\') {
                    char n = w[i + 1];
                    if (n == '\n') {
                        i += 2;
                    } else if (n && strchr("$`\"\\", n)) {
                        mb_put(m, n, F_QUOTED);
                        i += 2;
                    } else {
                        mb_put(m, '\\', F_QUOTED);
                        i++;
                    }
                } else if (w[i] == '$') {
                    if (!expand_dollar(w, &i, m, true))
                        return false;
                } else if (w[i] == '`') {
                    if (!expand_btick(w, &i, m, true))
                        return false;
                } else {
                    mb_put(m, w[i], F_QUOTED);
                    i++;
                }
            }
            if (w[i] == '"')
                i++;
            if (!(m->at_empty && m->ch.len == mark))
                mb_put(m, '\0', F_QNULL);
            m->at_empty = false;
        } else if (c == '$') {
            if (!expand_dollar(w, &i, m, false))
                return false;
        } else if (c == '`') {
            if (!expand_btick(w, &i, m, false))
                return false;
        } else {
            mb_put(m, c, F_LIT);
            i++;
            if (assign && w[i] == '~' &&
                ((c == '=' && !seen_eq) || (c == ':' && seen_eq))) {
                size_t j = try_tilde(w, i, m, true);
                if (j == i) {
                    mb_put(m, '~', F_LIT);
                    j = i + 1;
                }
                i = j;
            }
            if (c == '=')
                seen_eq = true;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Brace expansion                                                     */
/* ------------------------------------------------------------------ */

/* Index just past the quoted or nested region starting at w[i], or
 * i + 1 for an ordinary character. */
static size_t skip_region(const char *w, size_t i)
{
    size_t j;
    switch (w[i]) {
    case '\\':
        return w[i + 1] ? i + 2 : i + 1;
    case '\'':
        j = lex_skip_single(w, i);
        break;
    case '"':
        j = lex_skip_double(w, i);
        break;
    case '`':
        j = lex_skip_btick(w, i);
        break;
    case '$':
        if (w[i + 1] == '(')
            j = lex_skip_paren(w, i + 1);
        else if (w[i + 1] == '{')
            j = lex_skip_brace(w, i + 1);
        else
            return i + 1;
        break;
    default:
        return i + 1;
    }
    return j == UNTERMINATED ? strlen(w) : j;
}

/* Recognises "1..5" or "a..e" in s[0..n) and appends the items. */
static bool brace_range(const char *s, size_t n, StrVec *items)
{
    char *body = xstrndup(s, n);
    char *dots = strstr(body, "..");
    bool ok = false;
    if (dots && dots != body && dots[2]) {
        *dots = '\0';
        const char *a = body, *b = dots + 2;
        long x, y;
        if (parse_long(a, &x) && parse_long(b, &y)) {
            long count = (x > y ? x - y : y - x) + 1;
            if (count > 0 && count <= MAX_RANGE_ITEMS) {
                long step = x <= y ? 1 : -1;
                for (long v = x;; v += step) {
                    sv_push(items, fmt_long(v));
                    if (v == y)
                        break;
                }
                ok = true;
            }
        } else if (isalpha((unsigned char)a[0]) && !a[1] &&
                   isalpha((unsigned char)b[0]) && !b[1]) {
            int step = a[0] <= b[0] ? 1 : -1;
            for (int ch = a[0];; ch += step) {
                char item[2] = { (char)ch, '\0' };
                sv_push_copy(items, item);
                if (ch == b[0])
                    break;
            }
            ok = true;
        }
    }
    free(body);
    return ok;
}

static void brace_emit(const char *w, size_t open, size_t close,
                       const char *alt, size_t altlen, StrVec *out)
{
    StrBuf sb;
    sb_init(&sb);
    sb_putn(&sb, w, open);
    sb_putn(&sb, alt, altlen);
    sb_puts(&sb, w + close + 1);
    expand_braces(sb_cstr(&sb), out);
    sb_free(&sb);
}

void expand_braces(const char *w, StrVec *out)
{
    for (size_t i = 0; w[i];) {
        if (w[i] != '{') {
            i = skip_region(w, i);
            continue;
        }
        size_t j = i + 1, ncommas = 0, capcommas = 0;
        size_t *commas = NULL;
        int depth = 1;
        while (w[j]) {
            if (w[j] == '{') {
                depth++;
                j++;
            } else if (w[j] == '}') {
                if (--depth == 0)
                    break;
                j++;
            } else if (w[j] == ',' && depth == 1) {
                if (ncommas == capcommas) {
                    capcommas = capcommas ? capcommas * 2 : 8;
                    commas = xrealloc(commas, capcommas * sizeof *commas);
                }
                commas[ncommas++] = j++;
            } else {
                j = skip_region(w, j);
            }
        }
        if (w[j] == '}' && depth == 0) {
            if (ncommas > 0) {
                size_t start = i + 1;
                for (size_t k = 0; k <= ncommas; k++) {
                    size_t end = k < ncommas ? commas[k] : j;
                    brace_emit(w, i, j, w + start, end - start, out);
                    start = end + 1;
                }
                free(commas);
                return;
            }
            StrVec items;
            sv_init(&items);
            if (brace_range(w + i + 1, j - i - 1, &items)) {
                for (size_t k = 0; k < items.len; k++)
                    brace_emit(w, i, j, items.items[k],
                               strlen(items.items[k]), out);
                sv_free(&items);
                free(commas);
                return;
            }
            sv_free(&items);
        }
        free(commas);
        i++;
    }
    sv_push_copy(out, w);
}

/* ------------------------------------------------------------------ */
/* Field splitting and globbing                                        */
/* ------------------------------------------------------------------ */

static void glob_field(const char *ch, const char *fl, size_t n, StrVec *out)
{
    bool has_glob = false;
    for (size_t i = 0; i < n; i++) {
        if (fl[i] != F_QUOTED && (ch[i] == '*' || ch[i] == '?' ||
                                  ch[i] == '[')) {
            has_glob = true;
            break;
        }
    }
    if (!has_glob) {
        sv_push(out, xstrndup(ch, n));
        return;
    }
    StrBuf pat;
    sb_init(&pat);
    for (size_t i = 0; i < n; i++) {
        if (ch[i] == '\\' ||
            (fl[i] == F_QUOTED && strchr("*?[]", ch[i])))
            sb_putc(&pat, '\\');
        sb_putc(&pat, ch[i]);
    }
    glob_t g;
    if (glob(sb_cstr(&pat), 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            sv_push_copy(out, g.gl_pathv[i]);
        globfree(&g);
    } else {
        /* no match: keep the word as typed, like bash */
        sv_push(out, xstrndup(ch, n));
    }
    sb_free(&pat);
}

static void split_fields(const MBuf *m, StrVec *out)
{
    StrBuf ch, fl;
    sb_init(&ch);
    sb_init(&fl);
    bool have = false;

    for (size_t i = 0; i <= m->ch.len; i++) {
        bool at_end = i == m->ch.len;
        char c = at_end ? '\0' : m->ch.data[i];
        char f = at_end ? (char)F_BREAK : m->fl.data[i];
        bool sep = f == F_BREAK ||
                   (f == F_EXP && (c == ' ' || c == '\t' || c == '\n'));
        if (sep) {
            if (have)
                glob_field(sb_cstr(&ch), sb_cstr(&fl), ch.len, out);
            sb_clear(&ch);
            sb_clear(&fl);
            have = false;
        } else if (f == F_QNULL) {
            have = true;
        } else {
            sb_putc(&ch, c);
            sb_putc(&fl, f);
            have = true;
        }
    }
    sb_free(&ch);
    sb_free(&fl);
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

bool expand_words(char *const *raw, size_t n, StrVec *out)
{
    for (size_t i = 0; i < n; i++) {
        StrVec alts;
        sv_init(&alts);
        expand_braces(raw[i], &alts);
        for (size_t k = 0; k < alts.len; k++) {
            MBuf m;
            mb_init(&m);
            bool ok = expand_into(alts.items[k], &m,
                                  is_assignment(alts.items[k]));
            if (ok)
                split_fields(&m, out);
            mb_free(&m);
            if (!ok) {
                sv_free(&alts);
                return false;
            }
        }
        sv_free(&alts);
    }
    return true;
}

char *expand_single(const char *raw, bool assign)
{
    MBuf m;
    mb_init(&m);
    if (!expand_into(raw, &m, assign)) {
        mb_free(&m);
        return NULL;
    }
    StrBuf out;
    sb_init(&out);
    for (size_t i = 0; i < m.ch.len; i++) {
        char f = m.fl.data[i];
        if (f == F_BREAK)
            sb_putc(&out, ' ');
        else if (f != F_QNULL)
            sb_putc(&out, m.ch.data[i]);
    }
    mb_free(&m);
    return sb_take(&out);
}
