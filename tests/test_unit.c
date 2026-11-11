/* test_unit.c - unit tests for Eclipse's internal modules.
 *
 * Built by `make test-unit` and linked against every object file
 * except main.o. Each test function exercises one module directly. */
#include <fcntl.h>
#include <ftw.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alias.h"
#include "arith.h"
#include "complete.h"
#include "expand.h"
#include "history.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "prompt.h"
#include "settings.h"
#include "shell.h"
#include "suggest.h"
#include "util.h"
#include "vars.h"

static int checks, failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #cond);                                               \
        }                                                                 \
    } while (0)

#define CHECK_STR(got, want)                                              \
    do {                                                                  \
        const char *g_ = (got), *w_ = (want);                             \
        checks++;                                                         \
        if (!g_ || strcmp(g_, w_) != 0) {                                 \
            failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n",      \
                    __FILE__, __LINE__, g_ ? g_ : "(null)", w_);          \
        }                                                                 \
    } while (0)

/* ---- scratch directory -------------------------------------------- */

static char scratch[4096];

static int rm_visit(const char *path, const struct stat *st, int type,
                    struct FTW *info)
{
    (void)st;
    (void)type;
    (void)info;
    return remove(path);
}

static void scratch_remove(void)
{
    if (scratch[0])
        nftw(scratch, rm_visit, 16, FTW_DEPTH | FTW_PHYS);
}

static void scratch_create(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(scratch, sizeof scratch, "%s/eclipse-unit-XXXXXX",
             tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(scratch)) {
        perror("mkdtemp");
        exit(2);
    }
    atexit(scratch_remove);
}

static void touch(const char *name, const char *content)
{
    char *path = path_join(scratch, name);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        exit(2);
    }
    fputs(content, fp);
    fclose(fp);
    free(path);
}

static void make_dir(const char *name)
{
    char *path = path_join(scratch, name);
    if (mkdir_p(path, 0755) != 0) {
        perror(path);
        exit(2);
    }
    free(path);
}

/* ---- helpers ------------------------------------------------------ */

/* Some checks provoke error messages on purpose; hide those so that a
 * passing run prints nothing alarming. */
static int saved_stderr = -1;

static void quiet_begin(void)
{
    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
        dup2(null, STDERR_FILENO);
        close(null);
    }
}

static void quiet_end(void)
{
    fflush(stderr);
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
        saved_stderr = -1;
    }
}

/* Expands one raw word and joins the resulting fields with '|'. */
static char *expand_joined(const char *raw)
{
    char *words[] = { (char *)raw };
    StrVec out;
    sv_init(&out);
    if (!expand_words(words, 1, &out)) {
        sv_free(&out);
        return xstrdup("<error>");
    }
    StrBuf sb;
    sb_init(&sb);
    for (size_t i = 0; i < out.len; i++) {
        if (i)
            sb_putc(&sb, '|');
        sb_puts(&sb, out.items[i]);
    }
    sv_free(&out);
    return sb_take(&sb);
}

#define CHECK_EXPAND(raw, want)                                           \
    do {                                                                  \
        char *e_ = expand_joined(raw);                                    \
        CHECK_STR(e_, want);                                              \
        free(e_);                                                         \
    } while (0)

static ParseStatus parse_status(const char *src)
{
    List *l = NULL;
    char *err = NULL;
    ParseStatus ps = parse(src, &l, &err);
    list_free(l);
    free(err);
    return ps;
}

/* ---- util --------------------------------------------------------- */

static void test_util(void)
{
    StrBuf sb;
    sb_init(&sb);
    CHECK_STR(sb_cstr(&sb), "");
    sb_puts(&sb, "abc");
    sb_putc(&sb, 'd');
    sb_printf(&sb, "%d-%s", 42, "x");
    CHECK_STR(sb.data, "abcd42-x");
    CHECK(sb.len == 8);
    for (int i = 0; i < 1000; i++)
        sb_putc(&sb, 'z');
    CHECK(sb.len == 1008 && sb.data[1008] == '\0');
    char *taken = sb_take(&sb);
    CHECK(strlen(taken) == 1008 && sb.data == NULL);
    free(taken);

    StrVec sv;
    sv_init(&sv);
    sv_push_copy(&sv, "pear");
    sv_push_copy(&sv, "apple");
    sv_push_copy(&sv, "pear");
    sv_sort_unique(&sv);
    CHECK(sv.len == 2);
    CHECK_STR(sv.items[0], "apple");
    CHECK_STR(sv.items[1], "pear");
    CHECK(sv.items[2] == NULL);
    CHECK(sv_contains(&sv, "pear") && !sv_contains(&sv, "plum"));
    sv_free(&sv);

    CHECK(name_len("abc_1=x") == 5);
    CHECK(name_len("1abc") == 0);
    CHECK(is_valid_name("_x9") && !is_valid_name("a-b") && !is_valid_name(""));
    CHECK(is_assignment("A=1") && is_assignment("A=") && !is_assignment("=1"));
    CHECK(!is_assignment("a-b=1") && !is_assignment("plain"));

    long n;
    CHECK(parse_long("-17", &n) && n == -17);
    CHECK(!parse_long("12x", &n) && !parse_long("", &n));

    char *j = path_join("/a/", "/b");
    CHECK_STR(j, "/a/b");
    free(j);
    j = path_join("a", "b");
    CHECK_STR(j, "a/b");
    free(j);

    setenv("HOME", "/home/tester", 1);
    char *t = tilde_abbrev("/home/tester/docs");
    CHECK_STR(t, "~/docs");
    free(t);
    t = tilde_abbrev("/home/tester");
    CHECK_STR(t, "~");
    free(t);
    t = tilde_abbrev("/home/tester2/x");
    CHECK_STR(t, "/home/tester2/x");
    free(t);

    uint32_t cp;
    CHECK(utf8_decode("A", 1, &cp) == 1 && cp == 'A');
    CHECK(utf8_decode("\xC3\xA9", 2, &cp) == 2 && cp == 0xE9);
    CHECK(utf8_decode("\xE2\x9D\xAF", 3, &cp) == 3 && cp == 0x276F);
    CHECK(utf8_decode("\xF0\x9F\x98\x80", 4, &cp) == 4 && cp == 0x1F600);
    CHECK(utf8_decode("\xFF", 1, &cp) == 1 && cp == 0xFFFD);
    CHECK(utf8_decode("\xE2\x9D", 2, &cp) == 1 && cp == 0xFFFD); /* cut */
    CHECK(utf8_decode("\xC0\x80", 2, &cp) == 1 && cp == 0xFFFD); /* overlong */

    CHECK(display_width("hello") == 5);
    CHECK(display_width("\033[1;31mred\033[0m") == 3);
    CHECK(display_width("") == 0);

    sb_put_sanitized(&sb, "a\033[2Jb\nc");
    CHECK_STR(sb.data, "a?[2Jb?c");
    sb_free(&sb);

    make_dir("mk/a/b/c");
    char *deep = path_join(scratch, "mk/a/b/c");
    CHECK(is_directory(deep));
    free(deep);

    char *sh = find_in_path("sh");
    CHECK(sh != NULL && sh[0] == '/');
    free(sh);
    CHECK(find_in_path("no-such-program-eclipse-test") == NULL);
}

