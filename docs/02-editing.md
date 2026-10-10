# Editing

The EDIT keys: moving, selecting, cut and paste, undo, find and replace,
and the other everyday commands. These are the default keys. Chapter 6
covers the vi keys.

## Moving

| Key | Moves |
|---|---|
| Left, Right | one character. At the ends of a line they continue onto the previous or next line |
| Up, Down | one line. On a shorter line the cursor moves to its end |
| Home, End | the start and end of the line |
| PgUp, PgDn | a screen |
| Ctrl-Home, Ctrl-End | the start and end of the file |
| Ctrl-L | a line number, asked for on the status line |

## Typing

Characters are inserted at the cursor. Enter splits the line and, with
Auto Indent on (chapter 4), starts the new line with the same leading
blanks as the one above. Backspace deletes the character to the left and
joins the line to the previous one at column 0. Delete deletes the
character under the cursor and joins the next line at the end.

Tab inserts a tab character, or spaces up to the next tab stop when
View > Indent with Spaces is on. Shift-Tab does the same as Tab.

## Selecting

Hold Shift with any movement key except Ctrl-Home and Ctrl-End to select
from the cursor. The selection is shown in reverse video. A movement
without Shift drops it, Ctrl-Home and Ctrl-End stretch it to the start
or end of the file, and Esc leaves it in place.
Typing, Tab, Enter, or a paste replaces the selection with what was
typed. Backspace and Delete delete it.

## Cut, copy, and paste

| Key | Action |
|---|---|
| Ctrl-C | copy the selection. With no selection, copy the whole line |
| Ctrl-X | cut the selection. With no selection it shows a hint and cuts nothing |
| Ctrl-V | paste at the cursor, replacing the selection if there is one |

The clipboard is inside the editor and works over any connection. A
line copied with Ctrl-C and no selection is pasted at the cursor position,
including its line break, so paste it at the start of a line to insert it
as a line.

**Edit > Copy to Terminal** sends the selection, or the current line, to
the terminal's own clipboard, and **Edit > Copy File to Terminal** sends
the whole buffer. This is how to get text out of a vedit running over ssh
or telnet into a local application. The terminal must support the OSC 52
clipboard escape; one that does not ignores it. The limit is 100000
bytes. With `ui.clipboard = on` in the config, every Ctrl-C and Ctrl-X
also goes to the terminal clipboard.

Text pasted by the terminal (bracketed paste) is inserted literally: a
newline takes no auto-indent, a tab stays a tab, and control keys in
the paste are dropped. It replaces a selection like typing does.

## Undo and redo

Ctrl-Z undoes and Ctrl-Y redoes. A run of typing undoes as one step. The
Edit menu shows whether there is anything to undo or redo.

## Find

Ctrl-F, or Search > Find, asks for a pattern on the status line and
searches as you type: the cursor moves to the first match after where you
started, and the view follows. The prompt reads `ISearch:`, with
`(failing)` in front when nothing matches.

| Key | Action |
|---|---|
| typing | extends the pattern; Backspace shortens it |
| Enter | stop here. With an empty pattern, repeat the last search |
| Esc, Ctrl-C | cancel and return to where you started |

**Search > Repeat Find** finds the next match of the last pattern,
forward. The search wraps around the end of the file. The pattern is a
regular expression (the Regular expressions section of chapter 6), and matches do not
cross lines. Searches are case sensitive unless `edit.ignorecase = on` is
set in the config.

## Replace

Ctrl-R, or Search > Replace, asks for a pattern and a replacement, then
walks the matches from the cursor to the end of the file. At each one it
asks:

```
Replace this one? (y)es (n)o (a)ll (q)uit
```

`y` or Enter replaces it, `a` replaces it and every later one without
asking, `q` or Esc stops, and any other key skips it. The walk does not
wrap around to the top. The whole replace is one undo step.

The replacement can refer to the match: `&` is the whole match, `\1` to
`\9` are the parenthesized groups, and `\U`, `\L`, `\u`, `\l`, `\E` change
the case of what follows.

## Go to line

