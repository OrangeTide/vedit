# vedit

A text editor for terminals, in one C file. It looks and works like the
MS-DOS EDIT program: a menu bar, a framed text area, and keys that need no
mode. Press F2 and it becomes a vi.

It draws with a fixed subset of ANSI codes and needs no terminfo, so it runs
over telnet or ssh to a plain terminal, a MUD client, or the Windows
console. The same source builds gvedit, the editor in its own window for a
desktop without a terminal.

![Editing a C file, with syntax highlighting and the MS-EDIT chrome](docs/shot-edit.png)

## Building

The only requirement is a C11 compiler on a POSIX system, or mingw-w64 for
Windows. There are no library dependencies.

```sh
make                    # build, linked as ./vedit
make install            # install to ~/.local/bin (PREFIX overrides)
make static             # static musl build, for dropping onto a server
make gvedit             # the window build (needs SDL3)
```

Prebuilt binaries are on the
[releases page](https://github.com/OrangeTide/vedit/releases): static Linux
builds for x86_64 and aarch64 and a Windows x86_64 console build.

## Quick start

```sh
vedit notes.txt
```

Type to insert text. The arrow keys, Home, End, PgUp, and PgDn move around.
Hold Shift while moving to select.

| Key | Action |
|---|---|
| Ctrl-S | save |
| Ctrl-Q | quit (asks about unsaved changes) |
| Ctrl-Z, Ctrl-Y | undo, redo |
| Ctrl-C, Ctrl-X, Ctrl-V | copy, cut, paste |
| Ctrl-F, Ctrl-R | find, replace |
| F10 | open the menu bar |
| F1 | show the keys |
| F2 | switch between the EDIT keys and the vi keys |

## Manual

The manual is in `docs/`, in reading order:

1. [Getting started](docs/01-getting-started.md): the screen, opening,
   saving, quitting, the menu bar, help
2. [Editing](docs/02-editing.md): moving, selecting, clipboard, undo,
   find and replace, with the EDIT keys
3. [Files and buffers](docs/03-files.md): the file browser, several files
   at once, line endings, crash recovery, large files
4. [View settings](docs/04-view.md): line numbers, word wrap, tabs and
   indentation, color schemes, syntax highlighting
5. [Configuration](docs/05-configuration.md): the config file and every key
6. [The vi keys](docs/06-vi.md): the complete list of what the vi
   personality implements, for readers who do not know vi
7. [Tools](docs/07-tools.md): compile, make, and run; terminals inside the
   editor; the pane; tags; version control
8. [Other views](docs/08-views.md): draw mode, color text art, CSV tables,
   the hex dump, mail
9. [Terminals and platforms](docs/09-terminals.md): frame styles, color
   depth, slow links, gvedit, Windows
10. [Advanced](docs/10-advanced.md): syntax grammars, themes, other version
    control systems, embedding in a host, build-time knobs
11. [Showcase](docs/11-showcase.md): examples and pictures

[Developing](docs/developing.md) covers tests, CI, screenshots, and
releases. `man vedit` is the terse reference for options, environment
variables, and files.

## How it was made

This project was written primarily with Claude, mostly Claude Opus 4.8 and
Claude Fable 5.1, working from the author's design decisions and review in
Claude Code. The tests, screenshots, and documentation were produced the same
way.