/* ---- lexer -------------------------------------------------------- */

static void test_lexer(void)
{
    TokVec tv;
    char *err = NULL;

    CHECK(lex("echo \"a b\" | grep x >out 2>&1 &", &tv, &err) == LEX_OK);
    CHECK(tv.len == 11);
    CHECK(tv.items[0].type == T_WORD);
    CHECK_STR(tv.items[0].text, "echo");
    CHECK_STR(tv.items[1].text, "\"a b\"");
    CHECK(tv.items[2].type == T_PIPE);
    CHECK(tv.items[5].type == T_REDIR && tv.items[5].rtype == R_OUT &&
          tv.items[5].fd == -1);
    CHECK_STR(tv.items[6].text, "out");
    CHECK(tv.items[7].type == T_REDIR && tv.items[7].rtype == R_DUP_OUT &&
          tv.items[7].fd == 2);
    CHECK_STR(tv.items[8].text, "1");
    CHECK(tv.items[9].type == T_AMP);
    CHECK(tv.items[10].type == T_EOF);
    tokvec_free(&tv);

    CHECK(lex("a && b || c ; d", &tv, &err) == LEX_OK);
    CHECK(tv.items[1].type == T_AND && tv.items[3].type == T_OR &&
          tv.items[5].type == T_SEMI);
    tokvec_free(&tv);

    CHECK(lex("a >>f <g &>h &>>i <<<w 3>j", &tv, &err) == LEX_OK);
    CHECK(tv.items[1].rtype == R_APPEND);
    CHECK(tv.items[3].rtype == R_IN);
    CHECK(tv.items[5].rtype == R_OUT_ERR);
    CHECK(tv.items[7].rtype == R_APPEND_ERR);
    CHECK(tv.items[9].rtype == R_HERESTR);
    CHECK(tv.items[11].rtype == R_OUT && tv.items[11].fd == 3);
    tokvec_free(&tv);

    /* a digit that is not glued to a redirection is a normal word */
    CHECK(lex("echo 2 >f", &tv, &err) == LEX_OK);
    CHECK(tv.items[1].type == T_WORD && tv.items[2].fd == -1);
    tokvec_free(&tv);

    CHECK(lex("echo hi # a comment | not a pipe", &tv, &err) == LEX_OK);
    CHECK(tv.len == 3);
    tokvec_free(&tv);
    CHECK(lex("echo a#b", &tv, &err) == LEX_OK);
    CHECK_STR(tv.items[1].text, "a#b");
    tokvec_free(&tv);

    /* nested constructs stay inside one word */
    CHECK(lex("echo $(echo \"a ) b\" | cat) 'x y'\\ z", &tv, &err) == LEX_OK);
    CHECK(tv.len == 4);
    CHECK_STR(tv.items[1].text, "$(echo \"a ) b\" | cat)");
    CHECK_STR(tv.items[2].text, "'x y'\\ z");
    tokvec_free(&tv);

    CHECK(lex("echo ${A:-b c}d", &tv, &err) == LEX_OK);
    CHECK(tv.len == 3);
    tokvec_free(&tv);

    CHECK(lex("line one \\\ncontinued", &tv, &err) == LEX_OK);
    CHECK(tv.len == 4);
    CHECK_STR(tv.items[2].text, "continued");
    tokvec_free(&tv);

    CHECK(lex("echo 'open", &tv, &err) == LEX_INCOMPLETE);
    CHECK(lex("echo \"open", &tv, &err) == LEX_INCOMPLETE);
    CHECK(lex("echo $(open", &tv, &err) == LEX_INCOMPLETE);
    CHECK(lex("echo `open", &tv, &err) == LEX_INCOMPLETE);
    CHECK(lex("echo trailing\\", &tv, &err) == LEX_INCOMPLETE);
    CHECK(lex("echo trailing \\\n", &tv, &err) == LEX_INCOMPLETE);
    CHECK(lex("echo trail\\\n", &tv, &err) == LEX_INCOMPLETE);

    CHECK(lex("a ;; b", &tv, &err) == LEX_ERROR && err != NULL);
    free(err);
    CHECK(lex("(a)", &tv, &err) == LEX_ERROR);
    free(err);
    CHECK(lex("cat <<EOF", &tv, &err) == LEX_ERROR);
    CHECK(err && strstr(err, "here-doc"));
    free(err);
}

