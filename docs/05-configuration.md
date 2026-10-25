# Configuration

The config file sets the startup defaults: the keys, the look, the editing
options, and the per-project commands.

## The file

| Platform | Location |
|---|---|
| Unix | `$XDG_CONFIG_HOME/vedit/config`, normally `~/.config/vedit/config` |
| Windows | `%APPDATA%\vedit\config` |
| a build with `-DVEDIT_NO_XDG` | `~/.veditrc` |

`$VEDIT_CONFIG` names a different file. On the command line, `--config
FILE` names one and `--no-config` skips it. A missing file is ignored. A
file with a syntax error (a line without `=`, or an unclosed `[` or
quote) is dropped as a whole; only an explicit `--config` path reports
a missing or broken file.

The easiest way to start is **Options > Edit Config** (`:config` in the vi
keys). It opens the file in a buffer and, when the file does not exist
yet, fills the buffer with a commented template of the common keys.
Nothing is written until you save. Saving the config file applies it at
once: a key that is present is applied, a key that was removed keeps
its last value until restart, and `mail.dir` is read at startup only.

**Options > Reload Config** (`:reload`) re-reads the file after it was
changed by other means.

## The format

The file is in gitconfig style: `[section]` headers, `key = value` lines,
and `#` or `;` comments.

```ini
[table]
    width = 12           # a comment
```

- Keys are dotted, so `ui.scheme = black` with no section header also
  works.
- Booleans accept `on`/`off`, `yes`/`no`, `true`/`false`, or `1`/`0`.
- A value containing `#` or `;`, or ending in a space, must be in double
  quotes, or the reader takes the rest as a comment.
- A section with a name, such as `[command "c"]`, sets keys of the form
  `command.c.compile`.

## The keys most people set

```ini
[ui]
    scheme = black       # dos | black | plain, or a theme of your own
    number = on          # line numbers
    wrap = on            # word wrap

[edit]
    mode = vi            # the keymap at startup: vi | msedit (the EDIT keys)
    ignorecase = on      # searches ignore case

[indent]
    expand = on          # indent with spaces rather than tabs
```

## Precedence

From weakest to strongest: the built-in default, the config file, a
`VEDIT_*` environment variable, a command-line flag, and finally an
embedding host's own settings. So `--dec` on the command line beats
`ui.box` in the file.

A few per-project keys can be set from the environment, which suits a
per-project shell or an `.envrc`. The variable is `VEDIT_` followed by the
key in upper case with each `.` turned into `_`:

| Key | Variable |
|---|---|
| `tags.file` | `VEDIT_TAGS_FILE` |
| `cc.file` | `VEDIT_CC_FILE` |
| `edit.swapdir`, `edit.backupdir` | `VEDIT_EDIT_SWAPDIR`, `VEDIT_EDIT_BACKUPDIR` |
| `command.<lang>.<name>` | `VEDIT_COMMAND_<LANG>_<NAME>` |
| `vcs.<name>.<cmd>` | `VEDIT_VCS_<NAME>_<CMD>` |

Those keys, `VEDIT_MOUSE` (chapter 2), and the rendering flags, which
have their own variables listed in chapter 9, are the only ones that
read the environment.

## Every key

The values shown are the defaults in a terminal. gvedit draws its own
window, so it fixes the frame at UTF-8, the colors at 256, and the
scroll path on, whatever `ui.box`, `ui.colors`, `ui.scroll`, or their
environment variables say; it starts with `ui.clipboard = on`.

### `[ui]`: the look

| Key | Default | Meaning |
|---|---|---|
| `scheme` | `dos` | `dos`, `black`, `plain`, or a `[theme]` name (chapter 10) |
| `statusline` | `plain` | `plain` or `powerline`, the status line style (chapter 4) |
| `separator-left` | U+E0B0 | the glyph after each left-hand powerline segment; empty for none. Under ASCII box drawing the default is a space |
| `separator-right` | U+E0B2 | the glyph before each right-hand segment |
| `number` | `off` | line-number gutter |
| `wrap` | `off` | word wrap |
| `box` | `utf8` under a UTF-8 locale, else `ascii` | frame style: `utf8`, `dec`, or `ascii` (chapter 9) |
| `colors` | `256` when the terminal advertises it, else `16` | `256` or `16` (chapter 9) |
| `scroll` | `off` | use the terminal scroll region when scrolling (chapter 9) |
| `clipboard` | `off` | also send every copy and cut to the terminal clipboard (chapter 2) |
| `tabs` | `on` | mark hard tabs with a guide glyph |
| `paneheight` | `0` | rows for the pane under the text, 0 to 500; 0 is a third (chapter 7; builds with terminals) |
| `mouse` | `on` | mouse reporting (chapter 2) |

