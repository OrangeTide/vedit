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
    mode = vi            # vi | modeless (the EDIT keys)
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
| `number` | `off` | line-number gutter |
| `wrap` | `off` | word wrap |
| `box` | `utf8` under a UTF-8 locale, else `ascii` | frame style: `utf8`, `dec`, or `ascii` (chapter 9) |
| `colors` | `256` when the terminal advertises it, else `16` | `256` or `16` (chapter 9) |
| `scroll` | `off` | use the terminal scroll region when scrolling (chapter 9) |
| `clipboard` | `off` | also send every copy and cut to the terminal clipboard (chapter 2) |
| `tabs` | `on` | mark hard tabs with a guide glyph |
| `paneheight` | `0` | rows for the pane under the text, 0 to 500; 0 is a third (chapter 7; builds with terminals) |
| `mouse` | `on` | mouse reporting (chapter 2) |

### `[edit]`: editing

| Key | Default | Meaning |
|---|---|---|
| `mode` | `modeless` | `vi` or `modeless` |
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

### `[theme "<name>"]`

A color scheme of your own, selected with `ui.scheme`. Chapter 10 lists
the fields.
