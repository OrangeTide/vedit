# Changelog

All notable changes to vedit are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

The release number lives in `version.mk` and is bumped on every commit: the
patch level at least, or the minor level for a big change or one that breaks
the config file format. A release build stamps the git description onto the
number, so `vedit --version` on an untagged checkout reads like
`1.6.1-dev-gfce3eef`. Each tagged release also carries full notes in its
annotated tag (`git tag -n99 v1.6.0`), which is what the release page shows.

## [Unreleased]

### Added
- Configurable keymaps. F2 cycles a list of keymap modes set with
  `[keys] modes = ...`, and a config file can define its own mode by binding
  keys under `[keys "name"]`. A custom mode joins the F2 cycle as an equal of
  the built-in EDIT and vi modes.
- `[include]` config directive to merge another config file at that point.
- A WordStar keymap ships as `examples/wordstar.conf`: the cursor diamond, the
  Ctrl-K / Ctrl-Q / Ctrl-O prefixes, persistent blocks, numbered markers,
  overtype, scroll-by-line, a word-wrap toggle, center-line, and left and
  right margins that feed reflow.
- A simpler Emacs keymap ships as `examples/emacs.conf`.
- Per-keymap help: a mode carries its own F1 help text in its config file
  (`[keys "name"] help = ...`), so a personality is a single-file install.
- `veditmin`, a reduced build for embedding in a host that does not need the
  full feature set. New `VEDIT_NO_*` gates compile out the hex view, draw
  mode, swap files, reflow, themes, tags, the table view, and syntax
  highlighting; the text editor, the vi keys, and search stay in.
- The file browser's `File:` line expands a leading `~` to the home directory.
- The file browser shows a clickable breadcrumb bar of the current directory's
  path. It shows at most the five deepest levels, each capped at ten columns
  (truncated with an ellipsis), and clicking a segment changes into it.
- The command language gains four dialog verbs that return what the user chose:
  `ask` (a line prompt), `confirm` (yes/no), `choose` (a list), and `pick-file`
  (the file browser). Each blocks and resumes the program, so a program can read
  a value with `set x [ask "Name: "]`.
- The command language gains an `if COND { THEN } else { ELSE }` conditional.
  The condition is any value (a non-empty, non-`0` string, or a non-zero number,
  is true), so a dialog verb reads as a condition, and the untaken branch does
  not run. A condition may also compare two operands, `if A OP B { ... }`, with
  `=`, `==`, `!=` as text and `<`, `<=`, `>`, `>=` as numbers.
- The command language gains the boolean combinators `not`, `and`, and `or`
  over truthiness, and `cmp A OP B` to use a comparison as a value, so compound
  conditions such as `if [and [cmp $n < 10] [confirm {ok?}]] { ... }` work.
- The command language gains loops: `for VAR START END { BODY }` counts a
  variable over a range, and `while { COND } { BODY }` and `until { COND }
  { BODY }` repeat on a re-evaluated condition.
- The command language gains an `expr` verb: an arithmetic expression evaluator
  with `+ - * / %`, comparisons, `&& || !`, parentheses, and the functions
  `abs`, `int`, `mod`, `min`, and `max`, so a loop can advance a counter with
  `set i [expr $i + 1]`. The evaluator is standalone, ready to serve a future
  spreadsheet formula engine (see `docs/design/expr-and-spreadsheet.md`).
- The table view evaluates formulas. A cell starting with `=` is an arithmetic
  expression over A1-style cell references, shown computed in the grid while the
  file keeps the `=` text as plain CSV. Errors show as `#DIV/0!`, `#REF!`,
  `#CYCLE!`, or `#ERR!`. The `table.formulas` config turns it off.
- Table formulas take ranges: `A1:B3` names a block of cells, and the functions
  `SUM`, `AVG`, `COUNT`, `MIN`, and `MAX` fold a range (or plain numbers) into a
  value, so `=SUM(A1:A10)` totals a column. Function names are case-insensitive.
