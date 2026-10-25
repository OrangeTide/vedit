# Command language: design note

Status: landing in phases. Phase 0 (consolidation), Phase 1 (mini-program
bindings with the cursor-form verbs and the capture suffix), the value model
(typed values, $variables, [substitution], and the region-form verbs over the
char, line, and rect kinds), the rect kind (which also makes the WordStar
column block do real rectangular edits), and the resumable evaluator are
implemented. The resumable evaluator is synchronous: the prompt helpers block
on the modal frame stack, so a top-level program services a prompt request
through run_req and resumes with the next command when the frame closes; a quit
ends the program. A prompt inside a [substitution] stays an error, since a
substitution must yield a value at once. Still reserved and not built: arbitrary
and register buffers, and art mode's colored Cell-grid rectangle. This note
records the decisions behind the design. It is an
engineering document, not user manual text; the user-facing chapters stay in
`docs/01` through `docs/11`.

## Why

vedit has four command paths today, found by a survey of the source:

- modeless (CUA): `keymap[]` to `key_to_cmd` to `ed_dispatch` (the `Cmd` enum
  at `vedit.c:10185`, the switch at `ed_dispatch` around line 17757).
- vi: `vi_dispatch` delegating to the normal, insert, and visual key handlers,
  with the real state machine in `vi_normal_key` (around line 42795).
- config keymaps: the `ACT_*` actions run by `kb_run_action` (around 25853),
  bound from `[keys "name"]` config sections.
- menu: `MA_*` actions run by `run_menu_act` (around 28625).

The data layer below all of them is already clean and small. Every content
change goes through four reversible primitives, the `Eop` values OP_INSERT,
OP_DELETE, OP_SPLIT, OP_JOIN (`vedit.c:8466`), wrapped by the public text API
(`text_insert`, `text_delete`, `text_split`, `text_join` at 9665 to 9730) and a
transaction API (`text_undo_group_begin` and `text_undo_group_end`). Loading a
file bypasses these on purpose so it records no undo history.

The command layer above the primitives is not unified. The `ACT_` layer already
delegates 26 core actions verbatim to `ed_dispatch`, so cross-layer duplication
is small (a copied `FIND_NEXT`, a divergent `WRAP` toggle). The real gaps are
that outside vi there is no reusable motion abstraction (CUA motions are inline
`e->cx`/`e->cy` arithmetic), copy, cut, and paste are inlined in the
`ed_dispatch` switch rather than named helpers, and mutations are driven from
roughly 38 functions across six subsystems (core text, vi, table, hex, two
drawing modes, WordStar blocks), each opening its own undo group.

vi is the one place that already looks like the engine we want: a
count then operator then motion then apply pipeline with a real intermediate
representation, the `Motion` struct (`vedit.c:40934`), consumed by three
operator appliers that converge on the mutation primitives. It is coupled to
the `Editor` struct, not to vi semantics.

The goal is not to replace the handlers, most of which are already thin, and not
to rewrite vi. The goal is a small language whose programs can be the target of
a key binding, so a config author can compose new commands without C, and so
the config keymap personalities (and one day the built-in modeless mode) can be
expressed as config rather than hardcoded. See the keymap work this follows on
from in the configuration chapter.

## The grammar

