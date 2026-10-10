# Advanced

Defining syntax languages and color schemes in the config file, using
another version control system, embedding the editor in a program, and
the build-time knobs.

## Syntax languages

A language is a small state machine written in the config file. A state
has a color and an ordered list of `rule` lines, each keyed on a set of
characters; the first rule whose set contains the current byte fires and
names the next state.

```ini
[syntax]
    mn = mini             # the .mn extension uses this language

[color "mini"]            # class -> color [attrs]
    kw  = "#ffd700"
    num = cyan

[words "mini.keywords"]   # keyword groups; split on spaces, several lines allowed
    list = if else while return
    list = for do break continue

[state "mini.idle"]       # the first state is the start state (or start = in [language])
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

- **charset** is `*` for any byte (put it last), or a quoted set with
  ranges and escapes: `"a-zA-Z0-9_"`, `"0-9"`, `"\t\n"`, `"\""`.
- **`noeat`** re-processes the byte in the target state without consuming
  it.
- **`recolor`**, or `recolor=N`, repaints the last byte, or the last N
  bytes, in the target state's color.
- **`buffer`** starts recording a token.
- **`kw=<group>:<class>`** matches the recorded token against a `[words]`
  group and, on a hit, repaints it in `<class>`.
- **`mark`** records the current position, and a later rule with
  **`recolormark`** repaints everything from the mark to the current byte
  in the target state's color, for a region whose length is known only
  when its end is seen, such as a preprocessor line or `<stdio.h>`. The
  mark is per line and starts at the beginning of the line.
- **`col N`** in place of a charset fires when the line has reached
  display column N (tabs count to the next multiple of 8), so `rule = col
  72 long noeat` sends the rest of an overlong line to a state named
  `long` that you define. `col gitcommit.body:72` takes the column from
  that config key, with 72 when it is unset.

A state may set `include = <other-state>`: when none of its own rules
match, the included state's rules are tried, keeping the including
state's color. Colors take attributes: `keyword = yellow bold`, with
`bold`, `underline`, `reverse`, `dim`, and `italic`.

The state at the end of a line carries into the next line, which is how
block comments span lines. A newline is fed to the machine at the end of
each line, so a state that must not span lines, such as a `//` comment,
returns with `rule = "\n" idle`, and a keyword at the end of a line
is still recognized.

A state can hand the text after it to another language until an end
string, which is how a fenced code block in Markdown is colored as the
language its info string names:

```ini
[state "md.codeblock"]
    embed = auto          # the language named by the token buffered at entry
    end = ```             # the string that ends the region
    endbol = on           # counted only at the start of a line

[state "page.script"]
    embed = javascript    # a language by name
    end = </script>
    endcase = off         # matched without regard to case
```

`embed = auto` takes the first word of the token being buffered when the
state was entered and looks it up as a language name or extension; a word
with no language leaves the region in the state's own color. The embedded
language runs until `end` is seen, then the embedding state resumes on the
end string, so its own rules color and leave it. One level only: an
embedded language's own `embed` states are ignored.

A language named after a file extension is used for that extension
without a mapping. The built-in languages are written in this format, and
defining a language with the same name as a built-in replaces it outright,
so to change one, copy its whole grammar and edit that.

## Color schemes

A `[theme "name"]` section defines a scheme that `ui.scheme = name`
selects:

```ini
[theme "midnight"]
    base         = black      # start from dos | black | plain (default dos)
    content.fg   = 189        # the text area
    content.bg   = default
    frame.fg     = 60         # window border and scrollbars
    frame.bg     = default
    title.fg     = "#ffd787"  # file name in the top border
    bar.fg       = 231        # menu bar and status bar
    bar.bg       = 54
    guide.fg     = 60         # the hard-tab guide glyphs
    reverse-bars = off        # draw the bars in reverse video
    borderless   = on         # drop the right border so text reaches the edge
```

A color is `default` (the terminal's own), a palette index 0 to 255,
`#rrggbb`, or one of the sixteen ANSI names (`red`, `cyan`, and so on,
with a `bright-` prefix for 8 to 15). A `#rrggbb` value must be quoted,
since an unquoted `#` starts a comment. Unset fields keep the base
preset's value, and `base = black` turns `borderless` on unless the theme
sets it off. Up to eight themes can be defined.

