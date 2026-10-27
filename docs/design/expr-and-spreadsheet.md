# Expression evaluator and the spreadsheet: design note

Status: Phase A and a second cut of Phase B implemented. The standalone `expr_*`
evaluator, the command-language `expr` verb, and the table view's formulas are
built (`vedit.c`, the Expression evaluator and Table formulas sections). The
formula cut is numbers and ranges (`A1:B3` with `SUM`, `AVG`, `COUNT`, `MIN`,
`MAX`), recompute-on-read with a visiting stack for cycles, and reference
relabeling on row and column insert, delete, sort, and paste; string value
variants in `ExprVal`, incremental recalc, and `EVAL` are not built yet. This note records the design for two features that
share one piece: an arithmetic `expr` for the command language, and formulas for
the CSV/TSV table view. The shared piece is an expression
evaluator. The point of writing this before building `expr` is to give the
evaluator the right seam from the start, so the later spreadsheet engine reuses
it instead of growing a second parser. It is an engineering document, not user
manual text.

## Why

Two separate needs point at the same evaluator:

- The command language has no arithmetic. A `while` loop cannot advance a
  counter (there is no `set i $i + 1`), and a program cannot compute a value.
  Comparisons exist only as `if`'s infix form and `cmp`, with no `+`, `-`, `*`.
- The table view (`Tbl` at `vedit.c:7977`) wants formulas: a cell holding
  `=A1+B2` that shows a computed value.

Both want an infix expression evaluator with numbers, operators, precedence, and
function calls. Building one evaluator with a clean interface, and calling it
from both places, avoids two parsers and keeps the behavior consistent.

## What exists today

Two facts from the source shape the design.

- The table view is a view over text, not a cell model. `Tbl` holds only the
  delimiter, the header flag, column widths, and the cursor. Cells are parsed on
  the fly from each buffer line through the RFC 4180 field parser, and editing a
  cell edits the underlying CSV/TSV text. The text is the model. A1-style
  addressing already exists (`tbl_label`, the `:cell C7` command).
- There is no expression parser. `cl_parse` (`vedit.c:26509`) is a command
  parser. It produces words, `$name`, and `[program]` nodes (`CLW_LIT`,
  `CLW_VAR`, `CLW_SUB`). It has no infix arithmetic, no precedence, and no
  numeric evaluation. `expr` is a new thing, not an extension of `cl_parse`.

## The shared piece: a standalone expression evaluator

Build the evaluator as its own small unit, `expr_*`, with no dependency on the
command-language internals or on the table view. Its interface is a single call
plus an environment:

```c
int expr_eval(const char *src, const ExprEnv *env, ExprVal *out,
    char *err, size_t errlen);
```

- A tokenizer reads numbers, the operators, parentheses, identifiers, and
  function-call syntax `f(a, b)`.
- A precedence-climbing parser builds and evaluates in one pass over a small
  `ExprVal` value type.
- The parser knows the grammar (operators, calls, a `:` range operator); it does
  not know what any name means. That is the seam.

The `ExprEnv` supplies meaning through callbacks:

```c
typedef struct expr_env {
    void *ctx;
    int (*lookup)(void *ctx, const char *name, ExprVal *out);        /* a name */
    int (*deref)(void *ctx, const ExprVal *v, double *out);          /* range->num */
    int (*fold)(void *ctx, const ExprVal *range, double *sum,
        long *count, double *minv, double *maxv);                    /* aggregate */
    int (*call)(void *ctx, const char *fn, int argc, const ExprVal *argv,
        ExprVal *out);                                               /* f(...) */
} ExprEnv;
```

`lookup` resolves a bare name (`$i` in a program, `A1` in a sheet). In a sheet a
name is a single cell, returned as a 1x1 range so the `:` operator can join two
of them. `deref` reads a 1x1 range as one number (so a bare `A1` yields a
value); `fold` walks a range for the aggregate functions. `call` resolves a
function the evaluator does not know itself. The parser stays the same for both
callers; only the env differs. The command language leaves `deref`, `fold`, and
`call` null, so it never forms a range.

What is deliberately **not** shared: `cl_parse`. `expr` is a separate leaf
language. The command language keeps `$name` and `[program]` substitution and
calls `expr` as a verb; the spreadsheet calls `expr` directly and never touches
`cl_parse`. The reusable artifact is the arithmetic evaluator, not the command
parser.

## Two callers, two environments

