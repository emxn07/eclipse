/* builtins.c - the core commands implemented inside the shell. */
#include "builtins.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "alias.h"
#include "history.h"
#include "jobs.h"
#include "settings.h"
#include "shell.h"
#include "suggest.h"
#include "util.h"
#include "vars.h"

/* ------------------------------------------------------------------ */
/* Directory changes                                                   */
/* ------------------------------------------------------------------ */

#define DIRSTACK_MAX 64

static StrVec dirstack;

static void dirstack_push(const char *dir)
{
    if (dirstack.len > 0 &&
        strcmp(dirstack.items[dirstack.len - 1], dir) == 0)
        return;
    if (dirstack.len == DIRSTACK_MAX) {
        free(dirstack.items[0]);
        memmove(dirstack.items, dirstack.items + 1,
                (dirstack.len - 1) * sizeof *dirstack.items);
        dirstack.len--;
        dirstack.items[dirstack.len] = NULL;
    }
    sv_push_copy(&dirstack, dir);
}

char *dirstack_pop(void)
{
    if (dirstack.len == 0)
        return NULL;
    char *dir = dirstack.items[--dirstack.len];
    dirstack.items[dirstack.len] = NULL;
    return dir;
}

void dirstack_free(void)
{
    sv_free(&dirstack);
}

/* Offers the closest directory name when a path does not exist. */
static void hint_similar_dir(const char *target)
{
    char *copy = xstrdup(target);
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';
    char *slash = strrchr(copy, '/');
    const char *parent, *base;
    if (slash) {
        base = slash + 1;
        *slash = '\0';
        parent = *copy ? copy : "/";
    } else {
        base = copy;
        parent = ".";
    }
    DIR *d = opendir(parent);
    if (d) {
        StrVec names;
        sv_init(&names);
        struct dirent *e;
        char *exact_ci = NULL;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char *full = path_join(parent, e->d_name);
            if (is_directory(full)) {
                if (!exact_ci && strcasecmp(e->d_name, base) == 0)
                    exact_ci = xstrdup(e->d_name);
                sv_push_copy(&names, e->d_name);
            }
            free(full);
        }
        closedir(d);
        char *best = exact_ci ? exact_ci
                              : suggest_closest(base, names.items, names.len);
        if (best) {
            if (slash)
                msg_hint("did you mean '%s/%s'?",
                         strcmp(parent, "/") == 0 ? "" : parent, best);
            else
                msg_hint("did you mean '%s'?", best);
        }
        free(best);
        sv_free(&names);
    }
    free(copy);
}

int shell_chdir(const char *target, const char *cmd, bool remember)
{
    char *old = getcwd(NULL, 0);
    if (chdir(target) != 0) {
        int e = errno;
        if (e == ENOENT) {
            msg_error("%s: no such directory: %s", cmd, target);
            hint_similar_dir(target);
        } else if (e == ENOTDIR) {
            msg_error("%s: not a directory: %s", cmd, target);
        } else if (e == EACCES) {
            msg_error("%s: permission denied: %s", cmd, target);
        } else {
            msg_error("%s: %s: %s", cmd, target, strerror(e));
        }
        free(old);
        return 1;
    }
    char *now = getcwd(NULL, 0);
    if (old) {
        var_export("OLDPWD", old);
        if (remember && (!now || strcmp(old, now) != 0))
            dirstack_push(old);
    }
    if (now)
        var_export("PWD", now);
    free(old);
    free(now);
    return 0;
}

static int builtin_cd(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "--") == 0)
        first = 2;
    if (argc - first > 1) {
        msg_error("cd: too many arguments");
        msg_hint("quote the name if it contains spaces: cd \"My Folder\"");
        return 1;
    }
    if (argc - first == 0) {
        const char *home = var_get("HOME");
        return shell_chdir(home && *home ? home : home_dir(), "cd", true);
    }
    if (strcmp(argv[first], "-") == 0) {
        const char *oldpwd = var_get("OLDPWD");
        if (!oldpwd || !*oldpwd) {
            msg_error("cd: there is no previous directory yet");
            return 1;
        }
        char *target = xstrdup(oldpwd);
        int rc = shell_chdir(target, "cd", true);
        if (rc == 0)
            puts(var_get("PWD") ? var_get("PWD") : target);
        free(target);
        return rc;
    }
    return shell_chdir(argv[first], "cd", true);
}

