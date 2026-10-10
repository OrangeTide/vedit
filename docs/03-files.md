# Files and buffers

Opening and saving through the file browser, keeping several files open,
line endings, what the editor writes beside your files, and large files.

## The file browser

**File > Open** shows a file browser in a dialog. It lists the current
directory, sub-directories first (marked with a trailing `/`), then files,
sorted by name.

| Key | Action |
|---|---|
| Up, Down, PgUp, PgDn, Home, End | move the selection |
| a letter | jump to the next entry starting with it |
| Enter on a directory | descend into it (`../` goes up) |
| Enter on a file | open it |
| Tab | move to the `File:` line at the top |
| Esc | cancel |

The `File:` line takes a typed name or path. A directory there changes
into it. Any other name opens that file, which may be a new one.

**File > Save As**, and the first save of an unnamed buffer, use the same
browser with the `File:` line focused. For a named file it is filled with
the current name, in the file's directory; for an unnamed buffer it is
empty. Edit the name and press Enter to write there, or pick an existing
file from the list. Writing over a file that exists asks `Overwrite
name?` first, and a save into a directory that does not exist asks
`Create directory ...?`.

**File > New** starts an empty, unnamed buffer.

## Several files at once

File > Open and File > New replace the file in view, after the save
prompt if it has unsaved changes. To keep the current file open as well,
use `:e name` or `:enew` in the vi keys (chapter 6). Each open file is
then a buffer, and one buffer is shown at a time.

- **F8** switches to the next buffer and **Shift-F8** to the previous one.
- **File > Buffer List** lists them: the buffer number, a `*` on the
  current one, the name, `[+]` when it has unsaved changes, and the line
  count. Move with the arrows, or type a digit to jump to that buffer
  number, then Enter switches to it.
- `:bd` in the vi keys closes the current buffer. Quitting closes them
  all.

`:e` of a file that is already open, even under another spelling (a
symlink, a `./` or `../` detour, or a hard link), switches to the existing
buffer instead of loading a second copy.

Some features open buffers of their own: a terminal (chapter 7), a commit
message or a file's history (chapter 7), the parts of a mail message
(chapter 8). They appear in the buffer list like any other.

## Two panes

The text area can be split in two panes, one above the other or side by
side, each a full view with its own cursor, scrollbar, and title. The
two panes may show two files, or the same file at two places: an edit
made in one pane is in the other at once, since they share the buffer.

| Key | Menu item | Action |
|---|---|---|
| Ctrl-W s | View > Split Pane | split, one pane above the other |
| Ctrl-W v | View > Split Side by Side | split, side by side |
| Ctrl-W w, W, p, F6 | View > Other Pane | move the focus to the other pane |
| Ctrl-W h, j, k, l | | move the focus left, down, up, or right |
| Ctrl-W c, q | View > Unsplit | close the focused pane; the other fills the frame |
| Ctrl-W o | | close the other pane |

A split opens with the current file in both panes; the new pane is the
top or left one and takes the focus. It needs a plain text buffer (not
a terminal, nor the hex, table, or art view) and a frame of at least
five rows or twenty-four columns; the View items are grayed otherwise.
The other split key while split turns the panes the other way. In the
vi keys, `:split name` and `:vsplit name` open the file named in the
new pane, `:close` and `:only` close the focused pane or the other one,
and `:q`, `:q!`, `:wq`, and `:x` close the focused pane while two are
open and quit the editor otherwise. Ctrl-Q, File > Exit, `ZZ`, `:qa`,
and `ZQ` always quit. Ctrl-W is not a prefix in the vi insert mode.

Each pane has its own current buffer: F8, the buffer list, and `:e`
change the file in the focused pane and leave the other as it is. The
focused pane's title is drawn reversed, and the status line describes
it. A click in the other pane moves the focus there; the wheel over it
scrolls it in place.

The pane under the text (chapter 7) sits under the focused pane and
follows the focus; Ctrl-W j enters it from the text pane above it and
Ctrl-W w returns. Opening a split takes a text buffer out of that pane,
and Buffer in Pane waits until the split is closed. A window too small
for two panes shows the focused one until it grows again.

