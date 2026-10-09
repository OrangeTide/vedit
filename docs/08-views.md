# Other views

Besides the text view, a buffer can be shown as a drawing canvas, as
colored text art, as a table, or as a hex dump, and the editor can read
and write mail.

## Draw mode

Draw mode turns the buffer into a two-dimensional canvas for diagrams,
maps, and block art. Toggle it with the **Insert** key, Options > Draw
Mode, or `:draw`. The status line shows `-- DRAW --`. Help > Tutorial has
a walkthrough.

![Draw mode, with the free cursor over a box-and-arrow diagram](shot-draw.png)

| Key | Action |
|---|---|
| arrows | move one cell in any direction, past the end of a line and below the last line |
| typing | overwrite the cell under the cursor. Writing past the end of a line pads it with spaces |
| Enter | down one row, back to column 0 |
| Backspace, Delete | erase a cell to a space |
| Shift+arrows | mark a rectangle |
| Ctrl-C, Ctrl-X | copy the rectangle, or cut it and blank it |
| Ctrl-V | overlay the copied block at the cursor |
| Ctrl-B | draw an ASCII border around the rectangle: `+` corners, `-` and `\|` edges. A one-cell-wide or one-cell-tall rectangle becomes a line |
| Esc | drop the rectangle |
| Alt+1 to Alt+9, Alt+0 | insert a glyph from the active set of the glyph palette |
| Alt+G | the glyph palette |
| Ctrl-S, Ctrl-Q, Ctrl-Z, Ctrl-Y, Ctrl-F, Ctrl-L | save, quit, undo, redo, find, go to line |

The **glyph palette** (Options > Glyph Palette) has ten named sets of ten
glyphs: single, double, and heavy box lines, rounded corners, blocks,
quadrants, eighths, arrows, shapes, and marks. Up and Down pick the set,
which becomes the active one for the Alt+digit keys. Left and Right pick
a glyph, Enter inserts it, a digit inserts that slot, Esc closes.

The blanks you draw are real spaces, and draw mode does not trim trailing
spaces on save.

## Art view

A file whose name ends in `.ans` opens in the art view: the file's escape
sequences are replayed into a grid of cells, and you edit the cells. Each
cell holds one glyph with a foreground, a background, and attributes.
Saving writes the grid back as UTF-8 with the color sequences that
reproduce it, one line per row, so `cat file.ans` still shows the
picture. The status line shows `-- ART --`. Three scenes to try are in
`docs/`: `space.ans`, `mountain.ans`, and `beach.ans`.

