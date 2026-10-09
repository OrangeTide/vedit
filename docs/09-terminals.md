# Terminals and platforms

How the editor draws on different terminals, the flags and environment
variables that control it, the window build, and Windows.

## Command line

```
vedit [options] [file]
```

| Option | Effect |
|---|---|
| `--utf8`, `--dec`, `--ascii` (`-a`) | the frame style, below |
| `--256color`, `--16color` | the color depth, below |
| `--scroll`, `--no-scroll` | the scroll-region fast path, below |
| `--config FILE`, `--no-config` | the config file (chapter 5) |
| `--scale N` | gvedit only: draw the font N times its size |
| `-h`, `--help`, `-V`, `--version` | usage and version |

## Frame style

The frame, scrollbars, and menus are drawn in one of three ways:

| Flag | Variable | Style |
|---|---|---|
| `--utf8` | `VEDIT_BOX=utf8` | Unicode box-drawing |
| `--dec` | `VEDIT_BOX=dec` | DEC VT100 line-drawing, single-byte, for terminals that have the VT100 alternate character set but not UTF-8 |
| `--ascii` | `VEDIT_BOX=ascii` or `VEDIT_ASCII` | plain `+ - | ^ v # :` |

The default is UTF-8 under a UTF-8 locale and ASCII otherwise. Config:
`ui.box`.

## Color depth

| Flag | Variable | Colors |
|---|---|---|
| `--256color` | `VEDIT_COLORS=256` | the xterm 256-color palette |
| `--16color` | `VEDIT_COLORS=16` | the 16 ANSI colors, sent as the classic SGR codes |

The default is 256 when `COLORTERM` contains `truecolor` or `24bit`, or
`TERM` contains `256color`, and 16 otherwise. At 16 colors every color,
including the syntax colors, maps to the nearest ANSI color. Config:
`ui.colors`.

## Slow links

Only the rows that changed are sent on each redraw, and within a row only
the columns between the first and last change. On the `black` scheme a
run of trailing blanks is cleared with one erase-to-end-of-line.

`--scroll`, or `VEDIT_SCROLL=1`, or `ui.scroll = on`, also uses the
terminal's scroll region when the view scrolls or a line is inserted, so
only the one newly exposed row is repainted. It is off by default and
needs VT100 scroll-region support. `docs/demo.html` shows the byte
counts with and without it.

## Small windows

A narrow menu bar drops titles that would collide with the Help label;
those menus still open by their Alt mnemonics. Below a minimum usable
size the screen shows `window too small` until the window grows.

## gvedit

gvedit is the editor in a window of its own, for a desktop without a
terminal emulator and for Windows and macOS. It draws the screen with a
bitmap font and needs SDL3 at build and run time.

```sh
make gvedit
./gvedit --scale 2 file.txt
```

`--scale` zooms the font, 1 to 8; by default it follows the display
scale. Resizing the window resizes the editor, never the
font. The mouse, paste, terminal buffers, and the clipboard through OSC 52
all work as in a terminal. Closing the window asks about unsaved changes
as File > Exit does; closing it again while that question is pending ends
the run.

`VEDIT_GUI_TRACE=file` saves the byte stream the editor feeds the window.

## Windows

The same source builds a native Windows console program with mingw-w64
(see `docs/developing.md`). It runs in Windows Terminal, the classic
console, and under wine. Every key and the mouse work as on Unix, and
file names are UTF-8.

On a real Windows console the editor sends its ANSI stream through the
console's virtual-terminal processing; pass `--256color` or set
`VEDIT_COLORS=256` for 256 colors. Under wine it
draws through the console API instead, with the console's sixteen colors,
each color mapped to the nearest entry of the console's own color table.
`VEDIT_WIN_RENDER=console` or `=vt` forces one or the other.

The config file lives at `%APPDATA%\vedit\config`. The build commands,
`:!`, the formatter, and the version control commands run through
`cmd.exe`, so write them in its syntax; `$(file)` and the other variables
expand as usual. Mail works on a Maildir given as a Windows path.

Terminal buffers and the pane run their program under a pseudo console
(ConPTY, Windows 10 1809 and later; older consoles report `failed to
start terminal`). `:terminal` starts `%COMSPEC%`, and the build commands
run through `cmd.exe /d /c`. Wine 10 supports the pseudo console as well.

gvedit on Windows needs the SDL3 development package for mingw and runs
with `SDL3.dll` beside it.

For debugging a console problem, `VEDIT_WIN_LOG=file` records the console
calls that fail and the translated input bytes, and `VEDIT_WIN_TRACE=file`
saves the raw output stream of the VT path. Under wine started from a
Unix terminal, a lone Escape does not reach the program.

## Environment variables

| Variable | Effect |
|---|---|
| `VEDIT_CONFIG` | the config file |
| `VEDIT_BOX`, `VEDIT_ASCII` | the frame style |
| `VEDIT_COLORS` | the color depth |
| `VEDIT_SCROLL` | the scroll-region fast path |
| `VEDIT_MOUSE=1` or `0` (also `on`, `off`, `yes`, `no`) | mouse reporting, overriding the config |
| `VEDIT_TAGS_FILE`, `VEDIT_CC_FILE`, and the other per-project keys | chapter 5 |
| `VEDIT_GUI_TRACE`, `VEDIT_WIN_LOG`, `VEDIT_WIN_TRACE`, `VEDIT_WIN_RENDER` | gvedit and Windows, above |
