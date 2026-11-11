/* test_pty.c - interactive tests.
 *
 * Runs eclipse on a pseudo-terminal and types at it like a person
 * would: keystrokes go in, and the screen output is searched for what
 * should appear. This covers what `eclipse -c` cannot: the line editor,
 * Tab completion, history, Ctrl+C, Ctrl+Z and job control.
 *
 *   build/test_pty path/to/eclipse
 *
 * Each session gets a private HOME in a temporary directory. */
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 5000

static char eclipse_path[PATH_MAX];
static char root[PATH_MAX], home[PATH_MAX], work[PATH_MAX], config[PATH_MAX];

static int master = -1;
static pid_t child = -1;
static char screen[1 << 20]; /* everything the shell has printed */
static size_t screen_len, consumed;

static int passed, failed;

/* ---- plumbing ----------------------------------------------------- */

static void die(const char *what)
{
    perror(what);
    if (child > 0)
        kill(child, SIGKILL);
    exit(2);
}

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        continue;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *fp = fopen(path, "w");
    if (!fp)
        die(path);
    fputs(text, fp);
    fclose(fp);
}

static int rm_visit(const char *path, const struct stat *st, int type,
                    struct FTW *info)
{
    (void)st;
    (void)type;
    (void)info;
    return remove(path);
}

static void cleanup(void)
{
    if (child > 0)
        kill(child, SIGKILL);
    if (root[0])
        nftw(root, rm_visit, 16, FTW_DEPTH | FTW_PHYS);
}

static void start_shell(void)
{
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
        die("pty");
    char *slave_name = ptsname(master);
    if (!slave_name)
        die("ptsname");
    struct winsize ws = { .ws_row = 40, .ws_col = 120 };
    ioctl(master, TIOCSWINSZ, &ws);

    screen_len = consumed = 0;
    child = fork();
    if (child < 0)
        die("fork");
    if (child == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0)
            _exit(126);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2)
            close(slave);
        close(master);

        setenv("HOME", home, 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("LC_ALL", "C.UTF-8", 1);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
        unsetenv("ECLIPSE_HISTORY");
        unsetenv("NO_COLOR");
        if (chdir(work) != 0)
            _exit(126);
        execl(eclipse_path, "eclipse", "--config", config, (char *)NULL);
        _exit(127);
    }
}