![The space scene in the art view, with the pen's colors on the status line](shot-art.png)

The keys are draw mode's, applied to cells:

| Key | Action |
|---|---|
| arrows | move one cell. Moving below the grid adds rows |
| Home, End | the start of the row, and past its last glyph |
| PgUp, PgDn | a page |
| Enter | down one row, column 0 |
| Tab | the next 8-column stop |
| typing | overwrite the cell with the glyph in the current pen. Wide glyphs take two cells |
| Backspace, Delete | erase the cell to the left, or under the cursor |
| Shift+arrows, Ctrl-C, Ctrl-X, Ctrl-V, Ctrl-B | rectangles, as in draw mode. Cells keep their colors |
| Ctrl-Z, Ctrl-Y | undo, redo |
| Ctrl-S, Ctrl-Q, F1, F8 | as usual |

The **pen** is the color new glyphs get. A swatch of it, `Ab`, sits on the
status line.

| Key | Pen |
|---|---|
| Alt+Up, Alt+Down | the next or previous foreground: default, then colors 0 to 15 |
| Alt+Right, Alt+Left | the next or previous background |
| Alt+B, Alt+L, Alt+U | toggle bold, blink, underline |
| Alt+P | pick up the colors of the cell under the cursor |
| Alt+R | reset the pen |
| Alt+C | the color palette. At 16 colors, a grid of every foreground and background pair: Enter takes both, `F` or `B` one of them. At 256 colors, a swatch of every color: Enter or `F` sets the foreground, `B` the background |
| Alt+1 to Alt+0, Alt+G | the glyph palette; glyphs take the pen |

The grid is as wide as the widest line but at least 80 columns, and as
tall as the file. `art.width` forces a width from 80 to 1024, and
`art.view = off` opens `.ans` files as plain text, showing the raw escape
sequences. Colors beyond the first 16 load, render, and save unchanged,
but the pen cycles only the base 16; Alt+P picks the rest up from a cell.
A cell holds one code point.

A terminal buffer's output can be turned into an art buffer with Ctrl-W R
(chapter 7).

## Table view

A file whose name ends in `.csv`, `.tsv`, or `.tab` opens in the table
view. The text buffer stays the truth: a record is a line and a cell is a
range of bytes inside it, so undo, search, and the recovery journal work
as on text, and saving writes the lines back unchanged. A quoted field may
hold the delimiter, doubled quotes, and newlines; a record whose quoted
field spans several lines of the file is one row. A UTF-8 byte order mark
at the start of the file is kept and is not part of cell A1.

![A CSV file in the table view, columns fitted to their cells](shot-table.png)

The delimiter comes from the extension (`.tsv` and `.tab` are tab files,
which never quote) or, for `.csv`, from the first lines: the most
consistent of comma, semicolon, tab, and pipe. Columns are labelled A, B,
and so on to Z, AA, AB, like a spreadsheet. The grid has a label row on
top, a row-number gutter on the left, and the header row (line 1) frozen
under the labels while the rows scroll. Cells are cut to their column's
width with a marker. The status line shows the cell under the cursor and
its value: `-- TABLE --  C7: value`.

**View > Table View** (`:table`) turns the view on for any buffer, or off
again.

| Action | EDIT keys | vi keys |
|---|---|---|
| move by cell | arrows, Tab, Shift+Tab | `h j k l`, Tab, Shift+Tab |
| row start, row end | Home, End | `0`, `$` |
| first row, last row | Ctrl-Home, Ctrl-End | `gg`, `G` |
| page | PgUp, PgDn | Ctrl-B, Ctrl-F |
| edit the cell | Enter, with the cursor at the end | `a` (end), `i` (start) |
| replace the cell | type a character | `c` or `s`, then type |
| clear the cell | Delete | `x` |
| copy, paste a cell | Ctrl-C, Ctrl-V | Ctrl-C, Ctrl-V |
| cut the cell | Ctrl-X | Ctrl-X |
| search, next, previous | Ctrl-F, F3, Shift-F3 | `/`, `n`, `N` |
| go to a cell | `:cell C7` | `:cell C7` |
| insert a row above, below | Edit menu, `:rowadd`, `:rowadd!` | `O`, `o` |
| delete the row | Edit menu, `:rowdel` | `dd` |
| copy, paste rows | `:rowdel` copies; `p`, `P` paste | `yy`, `p` (below), `P` (above) |
| insert a column left, right | Edit menu, `:coladd`, `:coladd!` | the same |
| delete the column | Edit menu, `:coldel` | the same |
| undo, redo | Ctrl-Z, Ctrl-Y | `u`, Ctrl-R |
| sort the rows by this column | Edit > Sort Lines, `:sort` | `:sort`, `:sort! n` |
| save, quit | Ctrl-S, Ctrl-Q | `:w`, `:q` |

The `:` command line works in the table view in both personalities.

Editing a cell happens on the prompt line: the value is loaded into it,
Left, Right, Home, End, Backspace, and Delete edit it, Alt+Enter inserts a
newline into the value, Enter commits, and Esc leaves the cell as it was.
A committed value is quoted only when it holds the delimiter, a quote, or
a newline, and only the edited cell's bytes change. A tab file cannot
quote, so a tab or newline typed there becomes a space. Each commit is one
undo step.

A search starts from the current cell and the grid follows the match as
you type. `:cell C7` moves to column C, row 7; `:cell C` or `:cell 7`
moves one way only.

`:rowadd [N]` inserts N blank rows above the cursor row and `:rowadd! [N]`
below it. `:rowdel [N]` deletes N rows from the cursor, copying them
first, so `p` pastes them back. `:coladd [N]` inserts N empty columns
left of the cursor column, `:coladd! [N]` right of it, and `:coldel [N]`
deletes N columns. A column operation edits every row in one undo step. A
row's only cell is cleared rather than removed.

Other commands:

- `:table ,`, `:table ;`, `:table tab`, `:table pipe` force a delimiter.
  `:table off` leaves the view. `:table header` and `:table noheader` say
  whether line 1 is a frozen header row.
- `:colwidth N` sets the current column's width and `:colwidth N all`
  every column's. `:colwidth fit` fits the current column to its widest
  cell, and `:colwidth fit all` or Edit > Fit Column Widths fits them all.
  The default width is 10.

Config: `table.view`, `table.header`, `table.width` (chapter 5). A record
longer than 64 lines is left as separate lines and reported, so one stray
quote cannot swallow the rest of the file.

## Hex view

**View > Hex Dump** shows the buffer as a hex dump and lets you edit the
bytes. Choose it again, or press Esc or `q`, to return to the text view.
It works from either personality and has no key of its own.

Each row shows an offset, the bytes in hex with a gap after every eight,
and the same bytes as text with `.` for the unprintable ones. The cursor
byte is highlighted in both columns. The status line shows `HEX`, the
offset and the file size, which column is active and whether it is in
insert or overwrite mode, and the file name.

| Key | Action |
|---|---|
| arrows | one byte, or one row |
| PgUp, PgDn | a page |
| Home, End | the first and last byte of the file |
| Tab | switch between the hex column and the text column |
| `0` to `9`, `a` to `f` (hex column) | two digits write one byte |
| a printable character (text column) | writes that byte |
| Insert | switch between overwrite and insert |
| Delete, Backspace | delete the byte under the cursor, or the one before |
| `g` | go to an offset, in hex |
| `/` | find text |
| `\` | find bytes, as hex pairs such as `0a 0d` |
| `n`, `N` | the next match, or the previous |
| `w` | row width: 8, 16, or 32 bytes |
| `i` | show a data inspector line: the byte as signed and unsigned, as a character, and as 16-bit and 32-bit values in both byte orders |
| `v` | start a selection, or clear it |
| `y` | copy the selection, or the byte under the cursor |
| `p` | insert the clipboard at the cursor |
| Ctrl-S, Ctrl-Q | save, quit |
| Esc | clear the selection, or leave the hex view |

Overwriting cannot change a newline byte or write one; insert mode can
insert a newline, which splits the line. Each edit is one undo step, and
Edit > Undo reaches it. The hex view ignores the mouse except for the menu
bar.

## Mail

vedit reads and writes a Maildir, the one-file-per-message mailbox
format. It does not fetch mail or deliver it: a fetcher fills the Maildir,
vedit reads it and drops outgoing messages in an `Outbox` folder, and a
script submits those.

![The INBOX message list over an open message](shot-mail.png)

### Setting up

1. **Have a Maildir.** A delivery agent or Dovecot may keep one already.
   Otherwise `mbsync` (isync) or `offlineimap` mirror an IMAP account into
   one, `fetchmail` or `getmail` pull POP or IMAP mail into one, and
   `procmail` or `maildrop` deliver local mail into one. The tree must be
   Maildir++: the root holds `cur`, `new`, and `tmp` and is INBOX, and
   every other folder is a dot-directory beside them with the same three
   (`.Sent`, `.Drafts`, `.lists.vedit` for `lists/vedit`). A missing
   folder is created on first use.

2. **Point vedit at it:**

   ```ini
   [mail]
       dir  = ~/Maildir
       from = Jon Mayo <jon@example.org>
   ```

   `mail.dir` enables the Mail menu and the `:mail`, `:compose`, `:reply`,
   and `:send` commands. `mail.from` is the From line of every message you
   write.

3. **Arrange delivery** for the `Outbox` folder (below).

### Reading

- **Mail > Folders** (`:mail`) lists the folders. Choosing one lists its
  messages newest first, with `N` on unread ones and `A` on answered
  ones. **Mail > Messages** (`:mail .`) returns to the last folder, and
  `:mail Sent` lists a folder by name.
- Choosing a message opens it: the text part in a buffer headed by From,
  To, Cc, Date, and Subject, and every other MIME part as its own buffer
  named by part number and type, reachable through File > Buffer List.
  Save As writes a part to a file. Quoted-printable and base64 are
  decoded. Opening a message marks it read.
- The message buffers are ordinary buffers, so search, copy, and the vi
  keys work in them, and `:bd` closes one.

### Writing

- **Mail > Compose** (`:compose`, or `:compose address` to fill in To)
  opens a buffer with the header lines to complete, a blank line, and the
  body:

  ```
  From: Jon Mayo <jon@example.org>
  To:
  Cc:
  Subject:

  ```

  A header left empty is dropped when the message is sent. Others, such
  as `Bcc:` or `Reply-To:`, can be added.
- **Mail > Reply** (`:reply`) does the same for the message shown,
  addressed to its Reply-To or From, with `Re:` on the subject, the
  threading headers set, and the text quoted with `> `.
- **Mail > Send** (`:send`) completes the message (Date, Message-ID, and
  the MIME headers for a UTF-8 text body) and stores it under
  `Outbox/new`. The buffer is then clean, and a replied-to message is
  marked answered. Sending needs a `To:` line.

### Delivery

Something has to submit each file under `Outbox/new` and move it to
`Sent`. Any submission client that takes a message on standard input
works: `sendmail -t`, `msmtp -t`, or `ssmtp -t`. This script drains the
queue and can run from cron or from a terminal buffer:

```sh
#!/bin/sh
# Submit every queued message, then file it under Sent as read.
md=${MAILDIR:-$HOME/Maildir}
mkdir -p "$md/.Sent/cur" "$md/.Sent/new" "$md/.Sent/tmp"
for f in "$md"/.Outbox/new/*; do
    [ -f "$f" ] || continue
    if msmtp -t < "$f"; then
        mv "$f" "$md/.Sent/cur/$(basename "$f"):2,S"
    fi
done
```

A message that fails to submit stays in the Outbox.

Not supported: reply-all, attachments on outgoing mail, decoding of
encoded-word (`=?utf-8?...?=`) header values, deleting or moving messages
from the editor, and submitting from the editor itself.
