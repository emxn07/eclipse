/* friendly.c - the human-friendly commands: go, up, back, whereami,
 * mkcd, findfile, sysinfo, recent and extract. */
#include "builtins.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <ftw.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "history.h"
#include "prompt.h"
#include "settings.h"
#include "shell.h"
#include "util.h"
#include "vars.h"

/* Joins argv[from..argc) with spaces, so `go My Folder` just works. */
static char *join_args(int argc, char **argv, int from)
{
    StrBuf sb;
    sb_init(&sb);
    for (int i = from; i < argc; i++) {
        if (i > from)
            sb_putc(&sb, ' ');
        sb_puts(&sb, argv[i]);
    }
    return sb_take(&sb);
}

/* ------------------------------------------------------------------ */
/* go, up, back, whereami, mkcd                                        */
/* ------------------------------------------------------------------ */

/* Finds a directory in `parent` whose name equals `base` ignoring
 * case. Returns its full path (new string) or NULL. */
static char *find_dir_ignoring_case(const char *parent, const char *base)
{
    DIR *d = opendir(parent);
    if (!d)
        return NULL;
    char *found = NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, base) != 0)
            continue;
        char *full = path_join(parent, e->d_name);
        if (is_directory(full)) {
            found = full;
            break;
        }
        free(full);
    }
    closedir(d);
    return found;
}

/* Works out which directory the user means by `name`. Returns a new
 * string; *guessed is set when it is not literally what was typed. */
static char *resolve_destination(const char *name, bool *guessed)
{
    *guessed = false;
    if (is_directory(name))
        return xstrdup(name);
    *guessed = true;
    if (!strchr(name, '/')) {
        char *hit = find_dir_ignoring_case(".", name);
        if (hit) {
            char *plain = xstrdup(hit + 2); /* drop "./" */
            free(hit);
            return plain;
        }
        char *in_home = path_join(home_dir(), name);
        if (is_directory(in_home))
            return in_home;
        free(in_home);
        hit = find_dir_ignoring_case(home_dir(), name);
        if (hit)
            return hit;
    }
    *guessed = false;
    return xstrdup(name); /* let chdir explain what is wrong */
}

static void announce_dir(void)
{
    const char *pwd = var_get("PWD");
    if (!pwd)
        return;
    char *shown = tilde_abbrev(pwd);
    puts(shown);
    free(shown);
}

int builtin_back(int argc, char **argv)
{
    (void)argc;
    char *dir = dirstack_pop();
    if (!dir) {
        msg_error("%s: there is no earlier directory to return to", argv[0]);
        return 1;
    }
    int rc = shell_chdir(dir, argv[0], false);
    free(dir);
    if (rc == 0)
        announce_dir();
    return rc;
}

int builtin_go(int argc, char **argv)
{
    if (argc == 1)
        return shell_chdir(home_dir(), "go", true);
    if (argc == 2 && strcmp(argv[1], "-") == 0)
        return builtin_back(1, argv);

    char *name = join_args(argc, argv, 1);
    bool guessed;
    char *dest = resolve_destination(name, &guessed);
    int rc = shell_chdir(dest, "go", true);
    if (rc == 0 && guessed)
        announce_dir(); /* say where we ended up, since we guessed */
    free(dest);
    free(name);
    return rc;
}

int builtin_up(int argc, char **argv)
{
    long levels = 1;
    if (argc > 2) {
        msg_error("up: too many arguments");
        msg_hint("usage: up [LEVELS], for example: up 2");
        return 2;
    }
    if (argc == 2 && (!parse_long(argv[1], &levels) || levels < 1 ||
                      levels > 255)) {
        msg_error("up: '%s' is not a number of levels", argv[1]);
        msg_hint("usage: up [LEVELS], for example: up 2");
        return 2;
    }
    StrBuf path;
    sb_init(&path);
    for (long i = 0; i < levels; i++)
        sb_puts(&path, i ? "/.." : "..");
    int rc = shell_chdir(sb_cstr(&path), "up", true);
    sb_free(&path);
    return rc;
}

