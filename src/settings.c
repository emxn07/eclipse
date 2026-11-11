/* settings.c - user-tunable options (the `set` builtin) and colours. */
#include "settings.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

Settings settings;

typedef enum { OPT_STRING, OPT_COLOR, OPT_BOOL, OPT_LONG } OptType;

typedef struct {
    const char *key;
    OptType type;
    size_t offset;
    const char *def;
    const char *help;
} Option;

#define OPT(key, type, field, def, help) \
    { key, type, offsetof(Settings, field), def, help }

static const Option options[] = {
    OPT("prompt", OPT_STRING, prompt,
        "%{bracket}[%{user}%u%{bracket}@%{host}%h %{path}%w%{bracket}]"
        "%{reset}%g %s ",
        "prompt format (see 'help set')"),
    OPT("prompt2", OPT_STRING, prompt2, "%{bracket}...%{reset} %s ",
        "continuation prompt format"),
    OPT("symbol", OPT_STRING, symbol, NULL, "prompt symbol used by %s"),
    OPT("git_prefix", OPT_STRING, git_prefix, " (",
        "text before the git branch"),
    OPT("git_suffix", OPT_STRING, git_suffix, ")",
        "text after the git branch"),
    OPT("color.user", OPT_COLOR, color_user, "bold magenta", "user name"),
    OPT("color.host", OPT_COLOR, color_host, "magenta", "host name"),
    OPT("color.path", OPT_COLOR, color_path, "bold cyan", "directory"),
    OPT("color.git", OPT_COLOR, color_git, "yellow", "git branch"),
    OPT("color.symbol", OPT_COLOR, color_symbol, "bold magenta",
        "prompt symbol after success"),
    OPT("color.error", OPT_COLOR, color_error, "bold red",
        "prompt symbol after failure"),
    OPT("color.bracket", OPT_COLOR, color_bracket, "bright-black",
        "prompt brackets"),
    OPT("color.suggestion", OPT_COLOR, color_suggestion, "bright-black",
        "inline suggestions"),
    OPT("color", OPT_BOOL, color, "on", "use colour at all"),
    OPT("autosuggest", OPT_BOOL, autosuggest, "on",
        "suggest commands from history while typing"),
    OPT("suggest_commands", OPT_BOOL, suggest_commands, "on",
        "offer corrections for mistyped commands"),
    OPT("history_size", OPT_LONG, history_size, "5000",
        "number of commands kept in history"),
};

#define N_OPTIONS (sizeof options / sizeof options[0])

static const Option *find_option(const char *key)
{
    for (size_t i = 0; i < N_OPTIONS; i++)
        if (strcmp(options[i].key, key) == 0)
            return &options[i];
    return NULL;
}

static void *field_ptr(const Option *o)
{
    return (char *)&settings + o->offset;
}

static char *errf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static char *errf(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return xstrdup(buf);
}

static bool parse_bool(const char *v, bool *out)
{
    static const char *const yes[] = { "on", "true", "yes", "1" };
    static const char *const no[] = { "off", "false", "no", "0" };
    for (size_t i = 0; i < 4; i++) {
        if (strcasecmp(v, yes[i]) == 0) {
            *out = true;
            return true;
        }
        if (strcasecmp(v, no[i]) == 0) {
            *out = false;
            return true;
        }
    }
    return false;
}

bool settings_set(const char *key, const char *value, char **err)
{
    const Option *o = find_option(key);
    if (!o) {
        *err = errf("unknown setting '%s'", key);
        return false;
    }
    switch (o->type) {
    case OPT_COLOR: {
        StrBuf tmp;
        sb_init(&tmp);
        bool ok = color_parse(value, &tmp);
        sb_free(&tmp);
        if (!ok) {
            *err = errf("'%s' is not a colour (try: red, bold cyan, "
                        "bright-blue, 208, #ffaa00)",
                        value);
            return false;
        }
    } /* fall through */
    case OPT_STRING: {
        char **p = field_ptr(o);
        free(*p);
        *p = xstrdup(value);
        return true;
    }
    case OPT_BOOL: {
        bool b;
        if (!parse_bool(value, &b)) {
            *err = errf("'%s' must be 'on' or 'off', not '%s'", key, value);
            return false;
        }
        *(bool *)field_ptr(o) = b;
        return true;
    }
    case OPT_LONG: {
        long n;
        if (!parse_long(value, &n) || n < 0 || n > 1000000) {
            *err = errf("'%s' must be a number from 0 to 1000000, not '%s'",
                        key, value);
            return false;
        }
        *(long *)field_ptr(o) = n;
        return true;
    }
    }
    return false;
}