## Another version control system

The VCS menu (chapter 7) is driven by command lines in a `[vcs "<name>"]`
section. The configured systems are tried in order, then git, and the
first whose `branch` command succeeds owns the file. For Mercurial:

```ini
[vcs "hg"]
    branch = hg branch
    status = hg status $(file)
    log = hg log --template "{node|short} {date|shortdate} {desc|firstline}\n" $(file)
    show = hg diff -c $(rev) $(file)
    blame = hg annotate -c -u -d $(file)
    commit = hg commit -l $(msg) $(file)
```

| Command | Must |
|---|---|
| `branch` | print the branch, and fail outside a working copy |
| `status` | print one porcelain-style line for `$(file)` when it has changed, starting with `??` when untracked, and nothing when clean |
| `log` | print one line per commit, the revision first, the rest as the picker shows it |
| `show` | print the diff for `$(rev)` and `$(file)` |
| `blame` | print the file with each line led by its revision |
| `commit` | record `$(file)` with the message in the file `$(msg)` |

The same keys in a `[vcs "git"]` section replace the built-in git
commands.

## Embedding in a program

The host owns the file descriptors and the signal handlers. All terminal
I/O goes through a vtable, `struct vedit_io`, that the host fills
over its own transport. The host drives the editor with `vedit_run()` and
delivers window sizes with `vedit_set_size()`.

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
vedit_set_box_mode(v, VEDIT_BOX_DEC);  /* the frame style for this client */
vedit_set_colors(v, 16);               /* 16 or 256 */
vedit_set_scroll(v, 1);                /* the client supports a scroll region */
int rc = vedit_run(v);                 /* blocks until the player quits */
vedit_free(v);
```

`vedit.h` documents the full interface and the callback contracts. Box
mode and colors are set per instance, and two players on one server can
use different clients.

- `vedit_run()` drives its own loop by calling `io.poll` and `io.read`.
  In a single-threaded event-loop host, run the editor on its own thread
  or coroutine, or supply a `poll` callback that yields to the host loop.
- The config is handed in with `vedit_set_config()`, built from
  `vedit_cfg_new()` and `vedit_cfg_load()`. `vedit_set_config_path()`
  names the file, which `:reload` re-reads.
- Terminal buffers need the host to wait on the input and the child
  pseudo-terminals together, through the optional `io.poll_fds` callback.
  Without it the editor runs normally and `:terminal` reports that it
  needs a multiplexing host.
- The build commands, `:!`, and the formatter run through a tool runner
  installed with `vedit_set_tools()`; with none, those features are
  unavailable.
- Mail goes through `struct vedit_mail_api`, a vtable of folder, message
  list, flag, fetch, store, move, append, and send operations. The
  standalone binary installs the Maildir implementation when `mail.dir` is
  set; a host installs its own with `vedit_set_mail()`.
- Over raw telnet, the host handles the IAC negotiation and reads the
  window size from NAWS, then passes it to `vedit_set_size()`.

## Build-time knobs

Each define removes a feature from the binary. `VEDIT_NO_TERM` and
`VEDIT_NO_ART` are also make variables (`make VEDIT_NO_TERM=1`); the
others go in `CFLAGS` (`make CFLAGS+=-DVEDIT_NO_MAIL`, so the default
flags stay).

| Define | Removes |
|---|---|
| `VEDIT_NO_TERM` | terminal buffers, the pane under the text with its Ctrl-W keys and `:set paneheight`, `:terminal`, `:sterm`, `:sbuffer`, `:repost`, and the Terminal menu. The two text panes stay |
| `VEDIT_NO_ART` | the art view. The VT emulator goes once both this and `NO_TERM` are set |
| `VEDIT_NO_TOOLS` | the build commands, the Compile, Run, and VCS menus, the output pane, and `:format`. `:!`, `:log`, `:blame`, and `:commit` remain and report that the feature is unavailable |
| `VEDIT_NO_MAIL` | the Mail menu and commands |
| `VEDIT_NO_MOUSE` | mouse reporting and View > Mouse |
| `VEDIT_NO_XDG` | the XDG config path; `~/.veditrc` is used instead |

Open Header and the `cc.file` reader stay in a `NO_TOOLS` build.
