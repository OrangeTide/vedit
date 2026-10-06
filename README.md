# vedit

A single-file visual text editor for primitive terminals. vedit emulates a
modeless Microsoft EDIT personality and a vi personality. It draws through a
small cell grid that emits a fixed subset of ANSI control codes, with no
terminfo database and no differential compositor, so it runs over a telnet or
ssh link to a simple terminal emulator such as a MUD client.

vedit is one source file (`vedit.c`) plus a public header for embedding
(`vedit.h`).

![Editing a C file, with syntax highlighting and the MS-EDIT chrome](docs/shot-edit.png)

`docs/demo.html` is a self-contained page that illustrates the rendering work
(the scroll fast path, the color schemes, the line-number gutter, word wrap, and
the unified palette), with mockups drawn from vedit's real output.

## Building

```sh
make                    # build ./vedit
make RELEASE=1          # optimized build
make static             # static musl build, for dropping onto a server
make install            # install to ~/.local/bin (override PREFIX)
```

The only requirement is a C11 compiler and a POSIX system. There are no library
dependencies.

## Testing

```sh
make test               # unit and render tests through the taptest driver
make torture            # pseudo-random fuzz of the parsers and regex engine
make asan               # tests + torture under AddressSanitizer (with leaks)
make ubsan              # tests + torture under UndefinedBehaviorSanitizer
make cov                # line coverage of vedit.c from the unit tests
make cov-term           # line coverage of the terminal-buffer code
```

The tests live in `tests/` and run through a vendored copy of the `taptest`
TAP framework (its driver plus `test.h` / `testmain.c`). There are no external
dependencies. Each test file includes `vedit.c` as a single unit, with `main`
renamed, so it can call the internal helpers directly; integration tests drive
the editor over an in-memory `vedit_io` (`tests/memio.h`) that feeds scripted
keystrokes and captures the output, so no terminal is needed. `test_unit.c`
covers the pure helpers (UTF-8, rune width, color mapping, word-wrap layout, the
syntax tokenizer, buffer edits and undo); `test_render.c` drives whole-editor
behavior (cursor jumps, wrap, the gutter, status flags).

`tests/torture.c` is a fuzz suite that drives the untrusted-input surfaces, the
regex engine as search and replace use it, the config parser, and the UTF-8
codec, with pseudo-random input and a deterministic PRNG. It is not a
correctness oracle; it checks a few invariants and leans on the sanitizers to
catch memory and undefined-behavior faults. Set `TORTURE_ROUNDS` to change the
iteration count, and pass a seed as the second argument to `tests/torturet` to
reproduce a run.

CI (`.github/workflows/ci.yml`) runs the build and tests on gcc and clang, the
two sanitizer suites, and the torture fuzz on every push and pull request. A
single `ci-ok` job gates on all of them, so branch protection can require that
one check.

## Getting started

Open a file (or start an empty, named buffer) and you are editing:

```sh
vedit notes.txt
```

The top row is the menu bar, a frame surrounds the text, and the bottom row is
the status line. Out of the box vedit uses modeless, MS-EDIT style keys, so you
can just type, and the arrow keys, `Home`, `End`, `PgUp`, and `PgDn` move around.
`Enter` splits a line, `Backspace` and `Delete` remove a character, and holding
`Shift` while moving selects text.

A first session covers only a handful of keys:

- `Ctrl-S` saves (it asks for a name if the buffer has none), and `Ctrl-Q` quits,
  prompting when there are unsaved changes.
- `Ctrl-C`, `Ctrl-X`, and `Ctrl-V` copy, cut, and paste. With nothing selected,
  `Ctrl-C` copies the whole line.
- `Ctrl-Z` and `Ctrl-Y` undo and redo.
- `Ctrl-F` finds as you type (`Enter` repeats the last search), `Ctrl-R` replaces
  with confirmation, and `Ctrl-L` jumps to a line number.

Everything else lives in the menu bar. Press `F10`, or `Alt` plus the underlined
letter of a menu (for example `Alt+V` for View), then use the arrows and `Enter`,
or the underlined letter of an item. The View menu toggles line numbers, word
wrap, the color scheme, and syntax highlighting; the Edit menu has formatting and
tab conversions; the Search menu has find, replace, and go-to.

Two keys are worth knowing early:

- `F1` shows the key bindings for whichever personality is active. Press `t` on
  that screen for a tutorial.
- `F2` toggles the vi personality, for modal editing with `h j k l`, operators
  like `dw` and `cc`, `:` ex commands, and `/` search. Press `F2` again to return
  to the modeless keys.

If vedit or the connection dies with unsaved changes, reopen the file: a
crash-recovery snapshot is kept beside it and vedit offers to restore it. See
[Crash recovery](#crash-recovery-swap-and-backup-files) for the details.

## Running

```sh
vedit [file]
```

A missing file opens as a new, named buffer. Opening a file that is already open,
even by a different spelling (a symlink, a `./` or `../` detour, or a hard link),
switches to the existing buffer rather than loading a second copy. vedit requires
a terminal on both stdin and stdout when run from the command line.

At very small sizes vedit keeps the display coherent instead of drawing a broken
frame. A narrow menu bar drops whole titles that would collide with the Help
label rather than overprinting them, and those menus are still reachable by their
Alt mnemonics. Below a minimum usable size the screen shows a short `window too
small` notice until the window grows again.

### Box-drawing mode

The frame, scrollbars, and menus draw three ways, chosen for the client:

```sh
vedit --utf8 file     # Unicode box-drawing
vedit --dec  file     # DEC VT100 line-drawing (single-byte, widely supported)
vedit --ascii file    # plain ASCII:  + - | ^ v # :
```

The default is UTF-8 under a UTF-8 locale and ASCII otherwise. `VEDIT_BOX=utf8|
dec|ascii` and `VEDIT_ASCII` set it from the environment. DEC line-drawing is a
good middle ground for older clients that cannot render UTF-8 box-drawing but do
support the VT100 alternate charset.

### Color depth

```sh
vedit --256color file   # full xterm 256-color palette
vedit --16color file    # map everything to the 16 ANSI colors
```

The default is 256 when `TERM` or `COLORTERM` says the terminal supports it, and
16 otherwise; `VEDIT_COLORS=256|16` overrides. In 16-color mode every color is
mapped to the nearest of the 16 ANSI colors and sent with the classic `30`-`37` /
`90`-`97` SGR codes, which the most limited clients understand. Syntax
highlighting uses a separate, punchier palette in this mode, since the 256-color
scheme's pastels would otherwise collapse toward white.

### Fast scrolling

```sh
vedit --scroll file     # use the terminal scroll region (fewer bytes)
vedit --no-scroll file  # repaint instead (the default)
```

When the viewport scrolls or a line is inserted, the whole text area normally
shifts, so without help every visible row is repainted. With `--scroll` the
editor moves the text region with the terminal's scroll region (DECSTBM plus
`IND` / `RI`) and repaints only the one newly exposed line, which is a large
saving on a slow link. It is off by default because it relies on VT100
scroll-region support, which the most primitive line-at-a-time clients lack.
`VEDIT_SCROLL=1` turns it on from the environment, and an embedding host that
knows the client calls `vedit_set_scroll()`.