static char *option_value(const Option *o)
{
    switch (o->type) {
    case OPT_STRING:
    case OPT_COLOR:
        return xstrdup(*(char **)field_ptr(o));
    case OPT_BOOL:
        return xstrdup(*(bool *)field_ptr(o) ? "on" : "off");
    case OPT_LONG: {
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "%ld", *(long *)field_ptr(o));
        return sb_take(&sb);
    }
    }
    return NULL;
}

char *settings_get(const char *key)
{
    const Option *o = find_option(key);
    return o ? option_value(o) : NULL;
}

void settings_each(void (*fn)(const char *, const char *, const char *,
                              void *),
                   void *ctx)
{
    for (size_t i = 0; i < N_OPTIONS; i++) {
        char *v = option_value(&options[i]);
        fn(options[i].key, v, options[i].help, ctx);
        free(v);
    }
}

void settings_names(StrVec *out)
{
    for (size_t i = 0; i < N_OPTIONS; i++)
        sv_push_copy(out, options[i].key);
}

void settings_init(void)
{
    for (size_t i = 0; i < N_OPTIONS; i++) {
        const Option *o = &options[i];
        const char *def = o->def;
        if (!def) /* only "symbol": depends on the locale */
            def = locale_is_utf8() ? "\xE2\x9D\xAF" /* ❯ */ : ">";
        char *err = NULL;
        if (!settings_set(o->key, def, &err)) {
            /* A broken built-in default is a programming error. */
            fprintf(stderr, "eclipse: bad default for %s: %s\n", o->key, err);
            free(err);
            abort();
        }
    }
}

void settings_free(void)
{
    for (size_t i = 0; i < N_OPTIONS; i++) {
        const Option *o = &options[i];
        if (o->type == OPT_STRING || o->type == OPT_COLOR) {
            char **p = field_ptr(o);
            free(*p);
            *p = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* ------------------------------------------------------------------ */

bool color_enabled(int fd)
{
    if (!settings.color)
        return false;
    const char *nc = getenv("NO_COLOR");
    if (nc && *nc)
        return false;
    const char *term = getenv("TERM");
    if (!term || !*term || strcmp(term, "dumb") == 0)
        return false;
    return isatty(fd) == 1;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* Appends the SGR parameter(s) for one word; false if unknown. */
static bool color_word(const char *w, StrBuf *codes)
{
    static const char *const names[] = { "black", "red",     "green", "yellow",
                                         "blue",  "magenta", "cyan",  "white" };
    static const struct {
        const char *name;
        const char *code;
    } attrs[] = {
        { "bold", "1" },      { "dim", "2" },     { "italic", "3" },
        { "underline", "4" }, { "reverse", "7" }, { "default", "39" },
    };

    if (codes->len)
        sb_putc(codes, ';');
    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        if (strcasecmp(w, attrs[i].name) == 0) {
            sb_puts(codes, attrs[i].code);
            return true;
        }
    }
    int base = 30;
    const char *name = w;
    if (strncasecmp(w, "bright-", 7) == 0) {
        base = 90;
        name = w + 7;
    }
    for (int i = 0; i < 8; i++) {
        if (strcasecmp(name, names[i]) == 0) {
            sb_printf(codes, "%d", base + i);
            return true;
        }
    }
    if (strcasecmp(w, "grey") == 0 || strcasecmp(w, "gray") == 0) {
        sb_puts(codes, "90");
        return true;
    }
    if (w[0] == '#' && strlen(w) == 7) {
        int v[6];
        for (int i = 0; i < 6; i++) {
            v[i] = hex_val(w[i + 1]);
            if (v[i] < 0)
                return false;
        }
        sb_printf(codes, "38;2;%d;%d;%d", v[0] * 16 + v[1], v[2] * 16 + v[3],
                  v[4] * 16 + v[5]);
        return true;
    }
    long n;
    if (parse_long(w, &n) && n >= 0 && n <= 255) {
        sb_printf(codes, "38;5;%ld", n);
        return true;
    }
    return false;
}

bool color_parse(const char *spec, StrBuf *out)
{
    StrBuf codes;
    sb_init(&codes);
    char *copy = xstrdup(spec);
    char *save = NULL;
    bool ok = true;
    for (char *w = strtok_r(copy, " \t,", &save); w;
         w = strtok_r(NULL, " \t,", &save)) {
        if (strcasecmp(w, "none") == 0 || strcasecmp(w, "normal") == 0)
            continue;
        if (!color_word(w, &codes)) {
            ok = false;
            break;
        }
    }
    free(copy);
    if (ok && codes.len) {
        sb_puts(out, "\033[");
        sb_puts(out, codes.data);
        sb_putc(out, 'm');
    }
    sb_free(&codes);
    return ok;
}