The surface grammar is the Tcl shape, taking the parser design from antirez's
picol v2 (https://github.com/antirez/picol). picol proves the whole core fits
in a few hundred lines of C with no dependencies, which matches vedit. A
program is a sequence of commands; a command is a word and its arguments; a word
is bare text, a brace-quoted block, a quoted string, a variable reference, or a
bracketed command substitution.

The point of adopting the picol grammar now is forward compatibility. We
implement only a small subset today, but we reserve the full set of
metacharacters from the start, so adding a reserved construct later as live
syntax cannot change the meaning of a program written today.

### Reserved from day one

These characters are special in an unquoted word and may not appear literally
in one. To use any of them as literal text, brace or escape it. They are
reserved even where the subset below does not yet act on them.

| Character | Meaning |
|---|---|
| `{` `}` | group a block or program value; contents are literal, no substitution |
| `[` `]` | command substitution (reserved; compute a value by running a command) |
| `$` | variable or buffer reference (reserved) |
| `"` | quoted string with substitution (reserved) |
| `;` and newline | command separators |
| `#` | comment, only where a command begins (picol rule) |
| `\` | escape the next character |

Comments inside a program follow the picol rule: `#` starts a comment only where
a new command would begin. The inline `#` comments in the shipped `.conf` files
are a feature of the outer gitconfig-style parser, which strips them before the
command language ever sees a binding value. The two comment layers do not
overlap.

### The subset built first

- run a sequence of commands separated by `;` or newline.
- run a brace-quoted `{ }` block as a program value.
- call a builtin editor verb with literal (bare or brace-quoted) arguments.
- two return codes, ok and error.

Not built yet, but reserved by the grammar: `$` variables and buffer names,
`[ ]` command substitution, `expr`, `if`, `while`, `proc`, and user-defined
procedures. Each is additive under the rules above. Bare words are always
literal; evaluation happens only through explicit `[ ]` substitution, so a
count like `marker-goto 3` means the literal 3 forever, and computing a value is
the opt-in `marker-goto [expr $base + 1]` once substitution exists.

## The value model (the currency)

The language does not work in strings. Strings are demoted to leaf scalars for
things like search patterns, file names, and messages. The buffer-facing
currency is one handle type.

### One coordinate type

    Pos = { row, col }

Every coordinate is a row and a column, for all region kinds. A linewise region
still carries a column; commands may ignore it or use it as the active column.
A coordinate is never a bare line number.

### One handle type

    Region = { buffer, kind, start: Pos, end: Pos }

A region refers to a buffer. It is not free-floating coordinates; its positions
are positions within the buffer it names. A region is normalized so that
`start` is the upper-left or earlier corner and `end` is the lower-right or
later corner, so operators never have to sort endpoints.

`kind` is one of:

- `line`: rows `start.row` to `end.row`, whole lines, columns along for the
  ride.
- `rect`: the literal box, columns `start.col` to `end.col` across rows
  `start.row` to `end.row`. Here the upper-left and lower-right corner reading
  is exact.
- `char`: a text run from `start` through `end` in document order. Here the
  meaning is that start precedes end in reading order; on a later row the end
  column can read as less than the start column, which is correct for flowing
  text. This is the one kind where the two corners are not a geometric box, and
  a command that treated a charwise region as a box would clip the wrong
  columns.

The cursor is a `Pos`. A point region has `start == end`.

### Buffers are the objects handles reference

A clip is not a separate value type. It is a buffer, the scratch buffer. Copy
writes a region's content into the scratch buffer; paste copies a region of the
scratch buffer into the current buffer at a point. A program that must assemble
content over several commands builds it in scratch, then places it.

Because every handle names a buffer, a region addressing any open buffer is all
that multi-buffer editing needs. No separate mechanism is required.

The geometry kind lives on the handle, not on the buffer. The scratch buffer is
storage; the region that references it says whether the content is char, line,
or rect. A paste uses the source region's kind. This is where today's
`clip_linewise` flag moves to.

### Implemented today

- kinds: `char`, `line`, and `rect`.
- buffers: the current buffer and the current clip or scratch buffer.
- the core verbs over handles: motion to a `Pos`, build a `Region`, grab a
  region into scratch, put scratch at a point, delete a region, replace a region
  with scratch content, plus the existing named editor actions.

### The rect kind

A `rect` region is a column box: columns x1..x2 across rows y1..y2. It is
normalized per axis (top-left and bottom-right corners), unlike `char` which is
normalized by reading order. The rect verbs chose vi's block model over draw
mode's: delete removes the columns and closes up each row (text shifts left);
put and replace place a block clip as columns, shifting existing text right and
padding short rows with spaces; a copied rect is a blank-padded block in the
clip, marked blockwise. A rect over the Text buffer reuses three shared
primitives (`rect_grab`, `rect_delete_cols`, `rect_place_cols`) that draw mode,
the vi visual block, and the WordStar column block also call, so there is one
rectangle implementation over text. The WordStar `block-column-toggle`, which
was cosmetic (it changed only the highlight), now routes copy, move, and delete
through those primitives, so a column block finally does rectangular edits.

Art mode's rectangle stays separate. Art is a grid of `Cell` values carrying
color and attributes, stored outside the Text buffer, with its own `art_clip`.
A text rect and an art rect are not byte-identical, so folding art onto the
region model would need regions over a polymorphic Cell-payload buffer. That is
a larger project than the value model and is deliberately left out; `rect` over
the Text buffer does not depend on it.

### Reserved, not built

- arbitrary buffers beyond current and scratch (multi-buffer editing).
- registers as buffers. vi's 26 registers and the clipboard are, under this
  model, named scratch buffers, and a register reference is a buffer name. This
  is what eventually collapses `vi_regs[26]` and `clip` into one concept.

### A known refinement

A region is a coordinate snapshot into a buffer, so a persistent selection, or
anything that must survive edits (now possibly across buffers), needs its
endpoints anchored to marks that track insertions and deletions rather than raw
coordinates. A `Pos` can therefore be a raw coordinate or a mark handle. This is
noted in the type design and is not built first.

## Worked examples

Before any code, a set of real commands was written in the subset above, using
only verbs that exist today, to see where the currency makes a composition clean
and where it fights back. The verbs today are nullary and cursor-implicit: each
reads and writes the one cursor or the live selection and takes no target.
`{ a ; b }` is a sequence in one undo group.

Easy, linear macros over the cursor:

- Clear the current line but keep it: `{ line-end ; delete-to-bol }`. Go to the
  end, delete back to column 0. Clean, because the second verb starts where the
  first left the cursor.
- Open a blank line below and land on it: `{ line-end ; newline }`.
- Duplicate the current line: `{ copy ; line-begin ; paste }`. Works, but leans
  on two implicit conveniences: `copy` with no selection grabs the whole line
  and its newline, and `paste` lands at the cursor that `line-begin` placed.

These need nothing past the first subset. The easy majority of keymap
customization lives here.

Where the API starts to resist:

- Delete the word under the cursor, roughly: `{ word-left ; delete-word-right }`.
  Expressible but fuzzy. Two motions only approximate "the word I am on" and get
  boundaries wrong mid-word. It wants a region, not two motions.
- Faithful emacs kill-line, where the killed text is kept so it can be yanked
  back: this cannot be written today. `copy` acts on the selection or the whole
  line; there is no "copy from the cursor to end of line", and no verb builds a
  selection inside a program. So "clear the line" is easy and "kill the line" is
  not, and the only difference is whether a span can be named for both the copy
  and the delete.

Where the Region currency is required, not more macros:

- Delete lines 10 through 20: `delete [region line 10 20]` in the target syntax.
  Today no verb takes a range, and there is no loop, so it is eleven blind
  `delete-line` calls. A region argument collapses it to one.
- Swap two blocks: impossible today at any length. There is one persistent block
  slot and one clipboard slot, and no way to name a second. Swap and concat are
  exactly what the single-slot API cannot express, and are the motivation for
  named handles over buffers.
- Comment a column, insert a prefix down the left of several rows: needs the
  reserved `rect` kind and a column-insert verb. Impossible today.
- Accumulate several lines into the clip, then place once: impossible today,
  because `copy` overwrites the single slot. This is the case the capture suffix
  below is for.

### What the examples settled

1. The nullary cursor verbs compose well for linear macros and for nothing else.
   Every easy case is "do, then do", each verb consuming the cursor the last one
   left. That is the real value of the first subset.
2. The wall is always the same wall: a span or a second container cannot be
   named. Text objects, copy-a-range, line ranges, swap, rect, and accumulation
   all fail for that one reason. This is concrete validation of the Region over
   buffer currency. It is the line between a macro recorder and the commands
   actually worth writing.
3. `put` acting at the implicit cursor is the recurring friction. The gymnastics
   in the duplicate-line, swap, and accumulate cases are all "navigate the cursor
   to where paste will land". A `put <content> at <pos|region>` form removes it.

A hazard to audit before the region-form verbs land: the removing verbs are
inconsistent about whether they feed the clipboard. `delete-to-eol` does not; the
line-delete path goes through vi's yank. Clip-based composition stays fragile
until that is made uniform, which the capture suffix below also forces us to
settle.

## The verb convention

### Two forms, one operation

Every buffer verb has two shapes that name the same operation.

- Region form: the verb takes an explicit `Region` or `Pos` and acts on it:
  `delete $r`, `copy $r`, `put $clip at $p`, `reflow $r`.
- Cursor form: the verb with no region argument acts on an implicit region built
  from editor state, the live selection if there is one, otherwise a
  verb-specific default around the cursor (a point, the current line, or the span
  from the cursor to a motion target).

The cursor form is defined as the region form applied to that implicit region.
`delete-to-eol` is `delete [region char from cursor to line-end]`; `delete-line`
is `delete [region line at cursor]`. One operator per operation, with the nullary
verbs as the sugar rather than a parallel set. A macro author uses the cursor
forms and never meets a `Region`; a program that computes a span uses the region
form; both reach the same C.

The motion-carrying names (`delete-to-eol`, `delete-word-right`) are kept as
cursor-form shortcuts, not deprecated. They are ergonomic and already bound, and
each is equivalent to the general `delete <region>` once regions exist.

This is why the phasing works. The existing named actions are the cursor forms.
Phase 1 can ship cursor forms only, the vocabulary that exists, and add region
forms when the value model lands, renaming nothing.

### The capture suffix

A trailing `.` on a removing verb is a variation that also pushes the removed
text onto a capture store. `delete-to-eol` discards; `delete-to-eol.` deletes and
captures. It is one modifier in place of a parallel family of `cut-*` verbs, and
it answers the split the examples found: clear versus kill is the dot.

From the parser's view the dotted name is simply another command name; from the
user's view it is the same command with a capture variation. `.` is not added to
the reserved metacharacter set. It is special only as the last character of a
command-position word, so it stays literal inside arguments (file names, `3.14`)
and in the middle of a word.

Mechanics (settled by the user, 2026-10-24):

- The capture store is a distinct kill stack, separate from the CUA clipboard, so
  `.` never clobbers an explicit copy. Under the reserved registers-as-buffers
  model it is just one more named buffer, so it adds no new concept later; today
  it is a single hidden buffer used as a stack.
- `.` pushes, newest on top, so repeated captures accumulate. That is the
  mechanism the accumulate example wanted, and it matches an emacs kill ring.
  Reading back is a paste from the stack, peeking the top for now, with yank-pop
  and named-buffer access arriving with the register model.
- The verbs that accept `.` are the removing ones: `delete-left`,
  `delete-right`, `delete-word-right`, `delete-line`, `delete-to-eol`,
  `delete-to-bol`, and the region-form `delete`. `cut` already captures by
  definition, so `cut` is `delete.` over the selection. `cut` keeps its own name
  for familiarity, with the equivalence noted.

## Scope boundary

The language covers the synchronous buffer-and-cursor core: motion, edit,
clipboard, marks, blocks, and undo grouping, where composition pays off.
Anything that opens a modal prompt, changes view state, or drives a subsystem
(find, replace, goto, save, quit, tools, mail, version control, tables, draw
mode) stays a named host action that a program can invoke but not implement.

This matters because vedit's loop is event driven, not synchronous like Tcl. A
command cannot block and read a line from the user. Today handlers return a
`Req` token (`vedit.c:7954`) that the loop turns into a prompt or a quit, and
the modal UI is a stack of frames. The evaluator's return codes therefore need
one editor-specific outcome beyond ok and error: yield a host request. Fold that
into the return-code enum from the start, even while only ok and error are used,
so adding it later is not a signature change.

## Divergence from picol

- Value model. picol is stringly typed, everything is a string reparsed on
  demand. vedit is not: the currency is the `Region` handle over buffers, with a
  small set of value types (int, string, region, program), and a region is a
  reference into a buffer, not copied text. This is the same move Jim Tcl made
  away from pure strings, for the same reason: an editor manipulating blocks of
  text should not round-trip them through strings.
- Execution model. picol evaluates to completion on the C stack. vedit needs a
  command that opens a prompt to cooperate with the modal frame stack. See the
  scope boundary.

## Phasing

Phase 0, consolidation, valuable on its own and a prerequisite that de-risks the
rest:

- lift CUA copy, cut, and paste out of the `ed_dispatch` switch into named
  helpers so all layers and any future program share them.
- add a motion-to-position layer for the non-vi path so CUA motions stop being
  inline `e->cx`/`e->cy` arithmetic, sharing the `vi_pos_*` family where the
  semantics match.
- remove the real duplications (the copied `FIND_NEXT`, the divergent `WRAP`
  toggle).

Phase 1, mini-program bindings:

- keep the existing readable action-name tokens as the instruction set; they
  already bottom out in the text primitives and the motion helpers.
- let a binding's target be a program, not a single action, run by a new
  builtin that is itself an action, with the program as the binding argument.
- represent the program as the picol-subset grammar above; compile to an
  internal form if a compact representation is wanted, but keep the source
  readable words, not single-character opcodes.
- ship the cursor forms of the verbs first (they are the existing actions), then
  add the region forms with the value model. The capture suffix can arrive with
  the cursor forms, since it needs only a kill stack, not the `Region` type.

## Decisions settled

- surface grammar: the picol v2 shape, full metacharacter set reserved, small
  subset implemented.
- currency: one `Region` handle over buffers, `Pos` coordinates, kinds `char`,
  `line`, and `rect` with `char` and `line` built and `rect` reserved, scratch
  buffer as the clip, strings as scalars.
- do not rewrite vi onto the language; do not convert the menu application
  features; do not use single-character opcodes.
- reimplement the design in vedit's style; do not vendor picol.c. picol is an
  inspiration for the grammar and the small-interpreter shape only, not code to
  import. This keeps the value system tailored to regions and buffers, carries
  no third-party notice, and reads like the rest of the file.
- resumable evaluator. A command that opens a prompt suspends the program over
  the modal frame stack and resumes when the frame completes, rather than ending
  the program and handing control to the loop. This keeps programs composable
  across a prompt, reuses the existing frame stack, and is the model that lets
  more commands move onto the language over time. It is the larger piece of
  work, chosen for that reach.
- two-form verbs. Each operation has a region form that takes an explicit handle
  and a cursor form with no argument that is the region form over an implicit
  cursor or selection region. The existing named actions are the cursor forms,
  so one operator serves both the macro author and the program.
- capture suffix. A trailing `.` on a removing verb also pushes the removed text
  onto a kill stack (`delete-to-eol.` kills, `delete-to-eol` clears). It is a
  distinct command name to the parser and a variation to the user, replaces a
  family of `cut-*` verbs, and makes `cut` equal to `delete.` over the selection.
  Open: whether the store is a separate stack (recommended) or the clip, and
  whether `.` pushes (recommended) or replaces. Settled 2026-10-24: a
  separate kill stack, and `.` pushes (accumulates).