/* Reads whatever the shell prints, for up to timeout_ms. */
static void pump(int timeout_ms)
{
    struct pollfd pfd = { master, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return;
    if (screen_len >= sizeof screen - 1)
        return;
    ssize_t n = read(master, screen + screen_len,
                     sizeof screen - 1 - screen_len);
    if (n > 0) {
        screen_len += (size_t)n;
        screen[screen_len] = '\0';
    }
}

/* Waits until `needle` appears in output not yet matched. Later calls
 * only look at what came after it. */
static bool expect(const char *needle)
{
    long deadline = now_ms() + TIMEOUT_MS;
    size_t nlen = strlen(needle);
    for (;;) {
        if (screen_len - consumed >= nlen) {
            char *hit = memmem(screen + consumed, screen_len - consumed,
                               needle, nlen);
            if (hit) {
                consumed = (size_t)(hit - screen) + nlen;
                return true;
            }
        }
        long left = deadline - now_ms();
        if (left <= 0)
            return false;
        pump((int)left);
    }
}

/* True if `needle` does NOT show up within `ms` milliseconds. */
static bool absent(const char *needle, int ms)
{
    long deadline = now_ms() + ms;
    for (;;) {
        if (memmem(screen + consumed, screen_len - consumed, needle,
                   strlen(needle)))
            return false;
        long left = deadline - now_ms();
        if (left <= 0)
            return true;
        pump((int)left);
    }
}

static void type(const char *keys)
{
    size_t len = strlen(keys);
    while (len > 0) {
        ssize_t n = write(master, keys, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("write to pty");
        }
        keys += n;
        len -= (size_t)n;
    }
}

/* Waits for the shell to exit; returns its status or -1 on timeout. */
static int wait_exit(void)
{
    long deadline = now_ms() + TIMEOUT_MS;
    for (;;) {
        int st;
        pid_t r = waitpid(child, &st, WNOHANG);
        if (r == child) {
            child = -1;
            close(master);
            master = -1;
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        }
        if (now_ms() > deadline)
            return -1;
        pump(50); /* keep draining so the shell never blocks on output */
    }
}

static void result(bool ok, const char *name)
{
    if (ok) {
        passed++;
        return;
    }
    failed++;
    printf("FAIL: %s\n", name);
    /* show the tail of the screen to help diagnose */
    size_t from = screen_len > 400 ? screen_len - 400 : 0;
    printf("      last output: ");
    for (size_t i = from; i < screen_len; i++) {
        unsigned char c = (unsigned char)screen[i];
        if (c == '\033')
            fputs("\\e", stdout);
        else if (c == '\r')
            fputs("\\r", stdout);
        else if (c == '\n')
            fputs("\\n", stdout);
        else if (c < 0x20)
            printf("^%c", c + 0x40);
        else
            putchar(c);
    }
    putchar('\n');
}

/* Waits for a fresh prompt so the next keys are not typed too early. */
static void prompt(void)
{
    if (!expect("ECL> "))
        result(false, "(waiting for the prompt)");
}

/* ---- the tests ---------------------------------------------------- */

static void session_one(void)
{
    start_shell();
    result(expect("ECL> "), "the prompt appears");

    /* Typed text is echoed back, so tests print something that can only
     * come from running the command: arithmetic in the output. */
    type("echo sum-$((40+2))\r");
    result(expect("sum-42"), "a command runs");
    prompt();

    /* --- Tab completion --- */
    type("wherea\t\r");
    result(expect(work), "Tab completes a command name");
    prompt();

    type("cat unique_na\t\r");
    result(expect("file-content-ok"), "Tab completes a file name");
    prompt();

    type("cat alpha_\t");
    result(expect("alpha_one.txt") && expect("alpha_two.txt"),
           "Tab lists ambiguous matches");
    type("o\t\r");
    result(expect("content-of-alpha-one"), "Tab finishes after more typing");
    prompt();

    type("cat spa\t\r");
    result(expect("spaced-content"), "Tab escapes spaces in names");
    prompt();

    type("go sub\t\r");
    prompt();
    type("whereami\r");
    result(expect("/work/subdir"), "Tab completes directories for go");
    prompt();
    type("back\r");
    prompt();

    /* --- line editing --- */
    type("cho edit-$((2+2))\001e\r"); /* Ctrl+A, then insert at the start */
    result(expect("edit-4"), "Ctrl+A moves to the start of the line");
    prompt();

    type("echo abX\177c-$((1+2))\r"); /* Backspace */
    result(expect("abc-3"), "Backspace deletes");
    prompt();

    /* Left, Left, Right, Right ends after the Z; Backspace removes it */
    type("echo left-$((3+3))Z\033[D\033[D\033[C\033[C\177\r");
    result(expect("left-6"), "arrow keys move the cursor");
    prompt();

    type("echo kill-$((4+4)) trailing junk\033b\033b\013\r"); /* Alt+B, Ctrl+K */
    result(expect("kill-8") && absent("junk\r\n", 200),
           "Alt+B and Ctrl+K edit by word");
    prompt();

    type("echo caf\303\251-$((1+1))\r");
    result(expect("caf\303\251-2"), "UTF-8 input works");
    prompt();

    type("this line is abandoned\003");
    result(expect("^C"), "Ctrl+C abandons the line");
    prompt();
    type("echo alive-$((1+1))\r");
    result(expect("alive-2"), "the shell survives Ctrl+C");
    prompt();

    /* --- multi-line input --- */
    type("echo 'first\r");
    result(expect("MORE> "), "an open quote asks for more input");
    type("second'\r");
    result(expect("first\r\nsecond"), "the continued command runs");
    prompt();

    type("echo piped-$((7+7)) |\r");
    result(expect("MORE> "), "a trailing pipe asks for more input");
    type("tr a-z A-Z\r");
    result(expect("PIPED-14"), "the continued pipeline runs");
    prompt();

    /* --- bracketed paste: nothing runs until Enter --- */
    type("\033[200~echo paste-$((3+3))\necho second-$((4+4))\033[201~");
    result(absent("paste-6", 300), "pasted text is not executed");
    type("\r");
    result(expect("paste-6") && expect("second-8"),
           "pasted lines run after Enter");
    prompt();

    /* --- signals and job control --- */
    type("sleep 30\r");
    msleep(300);
    type("\003");
    prompt();
    type("echo status-$?\r");
    result(expect("status-130"), "Ctrl+C stops the foreground command");
    prompt();

    type("sleep 31\r");
    msleep(300);
    type("\032");
    result(expect("Stopped"), "Ctrl+Z suspends the foreground command");
    prompt();
    type("jobs\r");
    result(expect("Stopped") && expect("sleep 31"), "jobs lists it");
    prompt();
    type("bg\r");
    result(expect("sleep 31 &"), "bg resumes it in the background");
    prompt();
    type("jobs\r");
    result(expect("Running"), "jobs shows it running");
    prompt();
    type("fg\r");
    msleep(300);
    type("\003");
    prompt();
    type("echo fg-$?\r");
    result(expect("fg-130"), "fg brings it back to the foreground");
    prompt();

    type("sleep 0.2 &\r");
    result(expect("[1]"), "a background job is announced");
    prompt();
    msleep(600);
    type("\r");
    result(expect("Done"), "its completion is reported");
    prompt();

    type("sleep 32\r");
    msleep(300);
    type("\032");
    result(expect("Stopped"), "(suspending a second job)");
    prompt();
    type("exit\r");
    result(expect("stopped jobs"), "exit warns about stopped jobs");
    prompt();
    type("kill %1\r");
    prompt();
    msleep(300);
    type("\r");
    result(expect("Terminated"), "kill %1 ends a stopped job");
    prompt();

    /* a program that reads the terminal gets it, and gives it back */
    type("cat\r");
    msleep(300);
    type("typed into cat\r");
    result(expect("typed into cat\r\ntyped into cat"),
           "a foreground program can read the terminal");
    type("\004");
    prompt();

    /* --- friendly errors --- */
    type("whereamii\r");
    result(expect("did you mean whereami"), "typos get a suggestion");
    prompt();

    /* --- history --- */
    type("echo hist-$((6*7))\r");
    result(expect("hist-42"), "(running a command to recall)");
    prompt();
    type("\033[A\r");
    result(expect("hist-42"), "Up recalls the previous command");
    prompt();

    type("echo other\r");
    prompt();
    type("echo hi\033[A\r"); /* only history starting with "echo hi" */
    result(expect("hist-42"), "Up searches by the typed prefix");
    prompt();

    type("\0226*7");
    result(expect("(search: '6*7')"), "Ctrl+R starts a history search");
    type("\r");
    result(expect("hist-42"), "Enter runs the found command");
    prompt();

    type("echo bang-$((5+5))\r");
    result(expect("bang-10"), "(running a command to repeat)");
    prompt();
    type("!!\r");
    result(expect("bang-10"), "!! repeats the previous command");
    prompt();

    type(" echo private-$((9+9))\r");
    result(expect("private-18"), "(running a command typed after a space)");
    prompt();

    /* --- Ctrl+D leaves --- */
    type("\004");
    result(wait_exit() == 0, "Ctrl+D exits the shell");
}

static void session_two(void)
{
    start_shell();
    result(expect("ECL> "), "a second session starts");

    type("history\r");
    result(expect("echo hist-$((6*7))"), "history persists across sessions");
    result(absent("private", 200),
           "commands typed after a space are not recorded");
    prompt();

    /* inline suggestion from history, accepted with the Right arrow */
    type("echo hist-$((6");
    msleep(150);
    type("\033[C\r");
    result(expect("hist-42"), "Right accepts the inline suggestion");
    prompt();

    type("recent 3\r");
    result(expect("echo hist-$((6*7))"), "recent lists earlier commands");
    prompt();

    char path[PATH_MAX + 64];
    struct stat st;
    snprintf(path, sizeof path, "%s/.local/share/eclipse/history", home);
    result(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600,
           "the history file is private (mode 600)");

    type("exit 7\r");
    result(wait_exit() == 7, "exit returns the requested status");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s path/to/eclipse\n", argv[0]);
        return 2;
    }
    if (!realpath(argv[1], eclipse_path))
        die(argv[1]);

    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/eclipse-pty-XXXXXX",
             tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root))
        die("mkdtemp");
    atexit(cleanup);
    char resolved[PATH_MAX];
    if (!realpath(root, resolved))
        die("realpath");
    snprintf(root, sizeof root, "%s", resolved);

    snprintf(home, sizeof home, "%.*s/home", (int)sizeof home - 16, root);
    snprintf(work, sizeof work, "%.*s/work", (int)sizeof work - 16, root);
    snprintf(config, sizeof config, "%.*s/config", (int)sizeof config - 16,
             root);
    char subdir[PATH_MAX + 16];
    snprintf(subdir, sizeof subdir, "%s/subdir", work);
    if (mkdir(home, 0700) != 0 || mkdir(work, 0700) != 0 ||
        mkdir(subdir, 0700) != 0)
        die("mkdir");

    write_file(root, "config",
               "set prompt 'ECL> '\n"
               "set prompt2 'MORE> '\n"
               "set color off\n");
    write_file(work, "unique_name_file.txt", "file-content-ok\n");
    write_file(work, "alpha_one.txt", "content-of-alpha-one\n");
    write_file(work, "alpha_two.txt", "content-of-alpha-two\n");
    write_file(work, "spaced name.txt", "spaced-content\n");

    session_one();
    session_two();

    printf("pty tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