/* ---- parser ------------------------------------------------------- */

static void test_parser(void)
{
    List *l;
    char *err;

    CHECK(parse("A=1 B=2 cmd arg >out | two && three || four &\nfive", &l,
                &err) == PARSE_OK);
    CHECK(l->n == 2);
    AndOr *ao = l->items[0];
    CHECK(ao->background && ao->npipes == 3);
    CHECK(ao->conns[0] == CONN_AND && ao->conns[1] == CONN_OR);
    Pipeline *p = ao->pipes[0];
    CHECK(p->ncmds == 2 && !p->negate);
    Command *c = p->cmds[0];
    CHECK(c->nassigns == 2 && c->nwords == 2 && c->nredirs == 1);
    CHECK_STR(c->assigns[1], "B=2");
    CHECK_STR(c->words[0], "cmd");
    CHECK(c->redirs[0].fd == 1 && c->redirs[0].type == R_OUT);
    CHECK_STR(c->redirs[0].target, "out");
    CHECK(!l->items[1]->background);
    char *text = andor_text(ao);
    CHECK_STR(text, "A=1 B=2 cmd arg > out | two && three || four");
    free(text);
    list_free(l);

    CHECK(parse("! grep -q x file", &l, &err) == PARSE_OK);
    CHECK(l->items[0]->pipes[0]->negate);
    list_free(l);

    /* NAME=value after the command name is an ordinary argument */
    CHECK(parse("env A=1", &l, &err) == PARSE_OK);
    CHECK(l->items[0]->pipes[0]->cmds[0]->nassigns == 0);
    CHECK(l->items[0]->pipes[0]->cmds[0]->nwords == 2);
    list_free(l);

    CHECK(parse("cmd <in 2>>log", &l, &err) == PARSE_OK);
    c = l->items[0]->pipes[0]->cmds[0];
    CHECK(c->redirs[0].fd == 0 && c->redirs[1].fd == 2);
    text = andor_text(l->items[0]);
    CHECK_STR(text, "cmd < in 2>> log");
    free(text);
    list_free(l);

    CHECK(parse("", &l, &err) == PARSE_OK && l->n == 0);
    list_free(l);
    CHECK(parse("  # only a comment\n\n", &l, &err) == PARSE_OK && l->n == 0);
    list_free(l);
    CHECK(parse("a; b;", &l, &err) == PARSE_OK && l->n == 2);
    list_free(l);

    CHECK(parse_status("ls |") == PARSE_INCOMPLETE);
    CHECK(parse_status("ls &&") == PARSE_INCOMPLETE);
    CHECK(parse_status("ls ||\n") == PARSE_INCOMPLETE);
    CHECK(parse_status("echo 'abc") == PARSE_INCOMPLETE);
    CHECK(parse_status("ls |\n wc") == PARSE_OK);

    CHECK(parse_status("| ls") == PARSE_ERROR);
    CHECK(parse_status("ls ; ; ls") == PARSE_ERROR);
    CHECK(parse_status("&& ls") == PARSE_ERROR);
    CHECK(parse_status("ls >") == PARSE_ERROR);
    CHECK(parse_status("ls > | wc") == PARSE_ERROR);
    CHECK(parse_status("ls & & ls") == PARSE_ERROR);

    CHECK(parse("ls >", &l, &err) == PARSE_ERROR);
    CHECK(err && strstr(err, "file name"));
    free(err);
}

static void test_aliases(void)
{
    List *l;
    char *err;

    alias_clear();
    CHECK(alias_get("ll") == NULL);
    alias_set("ll", "ls -la");
    alias_set("aa", "first");
    alias_set("zz", "last");
    alias_set("aa", "replaced");
    CHECK(alias_count() == 3);
    CHECK_STR(alias_name(0), "aa");
    CHECK_STR(alias_value(0), "replaced");
    CHECK_STR(alias_name(2), "zz");
    CHECK_STR(alias_get("ll"), "ls -la");
    CHECK(alias_unset("aa") && !alias_unset("aa"));
    CHECK(alias_count() == 2);
    CHECK(alias_valid_name("ll") && alias_valid_name("..") &&
          !alias_valid_name("a b") && !alias_valid_name("a=b") &&
          !alias_valid_name(""));

    /* expansion happens in command position only */
    CHECK(parse("ll /tmp; echo ll | ll", &l, &err) == PARSE_OK);
    Command *c = l->items[0]->pipes[0]->cmds[0];
    CHECK(c->nwords == 3);
    CHECK_STR(c->words[0], "ls");
    CHECK_STR(c->words[1], "-la");
    c = l->items[1]->pipes[0]->cmds[0];
    CHECK(c->nwords == 2);
    CHECK_STR(c->words[1], "ll");
    c = l->items[1]->pipes[0]->cmds[1];
    CHECK_STR(c->words[0], "ls");
    list_free(l);

    /* quoting the name suppresses the alias */
    CHECK(parse("\\ll", &l, &err) == PARSE_OK);
    CHECK_STR(l->items[0]->pipes[0]->cmds[0]->words[0], "\\ll");
    list_free(l);

    /* an alias may refer to its own name without recursing */
    alias_set("ls", "ls --color");
    CHECK(parse("ls", &l, &err) == PARSE_OK);
    c = l->items[0]->pipes[0]->cmds[0];
    CHECK(c->nwords == 2);
    CHECK_STR(c->words[1], "--color");
    list_free(l);

    /* aliases can contain operators */
    alias_set("both", "echo a; echo b |");
    CHECK(parse("both cat", &l, &err) == PARSE_OK);
    CHECK(l->n == 2 && l->items[1]->pipes[0]->ncmds == 2);
    list_free(l);

    /* a loop through an operator is caught rather than hanging */
    alias_set("loop", "true; loop");
    CHECK(parse("loop", &l, &err) == PARSE_ERROR);
    CHECK(err && strstr(err, "loop"));
    free(err);

    alias_set("broken", "echo 'unterminated");
    CHECK(parse("broken", &l, &err) == PARSE_ERROR);
    free(err);

    alias_clear();
    CHECK(alias_count() == 0);
}

