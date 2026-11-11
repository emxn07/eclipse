/* util.c - memory, string and messaging helpers shared by every module. */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

#include "settings.h"

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

static void die_oom(void)
{
    static const char msg[] = "eclipse: out of memory\n";
    (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
    _exit(2);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die_oom();
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        die_oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die_oom();
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = xmalloc(len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* StrBuf                                                              */
/* ------------------------------------------------------------------ */

void sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need < sb->len)
        die_oom(); /* overflow */
    if (need <= sb->cap)
        return;
    size_t cap = sb->cap ? sb->cap : 32;
    while (cap < need) {
        if (cap > SIZE_MAX / 2)
            die_oom();
        cap *= 2;
    }
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
}

void sb_putc(StrBuf *sb, char c)
{
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_putn(StrBuf *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    if (n)
        memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s)
{
    sb_putn(sb, s, strlen(s));
}

void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_reserve(sb, (size_t)n);
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
        sb->len += (size_t)n;
    }
    va_end(ap2);
}

void sb_clear(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb_init(sb);
}

const char *sb_cstr(const StrBuf *sb)
{
    return sb->data ? sb->data : "";
}

char *sb_take(StrBuf *sb)
{
    char *p = sb->data ? sb->data : xstrdup("");
    sb_init(sb);
    return p;
}

/* ------------------------------------------------------------------ */
/* StrVec                                                              */
/* ------------------------------------------------------------------ */

void sv_init(StrVec *sv)
{
    sv->items = NULL;
    sv->len = 0;
    sv->cap = 0;
}

void sv_push(StrVec *sv, char *owned)
{
    if (sv->len + 2 > sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 8;
        sv->items = xrealloc(sv->items, sv->cap * sizeof *sv->items);
    }
    sv->items[sv->len++] = owned;
    sv->items[sv->len] = NULL;
}

void sv_push_copy(StrVec *sv, const char *s)
{
    sv_push(sv, xstrdup(s));
}