int builtin_whereami(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        msg_error("whereami: cannot determine the current directory: %s",
                  strerror(errno));
        return 1;
    }
    fputs(cwd, stdout);
    if (isatty(STDOUT_FILENO)) {
        /* extra context for people; scripts and pipes get the bare path */
        char *branch = git_branch(cwd);
        if (branch) {
            bool color = color_enabled(STDOUT_FILENO);
            StrBuf clean;
            sb_init(&clean);
            sb_put_sanitized(&clean, branch);
            printf("%s  (git branch: %s)%s", color ? "\033[2m" : "",
                   sb_cstr(&clean), color ? "\033[0m" : "");
            sb_free(&clean);
            free(branch);
        }
    }
    putchar('\n');
    free(cwd);
    return 0;
}

int builtin_mkcd(int argc, char **argv)
{
    if (argc != 2) {
        msg_error("mkcd: expected exactly one folder name");
        msg_hint("quote the name if it contains spaces: mkcd \"My Folder\"");
        return 2;
    }
    if (mkdir_p(argv[1], 0777) != 0) {
        msg_error("mkcd: cannot create '%s': %s", argv[1], strerror(errno));
        return 1;
    }
    return shell_chdir(argv[1], "mkcd", true);
}

/* ------------------------------------------------------------------ */
/* findfile                                                            */
/* ------------------------------------------------------------------ */

/* nftw() has no context argument, so the search state is file-local. */
static struct {
    const char *pattern;
    bool is_glob;
    bool color;
    size_t matches;
} ff;

static bool path_is_pseudo_fs(const char *path)
{
    return strcmp(path, "/proc") == 0 || strcmp(path, "/sys") == 0 ||
           strcmp(path, "/dev") == 0 || strcmp(path, "/run") == 0;
}

static int findfile_visit(const char *path, const struct stat *st, int type,
                          struct FTW *info)
{
    (void)st;
    if (shell_interrupted)
        return FTW_STOP;
    const char *base = path + info->base;
    if (info->level > 0) {
        const char *hit = NULL;
        size_t hitlen = 0;
        if (ff.is_glob) {
            if (fnmatch(ff.pattern, base, FNM_CASEFOLD) == 0) {
                hit = base;
                hitlen = strlen(base);
            }
        } else if ((hit = strcasestr(base, ff.pattern)) != NULL) {
            hitlen = strlen(ff.pattern);
        }
        if (hit) {
            const char *shown = strncmp(path, "./", 2) == 0 ? path + 2 : path;
            const char *slash = (type == FTW_D || type == FTW_DNR) ? "/" : "";
            if (ff.color) {
                printf("%.*s\033[1;33m%.*s\033[0m%s%s\n",
                       (int)(hit - shown), shown, (int)hitlen, hit,
                       hit + hitlen, slash);
            } else {
                printf("%s%s\n", shown, slash);
            }
            ff.matches++;
        }
    }
    if (type == FTW_D && info->level > 0 &&
        (strcmp(base, ".git") == 0 || path_is_pseudo_fs(path)))
        return FTW_SKIP_SUBTREE;
    return FTW_CONTINUE;
}