/* ---- arithmetic --------------------------------------------------- */

static long long arith(const char *expr)
{
    long long v = -99999;
    char *err = NULL;
    if (!arith_eval(expr, &v, &err)) {
        free(err);
        return -99999;
    }
    return v;
}

static void test_arith(void)
{
    CHECK(arith("1 + 2 * 3") == 7);
    CHECK(arith("(1 + 2) * 3") == 9);
    CHECK(arith("10 / 3") == 3 && arith("10 % 3") == 1);
    CHECK(arith("-5 + 2") == -3 && arith("- -5") == 5);
    CHECK(arith("2 - 3 - 4") == -5); /* left associative */
    CHECK(arith("1 << 4") == 16 && arith("256 >> 2") == 64);
    CHECK(arith("3 > 2") == 1 && arith("3 <= 2") == 0);
    CHECK(arith("1 == 1 && 2 != 3") == 1);
    CHECK(arith("0 || 0") == 0 && arith("!0") == 1 && arith("~0") == -1);
    CHECK(arith("6 & 3") == 2 && arith("6 | 3") == 7 && arith("6 ^ 3") == 5);
    CHECK(arith("0x10 + 010") == 24);
    CHECK(arith("  ") == 0);
    var_set("COUNT", "41");
    CHECK(arith("COUNT + 1") == 42);
    CHECK(arith("UNSET_VARIABLE_X + 1") == 1);
    var_unset("COUNT");

    long long v;
    char *err = NULL;
    CHECK(!arith_eval("1 / 0", &v, &err) && strstr(err, "zero"));
    free(err);
    CHECK(!arith_eval("1 +", &v, &err));
    free(err);
    CHECK(!arith_eval("(1", &v, &err));
    free(err);
    CHECK(!arith_eval("1 2", &v, &err));
    free(err);
    CHECK(!arith_eval("12abc", &v, &err));
    free(err);
    /* must not overflow or crash */
    CHECK(arith_eval("-9223372036854775807 - 1", &v, &err));
    CHECK(arith_eval("(-9223372036854775807 - 1) / -1", &v, &err));
    CHECK(arith_eval("(-9223372036854775807 - 1) % -1", &v, &err) && v == 0);
}

/* ---- expansion ---------------------------------------------------- */