- Command language: the `expr` verb. Its env `lookup` reads the `ClEval`
  variable table; its `call` offers a small set of math functions. `expr`
  converts `ExprVal` to a `ClVal` for the result. This unblocks
  `set i [expr $i + 1]` and computed arguments.
- Spreadsheet: the table view. Its env `lookup` parses a reference (`A1`, or a
  range `A1:B3`) and reads the cell value cache, driving that cell's own
  evaluation first. Its `call` offers the sheet functions (`SUM`, `AVG`, and the
  rest). The `:` range operator produces a range value only the sheet env gives
  meaning to; the command language never uses it.

## Bidirectional embedding: EVAL and flexibility

The two languages embed each other, so the evaluator has to be flexible about
values and about re-entering.

- Command language into expr: the `expr` verb runs an arithmetic string and
  yields a `ClVal`.
- Expr into the command language: an `EVAL("program")` function, registered in
  the env's function table, runs a command-language program through the existing
  `cl_eval_string` and returns its result as an `ExprVal`. So a cell formula
  `=EVAL("...")` can run editor logic, and a program can compute with `expr`.

This mutual call path has three consequences.

1. Value coercion. `ExprVal` and `ClVal` must convert both ways: a number or a
   string crosses cleanly, a region or position does not and is an error at the
   boundary. `ExprVal` therefore carries number, string, and error at least, not
   number alone.
2. A shared recursion budget. `expr` calling `EVAL` calling a program calling
   `expr` can recurse. The existing `CL_RUN_MAX` depth guard should cover the
   combined depth, not just command-language nesting, so one counter bounds the
   whole cycle.
3. Side effects during recalc are the real hazard. A sheet recomputes formulas
   implicitly, on edit and on render, and possibly across many cells at once. An
   `EVAL` that deletes lines, moves the cursor, or opens a modal prompt would
   fire at surprising times and many times over. The decision for formula
   context is to run the embedded program in a restricted mode: no buffer
   mutation and no modal prompts during recalc. The command language's dialog
   verbs and editing verbs are available from a key binding, where the user
   asked for them; they are not available from a cell that the view recomputes
   on its own. How to express that restriction (a read-only `ClEval` flag, or a
   separate builtin set) is an open question below.

## CSV with formulas: the format

The format change is almost nothing, because the text stays the model.

- A cell whose text begins with `=` is a formula. On disk it is an ordinary CSV
  string field, so the file round-trips losslessly through other tools and the
  format stays plain CSV. The engine strips the leading `=` before evaluating.
- Each cell has a source (the literal or the `=formula`) and a computed display
  value. The grid renders computed values; in-cell edit, or a formula bar, shows
  the source. Computed values are never written to disk.
- `Tbl` grows a value cache keyed by (row, col), filled on demand and
  invalidated on edit. This is the first cell state that is not reparsed from the
  text each time, so its lifetime and invalidation need care.
- References are A1-style, mapping onto the (col, row) the view already derives;
  ranges are `A1:B3`. A formula that contains the delimiter, such as
  `=SUM(A1,A2)`, is RFC 4180 quoted in the file, which the field parser already
  round-trips, so the engine receives the formula intact.
- Computed errors are display strings: `#REF!`, `#DIV/0!`, `#CYCLE!`.

## The recalc engine is separate, and is the bulk

The evaluator is the small shared piece. The engine around it is spreadsheet
specific and is most of the work:

- A dependency order. Formulas reference other cells, so recompute needs a
  topological order and cycle detection. Start with recompute-all on any edit,
  which is simple and fine for modest sheets, and add incremental dependency
  tracking later. Cycle detection is needed either way.
- Reference adjustment on structural edits (built). The table view inserts and
  deletes rows and columns (`tbl_row_add`, `tbl_col_del`, and the rest), and
  each of those now calls `tbl_reloc` inside its undo step to relabel every
  formula's references: a reference past the edit shifts, one into a deleted
  band becomes `#REF!`, and a range straddling a deletion shrinks. It rewrites
  the formula text (`tbl_reloc_formula`) rather than any cell model, so it is a
  text pass with no parser involvement.
- Reference adjustment on sort (built). `sort_lines` builds a row map from the
  sort permutation and calls `tbl_sort_reloc`, which relabels every single-cell
  reference to a sorted row so it follows the cell to its new place
  (`tbl_sort_formula`). A range is left unchanged: the rows it spans are
  permuted among themselves, so the order-independent aggregates are unaffected,
  and there is no contiguous range for a scattered set.
