/* complete.c - Tab completion for commands, files, variables and the
 * arguments of a few builtins. */
#include "complete.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alias.h"
#include "builtins.h"
#include "settings.h"
#include "suggest.h"
#include "vars.h"

void completion_free(Completion *c)
{
    sv_free(&c->items);
    free(c->word);
    c->word = NULL;
}

char *complete_escape(const char *text, char quote)
{
    StrBuf sb;
    sb_init(&sb);
    for (const char *p = text; *p; p++) {
        if (quote == '\'') {
            if (*p == '\'')
                sb_puts(&sb, "'\\'"); /* close, escaped quote, reopen */
            sb_putc(&sb, *p);
        } else if (quote == '"') {
            if (strchr("\"\\$`", *p))
                sb_putc(&sb, '\\');
            sb_putc(&sb, *p);
        } else {
            bool leading_tilde = p == text && *p == '~';
            if (!leading_tilde &&
                strchr(" \t\n\\'\"$`&|;<>()*?[]#{}~", *p))
                sb_putc(&sb, '\\');
            sb_putc(&sb, *p);
        }
    }
    return sb_take(&sb);
}

char *complete_common_prefix(const StrVec *items)
{
    if (items->len == 0)
        return xstrdup("");
    const char *first = items->items[0];
    size_t len = strlen(first);
    for (size_t i = 1; i < items->len; i++) {
        size_t k = 0;
        while (k < len && items->items[i][k] == first[k])
            k++;
        len = k;
    }
    /* do not cut a multi-byte character in half */
    while (len > 0 && ((unsigned char)first[len] & 0xC0) == 0x80)
        len--;
    return xstrndup(first, len);
}

/* ------------------------------------------------------------------ */
/* Candidate sources                                                   */
/* ------------------------------------------------------------------ */

static bool has_prefix(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void add_commands(const char *prefix, StrVec *out)
{
    for (size_t i = 0; i < builtin_count(); i++)
        if (has_prefix(builtin_at(i)->name, prefix))
            sv_push_copy(out, builtin_at(i)->name);
    for (size_t i = 0; i < alias_count(); i++)
        if (has_prefix(alias_name(i), prefix))
            sv_push_copy(out, alias_name(i));
    path_commands(prefix, out);
}

static void add_variables(const char *prefix, const char *sigil,
                          const char *suffix, StrVec *out)
{
    StrVec names;
    sv_init(&names);
    var_names(&names);
    for (size_t i = 0; i < names.len; i++) {
        if (!has_prefix(names.items[i], prefix))
            continue;
        StrBuf sb;
        sb_init(&sb);
        sb_puts(&sb, sigil);
        sb_puts(&sb, names.items[i]);
        sb_puts(&sb, suffix);
        sv_push(out, sb_take(&sb));
    }
    sv_free(&names);
}

typedef enum { FILES_ANY, FILES_DIRS, FILES_EXEC } FileFilter;

static void add_files(const char *word, FileFilter filter, StrVec *out)
{
    if (strcmp(word, "~") == 0) {
        sv_push_copy(out, "~/");
        return;
    }
    const char *slash = strrchr(word, '/');
    char *dirpart = slash ? xstrndup(word, (size_t)(slash - word) + 1)
                          : xstrdup("");
    const char *base = slash ? slash + 1 : word;

    char *lookup;
    if (dirpart[0] == '~' && dirpart[1] == '/')
        lookup = path_join(home_dir(), dirpart + 2);
    else
        lookup = xstrdup(*dirpart ? dirpart : ".");

    DIR *d = opendir(lookup);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (name[0] == '.' && base[0] != '.')
                continue; /* hidden files only when asked for */
            if (!has_prefix(name, base))
                continue;
            bool is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
                struct stat st;
                is_dir = fstatat(dirfd(d), name, &st, 0) == 0 &&
                         S_ISDIR(st.st_mode);
            }
            if (filter == FILES_DIRS && !is_dir)
                continue;
            if (filter == FILES_EXEC && !is_dir &&
                faccessat(dirfd(d), name, X_OK, 0) != 0)
                continue;
            StrBuf sb;
            sb_init(&sb);
            sb_puts(&sb, dirpart);
            sb_puts(&sb, name);
            if (is_dir)
                sb_putc(&sb, '/');
            sv_push(out, sb_take(&sb));
        }
        closedir(d);
    }
    free(lookup);
    free(dirpart);
}

/* Completions for the arguments of builtins that take names rather
 * than files. Returns false if `cmd` is not one of them. */