static void test_expand(void)
{
    setenv("HOME", "/home/tester", 1);
    var_set("NAME", "world");
    var_set("EMPTY", "");
    var_set("SPACED", "a  b c");
    var_set("STAR", "*");
    var_unset("NOPE");

    CHECK_EXPAND("plain", "plain");
    CHECK_EXPAND("$NAME", "world");
    CHECK_EXPAND("${NAME}ly", "worldly");
    CHECK_EXPAND("pre-$NAME-post", "pre-world-post");
    CHECK_EXPAND("'$NAME'", "$NAME");
    CHECK_EXPAND("\"$NAME\"", "world");
    CHECK_EXPAND("\\$NAME", "$NAME");
    CHECK_EXPAND("\"a \\\" b\"", "a \" b");
    CHECK_EXPAND("\"keep \\n\"", "keep \\n");
    CHECK_EXPAND("a\\ b", "a b");
    CHECK_EXPAND("$", "$");
    CHECK_EXPAND("a$", "a$");

    /* unset and empty variables vanish unless quoted */
    CHECK_EXPAND("$NOPE", "");
    CHECK_EXPAND("x$NOPE", "x");
    {
        char *words[] = { "$NOPE", "\"$NOPE\"", "''", "$EMPTY" };
        StrVec out;
        sv_init(&out);
        CHECK(expand_words(words, 4, &out));
        CHECK(out.len == 2);
        CHECK_STR(out.items[0], "");
        sv_free(&out);
    }

    /* field splitting applies to unquoted expansions only */
    CHECK_EXPAND("$SPACED", "a|b|c");
    CHECK_EXPAND("\"$SPACED\"", "a  b c");
    CHECK_EXPAND("x${SPACED}y", "xa|b|cy");

    /* parameter forms */
    CHECK_EXPAND("${NOPE:-fallback}", "fallback");
    CHECK_EXPAND("${NAME:-fallback}", "world");
    CHECK_EXPAND("${EMPTY:-fallback}", "fallback");
    CHECK_EXPAND("${EMPTY-fallback}", "");
    CHECK_EXPAND("${NAME:+alt}", "alt");
    CHECK_EXPAND("${NOPE:+alt}", "");
    CHECK_EXPAND("${NOPE:-$NAME}", "world");
    CHECK_EXPAND("${#NAME}", "5");
    CHECK_EXPAND("${#NOPE}", "0");
    CHECK_EXPAND("${NOPE:=assigned}", "assigned");
    CHECK_STR(var_get("NOPE"), "assigned");
    var_unset("NOPE");
    quiet_begin();
    CHECK_EXPAND("${NOPE:?must be set}", "<error>");
    CHECK_EXPAND("${NAME%x}", "<error>");
    quiet_end();

    /* tilde */
    CHECK_EXPAND("~", "/home/tester");
    CHECK_EXPAND("~/docs", "/home/tester/docs");
    CHECK_EXPAND("a~", "a~");
    CHECK_EXPAND("'~'", "~");
    CHECK_EXPAND("\"~\"", "~");
    CHECK_EXPAND("~no-such-user-eclipse/x", "~no-such-user-eclipse/x");
    CHECK_EXPAND("PATH=~/bin:~/sbin", "PATH=/home/tester/bin:/home/tester/sbin");
    char *single = expand_single("X=~/a", true);
    CHECK_STR(single, "X=/home/tester/a");
    free(single);

    /* arithmetic */
    CHECK_EXPAND("$((1 + 2 * 3))", "7");
    CHECK_EXPAND("n=$(( (2 + 3) * 4 ))", "n=20");
    var_set("N", "6");
    CHECK_EXPAND("$(($N * 7))", "42");
    CHECK_EXPAND("$((N * 7))", "42");
    quiet_begin();
    CHECK_EXPAND("$((1 / 0))", "<error>");
    quiet_end();

    /* special parameters */
    shell.last_status = 3;
    CHECK_EXPAND("$?", "3");
    char *args[] = { "one", "two words", "" };
    shell.pos_args = args;
    shell.pos_argc = 3;
    CHECK_EXPAND("$#", "3");
    CHECK_EXPAND("$1", "one");
    CHECK_EXPAND("\"$2\"", "two words");
    CHECK_EXPAND("$9", "");
    {
        char *words[] = { "\"$@\"" };
        StrVec out;
        sv_init(&out);
        CHECK(expand_words(words, 1, &out));
        CHECK(out.len == 3);
        CHECK_STR(out.items[1], "two words");
        CHECK_STR(out.items[2], "");
        sv_free(&out);
    }
    CHECK_EXPAND("\"$*\"", "one two words ");
    shell.pos_args = NULL;
    shell.pos_argc = 0;
    {
        char *words[] = { "\"$@\"" };
        StrVec out;
        sv_init(&out);
        CHECK(expand_words(words, 1, &out));
        CHECK(out.len == 0); /* "$@" with no arguments is nothing at all */
        sv_free(&out);
    }

    /* braces */
    CHECK_EXPAND("{a,b,c}", "a|b|c");
    CHECK_EXPAND("x{a,b}y", "xay|xby");
    CHECK_EXPAND("{a,b}{1,2}", "a1|a2|b1|b2");
    CHECK_EXPAND("{a,{b,c}}", "a|b|c");
    CHECK_EXPAND("{1..4}", "1|2|3|4");
    CHECK_EXPAND("{3..1}", "3|2|1");
    CHECK_EXPAND("{a..d}", "a|b|c|d");
    CHECK_EXPAND("f{,.bak}", "f|f.bak");
    CHECK_EXPAND("{}", "{}");
    CHECK_EXPAND("{solo}", "{solo}");
    CHECK_EXPAND("'{a,b}'", "{a,b}");
    CHECK_EXPAND("\"{a,b}\"", "{a,b}");
    CHECK_EXPAND("\\{a,b}", "{a,b}");
    CHECK_EXPAND("${NAME}{1,2}", "world1|world2");

    /* globbing */
    make_dir("glob");
    touch("glob/one.txt", "");
    touch("glob/two.txt", "");
    touch("glob/three.md", "");
    touch("glob/.hidden.txt", "");
    char *old = getcwd(NULL, 0);
    char *gdir = path_join(scratch, "glob");
    CHECK(chdir(gdir) == 0);
    CHECK_EXPAND("*.txt", "one.txt|two.txt");
    CHECK_EXPAND("t*", "three.md|two.txt");
    CHECK_EXPAND("???.txt", "one.txt|two.txt");
    CHECK_EXPAND("[ot]*.md", "three.md");
    CHECK_EXPAND("'*.txt'", "*.txt");
    CHECK_EXPAND("\"*\".txt", "*.txt");
    CHECK_EXPAND("\\*.txt", "*.txt");
    CHECK_EXPAND("*.nomatch", "*.nomatch"); /* kept as typed */
    CHECK_EXPAND("$STAR.md", "three.md");   /* expansions glob too */
    CHECK_EXPAND("\"$STAR\".md", "*.md");
    CHECK_EXPAND("{one,two}.*", "one.txt|two.txt");
    CHECK(chdir(old) == 0);
    free(old);
    free(gdir);

    var_unset("NAME");
    var_unset("EMPTY");
    var_unset("SPACED");
    var_unset("STAR");
    var_unset("N");
}

/* ---- variables ---------------------------------------------------- */

static void test_vars(void)
{
    var_unset("ECL_T");
    CHECK(var_get("ECL_T") == NULL);
    var_set("ECL_T", "local");
    CHECK_STR(var_get("ECL_T"), "local");
    CHECK(getenv("ECL_T") == NULL && !var_is_exported("ECL_T"));
    var_export("ECL_T", NULL);
    CHECK_STR(getenv("ECL_T"), "local");
    var_set("ECL_T", "changed"); /* stays exported */
    CHECK_STR(getenv("ECL_T"), "changed");
    var_export("ECL_U", "direct");
    CHECK_STR(getenv("ECL_U"), "direct");
    var_unset("ECL_T");
    var_unset("ECL_U");
    CHECK(var_get("ECL_T") == NULL && getenv("ECL_U") == NULL);

    var_set("ECL_LOCAL_ONLY", "1");
    StrVec names;
    sv_init(&names);
    var_names(&names);
    CHECK(sv_contains(&names, "ECL_LOCAL_ONLY") && sv_contains(&names, "HOME"));
    sv_free(&names);
    var_unset("ECL_LOCAL_ONLY");
}

