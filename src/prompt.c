/* prompt.c - renders the prompt from a format string. */
#include "prompt.h"

#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "jobs.h"
#include "settings.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* Git                                                                 */
/* ------------------------------------------------------------------ */

/* First line of a small file, without the newline; NULL on failure. */
static char *read_first_line(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return NULL;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return xstrdup(buf);
}

static char *branch_from_head(const char *head_path)
{
    char *line = read_first_line(head_path);
    if (!line)
        return NULL;
    char *result = NULL;
    if (strncmp(line, "ref: ", 5) == 0) {
        const char *ref = line + 5;
        if (strncmp(ref, "refs/heads/", 11) == 0)
            ref += 11;
        if (*ref)
            result = xstrdup(ref);
    } else {
        size_t hex = 0;
        while (isxdigit((unsigned char)line[hex]))
            hex++;
        if (hex >= 7)
            result = xstrndup(line, 7); /* detached HEAD */
    }
    free(line);
    return result;
}

char *git_branch(const char *dir)
{
    if (dir[0] != '/')
        return NULL;
    char *path = xstrdup(dir);
    char *result = NULL;

    for (;;) {
        char *dotgit = path_join(path, ".git");
        struct stat st;
        if (stat(dotgit, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                char *head = path_join(dotgit, "HEAD");
                result = branch_from_head(head);
                free(head);
            } else if (S_ISREG(st.st_mode)) {
                /* worktrees and submodules: ".git" is a pointer file */
                char *line = read_first_line(dotgit);
                if (line && strncmp(line, "gitdir: ", 8) == 0) {
                    const char *gd = line + 8;
                    char *gitdir = gd[0] == '/' ? xstrdup(gd)
                                                : path_join(path, gd);
                    char *head = path_join(gitdir, "HEAD");
                    result = branch_from_head(head);
                    free(head);
                    free(gitdir);
                }
                free(line);
            }
            free(dotgit);
            break;
        }
        free(dotgit);

        char *slash = strrchr(path, '/');
        if (!slash || path[1] == '\0')
            break; /* reached the root */
        if (slash == path)
            path[1] = '\0';
        else
            *slash = '\0';
    }
    free(path);
    return result;
}

/* ------------------------------------------------------------------ */
/* Pieces                                                              */
/* ------------------------------------------------------------------ */

static const char *user_name(void)
{
    static char *cached;
    if (!cached) {
        struct passwd *pw = getpwuid(geteuid());
        const char *env = getenv("USER");
        cached = xstrdup(pw && pw->pw_name ? pw->pw_name
                                           : (env && *env ? env : "user"));
    }
    return cached;
}

static const char *host_name(void)
{
    static char cached[256];
    if (!cached[0]) {
        if (gethostname(cached, sizeof cached - 1) != 0 || !cached[0])
            strcpy(cached, "localhost");
        cached[sizeof cached - 1] = '\0';
    }
    return cached;
}

/* Emits the escape sequence for %{spec}. */
static void put_color(StrBuf *out, const char *spec, bool color)
{
    if (!color)
        return;
    static const struct {
        const char *name;
        char **value;
    } theme[] = {
        { "user", &settings.color_user },
        { "host", &settings.color_host },
        { "path", &settings.color_path },
        { "git", &settings.color_git },
        { "symbol", &settings.color_symbol },
        { "error", &settings.color_error },
        { "bracket", &settings.color_bracket },
        { "suggestion", &settings.color_suggestion },
    };
    if (strcmp(spec, "reset") == 0) {
        sb_puts(out, "\033[0m");
        return;
    }
    for (size_t i = 0; i < sizeof theme / sizeof theme[0]; i++) {
        if (strcmp(spec, theme[i].name) == 0) {
            /* reset first so attributes such as bold do not leak */
            sb_puts(out, "\033[0m");
            color_parse(*theme[i].value, out);
            return;
        }
    }
    color_parse(spec, out); /* unknown specs simply produce nothing */
}

char *prompt_render(const char *format, int last_status, bool color)
{
    StrBuf out;
    sb_init(&out);
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        const char *pwd = getenv("PWD");
        cwd = xstrdup(pwd && *pwd ? pwd : "?");
    }

    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            sb_putc(&out, *p);
            continue;
        }
        p++;
        switch (*p) {
        case '\0':
            sb_putc(&out, '%');
            p--;
            break;
        case '%':
            sb_putc(&out, '%');
            break;
        case 'u':
            sb_put_sanitized(&out, user_name());
            break;
        case 'h': {
            const char *h = host_name();
            char *shortname = xstrndup(h, strcspn(h, "."));
            sb_put_sanitized(&out, shortname);
            free(shortname);
            break;
        }
        case 'H':
            sb_put_sanitized(&out, host_name());
            break;
        case 'w': {
            char *abbr = tilde_abbrev(cwd);
            sb_put_sanitized(&out, abbr);
            free(abbr);
            break;
        }
        case 'W': {
            char *abbr = tilde_abbrev(cwd);
            const char *slash = strrchr(abbr, '/');
            sb_put_sanitized(&out, (slash && slash[1]) ? slash + 1 : abbr);
            free(abbr);
            break;
        }
        case 'd':
            sb_put_sanitized(&out, cwd);
            break;
        case 'g': {
            char *branch = git_branch(cwd);
            if (branch) {
                put_color(&out, "git", color);
                sb_puts(&out, settings.git_prefix);
                sb_put_sanitized(&out, branch);
                sb_puts(&out, settings.git_suffix);
                put_color(&out, "reset", color);
                free(branch);
            }
            break;
        }
        case 's':
            put_color(&out, last_status == 0 ? "symbol" : "error", color);
            sb_puts(&out, settings.symbol);
            put_color(&out, "reset", color);
            break;
        case '?':
            sb_printf(&out, "%d", last_status);
            break;
        case 't':
        case 'T': {
            time_t now = time(NULL);
            struct tm tm;
            char buf[16];
            if (localtime_r(&now, &tm) &&
                strftime(buf, sizeof buf, *p == 't' ? "%H:%M" : "%H:%M:%S",
                         &tm) > 0)
                sb_puts(&out, buf);
            break;
        }
        case 'j':
            sb_printf(&out, "%zu", jobs_count());
            break;
        case 'n':
            sb_putc(&out, '\n');
            break;
        case '{': {
            const char *close = strchr(p, '}');
            if (!close) {
                sb_puts(&out, "%{");
                break;
            }
            char *spec = xstrndup(p + 1, (size_t)(close - p - 1));
            put_color(&out, spec, color);
            free(spec);
            p = close;
            break;
        }
        default:
            sb_putc(&out, '%');
            sb_putc(&out, *p);
            break;
        }
    }
    free(cwd);
    return sb_take(&out);
}
