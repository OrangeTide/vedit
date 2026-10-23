# Command language: design note

Status: design, not implemented. This note records the decisions behind a
config-driven command language for vedit so the grammar and the value model are
settled before any code lands. It is an engineering document, not user manual
text; the user-facing chapters stay in `docs/01` through `docs/11`.

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

- kinds: `char` and `line`.
- buffers: the current buffer and the current clip or scratch buffer.
- the core verbs over handles: motion to a `Pos`, build a `Region`, grab a
  region into scratch, put scratch at a point, delete a region, replace a region
  with scratch content, plus the existing named editor actions.

### Reserved, not built

- the `rect` kind. The tag includes it from the start, so adding rectangular
  operators later is additive and needs no verb signature change. This is what
  will unify the WordStar block, the draw mode, and the art mode rectangle
  operations onto one region-and-operator model. It unifies the selection and
  operator model; the stored payload stays polymorphic, because draw cells carry
  color and attributes that plain text does not, so a text rect and a draw rect
  are not byte-identical values.
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
