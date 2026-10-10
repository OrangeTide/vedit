# View settings

The toggles in the View menu, tab stops and indentation, the color
schemes, and syntax highlighting. Each setting names its config key, for
making it the default (chapter 5), and the vi `:set` spelling is in
chapter 6.

## Line numbers

**View > Line Numbers** draws a line-number gutter on the left. Its width
follows the number of lines in the buffer, and the current line's number
is brighter. `NUM` shows on the status line while it is on.
Config: `ui.number`.

## Word wrap

**View > Word Wrap** folds long lines at the window width instead of
scrolling sideways. Lines break at word boundaries, a word wider than the
window is broken mid-word, and the continuation rows have a blank gutter.
The file is unchanged. Up and Down move by buffer line, Home and End go to
the start and end of the buffer line, and Left and Right move by
character. `WRAP` shows on the status line.
Config: `ui.wrap`.

Word wrap changes only the display. To break the lines in the file, see
Reflow Paragraph in chapter 2. With Word Wrap on, typing past the text
width breaks the line being typed as well.

## Text width

**View > Text Width** asks for the column at which Edit > Reflow
Paragraph and the vi `gq` operator break lines, and at which typing wraps
when Word Wrap is on. The default is 79. 0 means the window width.
Config: `edit.textwidth`, or `:set tw=N` for the session.

## Tabs

A tab character advances to the next tab stop, every 8 columns unless the
buffer has a ruler (below).

**View > Show Tabs** (on by default) marks the first column of every tab
with a faint guide glyph: an arrow in UTF-8 mode, `>` in DEC or ASCII mode.
Config: `ui.tabs`.

**View > Indent with Spaces** makes the Tab key insert spaces up to the
next stop instead of a tab character. It is per buffer.
A new buffer takes its default from the config, per language or globally:

```ini
[indent]
    expand = off         # off = tabs, on = spaces

[indent "c"]              # the language name from the table above
    expand = on          # buffers of this language indent with spaces
```

**Options > Tab Stops** gives the buffer a ruler: a list of columns such
as `5 9 17` where tabs stop, continuing past the last one at the usual
interval. The ruler changes how tabs are drawn, what Tab inserts when
indenting with spaces, and how Tabs to Spaces and Spaces to Tabs
convert. The file still holds plain tab characters. Enter `off` to clear
it. Config: `edit.tabstop` for the interval and
`edit.tabstops` for the list.

In the vi keys, `>>` and `<<` shift a line by one tab stop. The config
key `edit.shiftwidth = N` shifts by N columns instead.

## Auto indent

**View > Auto Indent** (on by default) starts each new line with the same
leading blanks as the line you pressed Enter on, copied as they are, tabs
or spaces. Config:
`edit.autoindent`.

## Line endings

**View > Line Endings** cycles the style written on save: `LF`, `CRLF`,
or `NUL`. Chapter 3 has the details.

## Mouse

**View > Mouse** turns mouse reporting on and off for the session.
Chapter 2 describes what the mouse does. Config: `ui.mouse`.

## Color schemes

**View > Color Scheme** cycles through three looks:

| Scheme | Look |
|---|---|
| `dos` | blue text area, the default |
| `black` | the text area on the terminal's own background, with the right border dropped so text reaches the last column |
| `plain` | monochrome, with reverse video for the bars |

The scheme covers the whole interface: menus, dialogs, prompts, and the
help screen. Config: `ui.scheme`. A config file can also define schemes of
its own (chapter 10).

## Syntax highlighting

**View > Syntax Highlight** colors the buffer according to its file type.
It is on by default, and a file whose type is not recognized is left
plain. The type comes from the file extension, or from the whole name for
files such as `config` and `COMMIT_EDITMSG`:

| Language | Files |
|---|---|
| C | `.c .h .cc .cpp .cxx .hpp .hh .lpc .i` |
| shell | `.sh .bash` |
| Markdown | `.md .markdown .mdown .mkd` |
| JavaScript | `.js .mjs .cjs` |
| HTML | `.html .htm .xhtml` |
| INI | `.ini .cfg .conf .gitconfig .editorconfig .veditrc .desktop .service`, and a file named `config` |
| git commit message | `COMMIT_EDITMSG`, `MERGE_MSG`, `SQUASH_MSG`, `TAG_EDITMSG`, `.gitmessage` |
| diff | `.diff .patch .rej`, and the buffer that History opens (chapter 7) |
| blame | the buffer that Blame opens (chapter 7) |

The C grammar colors keywords, types, strings, character and number
literals, comments, and preprocessor lines. The shell grammar also colors
`$var` and `${var}` inside double quotes. The JavaScript grammar handles
template literals that span lines. The HTML grammar colors tags,
attributes, comments, and entities, and colors the body of a `<script>`
tag as JavaScript. A fenced code block in Markdown whose info string names
a language the editor knows is colored as that language. A git commit
message has its subject marked past column 50 and body lines past 72
(`gitcommit.subject` and `gitcommit.body` change the widths), and `#`
lines are comments.

In the vi keys, `:syntax off` and `:syntax on` turn it off and on, and
`:syntax c` forces a language. The config file can map more extensions and define
new languages (chapter 10). Config: `syntax.enable`.