- Table formula references are relabeled when a row or column is inserted or
  deleted, so a formula keeps pointing at the same cells. A reference to a
  deleted cell becomes `#REF!`, and a range straddling a deletion shrinks.
- The command language gains a `menu TITLE LABEL BODY ...` verb: it lists the
  labels and runs the chosen entry's program, so a keymap can build its own menu
  with no change to the editor.

### Changed
- View and mode key dispatch is centralized, and the grid-view render path is
  a single dispatch.
- Reflow indents its output to the configured left margin.
- An unqualified `[include]` path (a bare or slashed name) now resolves against
  the config directory rather than the including file's directory. An
  explicitly relative `./` or `../` path still resolves against the including
  file, and an absolute path is still used as given.
- A leading `~` in a config path value (`edit.swapdir`, `edit.backupdir`,
  `tags.file`, `cc.file`, an `[include]` path) expands against `$HOME`.

## [1.6.0] - 2026-10-16
- WebAssembly builds: a headless `wasm32-wasi` binary (`make wasm`) and a
  browser build on emscripten and xterm.js (`make web`).
- The find prompt edits in place, with a caret and search history.
- Outgoing mail can attach files, and received parts can be saved.
- The event loop is a stack of modal frames.

## [1.5.0] - 2026-10-10
- Two text panes with `:split` and `:vsplit`.
- Paragraph re-wrap: the `gq` operator and `textwidth`.
- VCS diff against HEAD or a revision with `:diff`.
- A Powerline style for the status line, and mail replies and delivery.

## [1.4.1] - 2026-10-08
- The complete manual, split into chapters, plus a GitHub Pages site.
- Quit asks about every modified buffer; `:symbol`; smaller fixes.

## [1.4.0] - 2026-10-08
- gvedit, the editor in its own window (SDL3), with a Windows cross build.
- A Windows port: mingw build, Win32 console backend, ConPTY terminals.
- A thin file layer over file descriptors; swap as a base plus an edit
  journal; files are mapped rather than read whole into memory.
- Version control: branch and change mark in the status bar, a file-history
  picker, read-only diffs, blame, and a commit buffer.
- Mail: an alpine-style reader and composer. Mouse support. A Date picker.

## [1.3.0] - 2026-10-06
- A table view for CSV and TSV, with grid editing, sorting, and column fit.
- An art view for color text art (`.ans`), with glyph and color palettes.
- A terminal pane under the text buffer; repost a terminal's output into a
  buffer. JavaScript and HTML grammars, and grammars that embed another.

## [1.2.0] - 2026-10-06
- Build commands run in a terminal buffer.
- CI jobs for cross, static, no-terminal, and macOS builds.

## [1.1.1] - 2026-10-06
- Open the menu bar from a terminal with Ctrl-W m.

## [1.1.0] - 2026-10-06
- Terminal buffers on a PTY (`VEDIT_TERM`), built by default.
- vi marks, the jump list, registers, and keyboard macros (`q`, `@`).
- `--version`, a man page, a getting-started guide, and a format-on-save hook.
- Swap files, backups, and atomic save. Crash-flush of dirty buffers.
- Markdown syntax highlighting; context-sensitive menus.

## [1.0.0] - 2026-10-04
- First release: the single-file terminal text editor, with the EDIT keys, the
  vi personality, syntax highlighting, color-depth handling, and draw mode.

[Unreleased]: https://github.com/OrangeTide/vedit/compare/v1.6.0...HEAD
[1.6.0]: https://github.com/OrangeTide/vedit/compare/v1.5.0...v1.6.0
[1.5.0]: https://github.com/OrangeTide/vedit/compare/v1.4.1...v1.5.0
[1.4.1]: https://github.com/OrangeTide/vedit/compare/v1.4.0...v1.4.1
[1.4.0]: https://github.com/OrangeTide/vedit/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/OrangeTide/vedit/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/OrangeTide/vedit/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/OrangeTide/vedit/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/OrangeTide/vedit/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/OrangeTide/vedit/releases/tag/v1.0.0
