#!/usr/bin/env python3
"""Play the README demo in the terminal you run this from.

    make && python3 tools/demo.py

The script starts ./eclipse on a pseudo-terminal inside a throwaway HOME,
types a short demo at it and passes everything Eclipse prints straight
through to your terminal. Nothing is faked: your terminal draws the real
output of the real shell. When the demo is on screen it waits so that you
can take a screenshot (see DEVELOPMENT.md, "Regenerating the screenshot"),
then cleans up and exits.

The prompt is configured to say "you@arch" so that no real user or host
name ends up in the picture. Needs only Python 3.

Options:
    --hold SECONDS     keep the finished demo on screen this long
                       (default: until Enter is pressed)
    --ready-file PATH  create PATH once the demo is fully on screen
"""
import argparse
import fcntl
import os
import pty
import select
import shutil
import signal
import sys
import tempfile
import termios
import time
import tty

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ECLIPSE = os.path.join(REPO, "eclipse")

DEMO_CONFIG = (
    "set prompt '%{bracket}[%{user}you%{bracket}@%{host}arch "
    "%{path}%w%{bracket}]%{reset}%g %s '\n"
)

# (keys, seconds to wait afterwards)
DEMO = [
    ("go projects/eclipse\r", 0.4),
    ("gti status\r", 0.6),
    ("findfile parse\r", 0.5),
    ('ls src/*.c | wc -l && echo "all here"\r', 0.6),
    ("sleep 60 &\r", 0.4),
    ("jobs\r", 0.4),
    ("mkcd ~/notes/today\r", 0.4),
    ("back\r", 0.4),
    ("find", 0.5),  # left unfinished, to show the grey suggestion
]

# typed after the screenshot: clear the line, stop the job, leave
CLEANUP = ["\x15", "kill %1\r", "exit\r"]


def stage_home(root):
    """A small fake home directory for the demo to walk around in."""
    project = os.path.join(root, "projects", "eclipse")
    os.makedirs(os.path.join(project, ".git"))
    with open(os.path.join(project, ".git", "HEAD"), "w") as f:
        f.write("ref: refs/heads/main\n")
    shutil.copytree(os.path.join(REPO, "src"), os.path.join(project, "src"))
    for name in ("Makefile", "README.md", "LICENSE"):
        shutil.copy(os.path.join(REPO, name), project)
    config = os.path.join(root, "demo-config")
    with open(config, "w") as f:
        f.write(DEMO_CONFIG)
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--hold", type=float, default=None)
    parser.add_argument("--ready-file")
    args = parser.parse_args()

    if not os.access(ECLIPSE, os.X_OK):
        sys.exit("tools/demo.py: build ./eclipse first (run `make`)")
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        sys.exit("tools/demo.py: run this in a terminal window")

    root = tempfile.mkdtemp(prefix="eclipse-demo-")
    saved_modes = termios.tcgetattr(sys.stdin.fileno())
    pid = None
    try:
        config = stage_home(root)
        env = dict(os.environ, HOME=root)
        for name in ("ECLIPSE_HISTORY", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                     "NO_COLOR"):
            env.pop(name, None)

        # give the inner terminal the same size as the real one
        winsize = fcntl.ioctl(sys.stdout.fileno(), termios.TIOCGWINSZ,
                              b"\0" * 8)
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(root)
            os.execve(ECLIPSE, ["eclipse", "--config", config], env)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)
        os.kill(pid, signal.SIGWINCH)

        # raw mode: our terminal must pass Eclipse's output through
        # untouched and not echo anything typed into it meanwhile
        tty.setraw(sys.stdin.fileno())

        def relay(seconds):
            deadline = time.time() + seconds
            while True:
                left = deadline - time.time()
                if left <= 0:
                    return
                ready, _, _ = select.select([fd], [], [], left)
                if not ready:
                    return
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                os.write(sys.stdout.fileno(), data)

        def send(keys, wait):
            for ch in keys:
                os.write(fd, ch.encode())
                relay(0.03)
            relay(wait)

        relay(0.6)
        for keys, wait in DEMO:
            send(keys, wait)

        if args.ready_file:
            open(args.ready_file, "w").close()
        if args.hold is not None:
            relay(args.hold)
        else:
            # wait for Enter (anything typed is discarded, not echoed)
            while os.read(sys.stdin.fileno(), 1) not in (b"\r", b"\n", b""):
                pass

        for keys in CLEANUP:
            send(keys, 0.3)
    finally:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, saved_modes)
        if pid:
            try:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            except OSError:
                pass
        shutil.rmtree(root, ignore_errors=True)
        print()


if __name__ == "__main__":
    main()