/* ---- suggestions -------------------------------------------------- */

static void test_suggest(void)
{
    CHECK(edit_distance("", "") == 0);
    CHECK(edit_distance("abc", "") == 3);
    CHECK(edit_distance("git", "git") == 0);
    CHECK(edit_distance("gti", "git") == 1);    /* transposition */
    CHECK(edit_distance("gitt", "git") == 1);   /* deletion */
    CHECK(edit_distance("gt", "git") == 1);     /* insertion */
    CHECK(edit_distance("gat", "git") == 1);    /* substitution */
    CHECK(edit_distance("kitten", "sitting") == 3);
    CHECK(suggest_threshold(2) == 1 && suggest_threshold(5) == 2 &&
          suggest_threshold(12) == 3);

    char *cands[] = { "Documents", "Downloads", "Desktop", "Music" };
    char *best = suggest_closest("Documnets", cands, 4);
    CHECK_STR(best, "Documents");
    free(best);
    best = suggest_closest("Dwnloads", cands, 4);
    CHECK_STR(best, "Downloads");
    free(best);
    CHECK(suggest_closest("zzzzzz", cands, 4) == NULL);

    /* swapped letters beat an equally close substitution */
    char *close[] = { "gtf", "git", "gtk" };
    best = suggest_closest("gti", close, 3);
    CHECK_STR(best, "git");
    free(best);
    char *shorts[] = { "sh", "ss", "ls" };
    best = suggest_closest("sl", shorts, 3);
    CHECK_STR(best, "ls");
    free(best);
    CHECK(suggest_closest("", cands, 4) == NULL);

    /* builtins are offered for near misses */
    StrVec s;
    sv_init(&s);
    suggest_commands("whereamii", &s, 3);
    CHECK(s.len >= 1);
    if (s.len)
        CHECK_STR(s.items[0], "whereami");
    sv_free(&s);
    suggest_commands("exitt", &s, 3);
    CHECK(sv_contains(&s, "exit"));
    CHECK(s.len <= 3);
    sv_free(&s);
    /* a worse guess is not listed next to a clearly better one */
    suggest_commands("hsitory", &s, 3);
    CHECK(s.len == 1);
    if (s.len)
        CHECK_STR(s.items[0], "history");
    sv_free(&s);
}

/* ---- history ------------------------------------------------------ */

static void test_history(void)
{
    char *enc = history_encode("line one\nline \\two");
    CHECK_STR(enc, "line one\\nline \\\\two");
    char *dec = history_decode(enc);
    CHECK_STR(dec, "line one\nline \\two");
    free(enc);
    free(dec);
    dec = history_decode("a\\\\nb"); /* an escaped backslash, then 'n' */
    CHECK_STR(dec, "a\\nb");
    free(dec);

    char *file = path_join(scratch, "state/deep/history");
    setenv("ECLIPSE_HISTORY", file, 1);
    history_init(true);
    CHECK(history_count() == 0);
    CHECK_STR(history_file(), file);

    history_add("echo one");
    history_add("echo one");        /* immediate repeat: ignored */
    history_add("   ");             /* blank: ignored */
    history_add(" secret command"); /* leading space: ignored */
    history_add("echo two  \n");    /* trailing blanks trimmed */
    history_add("multi\nline");
    CHECK(history_count() == 3);
    CHECK_STR(history_get(0), "echo one");
    CHECK_STR(history_get(1), "echo two");
    CHECK_STR(history_get(2), "multi\nline");
    CHECK(history_get(3) == NULL);

    /* the file is private and survives a restart */
    struct stat st;
    CHECK(stat(file, &st) == 0 && (st.st_mode & 0777) == 0600);
    history_init(true);
    CHECK(history_count() == 3);
    CHECK_STR(history_get(2), "multi\nline");

    /* !! and !$ */
    bool changed;
    history_add("ls -l /tmp");
    char *x = history_expand("sudo !!", &changed);
    CHECK_STR(x, "sudo ls -l /tmp");
    CHECK(changed);
    free(x);
    x = history_expand("cd !$", &changed);
    CHECK_STR(x, "cd /tmp");
    free(x);
    x = history_expand("echo '!!' \\!! wow!", &changed);
    CHECK_STR(x, "echo '!!' \\!! wow!");
    CHECK(!changed);
    free(x);

    /* history_size caps memory and, on the next start, the file */
    char *err = NULL;
    CHECK(settings_set("history_size", "3", &err));
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "cmd %d", i);
        history_add(buf);
    }
    CHECK(history_count() == 3);
    CHECK_STR(history_get(0), "cmd 7");
    history_init(true);
    CHECK(history_count() == 3);
    CHECK_STR(history_get(2), "cmd 9");
    FILE *fp = fopen(file, "r");
    int lines = 0, ch;
    while (fp && (ch = fgetc(fp)) != EOF)
        if (ch == '\n')
            lines++;
    if (fp)
        fclose(fp);
    CHECK(lines == 3);

    history_clear();
    CHECK(history_count() == 0);
    CHECK(stat(file, &st) == 0 && st.st_size == 0);
    x = history_expand("!!", &changed);
    CHECK(x == NULL); /* nothing to repeat */

    CHECK(settings_set("history_size", "5000", &err));
    history_free();
    unsetenv("ECLIPSE_HISTORY");
    free(file);

    /* without persistence nothing touches the disk */
    history_init(false);
    history_add("in memory");
    CHECK(history_count() == 1 && history_file() == NULL);
    history_free();
}

/* ---- settings and colours ----------------------------------------- */

