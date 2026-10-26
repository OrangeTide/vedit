# The command language

A key binding does not have to be a single action. The configuration chapter
shows how a keymap binds a key to an action; this chapter covers the small
language a binding runs instead, so one key can take several steps and act on
named positions and spans.

A binding runs a program when its target is wrapped in braces. The program's
commands run in order, separated by `;` or a newline:

```ini
[keys "wordstar"]
    bind = "^Y { line-end ; delete-to-bol }"   # clear the line's text
    bind = "^N { line-end ; newline }"         # open a blank line below
```

Quote the whole value when a program uses `;` or `#`. The config file treats
both as the start of a comment, so a program that uses them must sit inside
double quotes, which the parser strips before reading the program. Inside the
program, braces group an argument that holds spaces and a backslash escapes the
next character. A command that opens a prompt (`find`, `goto-line`, `save`, and
the like) runs to completion and the program then continues, so a prompt need
not be the last step. A quit ends the program. An unknown command name is
reported and stops the program.

Every keymap action name from the configuration chapter is also a command. On
top of them the language passes typed values, not only text, so a command can
name a position or a span and a verb can act on it.

**Words and values.** A bare or braced word is a string, `$name` is a variable,
and `[ ... ]` runs a bracketed program and uses its value. A `$name` or a
`[program]` stands alone as one whole argument; the characters `[`, `]`, `$`,
and `"` stay reserved inside a plain word, so brace or escape one to use it as
literal text. A double quote is reserved for later use and is an error for now.
`set NAME VALUE` binds a variable for the rest of the run; variables do not
persist between keypresses.

**Positions.** A position is a row and a column, both 1-based as in the status
bar. `cursor` is the current cursor and `pos ROW COL` an explicit one. `to
MOTION` is where a vi motion reaches from the cursor. The motions are `word`,
`word-back`, `word-end` and their whitespace-delimited twins `big-word`,
`big-word-back`, `big-word-end`; `line-begin`, `first-nonblank`, `line-end`;
`file-top` and `file-bottom`, which land on the first non-blank of the first or
last line as vi's `gg` and `G` do; and `matching-bracket`, the match of the
bracket at or after the cursor, which is an error when the line has none. `mark
NAME` reads a vi mark as a
position and `mark NAME POS` sets it; these are vi's own marks, shared, so a
letter mark is settable while the special marks are read-only, and a mark
follows its text through edits. `go POS` moves the cursor to a position,
recording the old spot so `jump-back` returns to it, so `go [mark a]` jumps to
a mark and `go [pos 5 1]` jumps to a line.

**Regions.** A region names a span of text:

- `region line N1 N2` is whole rows N1 through N2 (1-based, inclusive).
- `region char P1 P2` is the text run between two positions, in reading order.
- `region rect P1 P2` is the column box with those two corners: columns P1's
  through P2's across their rows.
- `region KIND mark A B` is the same shape, but anchored: its two endpoints are
  the marks A and B, read fresh at each use. Because a mark follows its text, an
  anchored region keeps spanning the same text, so a span bracketed on one key
  stays right for another key after the buffer changes. A trailing `file N`
  anchors it in another open file, whose own marks A and B track that file's
  edits, so `region char mark a b file 2` follows the marked span in file 2.
- `selection` is the current selection, its kind following the selection's
  shape: a linewise selection is a line region, a block selection a rect, and
  anything else a character run. It is an error when nothing is selected.
- `block` is the WordStar block bracketed by its begin and end markers, a rect
  for a column block and a character run otherwise. It is the counterpart of
  `selection` for the persistent markers, so a WordStar keymap can act on the
  marked block through the command language. It is an error when either marker
  is unset.
- `span inner OBJECT` and `span around OBJECT` are vi text objects. The objects
  are `word` and `big-word`; the bracket pairs `paren`, `brace`, `bracket` and
  `angle`; the quotes `double-quote`, `single-quote` and `back-quote`; and
  `paragraph`. `inner` is the content, `around` takes the delimiters or the
  trailing space too, so `span inner word` is the word under the cursor and
  `span around paren` is a `(...)` with its parentheses. `paragraph` is a line
  region of the whole rows the cursor's paragraph covers, as vi's `ip` and `ap`
  are; `around` adds the blank lines after it.
- `$clip` is the clipboard, and `reg NAME` a named register. A register persists
  between keypresses; a single letter `a` to `z` is vi's register of that
  letter, so a program and vi share it, and any other name is a register of its
  own.

A region measures against the current file. A trailing `file N` points it at
another open file instead, numbered from 1 in buffer order, and `put` takes the
same suffix, so `region line 1 3 file 2` is the first three lines of the second
open file. A char region's end is exclusive, so `[to line-end]` reaches the
line's end while `[to word-end]` stops on the word's last character; `span` uses
the object's own span, so `delete [span inner word]` is vi's `diw`.

**Verbs.** A verb with no argument acts on the selection or a default around the
cursor, exactly as the plain action names do; given a region it acts on that
span:

- `delete REGION` removes the span, and `delete.` also pushes it onto the kill
  stack. Deleting a rect removes the columns and closes up each row.
- `copy SRC [DST]` grabs the span into the clipboard, or into the register DST
  names; a rect is grabbed as a blank-padded block.
- `put VALUE at POS` inserts a string or a region (the clip or a register) at a
  position; the word `at` is optional, and a trailing `file N` places into
  another open file. A rect, or a register or clip holding a block, goes in as
  columns, shifting existing text right and padding short rows.
- `replace REGION` removes the span and puts the clipboard where it began.
- `run SRC` evaluates a string or a register's text as a program, sharing the
  current run's variables, so a program stored in a register is a reusable
  macro.
- `go POS` moves the cursor, and `go-file N` switches the focus to the Nth open
  file, the way the next-buffer key does.
- `select REGION` makes a region the live on-screen selection: a line region
  becomes a linewise selection, a rect a block one, a char run a plain one, and
  the cursor lands at the far end. It is the counterpart of `selection`, which
  reads the selection out, so one key can bracket a span as an anchored `region
  ... mark a b` and a later key light it up as a real highlight. The region must
  be in the current file.
- `deselect` drops the selection, which a destructive verb over `selection` does
  not do on its own.

A key that copies the first three lines and places them at the top of the file:

```ini
[keys "demo"]
    bind = "^D { copy [region line 1 3] ; put $clip at [pos 1 1] }"
```

Registers persist, so one key can stash a region and another place it:

```ini
[keys "demo"]
    bind = ^Y { copy [region line 1 1] [reg a] }
    bind = ^P { put [reg a] at [cursor] }
```

A key can carry text from one open file to another, reading from file 2 and
placing at the top of file 1:

```ini
[keys "demo"]
    bind = "^T { copy [region line 1 1 file 2] [reg a] ; put [reg a] at [pos 1 1] }"
```

A WordStar keymap sets a block with begin and end markers, and `block` reaches
it, so one key can send the marked block into the top of a second open file:

```ini
[keys "wordstar"]
    bind = "^K M { copy [block] ; put $clip at [pos 1 1] file 2 }"
```

Marks persist too, so one key can bracket a span and a later key act on it as an
anchored region, even after the text in between has changed:

```ini
[keys "demo"]
    bind = ^B { mark a [cursor] }
    bind = ^E { mark b [cursor] }
    bind = ^X { delete [region char mark a b] }
```

`select` turns that same anchored span into a visible highlight, so a key can
show what the marks bracket before another key acts on it:

```ini
[keys "demo"]
    bind = ^H { select [region char mark a b] }
```
