/* main.c - option parsing and the interactive read-eval loop. */
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "alias.h"
#include "builtins.h"
#include "history.h"
#include "jobs.h"
#include "lineedit.h"
#include "parser.h"
#include "prompt.h"
#include "settings.h"
#include "shell.h"
#include "util.h"
#include "vars.h"

static void usage(FILE *fp)
{
    fputs("Usage: eclipse [OPTIONS] [SCRIPT [ARGUMENTS...]]\n"
          "\n"
          "Eclipse is a friendly interactive shell.\n"
          "\n"
          "Options:\n"
          "  -c COMMAND      run COMMAND and exit\n"
          "  --norc          do not read the config file\n"
          "  --config FILE   read FILE instead of ~/.config/eclipse/config\n"
          "  -h, --help      show this help\n"
          "  -V, --version   show the version\n"
          "\n"
          "With no SCRIPT and a terminal on standard input, Eclipse starts\n"
          "interactively. Type 'help' at the prompt to see what it can do.\n",
          fp);
}

static void init_environment(void)
{
    char *cwd = getcwd(NULL, 0);
    if (cwd) {
        var_export("PWD", cwd);
        free(cwd);
    }
    long level = 0;
    const char *shlvl = getenv("SHLVL");
    if (!shlvl || !parse_long(shlvl, &level) || level < 0)
        level = 0;
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", level + 1);
    var_export("SHLVL", buf);
    var_export("ECLIPSE_VERSION", ECLIPSE_VERSION);
}

/* Reads lines until they form a complete command, prompting for more
 * while a quote is open or the line ends in an operator. Returns the
 * source text, or NULL if the input was abandoned or invalid. */
static char *read_command(void)
{
    bool color = color_enabled(STDERR_FILENO);
    char *prompt = prompt_render(settings.prompt, shell.last_status, color);
    char *line = NULL;
    LeResult r = lineedit_read(prompt, &line);
    free(prompt);

    if (r == LE_EOF) {
        fputs("exit\n", stderr);
        shell_run_string("exit", NULL, 0);
        return NULL;
    }
    if (r == LE_INTERRUPT) {
        shell.last_status = 130;
        return NULL;
    }

    /* !! and !$ */
    bool changed = false;
    char *expanded = history_expand(line, &changed);
    if (!expanded) {
        msg_error("there is no previous command for '!!' to repeat");
        free(line);
        shell.last_status = 1;
        return NULL;
    }
    free(line);
    if (changed)
        fprintf(stderr, "%s\n", expanded);

    StrBuf full;
    sb_init(&full);
    sb_puts(&full, expanded);
    free(expanded);

    for (;;) {
        List *list;
        char *err;
        ParseStatus ps = parse(sb_cstr(&full), &list, &err);
        if (ps == PARSE_OK) {
            list_free(list);
            return sb_take(&full);
        }
        if (ps == PARSE_ERROR) {
            history_add(sb_cstr(&full)); /* so it can be recalled and fixed */
            msg_error("%s", err);
            free(err);
            sb_free(&full);
            shell.last_status = 2;
            return NULL;
        }
        /* incomplete: ask for more */
        char *prompt2 =
            prompt_render(settings.prompt2, shell.last_status, color);
        char *more = NULL;
        r = lineedit_read(prompt2, &more);
        free(prompt2);
        if (r != LE_LINE) {
            if (r == LE_EOF)
                msg_error("syntax error: unexpected end of input");
            sb_free(&full);
            shell.last_status = r == LE_INTERRUPT ? 130 : 2;
            return NULL;
        }
        sb_putc(&full, '\n');
        sb_puts(&full, more);
        free(more);
    }
}

static int repl(void)
{
    while (!shell.should_exit) {
        jobs_reap();
        jobs_notify();
        fflush(stdout);
        shell_interrupted = 0;
        shell.abort_line = false;

        char *source = read_command();
        if (!source)
            continue;
        history_add(source);

        bool warned = shell.exit_warned;
        shell_run_string(source, NULL, 0);
        free(source);
        if (shell.abort_line || shell_interrupted) {
            /* the terminal echoed "^C" but no newline */
            fputc('\n', stderr);
            fflush(stderr);
        }
        if (warned && !shell.should_exit)
            shell.exit_warned = false;
    }
    return shell.exit_status;
}

static void cleanup(void)
{
    history_free();
    alias_clear();
    vars_free();
    jobs_free();
    dirstack_free();
    lineedit_free();
    settings_free();
}

int main(int argc, char **argv)
{
    const char *command = NULL;
    const char *config = NULL;
    bool norc = false;
    int i = 1;

    setlocale(LC_ALL, "");
    settings_init();
    shell.pid = getpid();
    shell.arg0 = "eclipse";

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-c") == 0) {
            if (i + 1 >= argc) {
                msg_error("-c needs a command to run");
                return 2;
            }
            command = argv[++i];
            i++;
            break;
        } else if (strcmp(a, "--norc") == 0) {
            norc = true;
        } else if (strcmp(a, "--config") == 0) {
            if (i + 1 >= argc) {
                msg_error("--config needs a file name");
                return 2;
            }
            config = argv[++i];
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("eclipse %s\n", ECLIPSE_VERSION);
            return 0;
        } else if (strcmp(a, "--") == 0) {
            i++;
            break;
        } else if (a[0] == '-' && a[1]) {
            msg_error("unknown option '%s'", a);
            usage(stderr);
            return 2;
        } else {
            break;
        }
    }

    init_environment();
    int status;

    if (command) {
        /* eclipse -c 'cmd' [name [args...]] */
        if (i < argc) {
            shell.arg0 = argv[i];
            shell.pos_args = argv + i + 1;
            shell.pos_argc = argc - i - 1;
        }
        history_init(false);
        if (config)
            shell_load_config(config);
        status = shell_run_string(command, NULL, 0);
    } else if (i < argc) {
        /* eclipse script [args...] */
        shell.arg0 = argv[i];
        shell.pos_args = argv + i + 1;
        shell.pos_argc = argc - i - 1;
        history_init(false);
        if (config)
            shell_load_config(config);
        status = shell_source_file(argv[i]);
        if (status < 0) {
            msg_error("cannot open '%s': %s", argv[i], strerror(errno));
            status = 127;
        }
    } else if (isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
        shell_init_interactive();
        if (!norc)
            shell_load_config(config);
        history_init(true); /* after the config, which may set history_size */
        status = repl();
    } else {
        /* commands piped in on standard input; read unbuffered so that
         * the programs we start can consume the rest of the stream */
        setvbuf(stdin, NULL, _IONBF, 0);
        history_init(false);
        if (config)
            shell_load_config(config);
        status = shell_run_stream(stdin, NULL);
    }

    if (shell.should_exit)
        status = shell.exit_status;
    fflush(NULL);
    cleanup();
    return status & 0xFF;
}
