# vedit

A single-file visual text editor for primitive terminals. vedit emulates a
modeless Microsoft EDIT personality and a vi personality. It draws through a
small cell grid that emits a fixed subset of ANSI control codes, with no
terminfo database and no differential compositor, so it runs over a telnet or
ssh link to a simple terminal emulator such as a MUD client.

vedit is a port of the editor from the lumi project, reduced to one source file
(`vedit.c`) plus a public header for embedding (`vedit.h`).

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
make test
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

## Running

```sh
vedit [file]
```

A missing file opens as a new, named buffer. vedit requires a terminal on both
stdin and stdout when run from the command line.

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

[edit]
    mode = vi            # vi | modeless

[syntax]
    enable = on          # highlight recognized file types
```

Keys are dotted, so `ui.scheme = black` without a section header works too.
Booleans accept `on`/`off`, `yes`/`no`, `true`/`false`, or `1`/`0`. A missing or
unreadable file is ignored (an explicit `--config` path that cannot be read
prints a warning and the editor still starts).

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

Beyond the built-in C and shell highlighters, the config file can define a
language as a small state machine (the model joe uses), authored in the same
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
| Ctrl-F | incremental find (Enter repeats the last search) |
| Ctrl-R | replace, confirming each match (y / n / a / q) |
| Ctrl-T | go to a symbol defined in the buffer |
| Ctrl-L | go to a line number |
| Ctrl-Z / Ctrl-Y | undo / redo |
| Ctrl-S | save (prompts for a name if the buffer has none) |
| Ctrl-Q | quit (prompts if the buffer was modified) |
| Insert | toggle draw mode (2D / block editing, see below) |
| F8 / Shift-F8 | next / previous open buffer |
| F10 | activate the menu bar (then a letter opens that menu) |
| Alt+letter | open a menu directly (File, Edit, ...) |
| F1 | show the key bindings |
| F2 | toggle vi keys |

Ctrl-F searches incrementally: the cursor follows the first match from where
you started as you type, the status line shows the query (marked `(failing)`
when nothing matches), and the view scrolls to keep the match in sight. Enter
accepts and leaves the cursor on the match, storing the query so Repeat Find and
a later empty-query Ctrl-F jump to the next one. Esc cancels and restores the
starting position.

The menu bar works the MS-EDIT way. Press F10 to activate it, then press a
menu's highlighted letter (F, E, S, B, V, O, H) to open it, or use the arrow
keys and Enter. Alt+letter opens a menu in one step, but note that many desktop
terminal emulators capture Alt+letter for their own menus, so F10 then a letter
is the reliable path. Inside an open menu, each item's highlighted letter runs
it.

### vi keys

Press F2 to switch to the vi personality. NORMAL mode supports `h j k l`, `0 ^
$`, `w b e`, `gg G`, `f F t T`, `; ,`, `{ } ( )`, `% H M L |`, counts such as
`3j`, the operators `d c y` with motions (`dw`, `d$`, `dt`), `cc dd yy`, `x`,
`p`, `u`, and `i a A I o O` to insert. `Ctrl-Home` and `Ctrl-End` jump to the
first and last line, the same as `gg` and `G`, and work in insert mode too. `ZZ`
writes and quits, `ZQ` quits without writing. The `:` line runs
`w q wq q! qa wqa cq`, `:N`, `:set number` / `:set nonumber`, and `:set wrap` /
`:set nowrap`. `/` searches and `n` repeats. Substitution follows the usual vi
forms: `:s/old/new/`, `:s/old/new/g` for every match on the line, a leading
range such as `:%s/old/new/g` for the whole file, and `:g/pat/...` / `:v/pat/...`
to run a command on matching (or non-matching) lines. In the modeless
personality, Ctrl-R walks the matches one at a time and asks before each.

## Draw mode (ASCII art and maps)

Draw mode turns vedit into a 2D canvas for maps, box diagrams, and block art,
in the spirit of tools like DuhDraw. Toggle it with the **Insert** key (in either
personality), the Options menu, or the vi `:draw` command. The status line shows
`-- DRAW --` while it is on, and leaving it restores normal insert editing.

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

vedit has a small built-in highlighter, aimed at editing MUD source and scripts
over the connection. It is not a full language engine. One generic C-family
tokenizer covers C, C++, and LPC, and a second covers shell-style scripts. It
colors keywords, types, strings, character and number literals, line and block
comments (a block comment may span lines), preprocessor lines, and function calls.

The language is chosen from the file extension: `.c .h .cc .cpp .cxx .hpp .hh
.lpc .i` use the C-family rules, and `.sh .bash` use the shell rules. Highlighting
is on by default when the type is recognized, and files with no match are left
plain. Toggle it from the View menu, or with the vi `:syntax` command: `:syntax
off`, `:syntax on`, or `:syntax c` / `:syntax lpc` / `:syntax sh` to force a
language. The colors stay in the 16-color range so they render on limited clients.

To add a dialect, extend the keyword and type tables and the extension map in the
syntax section of `vedit.c`; the tokenizer itself is reused.

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

## What this port keeps and what it drops

The lumi editor sits on about ten libraries. vedit keeps the text buffer with
undo and redo, the modeless and vi personalities, the MS-EDIT chrome (menu bar,
frame, scrollbars, dialogs), find and replace, selection and an internal clipboard,
goto-line, multiple buffers, a hex view, a 2D/block draw mode, and lightweight
syntax highlighting. It replaces the drawing stack with
a self-contained ANSI renderer over the io vtable, and the keyboard decoder with
a compact one that covers UTF-8 text, control keys, arrows, navigation keys,
function keys, CSI modifiers, Alt+letter, and bracketed paste.

It drops, as overworked for a primitive-terminal editor:

- lumi's language engine (replaced by a small built-in highlighter, below),
- the build / compile / make commands and the quickfix error list,
- the config file,
- mouse input,
- the differential compositor and terminfo capability lookup.

The renderer repaints only the rows that changed, and within a row only the
columns between the first and last change, and emits color escapes only when the
pen changes. With `--scroll` it also moves the text area with the terminal
scroll region instead of repainting it. All of this keeps a redraw small on a
slow link.

### Ways to make it smaller or larger

If you want an even leaner build, the hex view and multiple-buffer support are
the next candidates to remove; each is self-contained. For a smaller input
surface, drop the vi personality.

If you embed over raw telnet rather than a cooked pty, the host (not vedit)
should handle telnet IAC negotiation and read the window size from NAWS, then
pass it to `vedit_set_size()`. A read-only pager mode and a hard line-length cap
for very small clients are reasonable additions at the editor layer.
