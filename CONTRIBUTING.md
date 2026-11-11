# Contributing to Eclipse

Thanks for your interest in improving Eclipse. This page explains how to
build it, test it and send a change.

## Reporting bugs and asking for features

Open an issue using the bug report or feature request template. For bugs,
the most useful things are:

- the exact command line you typed,
- what you expected and what happened instead,
- the output of `eclipse --version`, and your terminal emulator and `$TERM`
  if the problem is on screen.

If a command behaves differently in Eclipse and in bash, showing both
outputs makes the problem quick to pin down.

**Security problems** (for example, a way to make Eclipse run something
the user did not type) should not go in a public issue. Use GitHub's
private "Report a vulnerability" button on the repository's Security tab.

## Building

You need a C17 compiler and `make`. On Arch Linux:

```sh
sudo pacman -S --needed base-devel
make            # builds ./eclipse
make test       # runs the unit, integration and pty test suites
```

Useful targets:

| Target | Purpose |
|---|---|
| `make` | Build `./eclipse` |
| `make test` | Run every test suite |
| `make test-unit`, `make test-integration`, `make test-pty` | Run one suite |
| `make debug` | Build `build-debug/eclipse-debug` with AddressSanitizer and UBSan |
| `make install` | Install to `~/.local` (set `PREFIX` to change) |
| `make clean` | Remove build output |

To try your build without installing it, run `./eclipse --norc`.

## Making a change

1. Read [DEVELOPMENT.md](DEVELOPMENT.md). It describes each module, the
   rules the code relies on, and how to add a built-in command or setting.
2. Keep the change focused. One fix or feature per pull request.
3. Add or update tests. A bug fix should come with a test that failed
   before the fix:
   - logic inside a module → `tests/test_unit.c`
   - behaviour of a command line → `tests/integration.sh`
   - anything involving keys, the prompt or job control → `tests/test_pty.c`
4. Run `make test`. The build must stay free of compiler warnings.
5. For changes to memory handling, also run the integration suite against
   the sanitizer build:

   ```sh
   make debug
   sh tests/integration.sh build-debug/eclipse-debug
   ```

6. Update `README.md`, `docs/eclipse.1` and `config/config.example` if
   behaviour that users see has changed, and add a line to `CHANGELOG.md`.

## Code style

- C17, 4-space indentation, no tabs, lines up to 80 columns.
- Function braces on their own line; other braces on the same line.
- Every allocation goes through `xmalloc`, `xstrdup` and friends in
  `util.h`; build strings with `StrBuf` rather than fixed-size buffers.
- Never call `system()` or build a command line as a string. Run programs
  with an argument vector.
- Text that comes from outside (file names, branch names, pasted input)
  must pass through `sb_put_sanitized()` before it is printed as part of
  the interface.
- Error messages say what went wrong in plain words and, where possible,
  add a `msg_hint()` suggesting what to do.
- Comments explain why, not what.

## Commit messages

Write the subject in the imperative, under about 70 characters, with no
trailing full stop: `Fix cursor position after wide characters`. Use the
body to explain what the problem was and why this is the right fix.

## Licence

By contributing, you agree that your contribution is licensed under the
[MIT licence](LICENSE).