### `[keys]`: keybinding modes

| Key | Default | Meaning |
|---|---|---|
| `modes` | `msedit vi` | the single-word keymaps **F2** steps through, in order |

`F2` cycles through the modes named here, wrapping at the end. The two built-in
names are `msedit` (the EDIT keys) and `vi` (the vi keys); the default list
gives the plain `F2` toggle between them. `edit.mode` names which of these the
editor starts in.

#### Custom keymaps

A mode name that is not built in takes its keys from a `[keys "name"]` section.
Such a mode is a first-class editing mode: it joins the `F2` cycle beside
`msedit` and `vi`, `edit.mode` can start the editor in it, and `F1` shows its
own key list. A config file with its bindings and `help` lines is a complete
mode, installable on its own. The shipped `examples/wordstar.conf` turns vedit
into a WordStar editor this way; pull it in with an `[include]` and add its
name to `modes`.

Such a mode is modeless: a printable key inserts text, and nothing else is a
command until a `bind` line gives it one, so the mode starts empty. Each line
is:

```ini
[keys "wordstar"]
    bind = ^E up              # one key -> an action
    bind = ^K B block-begin   # a chord: a prefix key, then a second key
    bind = ^K 0 marker-set 0  # an action may take a number
```

A key is `^X` for Ctrl plus a character, a single character, or a named key:
`enter`, `esc`, `tab`, `bksp`, `del`, `space`, `home`, `end`, `pgup`, `pgdn`,
`up`, `down`, `left`, `right`, `f1` to `f12`. In a chord the second key is
matched whether or not Ctrl is held. An action the build does not provide is
skipped, so a keymap may list keys for features that are not present yet. Note
that a terminal reports Ctrl-J, Ctrl-M, and Ctrl-I as Enter, Enter, and Tab, so
bind those three by their key names, not as `^J`, `^M`, or `^I`.

A binding can also run a short program instead of a single action, so one key
performs several steps. Wrap the program in braces as the bind target; its
commands are the same action names, separated by `;` or a newline, and run in
order:

```ini
[keys "wordstar"]
    bind = "^Y { line-end ; delete-to-bol }"   # clear the line's text
    bind = "^N { line-end ; newline }"         # open a blank line below
```

Quote the whole value. The config file treats `;` and `#` as the start of a
comment, so a program that uses them must sit inside double quotes, which the
parser strips before reading the program. Inside the program, braces group an
argument that holds spaces and a backslash escapes the next character. A
program stops at the first command that opens a prompt or quits (`find`,
`save`, `quit`, and the like), so put such a command last. An unknown command
name is reported and stops the program.

#### Values, variables, and regions

A program's commands pass values, not only text. A bare or braced word is a
string, `$name` is a variable, and `[ ... ]` runs a bracketed program and uses
its value. A `$name` or a `[program]` stands alone as one whole argument; the
characters `[`, `]`, `$`, and `"` stay reserved inside a plain word, so brace
or escape one to use it as literal text. A double quote is reserved for later
use and is an error for now.

`set NAME VALUE` binds a variable for the rest of the run; variables do not
persist between keypresses. A position is a row and a column: `cursor` is the
current cursor, and `pos ROW COL` is an explicit one, both numbers 1-based as
in the status bar. A region names a span: `region line N1 N2` is whole rows N1
through N2 (1-based, inclusive), and `region char P1 P2` is the run between two
positions. `$clip` is the clipboard as a region.

The editing verbs take two forms. With no argument a verb acts on the selection
or a default around the cursor, exactly as the plain action names do. Given a
region it acts on that span:

- `delete REGION` removes the span, and `delete.` also pushes it onto the kill
  stack.
