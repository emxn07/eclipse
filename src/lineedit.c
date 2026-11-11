/* lineedit.c - the interactive line editor.
 *
 * The terminal is switched to raw mode for the duration of one line
 * and restored before the line is returned, so commands always run
 * with the user's normal terminal settings. The whole line is redrawn
 * after every key; that is simple, and fast enough to be invisible. */
#include "lineedit.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "complete.h"
#include "history.h"
#include "settings.h"
#include "shell.h"
#include "util.h"

#define OUT_FD STDERR_FILENO
#define ESC_TIMEOUT_MS 50
#define MAX_LISTED 200

enum {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_WORD_LEFT,
    KEY_WORD_RIGHT,
    KEY_KILL_WORD_BACK,
    KEY_KILL_WORD_FWD,
    KEY_PASTE_START,
    KEY_ESC,
    KEY_EOF,
    KEY_INTERRUPT,
    KEY_RESIZE,
    KEY_IGNORE
};

#define CTRL_KEY(c) ((c) & 0x1F)

typedef struct {
    StrBuf buf;         /* the line being edited */
    size_t pos;         /* cursor, as a byte offset into buf */
    const char *prompt; /* last line of the prompt */
    size_t pwidth;      /* its width in columns */
    size_t cursor_row;  /* cursor row relative to the prompt's row */
    bool at_edge;       /* cursor wrapped onto a fresh empty row */
    bool utf8;
    bool color;
    /* history navigation */
    size_t hist_index;  /* == history_count() while editing a new line */
    char *hist_saved;   /* the line being typed before navigating */
    char *hist_prefix;  /* only entries starting with this are visited */
    /* inline suggestion: what would follow buf, or NULL */
    char *suggestion;
    int pending_key;    /* a key to process before reading another */
} Editor;

static char *kill_buffer; /* last text removed with Ctrl+K/U/W */

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

static struct termios saved_termios;
static bool raw_active;

bool lineedit_supported(void)
{
    const char *term = getenv("TERM");
    return term && *term && strcmp(term, "dumb") != 0 &&
           isatty(STDIN_FILENO) && isatty(OUT_FD);
}

static bool raw_enter(void)
{
    if (shell.job_control)
        saved_termios = shell.tmodes;
    else if (tcgetattr(STDIN_FILENO, &saved_termios) != 0)
        return false;
    struct termios raw = saved_termios;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    /* TCSADRAIN keeps anything typed ahead of the prompt */
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0)
        return false;
    raw_active = true;
    return true;
}

static void raw_leave(void)
{
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_termios);
        raw_active = false;
    }
}

static void out(const char *s)
{
    (void)write_all(OUT_FD, s, strlen(s));
}