Two redraw savings are always on and need no flag: only rows that changed are
sent, and within a row only the columns between the first and last change are
repainted. On the black color scheme (below), a long run of trailing blanks in a
changed row is cleared with one erase-to-EOL instead of a column of spaces.

### Configuration file

A gitconfig-style file sets the startup defaults. It is read from the first of
`$VEDIT_CONFIG`, `$XDG_CONFIG_HOME/vedit/config`, and `~/.veditrc` that exists;
`--config FILE` points at a specific file and `--no-config` skips it.

```ini
# ~/.veditrc
[ui]
    scheme = black      # dos | black | plain
    number = on         # line-number gutter
    wrap   = on         # word wrap
    box    = dec         # utf8 | dec | ascii
    colors = 256         # 256 | 16
    scroll = on          # VT100 scroll-region fast path
    clipboard = off      # also mirror every copy/yank to the terminal (OSC 52)
    tabs   = on          # mark hard tabs with a guide glyph

[edit]
    mode = vi            # vi | modeless
    autoindent = on      # new lines copy the previous indent
    ignorecase = off     # on = searches match regardless of case
    shiftwidth = 0       # >> / << indent width in columns; 0 = one tab stop
    swap = on            # write a .swp crash-recovery snapshot (on by default)
    swapdir =            # where swap files go; empty = beside the file
    backup = off         # keep the previous version as a "~" file on save
    backupdir =          # where backups go; empty = beside the file
    formatonsave = off   # run command.<lang>.format before each save

[indent]
    expand = off         # off = indent with tabs, on = with spaces

[tags]
    file = /path/to/tags # ctags index; else a "tags" file beside the buffer

[cc]
    file = /path/to/compile_commands.json # include paths for gf / Open Header

[command]
    terminal = on        # run Compile / Make / Run in a terminal buffer

[error]
    pattern = ^([^(]+)\(([0-9]+),([0-9]+)\):  # extra build-error format

[syntax]
    enable = on          # highlight recognized file types
```

Keys are dotted, so `ui.scheme = black` without a section header works too.
Booleans accept `on`/`off`, `yes`/`no`, `true`/`false`, or `1`/`0`. A missing or
unreadable file is ignored (an explicit `--config` path that cannot be read
prints a warning and the editor still starts).

The keys that usually differ from one project to the next can be overridden by an
environment variable, which is handy in a per-project shell or an `.envrc`. The
variable is `VEDIT_` followed by the key uppercased with each `.` turned into `_`,
and a set, non-empty value wins over the config file:

| Config key                 | Environment variable       |
| -------------------------- | -------------------------- |
| `tags.file`                | `VEDIT_TAGS_FILE`          |
| `edit.swapdir`             | `VEDIT_EDIT_SWAPDIR`       |
| `edit.backupdir`           | `VEDIT_EDIT_BACKUPDIR`     |
| `cc.file`                  | `VEDIT_CC_FILE`            |
| `command.<lang>.compile`   | `VEDIT_COMMAND_<LANG>_COMPILE` |
| `command.<lang>.build`     | `VEDIT_COMMAND_<LANG>_BUILD`   |
| `command.<lang>.run`       | `VEDIT_COMMAND_<LANG>_RUN`     |
| `command.<lang>.format`    | `VEDIT_COMMAND_<LANG>_FORMAT`  |

Only these per-project keys read the environment; editor preferences such as the
theme or key mode stay in the config file.

After editing the config file, reload it without restarting: the `:reload` ex
command, or Options > Reload Config. It re-reads the file named at startup and
re-applies everything (the scheme and themes, box mode and colors, syntax
grammars, and the editor toggles). A session started with `--no-config`, or one
embedded in a host that supplies its own config, has no file to reload and says
so.

A `[theme "name"]` section defines a custom color scheme that `ui.scheme = name`
then selects, alongside the three built-ins:

```ini
[theme "midnight"]
    base         = black      # start from dos | black | plain (default dos)
    content.fg   = 189        # the text area
    content.bg   = default
    frame.fg     = 60         # window border and scrollbars
    frame.bg     = default
    title.fg     = "#ffd787"  # file name in the top border (quote a hex color)
    bar.fg       = 231        # menu bar and status bar
    bar.bg       = 54
    reverse-bars = off        # draw the bars in reverse video
    borderless   = on         # drop the right border so text reaches the edge
```