- `copy REGION` grabs the span into the clipboard.
- `put VALUE at POS` inserts a string or a region at a position; the word `at`
  is optional.
- `replace REGION` removes the span and inserts the clipboard where it began.

For example, a key that copies the first three lines and places them at the
top of the file:

```ini
[keys "demo"]
    bind = "^D { copy [region line 1 3] ; put $clip at [pos 1 1] }"
```

A `help` line adds a row to the F1 key-bindings screen shown while the keymap is
active, so the file documents its own keys:

```ini
[keys "wordstar"]
    help = "WordStar keys"
    help = "  ^E up   ^X down   ^S left   ^D right"
```

A keymap with no `help` lines shows only a short notice on F1, so writing good
help is the keymap author's job.

A few editor-wide keys stay fixed in every mode, so a keymap does not bind them:
F2 (cycle modes), F6 (other pane), F8 and Shift-F8 (next and previous buffer),
Ctrl-W (the pane prefix), Ctrl-] (jump to the tag under the cursor), the build
keys (chapter 7), F10 and Alt+letter (the menu bar), and the mouse. Everything
else is the mode's to claim. The built-in
`msedit` and `vi` keys are still compiled in rather than written as config, so
they are the one thing a keymap cannot yet redefine.

The actions are:

- Motion: `up`, `down`, `left`, `right`, `word-left`, `word-right`,
  `line-begin`, `line-end`, `file-top`, `file-bottom`, `page-up`, `page-down`,
  `screen-top`, `screen-bottom`, `scroll-up`, `scroll-down`, `jump-back`.
- Editing: `newline`, `split-line`, `tab`, `delete-left`, `delete-right`,
  `delete-word-right`, `delete-line`, `delete-to-eol`, `delete-to-bol`,
  `overtype-toggle`, `reflow`, `wrap-toggle`, `undo`, `redo`, `copy`, `cut`,
  `paste`, `kill-paste`.
- Capture: each removing verb has a twin whose name ends in a dot, such as
  `delete-to-eol.` or `delete-line.`, that removes the same text and also pushes
  it onto the kill stack. The plain verb discards; the dotted verb keeps. The
  kill stack is separate from the clipboard, so a capture never disturbs a
  `copy`, and repeated captures stack up with the newest on top. `kill-paste`
  inserts the top entry at the cursor.
- Format: `center-line`, `left-margin`, `right-margin`. The margins take the
  cursor's column; reflow and centering sit between them, and the right margin
  is also the type-time wrap width (`textwidth`).
- Markers: `marker-set N`, `marker-goto N` (N is 0 to 9).
- Blocks: `block-begin`, `block-end`, `block-goto-begin`, `block-goto-end`,
  `block-copy`, `block-move`, `block-delete`, `block-write`, `block-hide`,
  `block-column-toggle`. A block is two marks set independently of the cursor;
  copy and move act on it at the cursor, and write saves it to a file.
- Files and search: `find`, `find-next`, `replace`, `goto-line`, `symbol`,
  `file-insert`, `save`, `save-as`, `save-exit`, `quit`, `help`.

A complete WordStar keymap ships in `examples/wordstar.conf`. Pull it in with an
`[include]`, then add its name to the cycle:

```ini
[include]
    path = /path/to/examples/wordstar.conf
[keys]
    modes = msedit wordstar
```

A smaller `examples/emacs.conf` ships the same way, a simple subset of the
GNU Emacs keys. Meta (Alt) bindings are left out, since vedit has none and Alt
plus a letter opens the menu bar.

### `[include]`: merge another file

| Key | Default | Meaning |
|---|---|---|
| `path` | | read another config file in at this point |

A relative `path` resolves against the including file's directory. The line
takes effect where it appears, so later settings in the including file win.

### `[edit]`: editing