void sv_free(StrVec *sv)
{
    for (size_t i = 0; i < sv->len; i++)
        free(sv->items[i]);
    free(sv->items);
    sv_init(sv);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void sv_sort_unique(StrVec *sv)
{
    if (sv->len < 2)
        return;
    qsort(sv->items, sv->len, sizeof *sv->items, cmp_str);
    size_t w = 1;
    for (size_t r = 1; r < sv->len; r++) {
        if (strcmp(sv->items[r], sv->items[w - 1]) == 0)
            free(sv->items[r]);
        else
            sv->items[w++] = sv->items[r];
    }
    sv->len = w;
    sv->items[w] = NULL;
}

bool sv_contains(const StrVec *sv, const char *s)
{
    for (size_t i = 0; i < sv->len; i++)
        if (strcmp(sv->items[i], s) == 0)
            return true;
    return false;
}

char **sv_take(StrVec *sv)
{
    char **v = sv->items;
    if (!v) {
        v = xmalloc(sizeof *v);
        v[0] = NULL;
    }
    sv_init(sv);
    return v;
}

void strv_free(char **v)
{
    if (!v)
        return;
    for (size_t i = 0; v[i]; i++)
        free(v[i]);
    free(v);
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

void msg_error(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    if (color_enabled(STDERR_FILENO))
        fputs("\033[1;31meclipse:\033[0m ", stderr);
    else
        fputs("eclipse: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void msg_hint(const char *fmt, ...)
{
    va_list ap;
    bool color = color_enabled(STDERR_FILENO);
    fputs(color ? "\033[2m  hint: " : "  hint: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs(color ? "\033[0m\n" : "\n", stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Names and paths                                                     */
/* ------------------------------------------------------------------ */

size_t name_len(const char *s)
{
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    size_t i = 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '_')
        i++;
    return i;
}

bool is_valid_name(const char *s)
{
    size_t n = name_len(s);
    return n > 0 && s[n] == '\0';
}

bool is_assignment(const char *s)
{
    size_t n = name_len(s);
    return n > 0 && s[n] == '=';
}

const char *home_dir(void)
{
    const char *h = getenv("HOME");
    if (h && *h)
        return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *pw->pw_dir)
        return pw->pw_dir;
    return "/";
}

char *path_join(const char *a, const char *b)
{
    StrBuf sb;
    sb_init(&sb);
    sb_puts(&sb, a);
    if (sb.len == 0 || sb.data[sb.len - 1] != '/')
        sb_putc(&sb, '/');
    while (*b == '/')
        b++;
    sb_puts(&sb, b);
    return sb_take(&sb);
}

char *tilde_abbrev(const char *path)
{
    const char *home = home_dir();
    size_t hl = strlen(home);
    if (hl > 1 && strncmp(path, home, hl) == 0 &&
        (path[hl] == '/' || path[hl] == '\0')) {
        StrBuf sb;
        sb_init(&sb);
        sb_putc(&sb, '~');
        sb_puts(&sb, path + hl);
        return sb_take(&sb);
    }
    return xstrdup(path);
}

int mkdir_p(const char *path, unsigned mode)
{
    char *copy = xstrdup(path);
    int rc = 0;
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST) {
            rc = -1;
            break;
        }
        *p = '/';
    }
    if (rc == 0 && mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST)
        rc = -1;
    if (rc == 0 && !is_directory(copy)) {
        errno = ENOTDIR;
        rc = -1;
    }
    int saved = errno;
    free(copy);
    errno = saved;
    return rc;
}

bool is_directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_executable_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           access(path, X_OK) == 0;
}

char *find_in_path(const char *name)
{
    if (!*name)
        return NULL;
    if (strchr(name, '/'))
        return is_executable_file(name) ? xstrdup(name) : NULL;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    const char *p = path;
    for (;;) {
        const char *end = strchrnul(p, ':');
        char *dir = end == p ? xstrdup(".") : xstrndup(p, (size_t)(end - p));
        char *full = path_join(dir, name);
        free(dir);
        if (is_executable_file(full))
            return full;
        free(full);
        if (!*end)
            break;
        p = end + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

bool write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

bool locale_is_utf8(void)
{
    const char *cs = nl_langinfo(CODESET);
    return cs && (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "UTF8") == 0);
}

size_t utf8_decode(const char *s, size_t n, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (n == 0) {
        *cp = 0;
        return 0;
    }
    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    size_t need;
    uint32_t c, min;
    if ((u[0] & 0xE0) == 0xC0) {
        need = 2;
        c = u[0] & 0x1F;
        min = 0x80;
    } else if ((u[0] & 0xF0) == 0xE0) {
        need = 3;
        c = u[0] & 0x0F;
        min = 0x800;
    } else if ((u[0] & 0xF8) == 0xF0) {
        need = 4;
        c = u[0] & 0x07;
        min = 0x10000;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if (n < need) {
        *cp = 0xFFFD;
        return 1;
    }
    for (size_t i = 1; i < need; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        c = (c << 6) | (u[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = c;
    return need;
}

int cp_width(uint32_t cp)
{
    if (cp < 0x20 || cp == 0x7F)
        return 0;
    if (cp < 0x7F)
        return 1;
    int w = wcwidth((wchar_t)cp);
    /* Unknown to the locale (or a non-UTF-8 locale): assume one column. */
    return w < 0 ? 1 : w;
}

size_t display_width(const char *s)
{
    size_t n = strlen(s), i = 0, width = 0;
    while (i < n) {
        if (s[i] == '\033') {
            if (i + 1 < n && s[i + 1] == '[') {
                /* CSI: parameters then a final byte in 0x40..0x7E */
                i += 2;
                while (i < n && !((unsigned char)s[i] >= 0x40 &&
                                  (unsigned char)s[i] <= 0x7E))
                    i++;
                if (i < n)
                    i++;
            } else {
                i++;
            }
            continue;
        }
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        width += (size_t)cp_width(cp);
    }
    return width;
}

void sb_put_sanitized(StrBuf *out, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        sb_putc(out, (c < 0x20 || c == 0x7F) ? '?' : (char)c);
    }
}

bool parse_long(const char *s, long *out)
{
    if (!s || !*s)
        return false;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0')
        return false;
    *out = v;
    return true;
}