- Reference adjustment on paste (built). Pasting clipboard rows (`tbl_row_paste`)
  inserts those rows, so it reuses `tbl_reloc` to shift references in the rest of
  the sheet down, just as a manual row insert does; a skip band keeps that pass
  off the pasted rows. The pasted rows themselves then get a relative shift when
  their source is known: `tbl_row_copy` records the top source row on the
  clipboard (`clip_tbl_rows`, `clip_tbl_row`), and the paste shifts every
  reference in the pasted formulas by how far the rows moved, so a formula copied
  and pasted elsewhere takes its references along (`tbl_paste_shift`). That shift
  is the row half of `tbl_reloc_formula`, inserting or deleting the displacement
  worth of rows at the top, so a reference driven off the top becomes `#REF!`.
  When the clipboard has no source (an external paste), the pasted text stays
  literal. Pasting a single cell (`tbl_paste_cell`) makes no structural change,
  but when the clipboard holds a formula copied from a known cell
  (`tbl_copy_cell` records its row and column too) its references shift by the
  distance moved on both axes, through `tbl_cell_shift_formula`. That runs the
  row and column halves of `tbl_reloc_formula` in turn, so a reference pushed off
  the top or left edge becomes `#REF!`. The clipboard source is one flag and a
  (row, column) pair shared by the row and cell copies; `clip_set` clears the
  flag, so any other write to the clipboard drops the source and a later paste is
  literal.

Sharing the evaluator saves perhaps a fifth of the spreadsheet effort. The
engine, the cache, the dependency order, and reference adjustment are the rest.

## Value model

`ExprVal` holds a number, a string, or an error. The number is a double, printed
as an integer when it is whole, so there is one numeric type across the command
language (whose values are `long` today through `clv_arg_int`) and the sheet
(which wants floats). Each caller coerces at its boundary:

- `ClVal` string or int to and from `ExprVal`.
- Cell text to `ExprVal`: a number when it parses as one, else a string; a
  `=formula` computes to whichever the formula yields.

## Phasing

- Build `expr_*` with the `env` seam first, even though only the `expr` verb
  needs it at the start.
- Phase A (done): the `expr` verb. Env over `ClEval` variables through
  `cl_expr_lookup`, the built-in functions `abs`, `int`, `mod`, `min`, and
  `max`, no `EVAL` yet. This is the arithmetic the command language was missing
  and unblocks `while` counters and computed arguments. `ExprVal` is numeric
  only for now; the string and error variants arrive with Phase B. The built-in
  functions are libm-free on purpose, since only the GUI build links libm.
- Phase B (second cut done): the spreadsheet. The `=` convention, a cell
  resolver (`tbl_ref_lookup`) over A1 references returning a 1x1 range,
  recompute-on-read with a visiting stack for cycles (`tbl_formula_num`), error
  values, and the source-and-value status line. The `:` range operator with
  `tbl_deref` and `tbl_fold` backing the `SUM`, `AVG`, `COUNT`, `MIN`, and `MAX`
  functions. Reference relabeling on row and column insert or delete
  (`tbl_reloc`), on sort (`tbl_sort_reloc`), on a row paste (`tbl_reloc` for the
  rest of the sheet and `tbl_paste_shift` for the relative shift of the pasted
  rows), and on a cell paste (`tbl_cell_shift_formula`, a relative shift on both
  axes), rewriting the formula text in place within the edit's undo step. Still
  to come: a value cache, incremental recalc, string formulas, and `EVAL` with
  the restricted formula context.
- Gating. `expr_*` stays ungated and small, so the `expr` verb works in builds
  without the table, and the table works without arithmetic. The spreadsheet
  engine stays behind `VEDIT_TABLE`.

## Open questions

- How to restrict side effects and prompts for a program run from a formula: a
  read-only flag on `ClEval`, a separate builtin set, or evaluating `EVAL` cells
  only on explicit recompute rather than on render.
- Numeric precision and formatting: when to show an integer, how many decimals,
  locale.
- Range semantics and the function set for the first spreadsheet cut.
- What triggers recalc: on edit, on render, or on an explicit command, and how
  aggressively to cache so that an `EVAL` cell does not rerun on every frame.
- Whether `expr` should also serve `if` and `cmp`, replacing the current
  integer-only comparison path, or stay a separate verb.
