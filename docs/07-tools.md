# Tools

Compiling and running the file you are editing, terminals inside the
editor, the pane under the text, jumping to definitions, and version
control.

## Build commands

vedit can compile, build, and run the current file. The commands are per
language, taken from the config, so each file type has its own lines:

```ini
[command "c"]
    compile = gcc -Wall -c $(filename) -o $(filenoext).o
    build   = make
    run     = ./$(filenoext)
```

The section name is the syntax language of the file (chapter 4), such as
`c` or `sh`. A command runs in the file's directory and may use these
variables:

| Variable | Expands to |
|---|---|
| `$(file)` | the file path |
| `$(filename)` | the base name, with extension |
| `$(filenoext)` | the base name, without extension |
| `$(fileext)` | the extension, without the dot |
| `$(dir)` | the directory holding the file |

Once a command is configured for the file's language, the **Compile** and
**Run** menus appear:

| Key | Command | Action |
|---|---|---|
| Alt-F9 | Compile | run the `compile` command |
| F9 | Make | run the `build` command |
| Ctrl-F9 | Run | run the `run` command |
| Alt-F5 | View Output | go to the build terminal, or reopen the output |
| F4 | Next Error | move to the next diagnostic |
| Shift-F4 | Prev Error | move to the previous diagnostic |

A modified file asks `Save before building? (y)es (n)o` first.

The command runs in a terminal buffer (below) shown in the pane under the
file, labelled Compile, Make, or Run, so the output appears live and in
color, and a program that prompts can be answered. The buffer stays after
the command exits, with the exit status on its status line, and the next
build replaces it. `command.split = off` makes the terminal fill the frame
instead, and `command.terminal = off` captures the output into a plain
output pane with no terminal.

A command with a sibling key `run.interactive = on` (or `compile.`,
`build.`) runs on the real terminal instead, for a program that needs the
whole screen. The editor hides until it exits.

## Diagnostics

Output lines in the gcc and clang form (`file:line:col: message` or
`file:line: message`, including a Windows path with a drive letter) become
jump targets. F4 and Shift-F4 step through them from the editor, wrapping
at the ends, and Enter on a line in the output pane goes to it. A
diagnostic in another file opens or switches to that file.

The word after the location is read as the severity. Errors are red in
the pane, warnings yellow, notes cyan, and the status line counts errors
and warnings separately. When a build ends with errors the cursor lands on
the first one; a clean build leaves the terminal in front. F4 steps
through errors only, or through warnings when there are no errors. A line
in the location form with no recognized severity word, such as a linker
error, counts as an error.

For another format, such as MSVC's, add `error.pattern` keys. Each is a
regular expression with group 1 the file, group 2 the line, and an
optional group 3 the column. They are tried before the built-in forms, in
order, up to sixteen of them:

```ini
[error]
    pattern = ^([^(]+)\(([0-9]+),([0-9]+)\):
```

## Formatting

A `format` key names a filter for the file type. vedit feeds it the whole
buffer on standard input and replaces the buffer with its standard output,
which is how `gofmt`, `clang-format`, and `prettier` work by default:

```ini
[command "c"]
    format = clang-format
[command "go"]
    format = gofmt
```

**Edit > Format** (`:format`) runs it. The change is one undo step and the
cursor keeps its line. A formatter that exits non-zero or prints nothing
leaves the buffer untouched. `edit.formatonsave = on` runs it before every
save; a file type with no formatter saves unchanged.

## Terminal buffers

A buffer can be a live terminal running a shell or any other program,
drawn inside the editor frame. **Terminal > New Terminal**, or
`:terminal` in the vi keys, opens one running your login shell.
`:terminal make` or `:terminal htop` runs that command instead. The
command is split on spaces and run directly, without a shell. For pipes
or quoting, write `sh -c '...'`.

![A terminal buffer running a build inside the editor frame](shot-term.png)

The terminal is a real pseudo-terminal with a built-in emulator. It
understands 16, 256, and 24-bit colors, cursor movement, and the window
title the program sets, which becomes the buffer's frame label.
Scrollback holds twice the visible height.

While a terminal buffer, or a terminal in the pane, has focus, every key
goes to the program, including F1, F8, F10, and Alt+letter. **Ctrl-W** is the prefix for
talking to the editor instead:

| Key | Action |
|---|---|
| Ctrl-W m | open the menu bar |
| Ctrl-W : | an ex command line |
| Ctrl-W w | the next buffer |
| Ctrl-W W, Ctrl-W p | the previous buffer |
| Ctrl-W 1 to Ctrl-W 9 | switch to that buffer |
| Ctrl-W n | open another terminal |
| Ctrl-W r | copy the terminal's output into a new text buffer |
| Ctrl-W R | copy it into a new art-view buffer, keeping the colors (chapter 8) |
| Ctrl-W c, Ctrl-W q | close the terminal. Closing the last buffer quits |
| Ctrl-W +, Ctrl-W - | a row taller or shorter, for a terminal in the pane |
| Ctrl-W Ctrl-W | send a literal Ctrl-W to the program |

