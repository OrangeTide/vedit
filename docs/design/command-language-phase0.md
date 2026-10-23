# Phase 0: consolidation before the command language

Status: planned, not implemented. This is the first, self-contained step toward
the command language in `command-language.md`. It adds no new concepts, no
`Pos`, no `Region`, no interpreter. It only turns code that is inlined in the
dispatch switches into named helpers, and removes two small duplications, so the
later language has a clean vocabulary to call. Every change is behavior
preserving except one, a wrap bug fix, called out below.

Phase 0 is worth doing on its own merits (less duplication, named editing
operations) even if the language never ships.

Four commits, each independently reviewable and testable. Verify each with
`make test`, `make asan`, and both the `vedit` and `veditmin` builds. The
copy/cut/paste and wrap commits also drive the real app through the
`run-vedit` driver.

## Commit 1: name the clipboard operations

Today copy, cut, and paste are inlined in the `ed_dispatch` switch (`CMD_COPY`
at `vedit.c:17829`, `CMD_CUT` at 17856, `CMD_PASTE` at 17876). The `ACT_` layer
reaches them by delegating to `ed_dispatch(CMD_COPY)` and the menu by calling
`ed_dispatch` in its non-table branch, so there is one implementation already,
just not a named one.

Change: extract three helpers next to the other `ed_*` editing operations.

    void ed_copy(Editor *e);    /* selection or current line to the clip */
    void ed_cut(Editor *e);     /* selection to the clip, then delete it */
    void ed_paste(Editor *e);   /* clip at the cursor, replacing a selection */

`ed_cut` and `ed_paste` open and close their own undo group (today `CMD_CUT` and
`CMD_PASTE` set the `grouped` flag and rely on the `ed_dispatch` epilogue to
close it). After the move, each helper is self-contained, and the `grouped`
flag stays only for the selection-replace preamble that serves
INSERT/TAB/NEWLINE. The three `ed_dispatch` cases become one-line calls:

    case CMD_COPY:  ed_copy(e);  break;
    case CMD_CUT:   ed_cut(e);   break;
    case CMD_PASTE: ed_paste(e); break;

The `ACT_` and `MA_` paths are unchanged; they still reach the same code through
`ed_dispatch`. The menu keeps its table-mode branches (`tbl_copy_cell`,
`tbl_paste_cell`).

Risk: low. The bodies move verbatim; only the undo-group bracketing is made
local. Watch the empty-clipboard and no-selection messages, and the
current-line copy that appends a newline (17844 to 17849).

## Commit 2: one cursor-set funnel, name the motion targets

Outside vi the motions write `e->cx` and `e->cy` directly and inline the
arithmetic: `CMD_HOME`, `CMD_END`, `CMD_TOP`, `CMD_BOTTOM`, `CMD_UP`,
`CMD_DOWN`, `CMD_PGUP`, `CMD_PGDN` (`vedit.c:17934` onward). `move_left` and
`move_right` (16507, 16521) are the only named ones. vi has a separate clean
positional family (`vi_pos_word_fwd` and friends at 40590 onward) that returns
a position through pointers.

Change: add one funnel and route the CUA motions through it.

    void ed_goto(Editor *e, size_t row, size_t col);  /* set cursor, then clamp */

Each CUA motion case computes its target row and column, then calls `ed_goto`,
so the cursor is set in exactly one place and the clamp is not repeated. Keep
`move_left` and `move_right`. Keep the existing vertical-motion semantics (the
cases that only `clamp_col` today keep that behavior; do not introduce a desired
column here).

This does not yet build the motion-returns-`Pos` abstraction from the design
note; that arrives with the value model in Phase 1. It only removes the inline
scatter and gives one place that moves the cursor. Where vi already computes a
position (the word motions the `ACT_` layer calls through `vi_pos_word_*`), note
the overlap for Phase 1 but do not merge the families yet.

Risk: low, but motions are exercised everywhere. The win column and clamp
behavior must be identical. Rely on the render tests for cursor position.

## Commit 3: remove the find-next duplication

`ACT_FIND_NEXT` (`vedit.c:25943`) and `MA_FIND_NEXT` (28689) hold the same inline
snippet: if `e->last_find` is set, call `ed_find(e, e->last_find)`, else
`set_status(e, "no previous search")`.

Change: extract `void ed_find_next(Editor *e);` and call it from both. No
behavior change.

Risk: trivial.

## Commit 4: reconcile the wrap toggle (bug fix)

`ACT_WRAP_TOGGLE` (`vedit.c:25997`) and `MA_WRAP` (28736) both flip `e->wrap`
and set the status, but the menu also sets `e->left = 0` when wrap turns on, and
the keymap action does not. Wrapped text has no horizontal scroll, so leaving a
stale `e->left` is wrong; toggling wrap on from a keymap binding can leave the
view horizontally scrolled.

Change: extract `void ed_wrap_toggle(Editor *e);` with the correct behavior
(reset `e->left` to 0 when turning wrap on) and call it from both sites.

Risk: low. This is a behavior change on the `ACT_` path, intended: it fixes the
stale horizontal offset. Note it in the commit body. Verify by binding a wrap
toggle in a scratch config, scrolling right, then toggling wrap on through the
driver and checking the view resets.

## What Phase 0 does not touch

- vi. Its positional family is left as is; Phase 1 decides what it shares.
- the menu application features (`MA_*` beyond the four above).
- any grammar, value, or interpreter work. Those are Phase 1 and later.

After Phase 0 the editing vocabulary a program will call (`ed_copy`, `ed_cut`,
`ed_paste`, `ed_goto`, `ed_find_next`, `ed_wrap_toggle`, alongside the existing
`ed_*` edits and the text primitives) exists as named, reusable functions rather
than switch-case bodies.