## Line endings

**View > Line Endings** cycles the style a save writes: `LF` (Unix),
`CRLF` (DOS and Windows), or `NUL` (NUL-separated records). The style is
detected from the first line break when a file is loaded: a NUL before it
means NUL, a `\r\n` pair means CRLF, otherwise LF. A new buffer uses LF. The current style is always on the
status line.

The text in memory never holds the terminator, so changing the style only
changes what the next save writes. Changing it marks the buffer modified,
since the bytes on disk will differ.

## Crash recovery

While you edit a named file, vedit keeps a journal of your edits beside
it. A crash, a dropped connection, or a killed terminal does not lose
unsaved work. Two hidden files appear next to `name` at the first edit:

| File | What it is |
|---|---|
| `.name.swpf` | the base: a copy of the text as it was when editing started |
| `.name.swpm` | the journal: every edit, appended as it happens |

A clean save or quit removes both. Only a crash leaves them behind. On a
filesystem with reflinks (Btrfs, XFS, bcachefs, APFS) the base is a clone
that shares the file's blocks and takes no extra time or space; elsewhere
it is a copy. Once the journal grows large, the base is rewritten from the
current text and the journal starts over.

When you open a file that has a journal, vedit asks:

```
Unsaved changes found. (r)ecover (o)pen (d)elete (q)uit?
```

The message adds `maybe open elsewhere` when another running process
holds the file, and `file changed since` when the file on disk has
changed since the journal was started.

- `r` loads the base, replays the journal on top, and leaves the result in
  the buffer for you to check and save.
- `o` opens the file on disk and ignores the journal.
- `d` deletes the journal and the base and opens the file.
- `q`, or any other key, quits vedit at startup, or keeps the current
  buffer in File > Open and `:e`.

Replay stops at the first damaged record. A crash during a write loses at
most one edit. The journal is flushed to disk whenever input
goes quiet, and when the editor is killed by `SIGTERM` or `SIGHUP`. Only a
power loss can lose edits made since the last quiet moment.

Options in the config file (chapter 5):

| Key | Effect |
|---|---|
| `edit.swap = off` | no journal at all, for a filesystem that must not be written |
| `edit.swapdir = DIR` | keep every journal in one directory, named after the file's full path with `/` turned into `%` |
| `edit.backup = on` | after a save, keep the previous version as `name~` |
| `edit.backupdir = DIR` | where those backups go, named the same way |

A directory that does not exist or cannot be written is ignored, and the
file goes beside the original. A leading `~/` is expanded.

## The lock file

At the first edit, vedit also takes an Emacs-style lock beside the file,
`.#name`, which Emacs honors too. The owner is recorded as
`user@host.pid:boot`. Opening a file whose lock belongs to a running
process on this machine asks:

```
jon@myhost.1234:1728300000 is editing this file. (s)teal (r)ead-only (q)uit?
```

- `s` takes the file over.
- `r` opens it read-only. The status line shows `RO` and edits are
  refused; `:w!` in the vi keys still writes.
- `q`, or any other key, leaves it unopened.

A lock left by a process on this machine that has exited, or from an
earlier boot, is stale and is replaced without asking. A lock from another
host always asks. With `edit.swap = off` there is no lock.

## Large files

Opening a file maps it. A line is copied to memory only when it is first
changed, so a large log or data file opens in the time it takes to find
the line breaks, and the operating system pages the bytes in as you
scroll.

Before the first edit the view follows the file on disk, so a program that
truncates the file while you are looking at it pulls the pages away. vedit
catches that, shows the lost part as blank, and tells you. Reload with
File > Open, or `:e` in the vi keys. After the first edit, with the
journal on, the buffer is backed by the recovery base instead, and nothing
another program does to the original affects it.

`edit.mmap = off` in the config reads files into memory instead, for a
filesystem that does not map well, such as some network or FUSE mounts.