A `[command "<lang>"]` section sets the per-language build commands (see
[Build commands](#build-commands-a-primitive-ide)).

A color is `default` (the terminal's own color), a 0-255 palette index,
`#rrggbb`, or one of the sixteen ANSI names (`red`, `cyan`, ..., with a
`bright-` prefix for 8-15). A `#rrggbb` value must be quoted, since an unquoted
`#` starts a comment; quoting also protects any value with a `#` or trailing
spaces. Unset fields keep the base preset's value, and `base
= black` turns `borderless` on unless the theme sets it off. Up to eight themes
can be defined.

Settings rank from weakest to strongest: built-in default, then the config file,
then the `VEDIT_*` environment variables, then the command-line flags, then an
embedding host's `vedit_set_*()` calls. So a `--dec` flag or `VEDIT_BOX` beats
`ui.box` in the file, and the host always wins. The core never opens the file
itself; the command-line front end reads it and hands it in through
`vedit_set_config()`, which an embedding host can also call with a config it
builds from `vedit_cfg_new()` / `vedit_cfg_load()`.

### Custom syntax highlighting

Beyond the built-in C, shell, and Markdown highlighters, the config file can
define a language as a small state machine (the model joe uses), authored in the same
gitconfig format, no separate file. A language is a set of states; each state
has an ordered list of transition `rule` lines keyed on a character set.

```ini
[syntax]
    enable = on
    mn = mini             # map an extension to a language (or :syntax mini)

[color "mini"]            # class -> color [attrs], same grammar as themes
    kw  = "#ffd700"
    num = cyan

[words "mini.keywords"]   # keyword groups; split on spaces, multiple lines ok
    list = if else while return
    list = for do break continue

[state "mini.idle"]       # the first state is the start (or language.mini.start)
    color = text
    rule = "0-9"       num   recolor
    rule = "a-zA-Z_"   word  buffer
    rule = *           idle

[state "mini.num"]
    color = num
    rule = "0-9."      num
    rule = *           idle  noeat

[state "mini.word"]
    color = text
    rule = "a-zA-Z0-9_" word
    rule = *            idle noeat kw=mini.keywords:kw
```

Each `rule` is `charset  target-state  [options]`:

- **charset** is `*` (any byte, put it last) or a quoted set with ranges and
  escapes, for example `"a-zA-Z0-9_"`, `"0-9"`, `"\t\n"`, `"\""`.
- **options**: `noeat` re-processes the byte in the target state without
  consuming it; `recolor` (or `recolor=N`) repaints the last byte (or N bytes)
  in the target state's color; `buffer` starts recording a token;
  `kw=<group>:<class>` matches the buffered token against a `[words]` group and,
  on a hit, repaints it in `<class>`; `mark` and `recolormark` handle a region
  whose length is not known until its end is seen (see below). Colors accept
  attributes (`bold`, `underline`, `reverse`, `dim`, `italic`), e.g.
  `keyword = yellow bold`.

A state may also set `include = <other-state>`: when none of its own rules
match the byte, the machine falls through to the included state's rules (and
their includes, in turn). The including state keeps its own color, so a shared
rule set (operators, identifiers, whitespace) can be written once and reused.

`mark` records the current position, and a later rule with `recolormark`
repaints everything from that mark up to the current byte in the target state's
color. Unlike `recolor=N`, the span length need not be known in advance, so a
preprocessor line, an include like `<stdio.h>`, or a here-document body can be
colored as one region once its end matches. The mark is per line and defaults
to the start of the line, so on a continuation line of a multi-line region the
repaint starts at the left edge.

The carry state between lines is the current state, so multi-line constructs
(block comments, here-strings) work by staying in a state at end of line. At the
end of each line a newline is fed to the machine (as joe does), so a state that
should not span lines, such as a `//` line comment, returns to idle with a
`rule = "\n" idle`; a state with only a `*` rule stays and carries on. The
newline flush also lets a bare keyword at the very end of a line be recognized.
A language whose name matches a file extension is picked up automatically; a
`syntax.<ext> = <name>` line maps any other extension. The core reads no files:
the rules are config data, loaded through `vedit_set_config()` like everything
else.

The built-in C, shell, and Markdown highlighters are themselves grammars in this format,
compiled into the binary and loaded at startup, so they need no config file.
Defining a language of the same name in your config replaces the matching
built-in outright (the two are not merged), so to customize one, copy its whole
grammar and edit it rather than setting a single color.

### Color schemes

### Line numbers

**View > Line Numbers** toggles a line-number gutter down the left of the text,
the text-mode counterpart of the hex view's address column. Its width follows the
buffer's digit count, and the current line's number is drawn brighter than the
rest. In vi keys it toggles with `:set number` / `:set nonumber` (`:set nu` /
`:set nonu`, or `:set number!` to flip it).

### Word wrap

**View > Word Wrap** soft-wraps long lines to the window width instead of
scrolling horizontally. Lines break at word boundaries (a word wider than the
window is broken mid-word), the wrap is display-only so the file is unchanged,
and a wrapped line's continuation rows have a blank gutter. In vi keys it toggles
with `:set wrap` / `:set nowrap` (`:set wrap!` to flip). Up and down move by
whole lines; Home, End, and left/right move within the wrapped line as usual.
Wrap does not apply in draw mode, and the scroll-region fast path is skipped
while it is on.

While either toggle is on, the status bar shows a compact flag at the right,
`WRAP` for word wrap and `NUM` for line numbers, next to the line and column.

### Line endings

**View > Line Endings** sets how lines are separated on disk, cycling through
`LF` (Unix, `\n`), `CRLF` (DOS, `\r\n`), and `NUL` (NUL-separated records, `\0`).
The style is detected when a file is loaded: a NUL byte means NUL-separated, a
`\r\n` means DOS, and anything else is Unix. A new buffer defaults to LF. The
in-memory text never holds the terminator, so changing the style only changes
what a save writes. Changing it marks the buffer modified, since the bytes on
disk will differ. In vi keys it is `:set ff=unix|dos|nul` (`:set fileformat=`
also works). The current style always shows in the status bar, next to the line
and column.

### Tabs and indentation

Tabs expand to 8-column stops on screen. **View > Show Tabs** (on by default)
marks each hard tab's first column with a dim guide glyph (an arrow in UTF-8
mode, `>` in DEC or ASCII mode), so tabs and runs of spaces are easy to tell
apart. In vi keys it is `:set list` / `:set nolist`.

**View > Auto Indent** (on by default) starts each new line with the same
leading whitespace as the line you left, for Enter and for vi's `o` and `O`. In
vi keys it is `:set autoindent` / `:set noautoindent` (`:set ai` / `:set noai`).

**View > Indent with Spaces** controls what the Tab key and auto-indent insert:
with it off (the default) they use a hard tab, with it on they use spaces to the
next stop. It is per buffer. A fresh buffer's default comes from the config, by
language, then tabs when nothing sets it (see below). In vi keys it is
`:set expandtab` / `:set noexpandtab` (`:set et` / `:set noet`).

The vi `>>` and `<<` commands (and `>`/`<` over a visual selection) indent by
one tab stop by default. `edit.shiftwidth = N`, or `:set shiftwidth=N` (`:set
sw=N`) during a session, shifts by N columns instead: N spaces when indenting
with spaces, otherwise tabs with a spaces remainder. `N = 0` keeps the one-tab
default.

**Edit > Tabs to Spaces** and **Edit > Spaces to Tabs** rewrite whitespace over
the selection, or the whole buffer when there is no selection. "Tabs to Spaces"
expands every tab in each line; "Spaces to Tabs" repacks each line's leading
indent into tabs plus a spaces remainder. In vi keys, `:retab` does whichever
the current `expandtab` setting implies (tabs to spaces when indenting with
spaces, otherwise the reverse), and takes an optional line range.

The config sets a fresh buffer's indent style, per language or globally:

```ini
[indent]
    expand = off         # global default: off = tabs, on = spaces

[indent "python"]
    expand = on          # Python buffers indent with spaces
```

A per-language `[indent "<lang>"]` (the file's syntax language name) wins over
the global `[indent] expand`, which wins over the built-in default of tabs.

### Crash recovery (swap and backup files)

While you edit a named file, vedit keeps a swap file beside it, `.name.swp`,
refreshed whenever input goes quiet. It is a full snapshot of the buffer, so if
the editor or the connection dies with unsaved changes, the work is still on
disk. Open the file again and vedit notices the swap and asks: `(r)ecover`
loads the snapshot into a buffer you can then save, `(o)pen` ignores it, `(d)elete`
removes it, and `(q)uit` leaves the file unopened. If the swap was left by a
process that is still running, the prompt says so, in case the file is open in
another session. A clean save or quit removes the swap; only a crash leaves one
behind. A file that is not ours (for example a swap from Vim at the same name)
is never read or overwritten.

Swap files are on by default. `edit.swap = off` turns them off, as does
`:set noswapfile`, which is the right choice for a host that must not write to
the filesystem. `edit.swapdir = <dir>` collects them in one directory instead
of beside each file, naming each after the full path with `/` turned into `%`
so two files with the same name never collide. A leading `~/` in the directory
expands against `$HOME`; a directory that does not exist or cannot be written
falls back to beside-the-file.

Saving is atomic: vedit writes the new contents to a temporary file in the same
directory and renames it over the target, so a crash or a full disk never
leaves the file half-written, and the previous version survives until the new
one is complete. With `edit.backup = on` (or `:set backup`) that previous
version is also kept afterward as `name~`, or in `edit.backupdir` when set.

The idle snapshot is the baseline, but it is not the only time a swap is written.
When vedit is killed by `SIGTERM` or `SIGHUP`, for example because the window
closed or the system is shutting down, it flushes every dirty buffer to its swap
before exiting, so nothing is lost. On an out-of-memory abort or a fatal fault
such as a segfault it attempts the same flush on a best-effort basis, since the
program state may already be damaged by then. Only an uncatchable `SIGKILL` or a
power loss falls back to the last idle snapshot, losing just the edits made since
the last quiet moment. A new buffer that has never been saved has no name yet, so
it gets no swap until its first save.

### File browser

**File > Open...** shows a modal file browser instead of a bare filename
prompt. It lists the current directory with sub-directories first (each marked
with a trailing `/`), then files, sorted. The arrow keys, PgUp/PgDn, and
Home/End move the selection, and typing a letter jumps to the next entry that
starts with it. Enter on a directory descends into it (`../` goes up), and Enter
on a file opens it. A `File:` entry line at the top (reached with Tab) takes a
typed name or path: a directory there changes into it, any other name is opened
and may be a new file.

**File > Save As** (and the first save of an untitled buffer) uses the same
browser, but it starts with the `File:` entry line focused and pre-filled with
the current name, opened in the current file's directory. Type or edit the name
and press Enter to write there, or pick an existing file from the list to save
over it.

The browser is one client of a reusable list-picker control (`dlg_pick` with a
`Picksrc` of callbacks). The control owns the box, scrolling, selection, and
keys; a source supplies the rows and decides what choosing one means. The intent
is to reuse it for other lists, such as mailboxes for a mail reader or module and
function lists for editing scripts.

### Buffer switcher

**File > Buffer List** opens the same picker over the open buffers. Each row
shows the buffer number, a `*` on the active one, the file name, a `[+]` when it
has unsaved changes, and its line count. Move with the arrows (or type a letter
to jump by name) and press Enter to switch to that buffer. F8 and Shift-F8 still
cycle to the next and previous buffer without opening the list. The switcher is
a second client of the picker, a list with no entry line, where the file browser
adds one.

### Symbol jump

**Ctrl-T** (Search > Go to Symbol) scans the current buffer for definitions and
lists them in the picker; choosing one moves the cursor to its line. Each row
shows the name, a kind (`func`, `type`, `class`, `macro`), and the line number,
and typing a letter jumps to the next name that starts with it. The scan is a
set of simple line patterns rather than a parser: a top-level function (an
identifier at column 0 right before `(`, on a line that does not end in `;`, so
both the `name(args)` and the BSD split style are caught), a `struct` / `union`
/ `enum` / `class` tag with an opening brace, a `} Name;` typedef alias, and a
`#define`. It needs no tags file and no external tool, and the same patterns
cover C, C++, LPC, and shell `name()` functions. This is the picker's third
client and the first step toward editing larger sources.

When a `tags` file is available, its entries are merged into the same list, so
the picker reaches definitions across the whole project, not just the current
buffer. Each row is marked `[buffer]` (from the live scan) or `[tags]` (from the
file), and a tags row shows the file it points into instead of a line number.
Choosing a tags row opens or switches to that file and positions the cursor,
following the tag's line number or search pattern. A tags entry that points back
into the current file is dropped, since the live scan already lists it.

The tags file is the Exuberant or Universal ctags format (also classic vi tags):
one line per definition, `name <TAB> file <TAB> address`, where the address is a
line number or a `/pattern/` search, with optional `;"`-delimited fields for the
kind. Generate it with `ctags *.c` (see [ctags.io][ctags] or the older
[ctags.sourceforge.net][ectags]). vedit looks for a file named `tags` in the
current buffer's directory, or at the path set by `tags.file` in the config:

```ini
[tags]
    file = /path/to/project/tags
```

vi users also get the familiar jumps: **Ctrl-]** jumps to the tag named by the
identifier under the cursor, and **`:tag NAME`** (abbreviated `:ta`) jumps to a
named tag. When a name has several matches the picker opens on just those;
`:tag /pattern` opens the picker filtered to names containing the pattern. A
single match jumps straight there. Each jump pushes where you were onto a tag
stack, so you can return: in vi keys **Ctrl-T** or **`:pop`** (`:po`) pops back
to the previous position, and **Search > Pop Tag** does the same in either
personality (in the modeless keys Ctrl-T stays the symbol picker). The stack
records named buffers only, since a pop reopens by path.

[ctags]: https://ctags.io/
[ectags]: https://ctags.sourceforge.net/

### Color schemes

The **View > Color Scheme** menu cycles three looks: the DOS blue text area
(the default), a black scheme that leaves the text area on the terminal's
default background, and a monochrome scheme that uses reverse video for the
bars. The black scheme is the one where the erase-to-EOL redraw saving applies,
since its background is the terminal default and so can be cleared on any client
with or without back-color-erase. It also drops the right border and vertical
scrollbar so the text reaches the last column, which is what lets a whole
trailing run of blanks be cleared with one erase. The file position is still on
the status line. The scheme drives the whole interface, including the menus,
dialogs, prompts, and the help screen, not just the text area.

### Modeless keys (the default, MS-EDIT style)

| Key | Action |
| --- | --- |
| arrows | move the cursor |
| Home / End | start / end of line |
| Ctrl-Home / Ctrl-End | start / end of the file |
| PgUp / PgDn | scroll by a screen |
| Enter | split the line |
| Backspace / Delete | delete left / right |
| Shift-arrows | extend a selection |
| Ctrl-C / Ctrl-X | copy / cut (Ctrl-C with no selection copies the line) |
| Ctrl-V | paste the internal clipboard |
| Ctrl-F | incremental find, by regular expression (Enter repeats the last search) |
| Ctrl-R | replace by regular expression, confirming each match (y / n / a / q) |
| Ctrl-T | go to a symbol (buffer definitions, plus the tags file if present) |
| Ctrl-] | jump to the tag under the cursor (needs a tags file) |
| Ctrl-L | go to a line number |
| Ctrl-Z / Ctrl-Y | undo / redo |
| Ctrl-S | save (prompts for a name if the buffer has none) |
| Ctrl-Q | quit (prompts if the buffer was modified) |
| Insert | toggle draw mode (2D / block editing, see below) |
| F8 / Shift-F8 | next / previous open buffer |
| F10 | activate the menu bar (then a letter opens that menu) |
| Alt+letter | open a menu directly (File, Edit, ...) |
| F1 | show the key bindings (press `t` there for the tutorial) |
| F2 | toggle vi keys |

Ctrl-F searches incrementally: the cursor follows the first match from where
you started as you type, the status line shows the query (marked `(failing)`
when nothing matches), and the view scrolls to keep the match in sight. Enter
accepts and leaves the cursor on the match, storing the query so Repeat Find and
a later empty-query Ctrl-F jump to the next one. Esc cancels and restores the
starting position. The query is a regular expression, so a half-typed pattern
that is not yet valid simply matches nothing until it is.

Ctrl-R replaces by regular expression. It asks for a pattern and a replacement
template, then walks the matches from the cursor to the end of the buffer and
confirms each one. The template understands `&` and `\1`..`\9` for the whole
match and captured groups, and `\U \L \u \l \E` to change case.

### Clipboards

The copy, cut, and yank keys use an internal clipboard that paste reads back, so
they work the same on every terminal. To reach the terminal's own selection
buffer, the Edit menu adds **Copy to Terminal** (the selection, or the current
line) and **Copy File to Terminal** (the whole buffer); both send the text with
an OSC 52 escape, which the terminal copies to its clipboard. This is the way to
lift text out of a vedit running over ssh or telnet, where there is no local
clipboard to share. A terminal that does not implement OSC 52 ignores the
escape, so the commands are harmless there, just without effect. Setting
`ui.clipboard = on` also mirrors every ordinary copy, cut, and yank to the
terminal through OSC 52, so the internal clipboard and the terminal's stay in
sync.

The menu bar works the MS-EDIT way. Press F10 to activate it, then press a
menu's highlighted letter to open it, or use the arrow keys and Enter.
Alt+letter opens a menu in one step, but note that many desktop terminal
emulators capture Alt+letter for their own menus, so F10 then a letter is the
reliable path. Inside an open menu, each item's highlighted letter runs it.

The bar is context sensitive. An item that cannot act right now is grayed and
skipped (Paste with an empty clipboard, Undo with nothing to undo, Next Error
with no diagnostics). A whole menu is hidden when none of its items apply, so
Compile and Run appear only once a build command is configured for the file's
language, and the Terminal menu appears only when a terminal can be opened. The
remaining menus stay put, so their letters do not move.

![The Edit menu open, with grayed items that cannot act on a fresh buffer](docs/shot-menu.png)

### vi keys

Press F2 to switch to the vi personality. NORMAL mode supports `h j k l`, `0 ^
$`, `w b e`, `gg G`, `f F t T`, `; ,`, `{ } ( )`, `% H M L |`, counts such as
`3j`, the operators `d c y` with motions (`dw`, `d$`, `dt`), `cc dd yy`, `x`,
`p`, `u`, and `i a A I o O` to insert. Visual mode selects text: `v` charwise,
`V` linewise, and `Ctrl-V` blockwise, after which an operator acts on the
selection; `gv` reselects the previous range. A blockwise selection (a column
rectangle) supports `d`/`x` to delete
the columns, `y` to yank them, and `I` / `A` to insert at the left edge or append
past the right edge, replicating the typed text down every row when you press
Esc; a block yanked or deleted this way pastes back as a rectangle with `p`.
`qa` records the keys you type into register a until you press `q` again, and an
uppercase name (`qA`) appends to it instead of replacing it. `@a` plays a
register back as keystrokes, `@@` repeats the last one, and a count repeats the
macro (`3@a`). A register can also be filled by yanking into it (`"ayy`), since
both are just stored bytes. `m` followed by a letter sets a mark; `` ` `` and `'`
jump to it (to the exact spot and to the line's first non-blank). The marks
`` ` `` / `'`, `` `. ``, `` `^ ``, `` `< `` / `` `> ``, and `` `[ `` / `` `] `` are kept
automatically for the previous position, the last change, where insert mode
stopped, the last visual selection, and the bounds of the last change or yank. A long-range move (`G`, `gg`, a search, a mark jump) is a jump:
`Ctrl-O` steps back through the jump list and `Ctrl-I` (Tab) forward, `` ` `` /
`''` toggle between a jump's ends, and `g`` / `g'` jump without recording.
`:marks` lists the marks (choose one to jump to it), `:delmarks` clears some (or
`:delmarks!` all a-z), and `:jumps` lists the jump list. `Ctrl-Home` and
`Ctrl-End` jump to the first and last line, the same as `gg` and `G`, and work
in insert mode too. `ZZ`
writes and quits, `ZQ` quits without writing. The `:` line runs `write`, `quit`,
`wq`, `xit`, `qall`, `wqall`, `cquit`, `edit`, `enew`, `read`, `buffer`,
`bnext`, `bprevious`, `bdelete`, `buffers`, `tag`, `pop`, `retab`, `marks`,
`delmarks`, `jumps`, `:N`,
`:set number` / `:set nonumber`, and `:set wrap` / `:set nowrap`. `:!cmd` runs a
shell command the way the build commands run: in a terminal buffer labelled
with the command, or in the output pane when a terminal buffer is not possible
(through the host's command runner, so it is unavailable when none is
installed). `:terminal [cmd]` opens a
terminal buffer, described under
[Terminal buffers](#terminal-buffers). Command names
follow the usual vi abbreviation rule: any leading prefix of the full name down
to its standard short form works, so `:s` is `:substitute`, `:e` is `:edit`,
`:w` is `:write`, `:bn` is `:bnext`, while `:se` stays `:set` and `:sy` is
`:syntax`. `/` and `?` search incrementally (forward and backward, the
cursor following the first match as you type, Esc restoring the start) and `n` /
`N` repeat. Searches and substitutions take regular expressions. Substitution
follows the usual vi forms: `:s/old/new/`, `:s/old/new/g` for every match on the
line, a leading range such as `:%s/old/new/g` for the whole file, and
`:g/pat/...` / `:v/pat/...` to run a command on matching (or non-matching) lines.
The global command runs `d` (delete), `s///` (substitute), `y` (yank the lines
to the clipboard), or `>` / `<` (shift them). The replacement supports `&` and
`\1`..`\9` and the `\U \L \u \l \E` case
escapes. In the modeless personality, Ctrl-R walks the matches one at a time and
asks before each.

### Regular expressions

Search and replace are driven by a small vendored regex engine, [rx][rx]. The
pattern language is POSIX extended regular expressions with the common vi, sed,
and PCRE conveniences on top: `.`, `*` `+` `?` and their lazy `*?` `+?` `??`
forms, counted repetition `{n,m}`, bracket classes `[...]` with ranges,
`[:class:]`, and `\d \w \s`, the anchors `^` `$` and the word boundaries `\b \B`
`\< \>`, groups `(...)` and `(?:...)`, alternation `|`, and backreferences
`\1`..`\9`. Matching is byte oriented and runs one line at a time, so a pattern
matches within a single line. The engine lives in `vedit.c` between the
`Vendored: rx` banners and can be refreshed from upstream by replacing that
block.

[rx]: https://github.com/OrangeTide/rx

Searches are case sensitive by default. `edit.ignorecase = on`, or `:set
ignorecase` (`:set ic`) during a session, makes every search and the regex
replace match regardless of case; `:set noignorecase` turns it back off. In vi
keys, `*` and `#` search for the whole word under the cursor (wrapped in the
`\< \>` boundaries), so they skip a substring match inside a longer word.

### Build commands (a primitive IDE)

vedit can compile, build, and run the file you are editing, with a Turbo C++ key
set that also matches MS-EDIT. The commands are per language, taken from the
config, so each file type gets its own compile and run lines. The **Compile** and
**Run** menus and these keys drive them:

| Key        | Command     | What it does                                  |
|------------|-------------|-----------------------------------------------|
| `Alt+F9`   | Compile     | compile the current file                      |
| `F9`       | Make        | build the project                             |
| `Ctrl+F9`  | Run         | run the program                               |
| `Alt+F5`   | View Output | reopen the last captured output pane          |
| `F4`       | Next Error  | jump to the next diagnostic                   |
| `Shift+F4` | Prev Error  | jump to the previous diagnostic               |

Define the commands in a `[command "<lang>"]` section, where `<lang>` is the
syntax language name of the file (for example `c` or `sh`). Each command line may
use these variables, expanded before the command runs:

| Variable         | Expands to                                  |
|------------------|---------------------------------------------|
| `$(file)`        | the file path                               |
| `$(filename)`    | the base name, with extension               |
| `$(filenoext)`   | the base name, without extension            |
| `$(fileext)`     | the extension, without the dot              |
| `$(dir)`         | the directory holding the file              |

A command runs in the file's directory, so the base-name forms are usually what
you want. For example:

```ini
[command "c"]
    compile = gcc -Wall -c $(filename) -o $(filenoext).o
    build   = make
    run     = ./$(filenoext)
    run.interactive = on
```

Compile, Make, and Run start the command in a terminal buffer (see
[Terminal buffers](#terminal-buffers)), labelled with the command name, so its
output shows live, in color, and a program that prompts can be answered. The
buffer stays after the command exits, with the exit status and, when any were
parsed, the diagnostic counts on its status line, and the next build replaces
it. `Ctrl-W w` returns
to the file while a command runs; `Alt+F5` (Run > View Output) switches back to
the build terminal. The output is also captured for the parser below: it is the
same capture that feeds the output pane, which is what the command falls back to
when a terminal buffer is not possible, either because the host cannot multiplex
file descriptors, the editor was built with `VEDIT_NO_TERM`, or `command.terminal`
is `off` in the config:

```ini
[command]
    terminal = off       # capture into the output pane instead
```

Output that looks like a gcc, clang, or MSVC diagnostic (`file:line:col: ...` or
`file:line: ...`, including a Windows `C:\path` with a drive letter) becomes a
jump target: use `F4` and `Shift+F4` from the editor to step through them, or
press Enter on it in the output pane (Run > View Output from inside the build
terminal's menu opens the pane instead of switching). A diagnostic in another file opens or switches to
that file.

Toolchains with a different format are handled by adding `error.pattern` keys
under `[error]`. Each value is a regular expression where capture group 1 is the
file, group 2 the line, and an optional group 3 the column. The patterns are
tried before the built-ins, and the first match on a line wins, so a key can both
parse a new format and override the default reading of a line. Set as many as you
need; they accumulate in file order. A pattern that contains `#` or `;`, or that
ends in a space, must be wrapped in double quotes so the config reader keeps it
whole.

The severity word after the location (`error`, `warning`, `note`) is read and
used. The pane colors errors red, warnings yellow, and notes cyan, and the pane
title and the post-build status line count the errors and warnings separately.
When a build ends with errors the cursor lands on the first one, switching back
to the file from the build terminal; a clean build leaves the terminal in front
so its output can be read. `F4` and `Shift+F4` step through the errors, skipping warnings
and notes, and wrap around at the ends. When a build has no errors, they step
through its warnings instead. Anything that matches the location form but carries
no recognized severity word, such as a linker line, counts as an error.

A command marked with a sibling
`<command>.interactive = on` key runs on the real terminal instead of a terminal
buffer, for a program that needs the full terminal rather than the embedded
emulator. vedit leaves the alternate screen while it runs and returns when it
exits.

### Formatting

A `command.<lang>.format` key names a formatter for the file type, run as a
filter: vedit feeds it the whole buffer on standard input and replaces the buffer
with what it writes to standard output. The command reads stdin and writes
stdout, the way `gofmt`, `clang-format`, and `prettier` do by default.

```ini
[command "c"]
    format = clang-format
[command "go"]
    format = gofmt
```

Run it by hand with `:format` (vi keys) or Edit > Format. The change is one undo
step, and the cursor keeps its line. A formatter that fails (a nonzero exit) or
produces no output leaves the buffer untouched, so a syntax error in progress
never discards your work. Formatting needs a tool runner, so it is unavailable in
a build compiled with `VEDIT_NO_TOOLS` and in a host that installs no filter.

Turn on `edit.formatonsave` (or `:set formatonsave`) to run the formatter
automatically before every save. It is off by default. Only the buffer's own
`command.<lang>.format` runs, so a file type with no formatter configured saves
unchanged.

### Opening headers

Press `gf` in vi keys, or Search > Open Header, to open the header named on an
`#include` line. The name under the cursor works too when the cursor is not on an
`#include`. vedit looks for the file in this order: an absolute or
current-directory path as written, then for the `"..."` form the current file's
own directory, then the include search directories of the current file. The
resolved path is tidied (`.`, `..`, and doubled slashes are collapsed) before the
file opens, so the status line stays readable and the same header is not opened
twice under two spellings.

Those search directories come from a clang compilation database. Point the
`cc.file` config key at a `compile_commands.json` and vedit reads the `-I`,
`-isystem`, and `-iquote` flags from the entry whose `file` matches the buffer.
The reader understands just that one schema, not arbitrary JSON, and relative
directories are resolved against the entry's `directory`. Only the include flags
are read; the compile command itself is never executed, so opening a project does
not run commands from a file in that project. Without `cc.file`,
`gf` still searches the current and buffer directories, which covers a small
project. A build whose include paths a single command line cannot carry can
still point the Compile and Make commands at a `Makefile` or a wrapper script.

The build commands are compiled in by default and can be dropped by building with
`-DVEDIT_NO_TOOLS`, which removes the commands, the menus, and the output pane.
Header navigation (`gf` and the `cc.file` reader) is separate and stays available.
An embedding host supplies its own command runner (or none) through
`vedit_set_tools`; see [Embedding in a host](#embedding-in-a-host-for-example-a-mud).

### Terminal buffers

A buffer can be a live terminal running a shell, a build, or an agentic CLI,
drawn inside the editor frame. Open one with the ex command `:terminal` (press
F2 for vi keys first, since the `:` line is a vi-personality feature). With no
argument it runs your login shell (`$SHELL`, else `/bin/sh`); `:terminal <cmd>`
runs that command instead, for example `:terminal make` or `:terminal htop`. The
command is split on whitespace into an argument list and run directly, with no
shell in between, so shell syntax such as pipes, redirection, or quoting does not
apply. Wrap those in `sh -c '...'` yourself when you need them. `terminal`
abbreviates to `:term`. The **Terminal** menu (New Terminal / Close Terminal) is
the menu-bar equivalent; it appears only on a host that multiplexes fds.

![A terminal buffer running a colored build, inside the editor frame](docs/shot-term.png)

Each terminal buffer is a real pseudo-terminal with a built-in VT emulator. The
child's output is parsed into a grid that vedit draws at its own coordinates, so
the child never writes to your real terminal and the display cannot drift out of
step with the editor chrome. The emulator honors 16-color, 256-color, and 24-bit
truecolor SGR, cursor movement, cursor hide and show (DECTCEM), and the window
title the child sets with OSC 0/2, which becomes the buffer's frame label.
Scrollback holds twice the visible height.

While a terminal buffer has focus, keystrokes pass straight through to the child.
**Ctrl-W** is the prefix for editor control, in a mix of GNU screen and vi:

| Key                     | What it does                                        |
|-------------------------|-----------------------------------------------------|
| `Ctrl-W m`              | open the menu bar (F10 and Alt+letter go to the child) |
| `Ctrl-W w` / `Ctrl-W W` | next / previous buffer                              |
| `Ctrl-W n`              | open another terminal                               |
| `Ctrl-W 1`..`9`         | switch to that buffer                               |
| `Ctrl-W c` / `Ctrl-W q` | close the terminal (quits if it is the last buffer) |
| `Ctrl-W Ctrl-W`         | send a literal Ctrl-W to the child                  |

The editor shortcuts, including F1, F8, F10, and Alt+letter, reach the child
rather than the editor while a terminal has focus. `Ctrl-W m` is the way to
the menu bar, and from there to every editor command; when the menu closes,
focus returns to the terminal.

The build commands (F9, Alt+F9, Ctrl+F9) open a terminal buffer of their own,
labelled with the command name, described under
[Build commands](#build-commands-a-primitive-ide).

When the child exits, the buffer shows `[process exited N]` and waits for
`Ctrl-W q` to close.

The terminal is compiled in by default. Build with `-DVEDIT_NO_TERM`
(`make VEDIT_NO_TERM=1`) to drop it, along with all the pseudo-terminal and
emulator code, for a primitive embedding host that does not want it. The feature
needs a host that can wait on more than one file descriptor at once: the
command-line binary does this, but an embedding host must supply the `poll_fds`
callback in `struct vedit_io`, or `:terminal` reports that it needs a
multiplexing host.

## Draw mode (ASCII art and maps)

Draw mode turns vedit into a 2D canvas for maps, box diagrams, and block art,
in the spirit of tools like DuhDraw. Toggle it with the **Insert** key (in either
personality), the Options menu, or the vi `:draw` command. The status line shows
`-- DRAW --` while it is on, and leaving it restores normal insert editing. A
walkthrough is built in: open Help > Tutorial, or press `t` on the F1
key-bindings screen.

![Draw mode, with the free cursor over a box-and-arrow diagram](docs/shot-draw.png)

In draw mode:

- The cursor moves freely over a virtual grid. Arrows (or vi `h j k l`) go one
  cell in any direction, past the end of a line and below the last line, and do
  not wrap at the edges.
- Typing overwrites the cell under the cursor (insert is off). Writing in
  virtual space pads the line with spaces and adds blank lines as needed, so you
  can draw anywhere.
- Enter is a carriage return: down one row, back to column 0.
- Backspace and Delete erase a cell to a space instead of joining lines.
- Shift+arrows mark a rectangle. Ctrl-C copies it, Ctrl-X cuts it (blanking the
  rectangle in place), and Ctrl-V overlays the copied block at the cursor.
- Ctrl-B draws an ASCII border around the marked rectangle (`+` corners, `-` and
  `|` edges). A one-cell-wide or one-cell-tall selection reduces to a straight
  line, so the same key draws lines. The glyphs are plain ASCII, so they render
  on any client.
- Ctrl-S, Ctrl-Q, Ctrl-Z/Y, Ctrl-F, and Ctrl-L work as usual.

Because the blanks you draw are real spaces, draw mode does not trim trailing
whitespace on save. That is what you want for art; keep it in mind when drawing
a diagram into a source file.

## Syntax highlighting

vedit highlights through a single data-driven engine, the state-machine model
described under "Custom syntax highlighting" above. The C-family and shell
highlighters that ship with it are grammars in that same config format, compiled
into the binary and loaded at startup rather than written as hardcoded lexers.
They color keywords, types, strings, character and number literals, line and
block comments (a block comment may span lines), and preprocessor lines,
including a string inside a `#include` or `#define` and a macro continued over a
trailing backslash. The shell grammar also colors `$var` and `${var}`, including
inside double-quoted strings.

The language is chosen from the file extension: `.c .h .cc .cpp .cxx .hpp .hh
.lpc .i` use the C grammar, and `.sh .bash` use the shell grammar. Highlighting
is on by default when the type is recognized, and files with no match are left
plain. Toggle it from the View menu, or with the vi `:syntax` command: `:syntax
off`, `:syntax on`, or `:syntax c` / `:syntax sh` to force a language. The
built-in grammars use base-16 colors so they read the same at 16 and 256 colors
and stay legible on the blue chrome.

To add or change a language, define a grammar in your config (see "Custom syntax
highlighting"); a language named like a built-in replaces it.

## Embedding in a host (for example a MUD)

vedit owns no file descriptors and installs no signal handlers of its own. All
terminal I/O passes through a read/write/select-style vtable (`struct
vedit_io`). A host fills that vtable over its own transport, drives the editor
with `vedit_run()`, and delivers window resizes with `vedit_set_size()`. Raw
mode and signals belong to the command-line binding only; it catches SIGWINCH
for resizes and SIGTERM/SIGHUP to restore the terminal before it dies, so a
closed window or a kill does not leave the shell in raw mode and the alt screen.
In an embedded host the `begin`/`end` callbacks are a no-op and the host owns its
own signals.

```c
#include "vedit.h"

struct vedit_io io = {0};
io.ctx   = player;
io.read  = player_read;    /* pull bytes from the player's socket */
io.write = player_write;   /* push bytes to the player's socket */
io.poll  = player_poll;    /* wait for the socket to be readable */

struct vedit *v = vedit_new(&io);
vedit_open(v, "note.txt");
vedit_set_size(v, rows, cols);         /* for example from telnet NAWS */
vedit_set_box_mode(v, VEDIT_BOX_DEC);  /* pick a frame style for this client */
vedit_set_colors(v, 16);               /* 16 or 256, from MTTS negotiation */
vedit_set_scroll(v, 1);                /* client supports a VT100 scroll region */
int rc = vedit_run(v);                 /* blocks until the player quits */
vedit_free(v);
```

See `vedit.h` for the full interface and the exact callback contracts. The box
mode is per instance, so two players on one server can use different clients.
A host that knows the client's capability (for example from telnet terminal-type
negotiation) should call `vedit_set_box_mode()` rather than rely on the server
locale.

Note that `vedit_run()` drives its own loop by calling `io.poll` and `io.read`.
In a single-threaded event-loop host, run the editor on its own thread or
coroutine, or supply a `poll` callback that yields to the host loop. A
push-style state machine is not provided.

Terminal buffers (`:terminal`) need the host to wait on the input source and the
child pseudo-terminals together, so they are available only when the host fills
the optional `io.poll_fds` callback (the command-line binary does). Without it
the editor runs normally and `:terminal` reports that it needs a multiplexing
host. A host built with `-DVEDIT_NO_TERM` omits the feature and the callback
entirely.

## What it includes and what it leaves out

vedit has a text buffer with undo and redo, the modeless and vi personalities,
the MS-EDIT chrome (menu bar, frame, scrollbars, dialogs), regex find and
replace, selection and an internal clipboard, goto-line, multiple buffers, a hex
view, a 2D/block draw mode, per-language build commands with a quickfix error
list, selectable line endings (LF, CRLF, NUL), tab display with auto-indent and
tab/space conversion, a symbol picker that merges a buffer scan with a ctags
tags file, terminal buffers running a shell or a build through a built-in VT
emulator, and lightweight syntax highlighting. It draws through a
self-contained ANSI
renderer over the io vtable, and decodes the keyboard with a compact decoder that
covers UTF-8 text, control keys, arrows, navigation keys, function keys, CSI
modifiers, Alt+letter, and bracketed paste.

It deliberately leaves out, as overworked for a primitive-terminal editor:

- mouse input,
- a differential compositor and terminfo capability lookup.

Syntax highlighting is a data-driven FSM highlighter (see below) rather than a
general language engine.

The renderer repaints only the rows that changed, and within a row only the
columns between the first and last change, and emits color escapes only when the
pen changes. With `--scroll` it also moves the text area with the terminal
scroll region instead of repainting it. All of this keeps a redraw small on a
slow link.

### Ways to make it smaller or larger

If you want an even leaner build, `-DVEDIT_NO_TERM` drops the terminal buffers
and all the pseudo-terminal and emulator code, and `-DVEDIT_NO_TOOLS` drops the
build commands and the output pane. Beyond those, the hex view and
multiple-buffer support are the next candidates to remove; each is
self-contained. For a smaller input surface, drop the vi personality.

If you embed over raw telnet rather than a cooked pty, the host (not vedit)
should handle telnet IAC negotiation and read the window size from NAWS, then
pass it to `vedit_set_size()`. A read-only pager mode and a hard line-length cap
for very small clients are reasonable additions at the editor layer.