When the program exits the status line shows `[process exited N]` and the
buffer waits for Ctrl-W q.

`:!cmd` runs a shell command the same way, in a terminal labelled with
the command, or in the output pane when `command.terminal` is off.

## The pane

One buffer can be shown in a pane under the text, a third of the text
area high, so a build, a shell, a log, or a second file stays in view
while you edit.

![A build log in the pane under the file being edited](shot-pane.png)

From a text buffer, Ctrl-W followed by one of these:

| Key | Menu item | Action |
|---|---|---|
| s | Terminal > Split Terminal | open a shell in the pane, or focus the one there |
| b | Terminal > Buffer in Pane | show this buffer in the pane with the previous one above; again to undo |
| w, W, p, j | | move the focus into the pane, or back out |
| c, q | Terminal > Close Pane | close the pane. A terminal is closed, a text buffer kept |
| r, R | Terminal > Repost as Text, as Art | copy the pane terminal's output into a new buffer |
| +, - | Terminal > Taller Pane, Shorter Pane | change the height a row at a time |
| : | | an ex command line, in the vi keys |

In the vi keys, `:split` opens a shell in the pane and `:split cmd` runs
a command there, `:sbuffer [N]` shows this buffer or buffer N there, and
`:set paneheight=N` sets its height. `ui.paneheight` in the config sets
the default.

A new pane leaves the focus in the file. The pane's title is drawn
reversed while the focus is there. With a text buffer there, Ctrl-W w switches between the two buffers and
editing follows the focused one. The pane holds one thing: opening another
there hands a terminal back as a plain terminal buffer, or closes it when
its program has exited. F8 still reaches the pane's buffer, which fills
the frame while it is current. A window under eight text rows hides the
pane.

## Symbols and tags

**Ctrl-T** (Search > Go to Symbol, or `:symbol` in the vi keys) lists the
definitions in the current buffer and jumps to the one you choose. `:symbol
text` lists only the names containing `text`. The scan finds top-level
functions, `struct`, `union`, `enum`, and `class` tags, `} Name;`
typedefs, and `#define` lines. Each row shows the name, its kind, and the
line. The patterns cover
C, C++, LPC, and shell functions, with no tags file needed.

With a **tags file** the list also covers the rest of the project. vedit
reads a `tags` file in the current buffer's directory, or the one named by
`tags.file` in the config, in the Exuberant or Universal ctags format
(`ctags *.c` makes one). Rows from the file are marked `[tags]` and show
the file they point into; choosing one opens that file at the definition.

| Key | Action |
|---|---|
| Ctrl-] | jump to the definition of the identifier under the cursor. A definition in the current buffer needs no tags file; others do |
| `:tag NAME` | jump to a named tag. Several matches open the picker on them |
| `:tag /pat` | the picker, filtered to names containing `pat` |
| Search > Pop Tag, `:pop`, Ctrl-T in the vi keys | return to where you jumped from |

Each jump into another file pushes the previous position on a stack, so
Pop Tag retraces them.

## Opening a header

**Search > Open Header**, or `gf` in the vi keys, opens the file named on
an `#include` line, or the name under the cursor. It looks for the path as
written, then in the current file's directory for the `"..."` form, then
in the include directories of the current file.

Those directories come from a clang compilation database: point `cc.file`
at a `compile_commands.json` and vedit reads the `-I`, `-isystem`, and
`-iquote` flags of the entry whose `file` matches the buffer. The compile
command itself is not run. Without `cc.file`, only the path as written and
the current file's directory are searched.

## Version control

When the file sits in a git working copy, the status line shows the branch
as `git:main`, with `*` added when the file has uncommitted changes and
`?` when git does not track it. It is checked when a file is opened,
saved, or switched to. `vcs.enable = off` turns it off.

A **VCS** menu appears while the file has version control. Its mnemonic is
`%`, so F10 then `%` opens it.

- **History** (`:log`) lists the commits that touched the file, newest
  first. Choosing one opens the commit's diff of the file in a read-only
  buffer named `file@rev`, with diff highlighting. `RO` shows on the
  status line and edits are refused. Close it with `:bd` or leave it in
  the buffer list.
- **Blame** (`:blame`) opens the blame output read-only as `file@blame`,
  each line led by the revision that last changed it, with the cursor on
  the line you were on. Enter there opens that commit's diff.
- **Commit** (`:commit`) commits the current file on its own. The file
  must be saved first. It opens a message buffer named `file@commit` with
  the file named in `#` comment lines. Write the message, then choose
  Commit again (or `:commit`) to send it. On success the message buffer
  closes and the status line shows the commit's first output line.
  `:bd!` abandons a message.

![The history picker over a file in this repository](shot-history.png)

Chapter 10 shows how to use another version control system.
