#!/usr/bin/env python3
"""Regenerate docs/screenshot.svg from a real Eclipse session.

    make && python3 tools/screenshot.py

The script starts ./eclipse on a pseudo-terminal inside a throwaway HOME,
types a short demo, interprets the escape sequences Eclipse prints into a
grid of coloured cells (a very small terminal emulator), and writes that
grid as an SVG. Nothing in the picture is drawn by hand: change the shell
and the picture changes with it.

The prompt is configured to say "you@arch" so that no real user or host
name ends up in the image. Needs only Python 3; no third-party modules.
"""
import fcntl
import html
import os
import pty
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time
import unicodedata

COLS, ROWS = 80, 20
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ECLIPSE = os.path.join(REPO, "eclipse")
OUTPUT = os.path.join(REPO, "docs", "screenshot.svg")

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
    ("find", 0.4),  # left unfinished, to show the grey suggestion
]

# ---- colours ---------------------------------------------------------

BACKGROUND, FOREGROUND = "#16161e", "#c0caf5"
PALETTE = [
    "#414868", "#f7768e", "#9ece6a", "#e0af68",
    "#7aa2f7", "#bb9af7", "#7dcfff", "#a9b1d6",
    "#565f89", "#ff899d", "#b5e07a", "#f0c383",
    "#8db0ff", "#c7a9ff", "#a4daff", "#c0caf5",
]