static bool add_builtin_args(const char *cmd, const char *word, StrVec *out)
{
    if (strcmp(cmd, "set") == 0) {
        StrVec names;
        sv_init(&names);
        settings_names(&names);
        for (size_t i = 0; i < names.len; i++)
            if (has_prefix(names.items[i], word))
                sv_push_copy(out, names.items[i]);
        sv_free(&names);
        return true;
    }
    if (strcmp(cmd, "help") == 0) {
        for (size_t i = 0; i < builtin_count(); i++)
            if (has_prefix(builtin_at(i)->name, word))
                sv_push_copy(out, builtin_at(i)->name);
        return true;
    }
    if (strcmp(cmd, "type") == 0) {
        if (*word)
            add_commands(word, out);
        return true;
    }
    if (strcmp(cmd, "unalias") == 0) {
        for (size_t i = 0; i < alias_count(); i++)
            if (has_prefix(alias_name(i), word))
                sv_push_copy(out, alias_name(i));
        return true;
    }
    if (strcmp(cmd, "unset") == 0 || strcmp(cmd, "export") == 0) {
        if (!strchr(word, '=')) {
            add_variables(word, "", "", out);
            return true;
        }
    }
    return false;
}

static bool wants_directories(const char *cmd)
{
    static const char *const names[] = { "cd", "go", "mkcd", "rmdir",
                                         "pushd" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(cmd, names[i]) == 0)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Line analysis                                                       */
/* ------------------------------------------------------------------ */

void complete_line(const char *line, size_t pos, Completion *out)
{
    sv_init(&out->items);
    out->word = NULL;
    out->start = pos;
    out->quote = 0;
    out->verbatim = false;

    bool cmd_pos = true;      /* the next word names a command */
    bool after_redir = false; /* the next word is a redirection target */
    bool open_word = false;   /* the cursor is inside/at the end of a word */
    char *cmd = NULL;         /* name of the command being typed */
    char quote = 0;
    size_t wstart = pos;
    StrBuf word;
    sb_init(&word);

    size_t i = 0;
    while (i < pos) {
        char c = line[i];
        if (c == ' ' || c == '\t') {
            i++;
            continue;
        }
        if (c == '|' || c == '&' || c == ';' || c == '\n') {
            cmd_pos = true;
            after_redir = false;
            free(cmd);
            cmd = NULL;
            i++;
            continue;
        }
        if (c == '<' || c == '>') {
            i++;
            while (i < pos &&
                   (line[i] == '<' || line[i] == '>' || line[i] == '&'))
                i++;
            after_redir = true;
            continue;
        }

        /* scan one word, removing quotes as the shell would */
        wstart = i;
        sb_clear(&word);
        quote = 0;
        bool closed = false;
        while (i < pos) {
            c = line[i];
            if (quote == '\'') {
                if (c == '\'')
                    quote = 0;
                else
                    sb_putc(&word, c);
                i++;
            } else if (quote == '"') {
                if (c == '"') {
                    quote = 0;
                    i++;
                } else if (c == '\\' && i + 1 < pos &&
                           strchr("\"\\$`", line[i + 1])) {
                    sb_putc(&word, line[i + 1]);
                    i += 2;
                } else {
                    sb_putc(&word, c);
                    i++;
                }
            } else if (c == '\\') {
                if (i + 1 < pos)
                    sb_putc(&word, line[i + 1]);
                i += 2;
            } else if (c == '\'' || c == '"') {
                quote = c;
                i++;
            } else if (strchr(" \t\n|&;<>", c)) {
                closed = true;
                break;
            } else {
                sb_putc(&word, c);
                i++;
            }
        }
        if (!closed) {
            open_word = true;
            break;
        }
        if (after_redir) {
            after_redir = false;
        } else if (cmd_pos && !is_assignment(sb_cstr(&word))) {
            free(cmd);
            cmd = xstrdup(sb_cstr(&word));
            cmd_pos = false;
        }
    }
    if (!open_word) {
        sb_clear(&word);
        wstart = pos;
        quote = 0;
    }

    const char *w = sb_cstr(&word);
    out->start = wstart;
    out->quote = quote;
    out->word = xstrdup(w);

    if (w[0] == '$' && !strchr(w, '/') && quote != '\'') {
        if (w[1] == '{')
            add_variables(w + 2, "${", "}", &out->items);
        else
            add_variables(w + 1, "$", "", &out->items);
        out->verbatim = true;
    } else if (after_redir) {
        add_files(w, FILES_ANY, &out->items);
    } else if (cmd_pos) {
        if (strchr(w, '/') || w[0] == '~')
            add_files(w, FILES_EXEC, &out->items);
        else if (*w)
            add_commands(w, &out->items);
    } else if (cmd && add_builtin_args(cmd, w, &out->items)) {
        /* handled */
    } else {
        add_files(w, cmd && wants_directories(cmd) ? FILES_DIRS : FILES_ANY,
                  &out->items);
    }

    sv_sort_unique(&out->items);
    free(cmd);
    sb_free(&word);
}