int builtin_findfile(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || !*argv[1]) {
        msg_error("findfile: expected a name to search for");
        msg_hint("usage: findfile NAME [FOLDER]   e.g. findfile report, "
                 "findfile '*.pdf' ~/Documents");
        return 2;
    }
    const char *dir = argc == 3 ? argv[2] : ".";
    if (!is_directory(dir)) {
        msg_error("findfile: no such directory: %s", dir);
        return 1;
    }
    ff.pattern = argv[1];
    ff.is_glob = strpbrk(argv[1], "*?[") != NULL;
    ff.color = color_enabled(STDOUT_FILENO);
    ff.matches = 0;

    nftw(dir, findfile_visit, 32, FTW_PHYS | FTW_ACTIONRETVAL);
    fflush(stdout);

    if (shell_interrupted)
        return 130;
    if (ff.matches == 0) {
        msg_error("findfile: nothing matching '%s' in %s", argv[1],
                  strcmp(dir, ".") == 0 ? "this directory" : dir);
        if (!ff.is_glob)
            msg_hint("the search ignores case and matches part of a name; "
                     "wildcards work too: findfile '*.txt'");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sysinfo                                                             */
/* ------------------------------------------------------------------ */

/* Reads "KEY<sep>value" from a text file; returns a new trimmed string
 * with surrounding quotes removed, or NULL. */
static char *file_field(const char *path, const char *key, char sep)
{
    FILE *fp = fopen(path, "re");
    if (!fp)
        return NULL;
    char *line = NULL, *result = NULL;
    size_t cap = 0, klen = strlen(key);
    while (getline(&line, &cap, fp) >= 0) {
        if (strncmp(line, key, klen) != 0)
            continue;
        char *p = line + klen;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != sep)
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1]))
            len--;
        if (len >= 2 && (p[0] == '"' || p[0] == '\'') && p[len - 1] == p[0]) {
            p++;
            len -= 2;
        }
        result = xstrndup(p, len);
        break;
    }
    free(line);
    fclose(fp);
    return result;
}

static void info_line(const char *label, const char *value, bool color)
{
    StrBuf clean;
    sb_init(&clean);
    sb_put_sanitized(&clean, value);
    if (color)
        printf("  \033[1;35m%-9s\033[0m %s\n", label, sb_cstr(&clean));
    else
        printf("  %-9s %s\n", label, sb_cstr(&clean));
    sb_free(&clean);
}

static void format_uptime(double seconds, StrBuf *out)
{
    long total = (long)seconds;
    long days = total / 86400, hours = (total % 86400) / 3600,
         mins = (total % 3600) / 60;
    if (days > 0)
        sb_printf(out, "%ld day%s, ", days, days == 1 ? "" : "s");
    if (days > 0 || hours > 0)
        sb_printf(out, "%ld hour%s, ", hours, hours == 1 ? "" : "s");
    sb_printf(out, "%ld minute%s", mins, mins == 1 ? "" : "s");
}

static void format_usage(double used, double total, StrBuf *out)
{
    const double gib = 1024.0 * 1024.0 * 1024.0;
    sb_printf(out, "%.1f GiB used of %.1f GiB (%.0f%%)", used / gib,
              total / gib, total > 0 ? used * 100.0 / total : 0.0);
}

