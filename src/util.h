/* util.h - memory, string and messaging helpers shared by every module. */
#ifndef ECLIPSE_UTIL_H
#define ECLIPSE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- checked allocation: these never return NULL ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- growable string buffer (always NUL-terminated once written) ---- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_putc(StrBuf *sb, char c);
void sb_puts(StrBuf *sb, const char *s);
void sb_putn(StrBuf *sb, const char *s, size_t n);
void sb_printf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void sb_clear(StrBuf *sb);
void sb_free(StrBuf *sb);
/* Returns the contents ("" if nothing was written) without transferring. */
const char *sb_cstr(const StrBuf *sb);
/* Transfers ownership of the contents to the caller and resets the buffer. */
char *sb_take(StrBuf *sb);

/* ---- growable vector of owned strings ---- */
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrVec;

void sv_init(StrVec *sv);
void sv_push(StrVec *sv, char *owned);
void sv_push_copy(StrVec *sv, const char *s);
void sv_free(StrVec *sv);
void sv_sort_unique(StrVec *sv);
bool sv_contains(const StrVec *sv, const char *s);
/* Returns a NULL-terminated argv-style array and resets the vector. */
char **sv_take(StrVec *sv);
void strv_free(char **v);

/* ---- user-facing messages (stderr) ---- */
void msg_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void msg_hint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- names and paths ---- */
/* Length of the leading shell identifier in s (0 if none). */
size_t name_len(const char *s);
bool is_valid_name(const char *s);
/* True if s has the form NAME=... */
bool is_assignment(const char *s);
const char *home_dir(void);
char *path_join(const char *a, const char *b);
/* "/home/me/x" -> "~/x"; always returns a new string. */
char *tilde_abbrev(const char *path);
/* mkdir -p with the given mode; returns 0 on success. */
int mkdir_p(const char *path, unsigned mode);
bool is_directory(const char *path);
/* Finds an executable in $PATH; returns a new string or NULL. */
char *find_in_path(const char *name);

/* ---- low-level I/O ---- */
bool write_all(int fd, const void *buf, size_t n);

/* ---- text ---- */
bool locale_is_utf8(void);
/* Decodes one UTF-8 sequence; returns bytes consumed (>= 1). Invalid
 * bytes decode as U+FFFD and consume a single byte. */
size_t utf8_decode(const char *s, size_t n, uint32_t *cp);
/* Terminal columns occupied by one code point (0, 1 or 2). */
int cp_width(uint32_t cp);
/* Terminal columns occupied by s, ignoring ANSI CSI escape sequences. */
size_t display_width(const char *s);
/* Appends s to out with control characters replaced, so untrusted text
 * (directory names, branch names) cannot inject terminal escapes. */
void sb_put_sanitized(StrBuf *out, const char *s);
/* Parses a whole string as a base-10 long; false on any junk. */
bool parse_long(const char *s, long *out);

#endif
