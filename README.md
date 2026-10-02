# vedit

A single-file visual text editor for primitive terminals. vedit emulates a
modeless Microsoft EDIT personality and a vi personality. It draws through a
small cell grid that emits a fixed subset of ANSI control codes, with no
terminfo database and no differential compositor, so it runs over a telnet or
ssh link to a simple terminal emulator such as a MUD client.

vedit is a port of the editor from the lumi project, reduced to one source file
(`vedit.c`) plus a public header for embedding (`vedit.h`).

## Building

```sh
make                    # build ./vedit
make RELEASE=1          # optimized build
make static             # static musl build, for dropping onto a server
make install            # install to ~/.local/bin (override PREFIX)
```

The only requirement is a C11 compiler and a POSIX system. There are no library
dependencies.

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

### Modeless keys (the default, MS-EDIT style)

| Key | Action |
| --- | --- |
| arrows | move the cursor |
| Home / End | start / end of line |
| PgUp / PgDn | scroll by a screen |
| Enter | split the line |
| Backspace / Delete | delete left / right |
| Shift-arrows | extend a selection |
| Ctrl-C / Ctrl-X | copy / cut (Ctrl-C with no selection copies the line) |
| Ctrl-V | paste the internal clipboard |
| Ctrl-F | find (Enter repeats the last search) |
| Ctrl-L | go to a line number |
| Ctrl-Z / Ctrl-Y | undo / redo |
| Ctrl-S | save (prompts for a name if the buffer has none) |
| Ctrl-Q | quit (prompts if the buffer was modified) |
| F8 / Shift-F8 | next / previous open buffer |
| F10 | activate the menu bar (then a letter opens that menu) |
| Alt+letter | open a menu directly (File, Edit, ...) |
| F1 | show the key bindings |
| F2 | toggle vi keys |

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
`p`, `u`, and `i a A I o O` to insert. `ZZ` writes and quits, `ZQ` quits without
writing. The `:` line runs `w q wq q! qa wqa cq` and `:N`. `/` searches and `n`
repeats.

## Embedding in a host (for example a MUD)

vedit owns no file descriptors and installs no signal handlers of its own. All
terminal I/O passes through a read/write/select-style vtable (`struct
vedit_io`). A host fills that vtable over its own transport, drives the editor
with `vedit_run()`, and delivers window resizes with `vedit_set_size()`. Raw
mode and SIGWINCH belong to the command-line binding only; in an embedded host
the `begin`/`end` callbacks are a no-op.

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
frame, scrollbars, dialogs), find, selection and an internal clipboard,
goto-line, multiple buffers, and a hex view. It replaces the drawing stack with
a self-contained ANSI renderer over the io vtable, and the keyboard decoder with
a compact one that covers UTF-8 text, control keys, arrows, navigation keys,
function keys, CSI modifiers, Alt+letter, and bracketed paste.

It drops, as overworked for a primitive-terminal editor:

- syntax highlighting (the whole language engine),
- the build / compile / make commands and the quickfix error list,
- the config file,
- mouse input,
- the differential compositor and terminfo capability lookup.

The renderer repaints only the rows that changed and emits color escapes only
when the pen changes, which keeps a redraw small on a slow link.

### Ways to make it smaller or larger

If you want an even leaner build, the hex view and multiple-buffer support are
the next candidates to remove; each is self-contained. For a smaller input
surface, drop the vi personality.

If you embed over raw telnet rather than a cooked pty, the host (not vedit)
should handle telnet IAC negotiation and read the window size from NAWS, then
pass it to `vedit_set_size()`. A read-only pager mode and a hard line-length cap
for very small clients are reasonable additions at the editor layer.