Ctrl-L, or Search > Go to Line, asks for a line number. A number past the
end goes to the last line.

## The mouse

A left click in the text puts the cursor there. The wheel scrolls three
lines, and the cursor is pulled along when it would leave the window. A
click on the menu bar opens that menu, and clicks work in the menus and in
the Yes/No/Cancel dialogs. A click drops the selection. Dragging and the
right and middle buttons are ignored, and so is the mouse in the hex,
table, and art views, in terminal buffers, and in the pane under the
text; only the menu bar answers there.

With the mouse on, the terminal's own text selection usually needs Shift
held. View > Mouse turns reporting off for the session. `ui.mouse` in the
config sets the default and a `[mouse "<glob>"]` section matched against
`$TERM` overrides it per terminal (chapter 5).

## Sorting lines

**Edit > Sort Lines** sorts the selected lines, or the whole buffer, in
one undo step. The dialog has a key column, a string or decimal
comparison, reverse order, and ignore case. Tab moves between the fields,
Space toggles one, and Enter sorts. With a decimal sort, lines that hold no
number sort after the rest, or before them in reverse order. In the
table view the dialog sorts the data rows by the cursor's column.

## Inserting a date or a file

**Insert > Date** opens a calendar on today.

| Key | Action |
|---|---|
| Left, Right | a day |
| Up, Down | a week |
| PgUp, PgDn | a month |
| Home, End | a year |
| `t` | today |
| Tab, `f`, `F` | the next or previous date format |
| Enter | insert the date as shown under the grid |
| Esc | close |

The formats cycle from the ISO `2026-10-07` through `2026-10-07 14:30`, `07 Oct 2026`,
`October 07, 2026`, the weekday form, and the slashed forms. The chosen
format is kept for the rest of the session. `insert.dateformat` in the
config adds a `strftime` pattern of your own at the front of the cycle.

**Insert > File** browses for a file and inserts its lines below the
cursor line.

Both items are unavailable in the table and art views and in draw mode.

## Reflowing a paragraph

**Edit > Reflow Paragraph** re-wraps the paragraph under the cursor so
that no line runs past the text width. A paragraph is the run of lines
around the cursor up to an empty line. Each line keeps its prefix: the
indentation, `>` quote marks, and the comment leader of the language. A
list item hangs its continuation lines under its text. The rewrite is
one undo step. **View > Text Width** sets the width, 79 columns unless
changed; 0 means the window width. Chapter 6 describes the `gq` keys and
the prefixes in detail.

With Word Wrap on (chapter 4) and a text width other than 0, typing
past the width moves the word being typed to a new line with the same
prefix.

## Converting tabs and spaces

**Edit > Tabs to Spaces** expands every tab in the selected lines, or the
whole buffer, to spaces. **Edit > Spaces to Tabs** repacks each line's
leading indentation into tabs plus a remainder of spaces. Chapter 4 covers
tab stops and the indent settings.

## Key summary

| Key | Action |
|---|---|
| arrows, Home, End, PgUp, PgDn | move |
| Ctrl-Home, Ctrl-End | start and end of the file |
| Shift + movement | select |
| Enter, Backspace, Delete, Tab | edit |
| Ctrl-C, Ctrl-X, Ctrl-V | copy, cut, paste |
| Ctrl-Z, Ctrl-Y | undo, redo |
| Ctrl-F | find |
| Ctrl-R | replace |
| Ctrl-L | go to line |
| Ctrl-T | go to a symbol (chapter 7) |
| Ctrl-] | jump to the tag under the cursor (chapter 7) |
| Ctrl-W | split, pane, and terminal prefix (chapters 3 and 7) |
| F6 | the other pane of a split (chapter 3) |
| Ctrl-S | save |
| Ctrl-Q | quit |
| Insert | draw mode (chapter 8) |
| F1 | key bindings |
| F2 | the vi keys (chapter 6) |
| F4, Shift-F4, F9, Alt-F9, Ctrl-F9, Alt-F5 | build commands (chapter 7) |
| F8, Shift-F8 | next and previous buffer |
| F10, Alt+letter | the menu bar |

Every other control key and function key does nothing.