static int builtin_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        msg_error("pwd: cannot determine the current directory: %s",
                  strerror(errno));
        return 1;
    }
    puts(cwd);
    free(cwd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* exit, exec, source                                                  */
/* ------------------------------------------------------------------ */

static int builtin_exit(int argc, char **argv)
{
    long code = shell.last_status;
    if (argc > 2) {
        msg_error("exit: too many arguments");
        return 1;
    }
    if (argc == 2 && !parse_long(argv[1], &code)) {
        msg_error("exit: '%s' is not a number", argv[1]);
        return 2;
    }
    if (shell.interactive && !shell.exit_warned && jobs_stopped_count() > 0) {
        msg_error("there are stopped jobs");
        msg_hint("run 'jobs' to see them, or 'exit' again to leave anyway");
        shell.exit_warned = true;
        return 1;
    }
    shell.should_exit = true;
    shell.exit_status = (int)(code & 0xFF);
    return shell.exit_status;
}

static int builtin_exec(int argc, char **argv)
{
    if (argc == 1)
        return 0; /* redirections alone: exec.c keeps them in place */
    char *path = find_in_path(argv[1]);
    if (!path) {
        msg_error("exec: command not found: %s", argv[1]);
        return 127;
    }
    fflush(NULL);
    if (shell.interactive)
        shell_child_signals();
    execv(path, argv + 1);
    int e = errno;
    msg_error("exec: cannot run '%s': %s", argv[1], strerror(e));
    _exit(126);
}

static int builtin_source(int argc, char **argv)
{
    if (argc < 2) {
        msg_error("%s: expected a file name", argv[0]);
        msg_hint("usage: source FILE [ARGUMENTS...]");
        return 2;
    }
    char **saved_args = shell.pos_args;
    int saved_argc = shell.pos_argc;
    if (argc > 2) {
        shell.pos_args = argv + 2;
        shell.pos_argc = argc - 2;
    }
    int rc = shell_source_file(argv[1]);
    if (rc < 0) {
        msg_error("%s: cannot open '%s': %s", argv[0], argv[1],
                  strerror(errno));
        rc = 1;
    }
    shell.pos_args = saved_args;
    shell.pos_argc = saved_argc;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Variables                                                           */
/* ------------------------------------------------------------------ */

/* Prints s inside double quotes, escaped so it can be read back. */
static void print_dquoted(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\' || *s == '$' || *s == '`')
            putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

static int builtin_export(int argc, char **argv)
{
    extern char **environ;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-p") == 0)
        first = 2;
    if (argc - first == 0) {
        StrVec names;
        sv_init(&names);
        for (char **e = environ; e && *e; e++) {
            const char *eq = strchr(*e, '=');
            if (eq && eq != *e)
                sv_push(&names, xstrndup(*e, (size_t)(eq - *e)));
        }
        sv_sort_unique(&names);
        for (size_t i = 0; i < names.len; i++) {
            const char *v = getenv(names.items[i]);
            printf("export %s=", names.items[i]);
            print_dquoted(v ? v : "");
            putchar('\n');
        }
        sv_free(&names);
        return 0;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) {
        size_t n = name_len(argv[i]);
        if (n == 0 || (argv[i][n] != '\0' && argv[i][n] != '=')) {
            msg_error("export: '%s' is not a valid variable name", argv[i]);
            msg_hint("names use letters, digits and '_', and cannot start "
                     "with a digit");
            rc = 1;
            continue;
        }
        if (argv[i][n] == '=') {
            char *name = xstrndup(argv[i], n);
            var_export(name, argv[i] + n + 1);
            free(name);
        } else {
            var_export(argv[i], NULL);
        }
    }
    return rc;
}

static int builtin_unset(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!is_valid_name(argv[i])) {
            msg_error("unset: '%s' is not a valid variable name", argv[i]);
            rc = 1;
            continue;
        }
        var_unset(argv[i]);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Aliases                                                             */
/* ------------------------------------------------------------------ */

static void print_alias(const char *name, const char *value)
{
    printf("alias %s='", name);
    for (const char *p = value; *p; p++) {
        if (*p == '\'')
            fputs("'\\''", stdout);
        else
            putchar(*p);
    }
    puts("'");
}

static int builtin_alias(int argc, char **argv)
{
    if (argc == 1) {
        for (size_t i = 0; i < alias_count(); i++)
            print_alias(alias_name(i), alias_value(i));
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *v = alias_get(argv[i]);
            if (v) {
                print_alias(argv[i], v);
            } else {
                msg_error("alias: no alias named '%s'", argv[i]);
                msg_hint("define one with: alias %s='command'", argv[i]);
                rc = 1;
            }
            continue;
        }
        char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
        if (!alias_valid_name(name)) {
            msg_error("alias: '%s' is not a valid alias name", name);
            rc = 1;
        } else {
            alias_set(name, eq + 1);
        }
        free(name);
    }
    return rc;
}

static int builtin_unalias(int argc, char **argv)
{
    if (argc < 2) {
        msg_error("unalias: expected an alias name (or -a for all)");
        return 2;
    }
    if (strcmp(argv[1], "-a") == 0) {
        alias_clear();
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!alias_unset(argv[i])) {
            msg_error("unalias: no alias named '%s'", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

static int builtin_history(int argc, char **argv)
{
    size_t count = history_count();
    size_t show = count;
    if (argc > 2) {
        msg_error("history: too many arguments");
        return 2;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "clear") == 0) {
            history_clear();
            return 0;
        }
        long n;
        if (!parse_long(argv[1], &n) || n < 0) {
            msg_error("history: '%s' is not a number", argv[1]);
            msg_hint("usage: history [COUNT] | history -c");
            return 2;
        }
        if ((size_t)n < show)
            show = (size_t)n;
    }
    for (size_t i = count - show; i < count; i++)
        printf("%5zu  %s\n", i + 1, history_get(i));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Job control                                                         */
/* ------------------------------------------------------------------ */

static int builtin_jobs(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    jobs_list();
    return 0;
}

static int fg_bg(int argc, char **argv, bool foreground)
{
    if (!shell.job_control) {
        msg_error("%s: job control is only available in an interactive "
                  "shell", argv[0]);
        return 1;
    }
    jobs_reap();
    const char *err;
    Job *j = job_lookup(argc > 1 ? argv[1] : NULL, &err);
    if (!j) {
        msg_error("%s: %s%s%s", argv[0], err, argc > 1 ? ": " : "",
                  argc > 1 ? argv[1] : "");
        return 1;
    }
    if (job_is_completed(j)) {
        msg_error("%s: job %d has already finished", argv[0], j->id);
        return 1;
    }
    return foreground ? job_foreground(j) : job_background(j);
}

static int builtin_fg(int argc, char **argv)
{
    return fg_bg(argc, argv, true);
}

static int builtin_bg(int argc, char **argv)
{
    return fg_bg(argc, argv, false);
}

static int builtin_wait(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return jobs_wait_all();
}

/* "TERM", "SIGTERM", "term" or "15" -> signal number, or -1. */
static int parse_signal(const char *s)
{
    long n;
    if (parse_long(s, &n))
        return (n >= 0 && n < NSIG) ? (int)n : -1;
    if (strncasecmp(s, "SIG", 3) == 0)
        s += 3;
    for (int sig = 1; sig < NSIG; sig++) {
        const char *abbr = sigabbrev_np(sig);
        if (abbr && strcasecmp(abbr, s) == 0)
            return sig;
    }
    return -1;
}

static int builtin_kill(int argc, char **argv)
{
    int sig = SIGTERM;
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        for (int s = 1; s < NSIG; s++) {
            const char *abbr = sigabbrev_np(s);
            if (abbr)
                printf("%2d) SIG%s\n", s, abbr);
        }
        return 0;
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        const char *name = argv[1] + 1;
        i = 2;
        if (strcmp(argv[1], "-s") == 0) {
            if (argc < 3) {
                msg_error("kill: -s needs a signal name");
                return 2;
            }
            name = argv[2];
            i = 3;
        } else if (strcmp(argv[1], "--") == 0) {
            name = NULL;
        }
        if (name && (sig = parse_signal(name)) < 0) {
            msg_error("kill: unknown signal '%s'", name);
            msg_hint("run 'kill -l' to list signals");
            return 2;
        }
    }
    if (i >= argc) {
        msg_error("kill: expected a process id or job (such as %%1)");
        msg_hint("usage: kill [-SIGNAL] PID|%%JOB...");
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++) {
        if (argv[i][0] == '%') {
            const char *err;
            Job *j = job_lookup(argv[i], &err);
            if (!j) {
                msg_error("kill: %s: %s", err, argv[i]);
                rc = 1;
                continue;
            }
            if (job_signal(j, sig) != 0) {
                msg_error("kill: %s: %s", argv[i], strerror(errno));
                rc = 1;
            } else if (job_is_stopped(j) && (sig == SIGTERM || sig == SIGHUP)) {
                job_signal(j, SIGCONT); /* let it wake up and die */
            }
            continue;
        }
        long pid;
        if (!parse_long(argv[i], &pid)) {
            msg_error("kill: '%s' is not a process id or job", argv[i]);
            rc = 1;
            continue;
        }
        if (kill((pid_t)pid, sig) != 0) {
            msg_error("kill: %ld: %s", pid, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* type, set, help                                                     */
/* ------------------------------------------------------------------ */

static int builtin_type(int argc, char **argv)
{
    if (argc < 2) {
        msg_error("type: expected a command name");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        const char *alias = alias_get(name);
        char *path;
        if (alias) {
            printf("%s is an alias for '%s'\n", name, alias);
        } else if (builtin_find(name)) {
            printf("%s is a shell builtin\n", name);
        } else if ((path = find_in_path(name)) != NULL) {
            printf("%s is %s\n", name, path);
            free(path);
        } else {
            msg_error("type: %s: not found", name);
            rc = 1;
        }
    }
    return rc;
}

static void print_setting(const char *key, const char *value,
                          const char *help, void *ctx)
{
    (void)ctx;
    /* pad by display width: values such as the prompt symbol are not
     * ASCII, so printf's own padding would misalign them */
    printf("%-17s = %s", key, value);
    for (size_t w = display_width(value); w < 14; w++)
        putchar(' ');
    printf("  # %s\n", help);
}

static int builtin_set(int argc, char **argv)
{
    if (argc == 1) {
        settings_each(print_setting, NULL);
        return 0;
    }
    if (argv[1][0] == '-' || argv[1][0] == '+') {
        msg_error("set: shell options such as '%s' are not supported",
                  argv[1]);
        msg_hint("'set' changes Eclipse settings; run 'help set'");
        return 2;
    }

    char *key = xstrdup(argv[1]);
    StrBuf value;
    sb_init(&value);
    bool have_value = false;
    char *eq = strchr(key, '=');
    if (eq) { /* set key=value */
        *eq = '\0';
        sb_puts(&value, eq + 1);
        have_value = true;
    }
    for (int i = 2; i < argc; i++) {
        if (have_value)
            sb_putc(&value, ' ');
        sb_puts(&value, argv[i]);
        have_value = true;
    }

    int rc = 0;
    if (!have_value) {
        char *v = settings_get(key);
        if (v) {
            puts(v);
            free(v);
        } else {
            msg_error("set: unknown setting '%s'", key);
            rc = 1;
        }
    } else {
        char *err = NULL;
        if (!settings_set(key, sb_cstr(&value), &err)) {
            msg_error("set: %s", err);
            free(err);
            rc = 1;
        }
    }
    if (rc != 0) {
        StrVec names;
        sv_init(&names);
        settings_names(&names);
        if (!sv_contains(&names, key)) {
            char *best = suggest_closest(key, names.items, names.len);
            if (best)
                msg_hint("did you mean '%s'?", best);
            else
                msg_hint("run 'set' to list every setting");
            free(best);
        }
        sv_free(&names);
    }
    free(key);
    sb_free(&value);
    return rc;
}

static void help_group(BuiltinGroup group, const char *title, bool color)
{
    printf("%s%s%s\n", color ? "\033[1m" : "", title, color ? "\033[0m" : "");
    for (size_t i = 0; i < builtin_count(); i++) {
        const Builtin *b = builtin_at(i);
        if (b->group != group)
            continue;
        printf("  %s%-10s%s %s\n", color ? "\033[36m" : "", b->name,
               color ? "\033[0m" : "", b->summary);
    }
    putchar('\n');
}

static void help_set(void)
{
    puts("set                 list every setting\n"
         "set KEY             show one setting\n"
         "set KEY VALUE       change a setting\n"
         "\n"
         "Prompt format (settings 'prompt' and 'prompt2'):\n"
         "  %u  user name            %h  host name\n"
         "  %w  directory (~ short)  %W  last part of the directory\n"
         "  %d  directory (full)     %g  git branch, with prefix/suffix\n"
         "  %s  prompt symbol        %?  last exit status\n"
         "  %t  time HH:MM           %T  time HH:MM:SS\n"
         "  %j  number of jobs       %n  new line\n"
         "  %%  a literal percent sign\n"
         "  %{NAME}  switch colour: a theme colour (user, host, path, git,\n"
         "           symbol, error, bracket, suggestion), 'reset', or any\n"
         "           colour such as %{bold green}, %{208}, %{#ffaa00}\n"
         "\n"
         "Colours: black red green yellow blue magenta cyan white,\n"
         "  bright-NAME, a number 0-255, or #rrggbb; optionally preceded\n"
         "  by bold, dim, italic, underline or reverse.\n"
         "\n"
         "Example:\n"
         "  set prompt '%{path}%w%{reset}%g %s '\n"
         "  set color.path 'bold #7aa2f7'\n"
         "\n"
         "Put these lines in ~/.config/eclipse/config to keep them.");
}

static int builtin_help(int argc, char **argv)
{
    bool color = color_enabled(STDOUT_FILENO);
    if (argc > 1) {
        const Builtin *b = builtin_find(argv[1]);
        if (!b) {
            msg_error("help: '%s' is not an Eclipse command", argv[1]);
            msg_hint("it may be a program: try '%s --help' or 'man %s'",
                     argv[1], argv[1]);
            return 1;
        }
        printf("%s%s%s%s%s - %s\n", color ? "\033[1m" : "", b->name,
               color ? "\033[0m" : "", *b->usage ? " " : "", b->usage,
               b->summary);
        if (strcmp(b->name, "set") == 0) {
            putchar('\n');
            help_set();
        }
        return 0;
    }
    printf("Eclipse %s - a friendly shell\n\n", ECLIPSE_VERSION);
    help_group(GROUP_FRIENDLY, "Everyday commands", color);
    help_group(GROUP_CORE, "Shell commands", color);
    help_group(GROUP_JOBS, "Job control", color);
    puts("Run 'help COMMAND' for details. Everything else is a normal\n"
         "Linux program: pipes (|), redirection (> >> <), && || ; and &\n"
         "work as in other shells.");
    return 0;
}

/* ------------------------------------------------------------------ */
/* echo, true, false                                                   */
/* ------------------------------------------------------------------ */

/* Writes s with backslash escapes interpreted; false means "\c" was
 * seen and output must stop. */
static bool echo_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            putchar(*s);
            continue;
        }
        s++;
        switch (*s) {
        case 'n': putchar('\n'); break;
        case 't': putchar('\t'); break;
        case 'r': putchar('\r'); break;
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'e': putchar('\033'); break;
        case 'f': putchar('\f'); break;
        case 'v': putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case 'c': return false;
        case '0': {
            int v = 0, digits = 0;
            while (digits < 3 && s[1] >= '0' && s[1] <= '7') {
                v = v * 8 + (s[1] - '0');
                s++;
                digits++;
            }
            putchar(v);
            break;
        }
        case 'x': {
            int v = 0, digits = 0;
            while (digits < 2 && isxdigit((unsigned char)s[1])) {
                char c = (char)tolower((unsigned char)s[1]);
                v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
                s++;
                digits++;
            }
            if (digits == 0)
                fputs("\\x", stdout);
            else
                putchar(v);
            break;
        }
        default:
            putchar('\\');
            putchar(*s);
            break;
        }
    }
    return true;
}

static int builtin_echo(int argc, char **argv)
{
    bool newline = true, escapes = false;
    int i = 1;
    /* options must look exactly like -n, -e, -E or a mix such as -ne */
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p = argv[i] + 1;
        if (p[strspn(p, "neE")] != '\0')
            break;
        for (; *p; p++) {
            if (*p == 'n')
                newline = false;
            else
                escapes = *p == 'e';
        }
    }
    for (int first = i; i < argc; i++) {
        if (i > first)
            putchar(' ');
        if (!escapes) {
            fputs(argv[i], stdout);
        } else if (!echo_escaped(argv[i])) {
            newline = false;
            break;
        }
    }
    if (newline)
        putchar('\n');
    return ferror(stdout) ? (clearerr(stdout), 1) : 0;
}

static int builtin_true(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int builtin_false(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

static const Builtin builtins[] = {
    /* everyday */
    { "go", builtin_go, GROUP_FRIENDLY, "[FOLDER]",
      "change directory (no argument: home; '-': previous)" },
    { "up", builtin_up, GROUP_FRIENDLY, "[LEVELS]",
      "move up one directory, or several" },
    { "back", builtin_back, GROUP_FRIENDLY, "",
      "return to the directory you were in before" },
    { "whereami", builtin_whereami, GROUP_FRIENDLY, "",
      "show the current directory" },
    { "mkcd", builtin_mkcd, GROUP_FRIENDLY, "FOLDER",
      "create a directory and enter it" },
    { "findfile", builtin_findfile, GROUP_FRIENDLY, "NAME [FOLDER]",
      "search for files by name (wildcards allowed)" },
    { "extract", builtin_extract, GROUP_FRIENDLY, "ARCHIVE...",
      "unpack .tar.*, .zip, .gz, .bz2, .xz, .zst, .7z, .rar" },
    { "recent", builtin_recent, GROUP_FRIENDLY, "[COUNT]",
      "show recently used commands" },
    { "sysinfo", builtin_sysinfo, GROUP_FRIENDLY, "",
      "show information about this system" },
    /* core */
    { "cd", builtin_cd, GROUP_CORE, "[DIR]", "change directory" },
    { "pwd", builtin_pwd, GROUP_CORE, "", "print the current directory" },
    { "echo", builtin_echo, GROUP_CORE, "[-n] [-e] [TEXT...]",
      "print text" },
    { "export", builtin_export, GROUP_CORE, "[NAME[=VALUE]...]",
      "set environment variables, or list them" },
    { "unset", builtin_unset, GROUP_CORE, "NAME...", "remove variables" },
    { "alias", builtin_alias, GROUP_CORE, "[NAME[=VALUE]...]",
      "define or list aliases" },
    { "unalias", builtin_unalias, GROUP_CORE, "NAME... | -a",
      "remove aliases" },
    { "history", builtin_history, GROUP_CORE, "[COUNT] | -c",
      "show or clear the command history" },
    { "set", builtin_set, GROUP_CORE, "[KEY [VALUE]]",
      "show or change Eclipse settings" },
    { "source", builtin_source, GROUP_CORE, "FILE [ARGS...]",
      "run the commands in a file in this shell" },
    { ".", builtin_source, GROUP_CORE, "FILE [ARGS...]",
      "same as source" },
    { "type", builtin_type, GROUP_CORE, "NAME...",
      "say what a command name refers to" },
    { "exec", builtin_exec, GROUP_CORE, "COMMAND [ARGS...]",
      "replace the shell with a program" },
    { "true", builtin_true, GROUP_CORE, "", "succeed" },
    { "false", builtin_false, GROUP_CORE, "", "fail" },
    { ":", builtin_true, GROUP_CORE, "", "do nothing, successfully" },
    { "help", builtin_help, GROUP_CORE, "[COMMAND]", "show this help" },
    { "exit", builtin_exit, GROUP_CORE, "[STATUS]", "leave the shell" },
    /* jobs */
    { "jobs", builtin_jobs, GROUP_JOBS, "",
      "list background and stopped jobs" },
    { "fg", builtin_fg, GROUP_JOBS, "[%JOB]",
      "bring a job to the foreground" },
    { "bg", builtin_bg, GROUP_JOBS, "[%JOB]",
      "continue a stopped job in the background" },
    { "wait", builtin_wait, GROUP_JOBS, "",
      "wait for background jobs to finish" },
    { "kill", builtin_kill, GROUP_JOBS, "[-SIGNAL] PID|%JOB...",
      "send a signal to processes or jobs" },
};

#define N_BUILTINS (sizeof builtins / sizeof builtins[0])

const Builtin *builtin_find(const char *name)
{
    for (size_t i = 0; i < N_BUILTINS; i++)
        if (strcmp(builtins[i].name, name) == 0)
            return &builtins[i];
    return NULL;
}

size_t builtin_count(void)
{
    return N_BUILTINS;
}

const Builtin *builtin_at(size_t i)
{
    return &builtins[i];
}