int builtin_sysinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bool color = color_enabled(STDOUT_FILENO);
    StrBuf sb;
    sb_init(&sb);

    /* heading: user@host */
    char host[256] = "localhost";
    gethostname(host, sizeof host - 1);
    host[sizeof host - 1] = '\0';
    struct passwd *pw = getpwuid(geteuid());
    const char *user = pw ? pw->pw_name : "user";
    sb_printf(&sb, "%s@%s", user, host);
    size_t width = display_width(sb_cstr(&sb));
    printf("%s%s%s\n", color ? "\033[1m" : "", sb_cstr(&sb),
           color ? "\033[0m" : "");
    for (size_t i = 0; i < width; i++)
        putchar('-');
    putchar('\n');
    sb_clear(&sb);

    char *os = file_field("/etc/os-release", "PRETTY_NAME", '=');
    info_line("OS", os ? os : "Linux", color);
    free(os);

    struct utsname un;
    if (uname(&un) == 0) {
        sb_printf(&sb, "%s %s", un.sysname, un.release);
        info_line("Kernel", sb_cstr(&sb), color);
        sb_clear(&sb);
        info_line("Machine", un.machine, color);
    }

    FILE *fp = fopen("/proc/uptime", "re");
    if (fp) {
        double up = 0;
        if (fscanf(fp, "%lf", &up) == 1) {
            format_uptime(up, &sb);
            info_line("Uptime", sb_cstr(&sb), color);
            sb_clear(&sb);
        }
        fclose(fp);
    }

    char *cpu = file_field("/proc/cpuinfo", "model name", ':');
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu)
        sb_printf(&sb, "%s (%ld thread%s)", cpu, cores, cores == 1 ? "" : "s");
    else
        sb_printf(&sb, "%ld thread%s", cores, cores == 1 ? "" : "s");
    info_line("CPU", sb_cstr(&sb), color);
    sb_clear(&sb);
    free(cpu);

    double load[3];
    if (getloadavg(load, 3) == 3) {
        sb_printf(&sb, "%.2f %.2f %.2f (1, 5, 15 min)", load[0], load[1],
                  load[2]);
        info_line("Load", sb_cstr(&sb), color);
        sb_clear(&sb);
    }

    char *total_s = file_field("/proc/meminfo", "MemTotal", ':');
    char *avail_s = file_field("/proc/meminfo", "MemAvailable", ':');
    if (total_s && avail_s) {
        double total = strtod(total_s, NULL) * 1024.0;
        double avail = strtod(avail_s, NULL) * 1024.0;
        format_usage(total - avail, total, &sb);
        info_line("Memory", sb_cstr(&sb), color);
        sb_clear(&sb);
    }
    free(total_s);
    free(avail_s);

    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        double total = (double)vfs.f_blocks * (double)vfs.f_frsize;
        double freeb = (double)vfs.f_bfree * (double)vfs.f_frsize;
        format_usage(total - freeb, total, &sb);
        info_line("Disk (/)", sb_cstr(&sb), color);
        sb_clear(&sb);
    }

    sb_printf(&sb, "Eclipse %s", ECLIPSE_VERSION);
    info_line("Shell", sb_cstr(&sb), color);
    sb_clear(&sb);

    const char *term = getenv("TERM");
    if (term && *term)
        info_line("Terminal", term, color);
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    if (desktop && *desktop)
        info_line("Desktop", desktop, color);

    sb_free(&sb);
    return 0;
}

/* ------------------------------------------------------------------ */
/* recent                                                              */
/* ------------------------------------------------------------------ */

int builtin_recent(int argc, char **argv)
{
    long want = 10;
    if (argc > 2 || (argc == 2 && (!parse_long(argv[1], &want) || want < 1))) {
        msg_error("recent: expected how many commands to show");
        msg_hint("usage: recent [COUNT], for example: recent 20");
        return 2;
    }
    StrVec seen;
    sv_init(&seen);
    for (size_t i = history_count(); i-- > 0 && seen.len < (size_t)want;) {
        const char *entry = history_get(i);
        if (strcmp(entry, "recent") == 0 || strncmp(entry, "recent ", 7) == 0)
            continue;
        if (!sv_contains(&seen, entry))
            sv_push_copy(&seen, entry);
    }
    if (seen.len == 0) {
        puts("No commands in history yet.");
    } else {
        bool color = color_enabled(STDOUT_FILENO);
        for (size_t i = 0; i < seen.len; i++)
            printf("%s%3zu%s  %s\n", color ? "\033[2m" : "", i + 1,
                   color ? "\033[0m" : "", seen.items[i]);
    }
    sv_free(&seen);
    return 0;
}

/* ------------------------------------------------------------------ */
/* extract                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *suffix;
    const char *tool[4]; /* program and leading arguments */
    const char *package; /* Arch package that provides the program */
} Extractor;