def colour_256(n):
    if n < 16:
        return PALETTE[n]
    if n < 232:
        n -= 16
        levels = [0, 95, 135, 175, 215, 255]
        return "#%02x%02x%02x" % (levels[n // 36], levels[n // 6 % 6],
                                  levels[n % 6])
    grey = 8 + (n - 232) * 10
    return "#%02x%02x%02x" % (grey, grey, grey)


# ---- a very small terminal emulator ----------------------------------


def char_width(ch):
    if unicodedata.combining(ch):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


class Screen:
    """Understands what Eclipse emits: CR, LF, and CSI A B C D H J K m."""

    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.grid = [self._blank_row() for _ in range(rows)]
        self.cx = self.cy = 0
        self.pending_wrap = False
        self.style = {}
        self.partial = b""

    def _blank_row(self):
        return [(" ", {}) for _ in range(self.cols)]

    def _linefeed(self):
        if self.cy == self.rows - 1:
            self.grid.pop(0)
            self.grid.append(self._blank_row())
        else:
            self.cy += 1

    def _put(self, ch):
        width = char_width(ch)
        if width == 0:
            return
        if self.pending_wrap or self.cx + width > self.cols:
            self.cx = 0
            self._linefeed()
            self.pending_wrap = False
        self.grid[self.cy][self.cx] = (ch, dict(self.style))
        if width == 2 and self.cx + 1 < self.cols:
            self.grid[self.cy][self.cx + 1] = ("", dict(self.style))
        self.cx += width
        if self.cx >= self.cols:
            self.cx = self.cols - 1
            self.pending_wrap = True

    def _sgr(self, params):
        codes = [int(p) if p else 0 for p in params.split(";")] if params else [0]
        i = 0
        while i < len(codes):
            c = codes[i]
            if c == 0:
                self.style = {}
            elif c == 1:
                self.style["bold"] = True
            elif c == 2:
                self.style["dim"] = True
            elif c == 3:
                self.style["italic"] = True
            elif c == 4:
                self.style["underline"] = True
            elif 30 <= c <= 37:
                self.style["fg"] = PALETTE[c - 30]
            elif 90 <= c <= 97:
                self.style["fg"] = PALETTE[c - 90 + 8]
            elif c == 39:
                self.style.pop("fg", None)
            elif c == 38 and codes[i + 1:i + 2] == [5] and i + 2 < len(codes):
                self.style["fg"] = colour_256(codes[i + 2])
                i += 2
            elif c == 38 and codes[i + 1:i + 2] == [2] and i + 4 < len(codes):
                self.style["fg"] = "#%02x%02x%02x" % tuple(codes[i + 2:i + 5])
                i += 4
            i += 1

    def _csi(self, params, final):
        if params.startswith("?"):
            return  # private modes such as bracketed paste
        n = int(params) if params.isdigit() else 1
        if final == "A":
            self.cy = max(0, self.cy - n)
        elif final == "B":
            self.cy = min(self.rows - 1, self.cy + n)
        elif final == "C":
            self.cx = min(self.cols - 1, self.cx + n)
        elif final == "D":
            self.cx = max(0, self.cx - n)
        elif final == "H":
            self.cx = self.cy = 0
        elif final == "J":
            if params == "2":
                self.grid = [self._blank_row() for _ in range(self.rows)]
            else:
                for x in range(self.cx, self.cols):
                    self.grid[self.cy][x] = (" ", {})
                for y in range(self.cy + 1, self.rows):
                    self.grid[y] = self._blank_row()
        elif final == "K":
            for x in range(self.cx, self.cols):
                self.grid[self.cy][x] = (" ", {})
        elif final == "m":
            self._sgr(params)
            return
        self.pending_wrap = False

    def feed(self, data):
        data = self.partial + data
        text = data.decode("utf-8", errors="ignore")
        consumed = text.encode("utf-8")
        # keep a split multi-byte character for the next call
        self.partial = data[len(consumed):] if data.startswith(consumed) else b""
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b" and text[i + 1:i + 2] == "[":
                j = i + 2
                while j < len(text) and not "@" <= text[j] <= "~":
                    j += 1
                if j < len(text):
                    self._csi(text[i + 2:j], text[j])
                i = j + 1
                continue
            if ch == "\r":
                self.cx, self.pending_wrap = 0, False
            elif ch == "\n":
                self._linefeed()
                self.pending_wrap = False
            elif ord(ch) >= 0x20:
                self._put(ch)
            i += 1


# ---- running the demo ------------------------------------------------


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


def record():
    root = tempfile.mkdtemp(prefix="eclipse-screenshot-")
    try:
        config = stage_home(root)
        env = dict(os.environ, HOME=root, TERM="xterm-256color",
                   LC_ALL="C.UTF-8")
        for name in ("ECLIPSE_HISTORY", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                     "NO_COLOR"):
            env.pop(name, None)

        screen = Screen(COLS, ROWS)
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(root)
            os.execve(ECLIPSE, ["eclipse", "--config", config], env)
        fcntl.ioctl(fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, 0, 0))
        os.kill(pid, signal.SIGWINCH)

        def pump(seconds):
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
                screen.feed(data)

        pump(0.6)
        for keys, wait in DEMO:
            for ch in keys:
                os.write(fd, ch.encode())
                pump(0.02)
            pump(wait)

        snapshot = ([list(row) for row in screen.grid], screen.cx, screen.cy)

        # tidy up: clear the line, stop the background job, leave
        for keys in ("\x15", "kill %1\r", "exit\r"):
            os.write(fd, keys.encode())
            pump(0.3)
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
        os.waitpid(pid, 0)
        return snapshot
    finally:
        shutil.rmtree(root, ignore_errors=True)


# ---- writing the SVG -------------------------------------------------

CELL_W, CELL_H, FONT_SIZE = 8.6, 20, 14
PAD_X, PAD_Y, TITLE_H = 18, 14, 36
FONT = ("ui-monospace, 'SF Mono', 'Cascadia Mono', 'JetBrains Mono', "
        "Menlo, Consolas, 'DejaVu Sans Mono', 'Liberation Mono', monospace")


def runs(row):
    """Groups a row's cells into (start column, text, style) runs."""
    out, x = [], 0
    while x < len(row):
        ch, style = row[x]
        if ch == "":
            x += 1
            continue
        start, text, cells = x, "", 0
        while x < len(row) and row[x][1] == style and row[x][0] != "":
            text += row[x][0]
            cells += char_width(row[x][0])
            x += 1
        if text.strip():
            out.append((start, text, cells, style))
    return out


def to_svg(grid, cx, cy):
    last = max([y for y, row in enumerate(grid)
                if any(ch.strip() for ch, _ in row)] + [cy])
    rows = last + 1
    width = PAD_X * 2 + COLS * CELL_W
    height = TITLE_H + PAD_Y * 2 + rows * CELL_H

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" '
        f'height="{height:.0f}" viewBox="0 0 {width:.0f} {height:.0f}" '
        f'role="img" aria-label="An Eclipse shell session">',
        "<title>An Eclipse shell session</title>",
        f'<rect width="100%" height="100%" rx="10" fill="{BACKGROUND}"/>',
        f'<path d="M0 {TITLE_H}V10A10 10 0 0 1 10 0H{width - 10:.0f}'
        f'A10 10 0 0 1 {width:.0f} 10V{TITLE_H}Z" fill="#1f2030"/>',
        '<circle cx="22" cy="18" r="6" fill="#f7768e"/>',
        '<circle cx="42" cy="18" r="6" fill="#e0af68"/>',
        '<circle cx="62" cy="18" r="6" fill="#9ece6a"/>',
        f'<text x="{width / 2:.0f}" y="23" text-anchor="middle" '
        f'font-family="{FONT}" font-size="13" fill="#787c99">eclipse</text>',
        f'<g font-family="{FONT}" font-size="{FONT_SIZE}" '
        f'xml:space="preserve">',
    ]

    # the cursor, as a thin bar so it does not hide the suggestion
    parts.append(
        f'<rect x="{PAD_X + cx * CELL_W - 0.5:.1f}" '
        f'y="{TITLE_H + PAD_Y + cy * CELL_H + 2:.1f}" '
        f'width="2" height="{CELL_H - 3}" fill="#c0caf5"/>')

    for y in range(rows):
        baseline = TITLE_H + PAD_Y + y * CELL_H + 15
        for start, text, cells, style in runs(grid[y]):
            attrs = [
                f'x="{PAD_X + start * CELL_W:.1f}"', f'y="{baseline}"',
                f'textLength="{cells * CELL_W:.1f}"',
                'lengthAdjust="spacingAndGlyphs"',
                f'fill="{style.get("fg", FOREGROUND)}"',
            ]
            if style.get("bold"):
                attrs.append('font-weight="bold"')
            if style.get("italic"):
                attrs.append('font-style="italic"')
            if style.get("underline"):
                attrs.append('text-decoration="underline"')
            if style.get("dim"):
                attrs.append('opacity="0.6"')
            parts.append(f'<text {" ".join(attrs)}>{html.escape(text)}</text>')
    parts.append("</g></svg>")
    return "\n".join(parts) + "\n"


def main():
    if not os.access(ECLIPSE, os.X_OK):
        sys.exit("tools/screenshot.py: build ./eclipse first (run `make`)")
    grid, cx, cy = record()
    with open(OUTPUT, "w", encoding="utf-8") as f:
        f.write(to_svg(grid, cx, cy))
    print(f"wrote {os.path.relpath(OUTPUT, REPO)}")
    for row in grid:
        line = "".join(ch for ch, _ in row).rstrip()
        if line:
            print("  | " + line)


if __name__ == "__main__":
    main()