static void test_settings(void)
{
    char *err = NULL;
    char *v = settings_get("autosuggest");
    CHECK_STR(v, "on");
    free(v);
    CHECK(settings_set("autosuggest", "off", &err));
    CHECK(!settings.autosuggest);
    CHECK(settings_set("autosuggest", "YES", &err) && settings.autosuggest);
    CHECK(!settings_set("autosuggest", "maybe", &err) && err);
    free(err);
    err = NULL;
    CHECK(!settings_set("no_such_setting", "1", &err) && err);
    free(err);
    err = NULL;
    CHECK(!settings_set("history_size", "-4", &err));
    free(err);
    err = NULL;
    CHECK(!settings_set("history_size", "lots", &err));
    free(err);
    err = NULL;
    CHECK(settings_set("color.path", "bold #7aa2f7", &err));
    CHECK_STR(settings.color_path, "bold #7aa2f7");
    CHECK(!settings_set("color.path", "sparkly", &err) && err);
    free(err);
    CHECK_STR(settings.color_path, "bold #7aa2f7"); /* unchanged on error */
    CHECK(settings_set("color.path", "bold cyan", &err));
    CHECK(settings_get("nope") == NULL);

    StrBuf sb;
    sb_init(&sb);
    CHECK(color_parse("red", &sb));
    CHECK_STR(sb.data, "\033[31m");
    sb_clear(&sb);
    CHECK(color_parse("bold bright-blue", &sb));
    CHECK_STR(sb.data, "\033[1;94m");
    sb_clear(&sb);
    CHECK(color_parse("#ff8000", &sb));
    CHECK_STR(sb.data, "\033[38;2;255;128;0m");
    sb_clear(&sb);
    CHECK(color_parse("208", &sb));
    CHECK_STR(sb.data, "\033[38;5;208m");
    sb_clear(&sb);
    CHECK(color_parse("none", &sb) && sb.len == 0);
    CHECK(!color_parse("#12345", &sb) && sb.len == 0);
    CHECK(!color_parse("256", &sb));
    CHECK(!color_parse("bold nonsense", &sb) && sb.len == 0);
    sb_free(&sb);
}

/* ---- prompt and git ----------------------------------------------- */

static void test_prompt(void)
{
    char *old = getcwd(NULL, 0);

    make_dir("repo/.git");
    make_dir("repo/src/deep");
    touch("repo/.git/HEAD", "ref: refs/heads/feature/login\n");
    char *deep = path_join(scratch, "repo/src/deep");
    char *b = git_branch(deep);
    CHECK_STR(b, "feature/login");
    free(b);

    touch("repo/.git/HEAD", "0123456789abcdef0123456789abcdef01234567\n");
    b = git_branch(deep);
    CHECK_STR(b, "0123456"); /* detached HEAD */
    free(b);

    /* a worktree: .git is a file pointing elsewhere */
    make_dir("realgit");
    touch("realgit/HEAD", "ref: refs/heads/worktree-branch\n");
    make_dir("wt");
    touch("wt/.git", "gitdir: ../realgit\n");
    char *wt = path_join(scratch, "wt");
    b = git_branch(wt);
    CHECK_STR(b, "worktree-branch");
    free(b);
    free(wt);

    make_dir("plain");
    char *plain = path_join(scratch, "plain");
    /* (the scratch directory itself may live inside a repository, so
     * only check that this does not crash) */
    free(git_branch(plain));
    CHECK(git_branch("relative/path") == NULL);

    touch("repo/.git/HEAD", "ref: refs/heads/main\n");
    CHECK(chdir(deep) == 0);
    char *p = prompt_render("%W|%?|%%|%g|%n|%q|%", 7, false);
    CHECK_STR(p, "deep|7|%| (main)|\n|%q|%");
    free(p);
    p = prompt_render("%{bold red}x%{reset}%{user}%{nonsense}", 0, false);
    CHECK_STR(p, "x"); /* colours vanish when colour is off */
    free(p);
    p = prompt_render("%{red}x%{reset}", 0, true);
    CHECK_STR(p, "\033[31mx\033[0m");
    free(p);
    p = prompt_render("%s", 0, false);
    CHECK_STR(p, settings.symbol);
    free(p);
    p = prompt_render("%d", 0, false);
    CHECK(strstr(p, "repo/src/deep") != NULL);
    free(p);

    /* hostile branch names cannot smuggle escape sequences */
    touch("repo/.git/HEAD", "ref: refs/heads/evil\033[2Jname\n");
    p = prompt_render("%g", 0, false);
    CHECK(strchr(p, '\033') == NULL);
    free(p);

    CHECK(chdir(old) == 0);
    free(old);
    free(deep);
    free(plain);
}

/* ---- completion --------------------------------------------------- */

static bool completes_to(const char *line, const char *want)
{
    Completion c;
    complete_line(line, strlen(line), &c);
    bool found = sv_contains(&c.items, want);
    completion_free(&c);
    return found;
}

static size_t completion_count(const char *line)
{
    Completion c;
    complete_line(line, strlen(line), &c);
    size_t n = c.items.len;
    completion_free(&c);
    return n;
}