static size_t term_cols(void)
{
    struct winsize ws;
    if (ioctl(OUT_FD, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

/* Returns a byte, or -1 on end of input, -2 on timeout, -3 when
 * interrupted by SIGINT, -4 when the window was resized. */
static int read_byte(int timeout_ms)
{
    for (;;) {
        if (timeout_ms >= 0) {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            int r = poll(&pfd, 1, timeout_ms);
            if (r == 0)
                return -2;
            if (r < 0 && errno != EINTR)
                return -1;
            if (r < 0)
                continue;
        }
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1)
            return c;
        if (n == 0)
            return -1;
        if (errno == EINTR) {
            if (shell_interrupted)
                return -3;
            if (shell_winch) {
                shell_winch = 0;
                return -4;
            }
            continue;
        }
        return -1;
    }
}

static int decode_csi(const char *seq)
{
    static const struct {
        const char *seq;
        int key;
    } table[] = {
        { "A", KEY_UP },          { "B", KEY_DOWN },
        { "C", KEY_RIGHT },       { "D", KEY_LEFT },
        { "H", KEY_HOME },        { "F", KEY_END },
        { "1~", KEY_HOME },       { "7~", KEY_HOME },
        { "4~", KEY_END },        { "8~", KEY_END },
        { "3~", KEY_DELETE },     { "3;5~", KEY_KILL_WORD_FWD },
        { "1;5C", KEY_WORD_RIGHT }, { "1;3C", KEY_WORD_RIGHT },
        { "5C", KEY_WORD_RIGHT }, { "1;5D", KEY_WORD_LEFT },
        { "1;3D", KEY_WORD_LEFT }, { "5D", KEY_WORD_LEFT },
        { "200~", KEY_PASTE_START },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(seq, table[i].seq) == 0)
            return table[i].key;
    return KEY_IGNORE;
}

static int read_key(Editor *e)
{
    if (e->pending_key) {
        int k = e->pending_key;
        e->pending_key = 0;
        return k;
    }
    int c = read_byte(-1);
    if (c == -1)
        return KEY_EOF;
    if (c == -3)
        return KEY_INTERRUPT;
    if (c == -4)
        return KEY_RESIZE;
    if (c != 27)
        return c;

    int c1 = read_byte(ESC_TIMEOUT_MS);
    if (c1 < 0)
        return KEY_ESC;
    if (c1 == '[') {
        char seq[16];
        size_t n = 0;
        while (n < sizeof seq - 1) {
            int b = read_byte(ESC_TIMEOUT_MS);
            if (b < 0)
                break;
            seq[n++] = (char)b;
            if (b >= 0x40 && b <= 0x7E)
                break;
        }
        seq[n] = '\0';
        return decode_csi(seq);
    }
    if (c1 == 'O') {
        int b = read_byte(ESC_TIMEOUT_MS);
        char seq[2] = { (char)(b < 0 ? 0 : b), '\0' };
        return decode_csi(seq);
    }
    switch (c1) {
    case 'b':
        return KEY_WORD_LEFT;
    case 'f':
        return KEY_WORD_RIGHT;
    case 'd':
        return KEY_KILL_WORD_FWD;
    case 127:
    case 8:
        return KEY_KILL_WORD_BACK;
    default:
        return KEY_IGNORE;
    }
}

/* ------------------------------------------------------------------ */
/* Buffer editing                                                      */
/* ------------------------------------------------------------------ */

static void ed_insert(Editor *e, const char *s, size_t n)
{
    size_t old = e->buf.len;
    sb_putn(&e->buf, s, n); /* grows the buffer; fixed up below */
    memmove(e->buf.data + e->pos + n, e->buf.data + e->pos, old - e->pos);
    memcpy(e->buf.data + e->pos, s, n);
    e->pos += n;
}

static void ed_delete(Editor *e, size_t from, size_t to)
{
    if (from >= to)
        return;
    memmove(e->buf.data + from, e->buf.data + to, e->buf.len - to + 1);
    e->buf.len -= to - from;
    if (e->pos > to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
}

static void ed_set(Editor *e, const char *s)
{
    sb_clear(&e->buf);
    sb_puts(&e->buf, s);
    e->pos = e->buf.len;
}

static void ed_kill(Editor *e, size_t from, size_t to)
{
    if (from >= to)
        return;
    free(kill_buffer);
    kill_buffer = xstrndup(e->buf.data + from, to - from);
    ed_delete(e, from, to);
}

static bool is_cont(char c)
{
    return ((unsigned char)c & 0xC0) == 0x80;
}

static size_t prev_char(const Editor *e, size_t pos)
{
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && is_cont(e->buf.data[pos]))
        pos--;
    return pos;
}

static size_t next_char(const Editor *e, size_t pos)
{
    if (pos >= e->buf.len)
        return e->buf.len;
    pos++;
    while (pos < e->buf.len && is_cont(e->buf.data[pos]))
        pos++;
    return pos;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static size_t word_left(const Editor *e, size_t pos)
{
    while (pos > 0 && is_space(e->buf.data[pos - 1]))
        pos--;
    while (pos > 0 && !is_space(e->buf.data[pos - 1]))
        pos--;
    return pos;
}

static size_t word_right(const Editor *e, size_t pos)
{
    while (pos < e->buf.len && is_space(e->buf.data[pos]))
        pos++;
    while (pos < e->buf.len && !is_space(e->buf.data[pos]))
        pos++;
    return pos;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Appends the printable form of s[0..n) to dst (if not NULL) and
 * returns its width. Control characters are made visible so pasted
 * text can never hide what will be executed. */
static size_t render(const Editor *e, const char *s, size_t n, StrBuf *dst)
{
    size_t width = 0, i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' && e->utf8) {
            if (dst)
                sb_puts(dst, "\xE2\x8F\x8E"); /* ⏎ */
            width += 1;
            i++;
        } else if (c < 0x20 || c == 0x7F) {
            if (dst) {
                sb_putc(dst, '^');
                sb_putc(dst, (char)(c ^ 0x40));
            }
            width += 2;
            i++;
        } else {
            uint32_t cp;
            size_t k = utf8_decode(s + i, n - i, &cp);
            if (dst)
                sb_putn(dst, s + i, k);
            width += (size_t)cp_width(cp);
            i += k;
        }
    }
    return width;
}

static void refresh(Editor *e, bool show_suggestion)
{
    size_t cols = term_cols();
    StrBuf ab;
    sb_init(&ab);

    if (e->cursor_row > 0)
        sb_printf(&ab, "\033[%zuA", e->cursor_row);
    sb_puts(&ab, "\r\033[J");
    sb_puts(&ab, e->prompt);

    size_t total = e->pwidth + render(e, e->buf.data, e->buf.len, &ab);
    size_t cursor = e->pwidth + render(e, e->buf.data, e->pos, NULL);
    if (show_suggestion && e->suggestion) {
        if (e->color)
            color_parse(settings.color_suggestion, &ab);
        total += render(e, e->suggestion, strlen(e->suggestion), &ab);
        if (e->color)
            sb_puts(&ab, "\033[0m");
    }

    /* A terminal that has just filled its last column holds the cursor
     * there; step onto the next row explicitly so the arithmetic below
     * is exact. */
    if (total > 0 && total % cols == 0)
        sb_puts(&ab, "\r\n");
    size_t end_row = total / cols;
    size_t cur_row = cursor / cols, cur_col = cursor % cols;
    if (end_row > cur_row)
        sb_printf(&ab, "\033[%zuA", end_row - cur_row);
    sb_putc(&ab, '\r');
    if (cur_col > 0)
        sb_printf(&ab, "\033[%zuC", cur_col);

    e->cursor_row = cur_row;
    e->at_edge = cursor == total && total > 0 && total % cols == 0;
    (void)write_all(OUT_FD, ab.data, ab.len);
    sb_free(&ab);
}

/* Makes sure the prompt starts in column 0 even if the previous
 * command's output did not end with a newline: print a mark, pad to
 * the right edge and return. If the cursor was already in column 0 the
 * mark is erased again; otherwise it stays as a visible hint. */
static void ensure_fresh_line(const Editor *e)
{
    size_t cols = term_cols();
    StrBuf ab;
    sb_init(&ab);
    if (e->color)
        sb_puts(&ab, "\033[2m");
    sb_puts(&ab, e->utf8 ? "\xE2\x8F\x8E" : "%");
    if (e->color)
        sb_puts(&ab, "\033[0m");
    for (size_t i = 1; i < cols; i++)
        sb_putc(&ab, ' ');
    sb_puts(&ab, "\r\033[K");
    (void)write_all(OUT_FD, ab.data, ab.len);
    sb_free(&ab);
}

/* ------------------------------------------------------------------ */
/* History navigation and suggestions                                  */
/* ------------------------------------------------------------------ */

static void nav_reset(Editor *e)
{
    free(e->hist_saved);
    free(e->hist_prefix);
    e->hist_saved = NULL;
    e->hist_prefix = NULL;
    e->hist_index = history_count();
}

static void update_suggestion(Editor *e)
{
    free(e->suggestion);
    e->suggestion = NULL;
    if (!settings.autosuggest || e->hist_prefix || e->buf.len == 0 ||
        e->pos != e->buf.len)
        return;
    for (size_t i = history_count(); i-- > 0;) {
        const char *h = history_get(i);
        if (strlen(h) > e->buf.len &&
            strncmp(h, e->buf.data, e->buf.len) == 0) {
            e->suggestion = xstrdup(h + e->buf.len);
            return;
        }
    }
}

/* Called after every change to the text. */
static void edited(Editor *e)
{
    nav_reset(e);
    update_suggestion(e);
}

static void history_step(Editor *e, int direction)
{
    size_t count = history_count();
    if (!e->hist_prefix) {
        e->hist_prefix = xstrdup(e->buf.data);
        e->hist_saved = xstrdup(e->buf.data);
        e->hist_index = count;
    }
    size_t plen = strlen(e->hist_prefix);

    if (direction < 0) {
        for (size_t i = e->hist_index; i-- > 0;) {
            const char *h = history_get(i);
            if (strncmp(h, e->hist_prefix, plen) == 0 &&
                strcmp(h, e->buf.data) != 0) {
                ed_set(e, h);
                e->hist_index = i;
                return;
            }
        }
        return; /* nothing older: stay put */
    }
    for (size_t i = e->hist_index + 1; i < count; i++) {
        const char *h = history_get(i);
        if (strncmp(h, e->hist_prefix, plen) == 0 &&
            strcmp(h, e->buf.data) != 0) {
            ed_set(e, h);
            e->hist_index = i;
            return;
        }
    }
    /* past the newest entry: back to what was being typed */
    ed_set(e, e->hist_saved);
    e->hist_index = count;
}

/* Accepts the whole suggestion, or just its next word. */
static void accept_suggestion(Editor *e, bool one_word)
{
    if (!e->suggestion)
        return;
    size_t n = strlen(e->suggestion);
    if (one_word) {
        size_t k = 0;
        while (k < n && is_space(e->suggestion[k]))
            k++;
        while (k < n && !is_space(e->suggestion[k]))
            k++;
        n = k;
    }
    char *text = xstrndup(e->suggestion, n);
    e->pos = e->buf.len;
    ed_insert(e, text, n);
    free(text);
    edited(e);
}

/* ------------------------------------------------------------------ */
/* Tab completion                                                      */
/* ------------------------------------------------------------------ */

/* What to show in a listing: the last path component. */
static const char *display_name(const char *item)
{
    size_t len = strlen(item);
    if (len == 0)
        return item;
    const char *p = item + len - 1;
    if (*p == '/' && p > item)
        p--;
    while (p > item && p[-1] != '/')
        p--;
    return p;
}

static void list_candidates(Editor *e, const Completion *c)
{
    /* put the cursor at the end of the line, then start below it */
    size_t saved = e->pos;
    e->pos = e->buf.len;
    refresh(e, false);
    e->pos = saved;

    size_t shown = c->items.len < MAX_LISTED ? c->items.len : MAX_LISTED;
    size_t maxw = 0;
    for (size_t i = 0; i < shown; i++) {
        size_t w = display_width(display_name(c->items.items[i]));
        if (w > maxw)
            maxw = w;
    }
    size_t cols = term_cols();
    size_t colw = maxw + 2;
    size_t ncols = cols / colw ? cols / colw : 1;
    size_t nrows = (shown + ncols - 1) / ncols;

    StrBuf ab;
    sb_init(&ab);
    if (!e->at_edge)
        sb_puts(&ab, "\r\n");
    for (size_t r = 0; r < nrows; r++) {
        for (size_t col = 0; col < ncols; col++) {
            size_t idx = col * nrows + r;
            if (idx >= shown)
                continue;
            const char *name = display_name(c->items.items[idx]);
            StrBuf clean;
            sb_init(&clean);
            sb_put_sanitized(&clean, name);
            sb_puts(&ab, sb_cstr(&clean));
            bool last_in_row = (col + 1) * nrows + r >= shown;
            if (!last_in_row)
                for (size_t w = display_width(sb_cstr(&clean)); w < colw; w++)
                    sb_putc(&ab, ' ');
            sb_free(&clean);
        }
        sb_puts(&ab, "\r\n");
    }
    if (shown < c->items.len)
        sb_printf(&ab, "... and %zu more\r\n", c->items.len - shown);
    (void)write_all(OUT_FD, ab.data, ab.len);
    sb_free(&ab);
    e->cursor_row = 0; /* the prompt is redrawn on the row we are on */
}

static void complete(Editor *e)
{
    Completion c;
    complete_line(e->buf.data, e->pos, &c);
    if (c.items.len == 0) {
        completion_free(&c);
        return;
    }

    char *common = complete_common_prefix(&c.items);
    bool single = c.items.len == 1;
    if (!single && strlen(common) <= strlen(c.word)) {
        list_candidates(e, &c);
    } else {
        StrBuf text;
        sb_init(&text);
        if (c.quote)
            sb_putc(&text, c.quote);
        char *escaped = c.verbatim ? xstrdup(common)
                                   : complete_escape(common, c.quote);
        sb_puts(&text, escaped);
        free(escaped);
        size_t clen = strlen(common);
        bool is_dir = clen > 0 && common[clen - 1] == '/';
        if (single && !is_dir) {
            if (c.quote)
                sb_putc(&text, c.quote);
            if (e->buf.data[e->pos] != ' ')
                sb_putc(&text, ' ');
        }
        ed_delete(e, c.start, e->pos);
        e->pos = c.start;
        ed_insert(e, text.data, text.len);
        sb_free(&text);
        edited(e);
    }
    free(common);
    completion_free(&c);
}

/* ------------------------------------------------------------------ */
/* Bracketed paste                                                     */
/* ------------------------------------------------------------------ */

static void paste(Editor *e)
{
    static const char end_marker[] = "\033[201~";
    const size_t mlen = sizeof end_marker - 1;
    StrBuf raw;
    sb_init(&raw);
    for (;;) {
        int b = read_byte(-1);
        if (b == -4)
            continue;
        if (b < 0)
            break;
        sb_putc(&raw, (char)b);
        if (raw.len >= mlen &&
            memcmp(raw.data + raw.len - mlen, end_marker, mlen) == 0) {
            raw.len -= mlen;
            raw.data[raw.len] = '\0';
            break;
        }
    }
    /* Normalise line endings and drop other control characters; the
     * text is inserted, never executed, until the user presses Enter. */
    StrBuf clean;
    sb_init(&clean);
    for (size_t i = 0; i < raw.len; i++) {
        unsigned char c = (unsigned char)raw.data[i];
        if (c == '\r') {
            if (i + 1 < raw.len && raw.data[i + 1] == '\n')
                continue;
            sb_putc(&clean, '\n');
        } else if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F)) {
            sb_putc(&clean, (char)c);
        }
    }
    while (clean.len > 0 && clean.data[clean.len - 1] == '\n')
        clean.data[--clean.len] = '\0';
    if (clean.len > 0) {
        ed_insert(e, clean.data, clean.len);
        edited(e);
    }
    sb_free(&raw);
    sb_free(&clean);
}

