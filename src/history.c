/* history.c - command history with persistent storage.
 *
 * The file holds one entry per line, oldest first. Newlines and
 * backslashes inside an entry are escaped so multi-line commands survive.
 * Each new command is appended with a single write(), which keeps the
 * file consistent when several shells are open at once. */
#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings.h"
#include "util.h"

static char **entries;
static size_t nentries, capentries;
static char *file_path;

char *history_encode(const char *entry)
{
    StrBuf sb;
    sb_init(&sb);
    for (const char *p = entry; *p; p++) {
        if (*p == '\n')
            sb_puts(&sb, "\\n");
        else if (*p == '\\')
            sb_puts(&sb, "\\\\");
        else
            sb_putc(&sb, *p);
    }
    return sb_take(&sb);
}

char *history_decode(const char *line)
{
    StrBuf sb;
    sb_init(&sb);
    for (const char *p = line; *p; p++) {
        if (*p == '\\' && p[1] == 'n') {
            sb_putc(&sb, '\n');
            p++;
        } else if (*p == '\\' && p[1] == '\\') {
            sb_putc(&sb, '\\');
            p++;
        } else {
            sb_putc(&sb, *p);
        }
    }
    return sb_take(&sb);
}

static void push_entry(char *owned)
{
    if (nentries == capentries) {
        capentries = capentries ? capentries * 2 : 256;
        entries = xrealloc(entries, capentries * sizeof *entries);
    }
    entries[nentries++] = owned;
}

/* Drops the oldest entries so that at most `max` remain. */
static void trim_to(size_t max)
{
    if (nentries <= max)
        return;
    size_t drop = nentries - max;
    for (size_t i = 0; i < drop; i++)
        free(entries[i]);
    memmove(entries, entries + drop, max * sizeof *entries);
    nentries = max;
}

static char *default_path(void)
{
    const char *override = getenv("ECLIPSE_HISTORY");
    if (override && *override)
        return xstrdup(override);
    const char *xdg = getenv("XDG_DATA_HOME");
    char *base = (xdg && xdg[0] == '/') ? xstrdup(xdg)
                                        : path_join(home_dir(), ".local/share");
    char *path = path_join(base, "eclipse/history");
    free(base);
    return path;
}

static void ensure_parent_dir(const char *path)
{
    char *dir = xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        (void)mkdir_p(dir, 0700);
    }
    free(dir);
}

/* Atomically replaces the file with the in-memory history. */
static void rewrite_file(void)
{
    if (!file_path)
        return;
    ensure_parent_dir(file_path);
    StrBuf tmp;
    sb_init(&tmp);
    sb_printf(&tmp, "%s.tmp.%ld", file_path, (long)getpid());
    int fd = open(tmp.data, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        sb_free(&tmp);
        return;
    }
    bool ok = true;
    StrBuf out;
    sb_init(&out);
    for (size_t i = 0; i < nentries; i++) {
        char *enc = history_encode(entries[i]);
        sb_puts(&out, enc);
        sb_putc(&out, '\n');
        free(enc);
    }
    if (out.len)
        ok = write_all(fd, out.data, out.len);
    sb_free(&out);
    if (close(fd) != 0)
        ok = false;
    if (!ok || rename(tmp.data, file_path) != 0)
        unlink(tmp.data);
    sb_free(&tmp);
}

void history_init(bool persist)
{
    history_free();
    if (!persist)
        return;
    file_path = default_path();

    FILE *fp = fopen(file_path, "re");
    if (!fp)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    size_t total = 0;
    while ((n = getline(&line, &cap, fp)) >= 0) {
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (!*line)
            continue;
        push_entry(history_decode(line));
        total++;
    }
    free(line);
    fclose(fp);

    size_t max = (size_t)settings.history_size;
    if (total > max) {
        trim_to(max);
        rewrite_file();
    }
}

void history_free(void)
{
    for (size_t i = 0; i < nentries; i++)
        free(entries[i]);
    free(entries);
    entries = NULL;
    nentries = capentries = 0;
    free(file_path);
    file_path = NULL;
}

void history_add(const char *line)
{
    if (line[0] == ' ' || settings.history_size <= 0)
        return;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == ' ' ||
                       line[len - 1] == '\t'))
        len--;
    if (len == 0)
        return;
    char *entry = xstrndup(line, len);
    if (nentries > 0 && strcmp(entries[nentries - 1], entry) == 0) {
        free(entry);
        return;
    }
    push_entry(entry);
    trim_to((size_t)settings.history_size);

    if (!file_path)
        return;
    ensure_parent_dir(file_path);
    int fd = open(file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    char *enc = history_encode(entry);
    StrBuf out;
    sb_init(&out);
    sb_puts(&out, enc);
    sb_putc(&out, '\n');
    (void)write_all(fd, out.data, out.len);
    sb_free(&out);
    free(enc);
    close(fd);
}

size_t history_count(void)
{
    return nentries;
}

const char *history_get(size_t i)
{
    return i < nentries ? entries[i] : NULL;
}

void history_clear(void)
{
    for (size_t i = 0; i < nentries; i++)
        free(entries[i]);
    nentries = 0;
    if (file_path)
        rewrite_file();
}

const char *history_file(void)
{
    return file_path;
}

char *history_expand(const char *line, bool *changed)
{
    *changed = false;
    if (!strchr(line, '!'))
        return xstrdup(line);

    const char *last = nentries ? entries[nentries - 1] : NULL;
    StrBuf out;
    sb_init(&out);
    bool in_single = false, in_double = false;

    for (const char *p = line; *p; p++) {
        if (in_single) {
            in_single = *p != '\'';
            sb_putc(&out, *p);
            continue;
        }
        if (*p == '\\' && p[1]) {
            sb_putc(&out, *p);
            sb_putc(&out, *++p);
            continue;
        }
        if (*p == '\'' && !in_double)
            in_single = true;
        else if (*p == '"')
            in_double = !in_double;
        if (*p == '!' && (p[1] == '!' || p[1] == '$')) {
            if (!last) {
                sb_free(&out);
                return NULL;
            }
            if (p[1] == '!') {
                sb_puts(&out, last);
            } else {
                size_t end = strlen(last);
                while (end > 0 && (last[end - 1] == ' ' ||
                                   last[end - 1] == '\t' ||
                                   last[end - 1] == '\n'))
                    end--;
                size_t start = end;
                while (start > 0 && last[start - 1] != ' ' &&
                       last[start - 1] != '\t' && last[start - 1] != '\n')
                    start--;
                sb_putn(&out, last + start, end - start);
            }
            *changed = true;
            p++;
            continue;
        }
        sb_putc(&out, *p);
    }
    return sb_take(&out);
}