/* Longer suffixes come first so ".tar.gz" wins over ".gz". */
static const Extractor extractors[] = {
    { ".tar.gz", { "tar", "-xf", NULL }, "tar" },
    { ".tar.bz2", { "tar", "-xf", NULL }, "tar" },
    { ".tar.xz", { "tar", "-xf", NULL }, "tar" },
    { ".tar.zst", { "tar", "-xf", NULL }, "tar" },
    { ".tar.lz4", { "tar", "-xf", NULL }, "tar" },
    { ".tgz", { "tar", "-xf", NULL }, "tar" },
    { ".tbz2", { "tar", "-xf", NULL }, "tar" },
    { ".tbz", { "tar", "-xf", NULL }, "tar" },
    { ".txz", { "tar", "-xf", NULL }, "tar" },
    { ".tzst", { "tar", "-xf", NULL }, "tar" },
    { ".tar", { "tar", "-xf", NULL }, "tar" },
    { ".zip", { "unzip", "-q", NULL }, "unzip" },
    { ".jar", { "unzip", "-q", NULL }, "unzip" },
    { ".gz", { "gunzip", "-k", NULL }, "gzip" },
    { ".bz2", { "bunzip2", "-k", NULL }, "bzip2" },
    { ".xz", { "unxz", "-k", NULL }, "xz" },
    { ".lzma", { "unlzma", "-k", NULL }, "xz" },
    { ".zst", { "unzstd", "-q", NULL }, "zstd" },
    { ".7z", { "7z", "x", NULL }, "7zip" },
    { ".rar", { "unrar", "x", NULL }, "unrar" },
};

static const Extractor *find_extractor(const char *file)
{
    size_t flen = strlen(file);
    for (size_t i = 0; i < sizeof extractors / sizeof extractors[0]; i++) {
        size_t slen = strlen(extractors[i].suffix);
        if (flen > slen &&
            strcasecmp(file + flen - slen, extractors[i].suffix) == 0)
            return &extractors[i];
    }
    return NULL;
}

/* Runs a program directly (never through a shell) and waits for it. */
static int run_tool(char *const argv[])
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        msg_error("cannot start a new process: %s", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        shell_become_subshell();
        /* we wait synchronously, so the tool must not be suspendable */
        signal(SIGTSTP, SIG_IGN);
        execvp(argv[0], argv);
        msg_error("cannot run '%s': %s", argv[0], strerror(errno));
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR)
            return 1;
    }
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
}

static int extract_one(const char *file)
{
    struct stat st;
    if (stat(file, &st) != 0) {
        msg_error("extract: cannot read '%s': %s", file, strerror(errno));
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        msg_error("extract: '%s' is not a regular file", file);
        return 1;
    }
    const Extractor *ex = find_extractor(file);
    if (!ex) {
        msg_error("extract: don't know how to unpack '%s'", file);
        msg_hint("supported: .tar .tar.gz .tgz .tar.bz2 .tar.xz .tar.zst "
                 ".zip .gz .bz2 .xz .zst .7z .rar");
        return 1;
    }
    char *tool = find_in_path(ex->tool[0]);
    if (!tool) {
        msg_error("extract: '%s' is needed to unpack '%s' but is not "
                  "installed", ex->tool[0], file);
        msg_hint("install it with: sudo pacman -S %s", ex->package);
        return 127;
    }
    free(tool);

    /* A leading '-' must not be mistaken for an option. */
    char *safe = file[0] == '-' ? path_join(".", file) : xstrdup(file);
    char *argv[6];
    int n = 0;
    for (int i = 0; ex->tool[i]; i++)
        argv[n++] = (char *)ex->tool[i];
    argv[n++] = safe;
    argv[n] = NULL;

    int rc = run_tool(argv);
    if (rc == 0)
        printf("Extracted %s\n", file);
    else if (rc < 128)
        msg_error("extract: %s failed on '%s' (exit status %d)", ex->tool[0],
                  file, rc);
    free(safe);
    return rc;
}

int builtin_extract(int argc, char **argv)
{
    if (argc < 2) {
        msg_error("extract: expected an archive to unpack");
        msg_hint("usage: extract ARCHIVE...   e.g. extract photos.zip");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc && !shell_interrupted; i++) {
        int one = extract_one(argv[i]);
        if (one != 0)
            rc = one;
    }
    return rc;
}