/* ------------------------------------------------------------------ */
/* Reverse history search (Ctrl+R)                                     */
/* ------------------------------------------------------------------ */

/* Reads the rest of a UTF-8 character whose first byte is `first`. */
static size_t read_utf8(int first, char out[4])
{
    size_t need = 1;
    if ((first & 0xE0) == 0xC0)
        need = 2;
    else if ((first & 0xF0) == 0xE0)
        need = 3;
    else if ((first & 0xF8) == 0xF0)
        need = 4;
    out[0] = (char)first;
    size_t n = 1;
    while (n < need) {
        int b = read_byte(ESC_TIMEOUT_MS);
        if (b < 0)
            break;
        out[n++] = (char)b;
    }
    return n;
}

static bool search_from(Editor *e, const char *query, size_t *index)
{
    if (!*query)
        return false;
    for (size_t i = *index; i-- > 0;) {
        const char *h = history_get(i);
        if (strstr(h, query)) {
            ed_set(e, h);
            *index = i;
            return true;
        }
    }
    return false;
}

/* Returns true if the found line should be run immediately. */
static bool search(Editor *e)
{
    const char *saved_prompt = e->prompt;
    size_t saved_pwidth = e->pwidth;
    char *original = xstrdup(e->buf.data);
    StrBuf query, prompt;
    sb_init(&query);
    sb_init(&prompt);
    size_t index = history_count();
    bool found = true, accept = false, done = false;

    free(e->suggestion);
    e->suggestion = NULL;
    while (!done) {
        sb_clear(&prompt);
        sb_printf(&prompt, "(%s: '", found || !query.len ? "search"
                                                         : "no match");
        sb_put_sanitized(&prompt, sb_cstr(&query));
        sb_puts(&prompt, "') ");
        e->prompt = prompt.data;
        e->pwidth = display_width(prompt.data);
        refresh(e, false);

        int k = read_key(e);
        if (k == CTRL_KEY('R')) {
            /* same query, next older match */
            found = search_from(e, sb_cstr(&query), &index);
        } else if (k == 127 || k == 8) {
            while (query.len > 0) {
                bool cont = is_cont(query.data[query.len - 1]);
                query.data[--query.len] = '\0';
                if (!cont)
                    break;
            }
            index = history_count();
            found = search_from(e, sb_cstr(&query), &index);
            if (!query.len)
                ed_set(e, original);
        } else if (k == '\r' || k == '\n') {
            accept = true;
            done = true;
        } else if (k == CTRL_KEY('G') || k == CTRL_KEY('C') || k == KEY_ESC ||
                   k == KEY_INTERRUPT || k == KEY_EOF) {
            shell_interrupted = 0;
            ed_set(e, original);
            done = true;
        } else if (k >= 32 && k < 256 && k != 127) {
            char bytes[4];
            size_t n = read_utf8(k, bytes);
            sb_putn(&query, bytes, n);
            index = history_count();
            found = search_from(e, sb_cstr(&query), &index);
        } else if (k == KEY_RESIZE || k == KEY_IGNORE) {
            continue;
        } else {
            e->pending_key = k; /* e.g. an arrow key: keep editing */
            done = true;
        }
    }
    e->prompt = saved_prompt;
    e->pwidth = saved_pwidth;
    free(original);
    sb_free(&query);
    sb_free(&prompt);
    return accept;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

/* Plain line input for terminals that cannot do cursor movement. */
static LeResult read_plain(const char *prompt, char **line)
{
    fputs(prompt, stderr);
    fflush(stderr);
    char *buf = NULL;
    size_t cap = 0;
    errno = 0;
    ssize_t n = getline(&buf, &cap, stdin);
    if (n < 0) {
        bool interrupted = errno == EINTR && shell_interrupted;
        free(buf);
        clearerr(stdin);
        if (interrupted) {
            shell_interrupted = 0;
            fputc('\n', stderr);
            return LE_INTERRUPT;
        }
        return LE_EOF;
    }
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
    *line = buf;
    return LE_LINE;
}

LeResult lineedit_read(const char *prompt, char **line)
{
    *line = NULL;
    if (!lineedit_supported() || !raw_enter())
        return read_plain(prompt, line);

    Editor e;
    memset(&e, 0, sizeof e);
    sb_init(&e.buf);
    sb_puts(&e.buf, ""); /* allocate, so buf.data is never NULL */
    e.utf8 = locale_is_utf8();
    e.color = color_enabled(OUT_FD);
    e.hist_index = history_count();

    ensure_fresh_line(&e);

    /* Only the last line of a multi-line prompt takes part in editing. */
    const char *last_nl = strrchr(prompt, '\n');
    if (last_nl) {
        StrBuf head;
        sb_init(&head);
        for (const char *p = prompt; p <= last_nl; p++) {
            if (*p == '\n')
                sb_putc(&head, '\r');
            sb_putc(&head, *p);
        }
        (void)write_all(OUT_FD, head.data, head.len);
        sb_free(&head);
        e.prompt = last_nl + 1;
    } else {
        e.prompt = prompt;
    }
    e.pwidth = display_width(e.prompt);

    out("\033[?2004h"); /* bracketed paste on */
    refresh(&e, true);

    LeResult result = LE_LINE;
    bool done = false;
    while (!done) {
        int k = read_key(&e);
        switch (k) {
        case KEY_EOF:
            result = LE_EOF;
            done = true;
            continue;
        case KEY_INTERRUPT:
        case CTRL_KEY('C'):
            shell_interrupted = 0;
            result = LE_INTERRUPT;
            done = true;
            continue;
        case CTRL_KEY('D'):
            if (e.buf.len == 0) {
                result = LE_EOF;
                done = true;
                continue;
            }
            ed_delete(&e, e.pos, next_char(&e, e.pos));
            edited(&e);
            break;
        case '\r':
        case '\n':
            done = true;
            continue;
        case 127:
        case 8:
            if (e.pos > 0) {
                ed_delete(&e, prev_char(&e, e.pos), e.pos);
                edited(&e);
            }
            break;
        case KEY_DELETE:
            if (e.pos < e.buf.len) {
                ed_delete(&e, e.pos, next_char(&e, e.pos));
                edited(&e);
            }
            break;
        case KEY_LEFT:
        case CTRL_KEY('B'):
            e.pos = prev_char(&e, e.pos);
            break;
        case KEY_RIGHT:
        case CTRL_KEY('F'):
            if (e.pos == e.buf.len)
                accept_suggestion(&e, false);
            else
                e.pos = next_char(&e, e.pos);
            break;
        case KEY_HOME:
        case CTRL_KEY('A'):
            e.pos = 0;
            break;
        case KEY_END:
        case CTRL_KEY('E'):
            if (e.pos == e.buf.len)
                accept_suggestion(&e, false);
            else
                e.pos = e.buf.len;
            break;
        case KEY_WORD_LEFT:
            e.pos = word_left(&e, e.pos);
            break;
        case KEY_WORD_RIGHT:
            if (e.pos == e.buf.len)
                accept_suggestion(&e, true);
            else
                e.pos = word_right(&e, e.pos);
            break;
        case KEY_UP:
        case CTRL_KEY('P'):
            history_step(&e, -1);
            update_suggestion(&e);
            break;
        case KEY_DOWN:
        case CTRL_KEY('N'):
            history_step(&e, +1);
            update_suggestion(&e);
            break;
        case CTRL_KEY('K'):
            ed_kill(&e, e.pos, e.buf.len);
            edited(&e);
            break;
        case CTRL_KEY('U'):
            ed_kill(&e, 0, e.pos);
            edited(&e);
            break;
        case CTRL_KEY('W'):
        case KEY_KILL_WORD_BACK:
            ed_kill(&e, word_left(&e, e.pos), e.pos);
            edited(&e);
            break;
        case KEY_KILL_WORD_FWD:
            ed_kill(&e, e.pos, word_right(&e, e.pos));
            edited(&e);
            break;
        case CTRL_KEY('Y'):
            if (kill_buffer) {
                ed_insert(&e, kill_buffer, strlen(kill_buffer));
                edited(&e);
            }
            break;
        case CTRL_KEY('L'):
            out("\033[H\033[2J");
            e.cursor_row = 0;
            break;
        case CTRL_KEY('R'):
            if (search(&e)) {
                done = true;
                continue;
            }
            edited(&e);
            break;
        case '\t':
            complete(&e);
            break;
        case KEY_PASTE_START:
            paste(&e);
            break;
        case KEY_RESIZE:
        case KEY_ESC:
        case KEY_IGNORE:
            break;
        default:
            if (k >= 32 && k < 256) {
                char bytes[4];
                size_t n = read_utf8(k, bytes);
                ed_insert(&e, bytes, n);
                edited(&e);
            }
            break;
        }
        refresh(&e, true);
    }

    /* Leave the finished line on screen, without the suggestion, and
     * move below it. */
    e.pos = e.buf.len;
    refresh(&e, false);
    if (result == LE_INTERRUPT)
        out("^C");
    if (!e.at_edge || result == LE_INTERRUPT)
        out("\r\n");
    out("\033[?2004l"); /* bracketed paste off */
    raw_leave();

    if (result == LE_LINE)
        *line = sb_take(&e.buf);
    else
        sb_free(&e.buf);
    free(e.hist_saved);
    free(e.hist_prefix);
    free(e.suggestion);
    return result;
}

void lineedit_free(void)
{
    free(kill_buffer);
    kill_buffer = NULL;
}
