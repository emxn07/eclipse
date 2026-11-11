# Changelog

All notable changes to Eclipse are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0]

First release.

### Added

- Interactive shell with pipes, redirection (`<`, `>`, `>>`, `2>`, `&>`,
  `2>&1`, `<<<`), chaining (`&&`, `||`, `;`) and background jobs (`&`).
- Job control: `jobs`, `fg`, `bg`, `wait`, `kill`, Ctrl+C and Ctrl+Z.
- Variables, `export`, per-command environment, parameter forms
  (`${NAME:-default}` and friends), command substitution, arithmetic,
  tilde, wildcard and brace expansion.
- Human-friendly commands: `go`, `up`, `back`, `whereami`, `mkcd`,
  `findfile`, `extract`, `recent`, `sysinfo`.
- Line editor with UTF-8 input, inline suggestions from history, Tab
  completion, prefix history search, Ctrl+R search and bracketed paste.
- Persistent history with `!!` and `!$`.
- Customisable prompt with git branch detection and theme colours.
- Configuration file at `~/.config/eclipse/config`.
- "Did you mean" suggestions for mistyped commands, folders and settings.
- Unit, integration and pseudo-terminal test suites.