static void test_complete(void)
{
    char *e = complete_escape("my file (1).txt", 0);
    CHECK_STR(e, "my\\ file\\ \\(1\\).txt");
    free(e);
    e = complete_escape("~/a b", 0);
    CHECK_STR(e, "~/a\\ b");
    free(e);
    e = complete_escape("say \"hi\" $x", '"');
    CHECK_STR(e, "say \\\"hi\\\" \\$x");
    free(e);
    e = complete_escape("it's", '\'');
    CHECK_STR(e, "it'\\''s");
    free(e);

    StrVec sv;
    sv_init(&sv);
    sv_push_copy(&sv, "report-2023.txt");
    sv_push_copy(&sv, "report-2024.txt");
    sv_push_copy(&sv, "report-final.txt");
    char *lcp = complete_common_prefix(&sv);
    CHECK_STR(lcp, "report-");
    free(lcp);
    sv_free(&sv);
    sv_push_copy(&sv, "caf\xC3\xA9");
    sv_push_copy(&sv, "caf\xC3\xA8");
    lcp = complete_common_prefix(&sv);
    CHECK_STR(lcp, "caf"); /* not half of the accented letter */
    free(lcp);
    sv_free(&sv);

    make_dir("comp/Documents");
    make_dir("comp/Downloads");
    touch("comp/data.csv", "");
    touch("comp/my file.txt", "");
    touch("comp/.secret", "");
    touch("comp/run.sh", "");
    char *run = path_join(scratch, "comp/run.sh");
    chmod(run, 0755);
    free(run);
    char *old = getcwd(NULL, 0);
    char *dir = path_join(scratch, "comp");
    CHECK(chdir(dir) == 0);

    /* commands in command position */
    CHECK(completes_to("wherea", "whereami"));
    CHECK(completes_to("ls | wherea", "whereami"));
    CHECK(completes_to("true && wherea", "whereami"));
    CHECK(completes_to("FOO=1 wherea", "whereami"));
    CHECK(completion_count("") == 0); /* never list every command */
    alias_set("wherever", "pwd");
    CHECK(completes_to("wherev", "wherever"));
    alias_clear();

    /* files as arguments; directories get a slash */
    CHECK(completes_to("cat da", "data.csv"));
    CHECK(completes_to("cat D", "Documents/"));
    CHECK(completion_count("cat D") == 2);
    CHECK(completes_to("cat ", "data.csv"));
    CHECK(!completes_to("cat ", ".secret")); /* hidden unless asked */
    CHECK(completes_to("cat .s", ".secret"));
    CHECK(completes_to("cat my", "my file.txt"));
    CHECK(completes_to("cat my\\ f", "my file.txt"));
    CHECK(completes_to("cat 'my f", "my file.txt"));
    CHECK(completes_to("cat \"my f", "my file.txt"));
    CHECK(completes_to("echo hi > da", "data.csv"));
    CHECK(completion_count("cat nothing-here") == 0);

    /* cd-like commands only offer directories */
    CHECK(completes_to("cd D", "Downloads/"));
    CHECK(completion_count("cd d") == 0);
    CHECK(completion_count("go ") == 2);

    /* paths in command position offer executables and directories */
    CHECK(completes_to("./r", "./run.sh"));
    CHECK(!completes_to("./d", "./data.csv"));
    CHECK(completes_to("./D", "./Documents/"));

    /* the quote state and word start are reported */
    Completion c;
    complete_line("cat \"my f", 9, &c);
    CHECK(c.quote == '"' && c.start == 4);
    CHECK_STR(c.word, "my f");
    completion_free(&c);
    complete_line("cat data.csv", 6, &c); /* cursor in the middle */
    CHECK_STR(c.word, "da");
    completion_free(&c);

    /* variables */
    var_set("ECL_COMPLETE_ME", "1");
    CHECK(completes_to("echo $ECL_COMP", "$ECL_COMPLETE_ME"));
    CHECK(completes_to("echo ${ECL_COMP", "${ECL_COMPLETE_ME}"));
    CHECK(completes_to("unset ECL_COMP", "ECL_COMPLETE_ME"));
    var_unset("ECL_COMPLETE_ME");

    /* builtin arguments */
    CHECK(completes_to("set color.p", "color.path"));
    CHECK(completes_to("help fin", "findfile"));

    /* tilde paths keep their tilde */
    setenv("HOME", dir, 1);
    CHECK(completes_to("ls ~/Doc", "~/Documents/"));
    CHECK(completes_to("ls ~", "~/"));

    CHECK(chdir(old) == 0);
    free(old);
    free(dir);
}

/* ---- jobs --------------------------------------------------------- */

static void test_jobs(void)
{
    const char *err = NULL;
    CHECK(job_lookup(NULL, &err) == NULL && err != NULL);
    CHECK(job_lookup("%3", &err) == NULL && err != NULL);
    CHECK(jobs_count() == 0 && jobs_stopped_count() == 0);

    Job *j = job_new("sleep 100");
    job_add_process(j, 999999);
    job_register(j);
    CHECK(j->id == 1 && jobs_count() == 1);
    CHECK(job_lookup("%1", &err) == j);
    CHECK(job_lookup("1", &err) == j);
    CHECK(job_lookup("%sle", &err) == j);
    CHECK(job_lookup("%+", &err) == j);
    CHECK(job_lookup("%nope", &err) == NULL);
    CHECK(!job_is_completed(j) && !job_is_stopped(j));
    j->procs[0].stopped = true;
    CHECK(job_is_stopped(j) && jobs_stopped_count() == 1);
    j->procs[0].completed = true;
    CHECK(job_is_completed(j) && !job_is_stopped(j));
    jobs_free();
    CHECK(jobs_count() == 0);
}

int main(void)
{
    setlocale(LC_ALL, "");
    settings_init();
    shell.pid = getpid();
    scratch_create();

    test_util();
    test_lexer();
    test_parser();
    test_aliases();
    test_arith();
    test_expand();
    test_vars();
    test_suggest();
    test_history();
    test_settings();
    test_prompt();
    test_complete();
    test_jobs();

    settings_free();
    vars_free();
    if (failures) {
        fprintf(stderr, "unit tests: %d of %d checks FAILED\n", failures,
                checks);
        return 1;
    }
    printf("unit tests: all %d checks passed\n", checks);
    return 0;
}