| Key | Default | Meaning |
|---|---|---|
| `mode` | `msedit` | the keymap at startup: `vi` or `msedit` |
| `autoindent` | `on` | new lines copy the previous indent |
| `ignorecase` | `off` | searches ignore case |
| `shiftwidth` | `0` | columns for vi `>>` and `<<`, 0 to 32; 0 is one tab stop |
| `textwidth` | `79` | where Reflow Paragraph, `gq`, and typing with Word Wrap on break lines, 0 to 500; 0 is the window width (chapter 2) |
| `tabstop` | `8` | the interval between tab stops, 1 to 256 |
| `tabstops` | | a ruler of stops, such as `5 9 17` |
| `swap` | `on` | keep the crash-recovery journal (chapter 3) |
| `swapdir` | | where journals go; empty is beside the file |
| `mmap` | `on` | map files instead of reading them (chapter 3) |
| `backup` | `off` | keep the previous version as a `~` file on save |
| `backupdir` | | where backups go; empty is beside the file |
| `formatonsave` | `off` | run the language's formatter before each save (chapter 7) |

### `[indent]`: tabs or spaces

| Key | Default | Meaning |
|---|---|---|
| `expand` | `off` | `on` indents with spaces |
| `indent.<lang>.expand` | | the same for one language, named as in the table in chapter 4, such as `[indent "sh"]` |

### `[art]` and `[table]`: the views (chapter 8)

The `[art]` keys apply to a build with terminals.

| Key | Default | Meaning |
|---|---|---|
| `art.view` | `on` | open `.ans` files in the art view |
| `art.width` | `0` | grid columns, 80 to 1024; 0 takes it from the file |
| `table.view` | `on` | open `.csv`, `.tsv`, and `.tab` files in the table view |
| `table.header` | `on` | line 1 is a frozen header row |
| `table.width` | `10` | the default column width, 1 to 200; 0 means 10 |

### `[syntax]`, `[gitcommit]`, `[color]`, `[words]`, `[state]`: highlighting

| Key | Default | Meaning |
|---|---|---|
| `syntax.enable` | `on` | highlight recognized file types |
| `syntax.<ext>` | | map an extension or file name to a language, such as `syntax.mn = mini` |
| `gitcommit.subject` | `50` | mark a commit subject past this column |
| `gitcommit.body` | `72` | and body lines past this one |

Chapter 10 describes the `[color]`, `[words]`, and `[state]` sections that
define a language.

### `[tags]`, `[cc]`, `[command]`, `[error]`: the tools (chapter 7)

| Key | Default | Meaning |
|---|---|---|
| `tags.file` | | a ctags index; otherwise a `tags` file beside the buffer |
| `cc.file` | | a `compile_commands.json`, for the include paths Open Header searches |
| `command.terminal` | `on` | run Compile, Make, and Run in a terminal buffer |
| `command.split` | `on` | show that terminal in the pane under the file |
| `command.<lang>.compile` | | the per-language commands, in a `[command "c"]` section |
| `command.<lang>.build` | | |
| `command.<lang>.run` | | |
| `command.<lang>.run.interactive` | `off` | run on the real terminal rather than in a buffer; `compile.interactive` and `build.interactive` do the same for those |
| `command.<lang>.format` | | a filter that reformats the buffer |
| `error.pattern` | | an extra build-error pattern; may be repeated |

### `[vcs]`: version control (chapter 7)

| Key | Default | Meaning |
|---|---|---|
| `vcs.enable` | `on` | branch and change mark on the status line |
| `vcs.git.*` | built in | the git command lines, in a `[vcs "git"]` section |
| `vcs.<name>.*` | | another system's command lines (chapter 10) |

### `[insert]`

| Key | Default | Meaning |
|---|---|---|
| `dateformat` | | a `strftime` pattern that Insert > Date starts on |

### `[mouse "<glob>"]`

| Key | Meaning |
|---|---|
| `enable` | mouse on or off for terminals whose `$TERM` matches the glob. Later sections win |

### `[mail]` (chapter 8)

| Key | Meaning |
|---|---|
| `dir` | a Maildir++ tree. Setting it enables the Mail menu at the next start; a leading `~/` is expanded |
| `from` | the `From:` line of new messages |
| `sendcmd` | the command Mail > Deliver Outbox runs per queued message, reading it on standard input, such as `msmtp -t` |
| `maxattach` | the largest attachment to send, in megabytes (default 25) |

### `[theme "<name>"]`

A color scheme of your own, selected with `ui.scheme`. Chapter 10 lists
the fields.
