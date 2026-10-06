/*
 * vedit : a single-file visual text editor for primitive terminals.
 *
 * vedit is a self-contained text editor. It emulates a modeless Microsoft EDIT
 * personality and a vi personality, drawn through a small cell grid that emits
 * a fixed subset of ANSI control codes. It carries no terminfo database, no
 * differential compositor, and no syntax engine, so it runs over a telnet or
 * ssh link to a primitive terminal emulator such as a MUD client.
 *
 * All terminal I/O passes through a read()/write()/select()-style vtable
 * (struct vedit_io). The command-line binding wires that to a real tty with
 * raw mode and SIGWINCH. An embedding host (a MUD) supplies its own vtable
 * over the player's socket, where raw mode is a no-op and a resize arrives by
 * calling vedit_set_size().
 *
 * The file is organized top to bottom as: compatibility shim (this part),
 * the text buffer, the hex view, the modeless editor and its vi personality,
 * and finally the embed API and the command-line entry point.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Public interface (struct vedit_io, enum vedit_box_mode, the embed API). */
#include "vedit.h"

/****************************************************************
 * Vendored: rx, a compact regular expression engine.
 *
 * rx powers the editor's regex search and replace (vi / and ?,
 * the incremental find, and :s). It is a small backtracking
 * POSIX-ERE matcher with the common vi/sed/PCRE conveniences.
 *
 * Upstream:  https://github.com/OrangeTide/rx
 * Vendored:  version 1.0.0
 *
 * The block below is rx.h followed by rx.c, pasted verbatim from
 * upstream so a future copy can be re-vendored by replacing
 * everything between this banner and the "end vendored rx"
 * banner. The engine keeps its upstream 4-space indentation and
 * SPDX notice on purpose; do not reflow it to the editor's tab
 * style. The only edits to the upstream text are mechanical: the
 * rx.c line that includes "rx.h" is dropped (the header is
 * inlined just below), and the optional RX_MAIN command-line tool
 * at the end of rx.c is omitted.
 ****************************************************************/

/* ----- begin vendored rx.h (upstream 1.0.0) ----- */
/* rx.h : public interface for the compact regex engine
 * SPDX-License-Identifier: 0BSD OR CC0-1.0
 */

#ifndef RX_H
#define RX_H

#include <stddef.h>     /* size_t */

/* Library version. RX_VERSION is a single integer for comparisons, for
 * example: #if RX_VERSION >= RX_VERSION_MAKE(1, 2, 0) */
#define RX_VERSION_MAJOR 1
#define RX_VERSION_MINOR 0
#define RX_VERSION_PATCH 0
#define RX_VERSION_STRING "1.0.0"
#define RX_VERSION_MAKE(maj, min, pat) ((maj) * 10000 + (min) * 100 + (pat))
#define RX_VERSION \
    RX_VERSION_MAKE(RX_VERSION_MAJOR, RX_VERSION_MINOR, RX_VERSION_PATCH)

/* Compile and match flags. The match flags (bits 0..2) are stored in the
 * compiled object; RX_GLOBAL only affects rx_replace. */
#define RX_ICASE     0x01   /* case-insensitive matching           */
#define RX_MULTILINE 0x02   /* ^ and $ match at embedded newlines   */
#define RX_DOTALL    0x04   /* . also matches newline               */
#define RX_GLOBAL    0x08   /* rx_replace: replace every match      */

/* Search flags for rx_search. These occupy a separate bit range from the
 * compile flags above and are passed only to rx_search. */
#define RX_BACKWARD  0x10   /* search toward the start of the text  */
#define RX_WRAP      0x20   /* wrap past the far end on a miss       */

typedef struct rx rx_t;

/* Offsets of a capture into the subject text, or -1/-1 when unset.
 * Index 0 is always the whole match. */
typedef struct {
    long so;
    long eo;
} rx_match;

/* Compile `pattern`. On failure returns NULL and, if errp is non-NULL,
 * stores a static error string in *errp. */
rx_t *rx_compile(const char *pattern, int flags, const char **errp);

/* Free a compiled pattern. */
void rx_free(rx_t *re);

/* Number of capturing groups (group 0 excluded). */
int rx_ngroups(const rx_t *re);

/* Search `text` (of `len` bytes) from offset `start`. Fills up to
 * `nmatch` entries of `m`. Returns 1 on a match, 0 when none is found,
 * or -1 on error (such as the step budget being exceeded). */
int rx_exec(rx_t *re, const char *text, size_t len, size_t start,
            rx_match *m, int nmatch);

/* Report whether a match of `re` can contain a newline byte. Returns 0
 * when every possible match lies within a single line, which lets an
 * editor safely search and highlight one line at a time; returns 1 when
 * a match may span lines, so the whole buffer must be searched at once. */
int rx_matches_newline(const rx_t *re);

/* Editor-oriented search, convenient for incremental (type-as-you-go)
 * search. By default it finds the first match beginning at or after
 * `from`; with RX_BACKWARD it finds the last match beginning before
 * `from`. With RX_WRAP a miss continues from the far end of the text so
 * the whole buffer is covered, as in an editor's search. `m`, `nmatch`,
 * and the 1/0/-1 return value are exactly as for rx_exec. Backward
 * search considers non-overlapping matches and costs a scan of the
 * searched span. */
int rx_search(rx_t *re, const char *text, size_t len, size_t from,
              int sflags, rx_match *m, int nmatch);

/* Substitute matches of `re` in `text` using the `repl` template and
 * return a freshly malloc'd NUL-terminated string the caller frees.
 * With RX_GLOBAL every match is replaced, otherwise only the first.
 * Returns NULL on allocation failure or a matching error. */
char *rx_replace(rx_t *re, const char *text, size_t len, const char *repl,
                 int flags);

#endif /* RX_H */
/* ----- end vendored rx.h ----- */

/* ----- begin vendored rx.c (upstream 1.0.0, RX_MAIN tool omitted) ----- */
/* rx.c : compact POSIX-ERE regex engine with convenience extensions
 * SPDX-License-Identifier: 0BSD OR CC0-1.0
 */
/*
 * A single-file regular expression matcher and substitution engine meant
 * to be dropped into a sed, a vi clone, a grep, or any small text tool.
 * It is a backtracking engine, so it supports backreferences and the
 * common conveniences found in GNU sed, Vim, and PCRE on top of the
 * POSIX ERE core.
 *
 * Supported pattern syntax (ERE base, backslash makes a metacharacter
 * literal):
 *
 *   literals, .            any char (not newline unless RX_DOTALL)
 *   * + ?                  greedy quantifiers
 *   *? +? ??               lazy (non-greedy) quantifiers
 *   {n} {n,} {n,m} {,m}    counted repetition (and lazy variants)
 *   [...] [^...]           bracket expressions, ranges, [:class:]
 *   ^ $                    anchors (line anchors under RX_MULTILINE)
 *   ( ) (?:...)            capturing and non-capturing groups
 *   |                      alternation
 *   \1 .. \9               backreferences
 *   \d \D \w \W \s \S      Perl-style shorthand classes
 *   \b \B                  word boundary / non-boundary
 *   \< \>                  start-of-word / end-of-word (Vim, GNU)
 *   \n \t \r \f \v \a \0   control escapes, \xHH hex escape
 *
 * Substitution template syntax (rx_replace):
 *
 *   & or \0                whole match
 *   \1 .. \9               captured group
 *   \U \L                  upcase / downcase following output until \E
 *   \u \l                  upcase / downcase next output char
 *   \E                     end case conversion
 *   \n \t ... \xHH         control escapes
 *   \& \\                  literal & and backslash
 *
 * Matching is leftmost, with greedy-by-default preference (Perl/PCRE
 * semantics), not POSIX leftmost-longest.
 *
 * The matcher backtracks using heap-allocated choice and undo stacks
 * rather than the C call stack, so match depth is bounded by available
 * memory, not the native stack. A per-search step budget guards against
 * catastrophic backtracking by failing the search rather than hanging.
 */


#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define OK   0
#define ERR  (-1)

/* Per-search instruction budget. A search that exceeds it fails with an
 * error rather than looping on a pathological pattern. */
#ifndef RX_STEP_LIMIT
#define RX_STEP_LIMIT 20000000L
#endif

/* Largest repetition count accepted in a {n,m} interval, matching the
 * POSIX RE_DUP_MAX minimum. Keeps a huge count from blowing up the
 * compiled program. */
#ifndef RX_DUP_MAX
#define RX_DUP_MAX 32767
#endif

/* Largest group-nesting depth the recursive-descent parser accepts. The
 * parser recurses on the C stack for each nested group, so this bounds
 * that recursion to a compile error rather than a stack overflow. */
#ifndef RX_MAX_DEPTH
#define RX_MAX_DEPTH 1000
#endif

/****************************************************************
 * Instruction set for the backtracking virtual machine
 ****************************************************************/

enum {
    I_CHAR,     /* match one literal character (c)            */
    I_ANY,      /* match any character (newline per flags)    */
    I_CLASS,    /* match a character in the 256-bit set       */
    I_BOL,      /* assert beginning of text or line           */
    I_EOL,      /* assert end of text or line                 */
    I_WB,       /* assert word boundary                       */
    I_NWB,      /* assert not a word boundary                 */
    I_WSTART,   /* assert start of word (\<)                  */
    I_WEND,     /* assert end of word (\>)                    */
    I_SAVE,     /* record current position in slot x          */
    I_BREF,     /* match backreference to group x             */
    I_JMP,      /* jump to x                                  */
    I_SPLIT,    /* try x first, then y on failure             */
    I_MARK,     /* record position in scratch slot x (no undo) */
    I_PROGRESS, /* if position == scratch slot x, jump to y    */
    I_MATCH,    /* accept                                     */
};

typedef struct {
    int op;
    int x, y;           /* jump targets, save slot, or group number */
    unsigned char c;    /* literal for I_CHAR                       */
    unsigned char *set; /* 32-byte bitmap for I_CLASS (not owned)   */
} rx_inst;

struct rx {
    rx_inst *prog;
    int plen;
    int ngroup;             /* number of capturing groups (0 == none) */
    int nmark;              /* scratch slots for repetition guards    */
    int flags;
    int matches_nl;         /* 1 if a match can contain a newline byte */
    unsigned char **sets;   /* owned class bitmaps, freed in rx_free  */
    int nsets;
};

/****************************************************************
 * Bitmap helpers for character classes
 ****************************************************************/

static void
set_bit(unsigned char *set, int c)
{
    set[(c & 0xff) >> 3] |= (unsigned char)(1u << (c & 7));
}

static int
get_bit(const unsigned char *set, int c)
{
    return (set[(c & 0xff) >> 3] >> (c & 7)) & 1;
}

static void
set_range(unsigned char *set, int lo, int hi)
{
    int i;

    for (i = lo; i <= hi; i++)
        set_bit(set, i);
}

static void
invert_set(unsigned char *set)
{
    int i;

    for (i = 0; i < 32; i++)
        set[i] = (unsigned char)~set[i];
}

static int
is_word(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/****************************************************************
 * The backtracking matcher
 ****************************************************************/

/* A pending alternative: resume at `pc`/`sp`, after undoing every capture
 * recorded since this point (down to undo-log length `ulen`). */
typedef struct {
    int pc;
    long sp;
    long ulen;
} rx_choice;

/* One capture slot change, so backtracking can roll it back. */
typedef struct {
    int slot;
    long old;
} rx_undo;

typedef struct {
    rx_inst *prog;
    const char *text;
    long len;
    int flags;
    long *sav;          /* capture slots, 2 per group plus group 0 */
    int nsav;
    long steps;         /* remaining instruction budget            */
    int aborted;        /* set on budget exhaustion or allocation failure */

    rx_choice *cs;      /* choice-point (backtracking) stack       */
    long cn, ccap;
    rx_undo *ul;        /* capture undo log                        */
    long un, ucap;
} rx_ctx;

static int
chr_eq(rx_ctx *c, int a, int b)
{
    if (c->flags & RX_ICASE)
        return tolower((unsigned char)a) == tolower((unsigned char)b);
    return a == b;
}

/* Class membership. Case-insensitive bracket classes are folded at
 * compile time, so the flags are not consulted here. */
static int
in_class(rx_ctx *c, const unsigned char *set, int ch)
{
    (void)c;
    return get_bit(set, ch);
}

/* Record a capture-slot change on the undo log. Returns ERR on
 * allocation failure. */
static int
log_save(rx_ctx *c, int slot, long old)
{
    if (c->un >= c->ucap) {
        long ncap = c->ucap ? c->ucap * 2 : 64;
        rx_undo *nu = realloc(c->ul, (size_t)ncap * sizeof *nu);

        if (!nu)
            return ERR;
        c->ul = nu;
        c->ucap = ncap;
    }
    c->ul[c->un].slot = slot;
    c->ul[c->un].old = old;
    c->un++;
    return OK;
}

/* Push a pending alternative. Returns ERR on allocation failure. */
static int
push_choice(rx_ctx *c, int pc, long sp)
{
    if (c->cn >= c->ccap) {
        long ncap = c->ccap ? c->ccap * 2 : 64;
        rx_choice *nc = realloc(c->cs, (size_t)ncap * sizeof *nc);

        if (!nc)
            return ERR;
        c->cs = nc;
        c->ccap = ncap;
    }
    c->cs[c->cn].pc = pc;
    c->cs[c->cn].sp = sp;
    c->cs[c->cn].ulen = c->un;
    c->cn++;
    return OK;
}

/* Iterative backtracking matcher. Choice points and capture undo live on
 * heap stacks rather than the C call stack, so match depth is bounded by
 * available memory, not by the native stack. Returns the text offset
 * where the match completes, or -1 on failure (c->aborted distinguishes
 * a hard error such as the step budget or an allocation failure). */
static long
rx_run(rx_ctx *c, int pc, long sp)
{
    c->cn = 0;
    c->un = 0;

    for (;;) {
        rx_inst *in;
        int fail = 0;

        if (--c->steps < 0) {
            c->aborted = 1;
            return -1;
        }
        in = &c->prog[pc];

        switch (in->op) {
        case I_CHAR:
            if (sp < c->len && chr_eq(c, c->text[sp], in->c)) {
                pc++;
                sp++;
            } else {
                fail = 1;
            }
            break;

        case I_ANY:
            if (sp < c->len &&
                ((c->flags & RX_DOTALL) || c->text[sp] != '\n')) {
                pc++;
                sp++;
            } else {
                fail = 1;
            }
            break;

        case I_CLASS:
            if (sp < c->len &&
                in_class(c, in->set, (unsigned char)c->text[sp])) {
                pc++;
                sp++;
            } else {
                fail = 1;
            }
            break;

        case I_BOL:
            if (sp == 0 ||
                ((c->flags & RX_MULTILINE) && c->text[sp - 1] == '\n'))
                pc++;
            else
                fail = 1;
            break;

        case I_EOL:
            if (sp == c->len ||
                ((c->flags & RX_MULTILINE) && c->text[sp] == '\n'))
                pc++;
            else
                fail = 1;
            break;

        case I_WB:
        case I_NWB:
        case I_WSTART:
        case I_WEND: {
            int left = sp > 0 ? is_word((unsigned char)c->text[sp - 1]) : 0;
            int right = sp < c->len ? is_word((unsigned char)c->text[sp]) : 0;
            int ok;

            if (in->op == I_WB)
                ok = left != right;
            else if (in->op == I_NWB)
                ok = left == right;
            else if (in->op == I_WSTART)
                ok = right && !left;
            else
                ok = left && !right;
            if (ok)
                pc++;
            else
                fail = 1;
            break;
        }

        case I_SAVE:
            if (in->x < c->nsav) {
                if (log_save(c, in->x, c->sav[in->x]) != OK) {
                    c->aborted = 1;
                    return -1;
                }
                c->sav[in->x] = sp;
            }
            pc++;
            break;

        case I_BREF: {
            int g = in->x;
            long s, e, n, i;

            if (2 * g + 1 >= c->nsav) {
                pc++;
                break;
            }
            s = c->sav[2 * g];
            e = c->sav[2 * g + 1];
            if (s < 0 || e < 0) {       /* unset group matches empty */
                pc++;
                break;
            }
            n = e - s;
            if (sp + n > c->len) {
                fail = 1;
                break;
            }
            for (i = 0; i < n; i++) {
                if (!chr_eq(c, c->text[sp + i], c->text[s + i])) {
                    fail = 1;
                    break;
                }
            }
            if (!fail) {
                sp += n;
                pc++;
            }
            break;
        }

        case I_JMP:
            pc = in->x;
            break;

        case I_SPLIT:
            if (push_choice(c, in->y, sp) != OK) {
                c->aborted = 1;
                return -1;
            }
            pc = in->x;
            break;

        case I_MARK:
            /* Record the iteration-entry position. No undo entry: the
             * slot is always re-marked before the matching I_PROGRESS
             * reads it, so it never needs restoring on backtrack. */
            if (in->x < c->nsav)
                c->sav[in->x] = sp;
            pc++;
            break;

        case I_PROGRESS:
            /* A repetition body that consumed nothing: take the loop
             * exit instead of looping forever. */
            if (in->x < c->nsav && c->sav[in->x] == sp)
                pc = in->y;
            else
                pc++;
            break;

        case I_MATCH:
            return sp;

        default:
            fail = 1;
            break;
        }

        if (fail) {
            rx_choice cp;

            if (c->cn == 0)
                return -1;
            cp = c->cs[--c->cn];
            while (c->un > cp.ulen) {
                c->un--;
                c->sav[c->ul[c->un].slot] = c->ul[c->un].old;
            }
            pc = cp.pc;
            sp = cp.sp;
        }
    }
}

/****************************************************************
 * Compiler state and low-level emit helpers
 ****************************************************************/

typedef struct {
    const char *p;          /* current parse position */
    const char *pend;
    rx_inst *prog;
    int plen, pcap;
    int ngroup;
    int nmark;              /* scratch slots allocated for repetition guards */
    int maxref;             /* highest backreference seen */
    int depth;              /* current group-nesting depth */
    int flags;
    const char *err;
    unsigned char **sets;
    int nsets, setcap;
} comp;

/* A detached copy of a run of instructions, with jump targets stored
 * relative to the start of the run so the run can be re-emitted at any
 * offset. Used to expand quantifiers and lay out alternation branches. */
typedef struct {
    rx_inst *in;
    int n;
} tpl;

static int
emit(comp *c, int op)
{
    rx_inst *in;

    if (c->err)
        return 0;
    if (c->plen >= c->pcap) {
        int ncap = c->pcap ? c->pcap * 2 : 64;
        rx_inst *np = realloc(c->prog, (size_t)ncap * sizeof *np);

        if (!np) {
            c->err = "out of memory";
            return 0;
        }
        c->prog = np;
        c->pcap = ncap;
    }
    in = &c->prog[c->plen];
    memset(in, 0, sizeof *in);
    in->op = op;
    return c->plen++;
}

static unsigned char *
new_set(comp *c)
{
    unsigned char *set;

    if (c->err)
        return NULL;
    if (c->nsets >= c->setcap) {
        int ncap = c->setcap ? c->setcap * 2 : 8;
        unsigned char **ns = realloc(c->sets, (size_t)ncap * sizeof *ns);

        if (!ns) {
            c->err = "out of memory";
            return NULL;
        }
        c->sets = ns;
        c->setcap = ncap;
    }
    set = calloc(32, 1);
    if (!set) {
        c->err = "out of memory";
        return NULL;
    }
    c->sets[c->nsets++] = set;
    return set;
}

/* Detach prog[from .. plen) into a template and truncate the program
 * back to `from`. Jump targets inside the run are rebased to zero. */
static tpl
take_tpl(comp *c, int from)
{
    tpl t;
    int i;

    t.n = c->plen - from;
    t.in = NULL;
    if (c->err || t.n <= 0) {
        t.n = t.n < 0 ? 0 : t.n;
        c->plen = from;
        return t;
    }
    t.in = malloc((size_t)t.n * sizeof *t.in);
    if (!t.in) {
        c->err = "out of memory";
        t.n = 0;
        c->plen = from;
        return t;
    }
    for (i = 0; i < t.n; i++) {
        t.in[i] = c->prog[from + i];
        if (t.in[i].op == I_JMP) {
            t.in[i].x -= from;
        } else if (t.in[i].op == I_SPLIT) {
            t.in[i].x -= from;
            t.in[i].y -= from;
        } else if (t.in[i].op == I_PROGRESS) {
            t.in[i].y -= from;      /* x is a scratch slot, not a target */
        }
    }
    c->plen = from;
    return t;
}

/* Append a template at the current program end, rebasing jump targets. */
static void
put_tpl(comp *c, tpl t)
{
    int base, i;

    if (c->err)
        return;
    base = c->plen;
    for (i = 0; i < t.n; i++) {
        int at = emit(c, t.in[i].op);
        rx_inst *in;

        if (c->err)
            return;
        in = &c->prog[at];
        *in = t.in[i];
        if (in->op == I_JMP) {
            in->x += base;
        } else if (in->op == I_SPLIT) {
            in->x += base;
            in->y += base;
        } else if (in->op == I_PROGRESS) {
            in->y += base;          /* x is a scratch slot, not a target */
        }
    }
}

/****************************************************************
 * Recursive-descent compiler
 ****************************************************************/

static void parse_alt(comp *c);
static void parse_concat(comp *c);
static void parse_repeat(comp *c);
static void parse_atom(comp *c);

static int
hexval(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* Read a backslash escape that resolves to a single literal byte. The
 * backslash has already been consumed. Returns the byte value. */
static int
read_escape_char(comp *c)
{
    int e;

    if (c->p >= c->pend) {
        c->err = "trailing backslash";
        return 0;
    }
    e = (unsigned char)*c->p++;
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return '\a';
    case '0': return '\0';
    case 'x': {
        int h1, h2, v;

        if (c->p >= c->pend || (h1 = hexval((unsigned char)*c->p)) < 0)
            return 'x';         /* lone \x is a literal x */
        c->p++;
        v = h1;
        if (c->p < c->pend && (h2 = hexval((unsigned char)*c->p)) >= 0) {
            c->p++;
            v = v * 16 + h2;
        }
        return v;
    }
    default:
        return e;               /* literal: covers \\ \. \* \( and so on */
    }
}

/* Fill `set` (expected zeroed) with the members of a Perl-style shorthand
 * letter, inverting for the upper-case (negated) forms. */
static void
fill_shorthand(unsigned char *set, int letter)
{
    int negate = 0;

    switch (letter) {
    case 'D': negate = 1; /* fall through */
    case 'd':
        set_range(set, '0', '9');
        break;
    case 'W': negate = 1; /* fall through */
    case 'w':
        set_range(set, 'a', 'z');
        set_range(set, 'A', 'Z');
        set_range(set, '0', '9');
        set_bit(set, '_');
        break;
    case 'S': negate = 1; /* fall through */
    case 's':
        set_bit(set, ' ');
        set_bit(set, '\t');
        set_bit(set, '\n');
        set_bit(set, '\r');
        set_bit(set, '\f');
        set_bit(set, '\v');
        break;
    }
    if (negate)
        invert_set(set);
}

/* Fill a fresh class set for a Perl-style shorthand letter. */
static unsigned char *
shorthand_set(comp *c, int letter)
{
    unsigned char *set = new_set(c);

    if (!set)
        return NULL;
    fill_shorthand(set, letter);
    return set;
}

/* Add a POSIX [:name:] class to a bracket-expression set. On entry p
 * points just past "[:" . Returns OK, or ERR with c->err set. */
static int
add_posix_class(comp *c, unsigned char *set)
{
    const char *start = c->p;
    char name[16];
    size_t n = 0;
    int i;

    while (c->p < c->pend && *c->p != ':') {
        if (n < sizeof name - 1)
            name[n++] = *c->p;
        c->p++;
    }
    name[n] = '\0';
    if (c->p + 1 >= c->pend || c->p[0] != ':' || c->p[1] != ']') {
        c->p = start;
        c->err = "bad [: :] class";
        return ERR;
    }
    c->p += 2;      /* consume ":]" */

    for (i = 0; i < 256; i++) {
        int hit = 0;

        if (!strcmp(name, "alpha")) hit = isalpha(i);
        else if (!strcmp(name, "digit")) hit = isdigit(i);
        else if (!strcmp(name, "alnum")) hit = isalnum(i);
        else if (!strcmp(name, "space")) hit = isspace(i);
        else if (!strcmp(name, "upper")) hit = isupper(i);
        else if (!strcmp(name, "lower")) hit = islower(i);
        else if (!strcmp(name, "punct")) hit = ispunct(i);
        else if (!strcmp(name, "xdigit")) hit = isxdigit(i);
        else if (!strcmp(name, "cntrl")) hit = iscntrl(i);
        else if (!strcmp(name, "print")) hit = isprint(i);
        else if (!strcmp(name, "graph")) hit = isgraph(i);
        else if (!strcmp(name, "blank")) hit = (i == ' ' || i == '\t');
        else {
            c->err = "unknown [: :] class";
            return ERR;
        }
        if (hit)
            set_bit(set, i);
    }
    return OK;
}

/* Read one character inside a bracket expression, resolving escapes. */
static int
bracket_char(comp *c)
{
    if (*c->p == '\\') {
        c->p++;
        return read_escape_char(c);
    }
    return (unsigned char)*c->p++;
}

static void
parse_class(comp *c)
{
    unsigned char *set = new_set(c);
    int negate = 0;
    int first = 1;

    if (c->err)
        return;
    c->p++;             /* consume '[' */
    if (c->p < c->pend && *c->p == '^') {
        negate = 1;
        c->p++;
    }
    while (c->p < c->pend) {
        int lo;

        if (*c->p == ']' && !first)
            break;
        first = 0;

        if (*c->p == '[' && c->p + 1 < c->pend && c->p[1] == ':') {
            c->p += 2;
            if (add_posix_class(c, set) != OK)
                return;
            continue;
        }

        /* A shorthand class (\d \D \w \W \s \S) contributes its whole
         * membership to the bracket set rather than a single byte. */
        if (*c->p == '\\' && c->p + 1 < c->pend &&
            strchr("dDwWsS", (unsigned char)c->p[1])) {
            unsigned char tmp[32];
            int i;

            memset(tmp, 0, sizeof tmp);
            fill_shorthand(tmp, (unsigned char)c->p[1]);
            for (i = 0; i < 32; i++)
                set[i] |= tmp[i];
            c->p += 2;
            continue;
        }

        lo = bracket_char(c);
        if (c->err)
            return;
        if (c->p + 1 <= c->pend && *c->p == '-' &&
            c->p + 1 < c->pend && c->p[1] != ']') {
            int hi;

            c->p++;             /* consume '-' */
            hi = bracket_char(c);
            if (c->err)
                return;
            if (hi < lo) {
                c->err = "bad range in [ ]";
                return;
            }
            set_range(set, lo, hi);
        } else {
            set_bit(set, lo);
        }
    }
    if (c->p >= c->pend || *c->p != ']') {
        c->err = "unterminated [ ]";
        return;
    }
    c->p++;             /* consume ']' */

    /* Case folding must happen before negation so that a negated class
     * excludes both cases of each letter. */
    if (c->flags & RX_ICASE) {
        int i;

        for (i = 0; i < 256; i++) {
            if (get_bit(set, i) && isalpha(i)) {
                set_bit(set, tolower(i));
                set_bit(set, toupper(i));
            }
        }
    }
    if (negate)
        invert_set(set);

    {
        int at = emit(c, I_CLASS);

        if (!c->err)
            c->prog[at].set = set;
    }
}

static void
parse_escape(comp *c)
{
    int e;

    if (c->p >= c->pend) {
        c->err = "trailing backslash";
        return;
    }
    e = (unsigned char)*c->p;
    switch (e) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
        unsigned char *set;

        c->p++;
        set = shorthand_set(c, e);
        if (set) {
            int at = emit(c, I_CLASS);

            if (!c->err)
                c->prog[at].set = set;
        }
        return;
    }
    case 'b': c->p++; emit(c, I_WB); return;
    case 'B': c->p++; emit(c, I_NWB); return;
    case '<': c->p++; emit(c, I_WSTART); return;
    case '>': c->p++; emit(c, I_WEND); return;
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9': {
        int g = e - '0';
        int at;

        c->p++;
        if (g > c->maxref)
            c->maxref = g;
        at = emit(c, I_BREF);
        if (!c->err)
            c->prog[at].x = g;
        return;
    }
    default: {
        int ch = read_escape_char(c);
        int at;

        at = emit(c, I_CHAR);
        if (!c->err)
            c->prog[at].c = (unsigned char)ch;
        return;
    }
    }
}

static void
parse_atom(comp *c)
{
    int ch;

    if (c->err)
        return;
    if (c->p >= c->pend) {
        c->err = "unexpected end of pattern";
        return;
    }
    ch = (unsigned char)*c->p;
    switch (ch) {
    case '(': {
        int capturing = 1;
        int g = 0;

        c->p++;
        if (c->p + 1 < c->pend && c->p[0] == '?' && c->p[1] == ':') {
            c->p += 2;
            capturing = 0;
        }
        if (++c->depth > RX_MAX_DEPTH) {
            c->err = "pattern nested too deeply";
            return;
        }
        if (capturing) {
            g = ++c->ngroup;
            {
                int at = emit(c, I_SAVE);
                if (!c->err)
                    c->prog[at].x = 2 * g;
            }
        }
        parse_alt(c);
        c->depth--;
        if (c->err)
            return;
        if (c->p >= c->pend || *c->p != ')') {
            c->err = "missing )";
            return;
        }
        c->p++;
        if (capturing) {
            int at = emit(c, I_SAVE);
            if (!c->err)
                c->prog[at].x = 2 * g + 1;
        }
        return;
    }
    case '[':
        parse_class(c);
        return;
    case '.':
        c->p++;
        emit(c, I_ANY);
        return;
    case '^':
        c->p++;
        emit(c, I_BOL);
        return;
    case '$':
        c->p++;
        emit(c, I_EOL);
        return;
    case '\\':
        c->p++;
        parse_escape(c);
        return;
    case ')':
    case '|':
    case '*':
    case '+':
    case '?':
        c->err = "unexpected quantifier or operator";
        return;
    default: {
        /* Ordinary character. '{' and '}' fall here too and are treated
         * as literals when they are not a valid interval. */
        int at = emit(c, I_CHAR);
        if (!c->err)
            c->prog[at].c = (unsigned char)ch;
        c->p++;
        return;
    }
    }
}

/* Lay out `t` repeated according to {n,m}. m < 0 means unbounded. */
static void
build_rep(comp *c, tpl t, int n, int m, int lazy)
{
    int i;

    for (i = 0; i < n; i++)
        put_tpl(c, t);
    if (c->err)
        return;

    if (m < 0) {
        /* Trailing star appended after the n mandatory copies:
         *
         *   L1: SPLIT body, end      (greedy; swapped when lazy)
         *   body: MARK slot          record the iteration-entry position
         *         <t>
         *         PROGRESS slot, end  exit if the body consumed nothing
         *         JMP L1
         *   end:
         *
         * The MARK/PROGRESS pair stops a nullable body (such as a*) from
         * looping forever with no progress. */
        int slot = c->nmark++;
        int sp = emit(c, I_SPLIT);
        int b, mk, pr, j, end;

        if (c->err)
            return;
        b = c->plen;
        mk = emit(c, I_MARK);
        if (c->err)
            return;
        c->prog[mk].x = slot;       /* rebased to a real slot after parse */
        put_tpl(c, t);
        pr = emit(c, I_PROGRESS);
        j = emit(c, I_JMP);
        if (c->err)
            return;
        c->prog[j].x = sp;
        end = c->plen;
        c->prog[pr].x = slot;
        c->prog[pr].y = end;
        if (lazy) {
            c->prog[sp].x = end;
            c->prog[sp].y = b;
        } else {
            c->prog[sp].x = b;
            c->prog[sp].y = end;
        }
        return;
    }

    {
        int opt = m - n;
        int *splits;
        int k, end;

        if (opt <= 0)
            return;
        splits = malloc((size_t)opt * sizeof *splits);
        if (!splits) {
            c->err = "out of memory";
            return;
        }
        for (k = 0; k < opt; k++) {
            int sp = emit(c, I_SPLIT);
            int b;

            if (c->err) {
                free(splits);
                return;
            }
            b = c->plen;
            put_tpl(c, t);
            if (c->err) {
                free(splits);
                return;
            }
            if (lazy)
                c->prog[sp].y = b;
            else
                c->prog[sp].x = b;
            splits[k] = sp;
        }
        end = c->plen;
        for (k = 0; k < opt; k++) {
            if (lazy)
                c->prog[splits[k]].x = end;
            else
                c->prog[splits[k]].y = end;
        }
        free(splits);
    }
}

/* Parse the quantifier (if any) following an atom whose code occupies
 * prog[from .. plen). */
static void
parse_quantifier(comp *c, int from)
{
    int n = 1, m = 1;
    int have = 0;
    int lazy = 0;
    tpl t;

    if (c->err || c->p >= c->pend)
        return;

    switch (*c->p) {
    case '*':
        n = 0;
        m = -1;
        have = 1;
        c->p++;
        break;
    case '+':
        n = 1;
        m = -1;
        have = 1;
        c->p++;
        break;
    case '?':
        n = 0;
        m = 1;
        have = 1;
        c->p++;
        break;
    case '{': {
        const char *save = c->p;
        const char *q = c->p + 1;
        int lo = 0, hi, sawlo = 0;

        while (q < c->pend && isdigit((unsigned char)*q)) {
            /* Saturate rather than overflow; the cap check below rejects. */
            lo = lo > RX_DUP_MAX ? RX_DUP_MAX + 1 : lo * 10 + (*q - '0');
            sawlo = 1;
            q++;
        }
        if (q < c->pend && *q == ',') {
            q++;
            if (q < c->pend && isdigit((unsigned char)*q)) {
                hi = 0;
                while (q < c->pend && isdigit((unsigned char)*q)) {
                    hi = hi > RX_DUP_MAX ? RX_DUP_MAX + 1 : hi * 10 + (*q - '0');
                    q++;
                }
            } else {
                hi = -1;        /* {n,} unbounded */
            }
        } else {
            if (!sawlo) {
                /* "{" not followed by a count: treat as literal */
                return;
            }
            hi = lo;
        }
        if (q >= c->pend || *q != '}') {
            /* not a valid interval: leave '{' as a literal */
            c->p = save;
            return;
        }
        q++;                    /* consume '}' */
        if (lo > RX_DUP_MAX || hi > RX_DUP_MAX) {
            c->err = "repetition count too large";
            return;
        }
        if (hi >= 0 && hi < lo) {
            c->err = "bad {n,m} interval";
            return;
        }
        n = lo;
        m = hi;
        have = 1;
        c->p = q;
        break;
    }
    default:
        return;
    }

    if (!have)
        return;
    if (c->p < c->pend && *c->p == '?') {
        lazy = 1;
        c->p++;
    }

    t = take_tpl(c, from);
    if (c->err) {
        free(t.in);
        return;
    }
    build_rep(c, t, n, m < 0 ? -1 : m, lazy);
    free(t.in);
}

static void
parse_repeat(comp *c)
{
    int from = c->plen;

    parse_atom(c);
    if (c->err)
        return;
    parse_quantifier(c, from);
}

static void
parse_concat(comp *c)
{
    while (!c->err && c->p < c->pend && *c->p != '|' && *c->p != ')')
        parse_repeat(c);
}

static void
parse_alt(comp *c)
{
    int nbr = 0, cap = 0;
    tpl *branches = NULL;
    int from, i, k;
    int *jmps;

    /* Collect each alternative as a template. */
    for (;;) {
        from = c->plen;
        parse_concat(c);
        if (c->err)
            goto done;
        if (nbr >= cap) {
            int ncap = cap ? cap * 2 : 4;
            tpl *nb = realloc(branches, (size_t)ncap * sizeof *nb);

            if (!nb) {
                c->err = "out of memory";
                goto done;
            }
            branches = nb;
            cap = ncap;
        }
        branches[nbr++] = take_tpl(c, from);
        if (c->err)
            goto done;
        if (c->p < c->pend && *c->p == '|') {
            c->p++;
            continue;
        }
        break;
    }

    if (nbr == 1) {
        put_tpl(c, branches[0]);
        goto done;
    }

    /* Emit nested SPLIT/JMP so earlier branches are preferred. */
    jmps = malloc((size_t)(nbr - 1) * sizeof *jmps);
    if (!jmps) {
        c->err = "out of memory";
        goto done;
    }
    for (i = 0; i < nbr - 1; i++) {
        int sp = emit(c, I_SPLIT);

        if (c->err)
            break;
        c->prog[sp].x = c->plen;
        put_tpl(c, branches[i]);
        jmps[i] = emit(c, I_JMP);
        if (c->err)
            break;
        c->prog[sp].y = c->plen;
    }
    if (!c->err)
        put_tpl(c, branches[nbr - 1]);
    if (!c->err) {
        int end = c->plen;

        for (k = 0; k < nbr - 1; k++)
            c->prog[jmps[k]].x = end;
    }
    free(jmps);

done:
    for (i = 0; i < nbr; i++)
        free(branches[i].in);
    free(branches);
}

/****************************************************************
 * Public API: compile, exec, free
 ****************************************************************/

rx_t *
rx_compile(const char *pattern, int flags, const char **errp)
{
    comp c;
    rx_t *re;
    int at;

    memset(&c, 0, sizeof c);
    c.p = pattern;
    c.pend = pattern + strlen(pattern);
    c.flags = flags;

    emit(&c, I_SAVE);           /* slot 0: whole-match start */
    parse_alt(&c);
    if (!c.err && c.p != c.pend)
        c.err = "unbalanced ) or trailing characters";
    at = emit(&c, I_SAVE);      /* slot 1: whole-match end */
    if (!c.err)
        c.prog[at].x = 1;
    emit(&c, I_MATCH);

    if (!c.err && c.maxref > c.ngroup)
        c.err = "backreference to undefined group";

    /* Repetition-guard scratch slots are numbered from zero during the
     * parse; place them above the capture slots now that the group count
     * is final. */
    if (!c.err && c.nmark > 0) {
        int base = 2 * (c.ngroup + 1);
        int i;

        for (i = 0; i < c.plen; i++) {
            if (c.prog[i].op == I_MARK || c.prog[i].op == I_PROGRESS)
                c.prog[i].x += base;
        }
    }

    if (c.err) {
        int i;

        if (errp)
            *errp = c.err;
        for (i = 0; i < c.nsets; i++)
            free(c.sets[i]);
        free(c.sets);
        free(c.prog);
        return NULL;
    }

    re = malloc(sizeof *re);
    if (!re) {
        int i;

        if (errp)
            *errp = "out of memory";
        for (i = 0; i < c.nsets; i++)
            free(c.sets[i]);
        free(c.sets);
        free(c.prog);
        return NULL;
    }
    re->prog = c.prog;
    re->plen = c.plen;
    re->ngroup = c.ngroup;
    re->nmark = c.nmark;
    re->flags = c.flags;
    re->sets = c.sets;
    re->nsets = c.nsets;

    /* Decide once whether any match can contain a newline. A backreference
     * can only reproduce bytes some other instruction already matched, so
     * scanning the literal, any, and class instructions covers it. This
     * mirrors the matcher: I_ANY eats a newline only under RX_DOTALL, and
     * a class set already has case folding and negation baked in. */
    {
        int i;

        re->matches_nl = 0;
        for (i = 0; i < re->plen; i++) {
            rx_inst *in = &re->prog[i];

            if ((in->op == I_CHAR && in->c == '\n') ||
                (in->op == I_ANY && (re->flags & RX_DOTALL)) ||
                (in->op == I_CLASS && get_bit(in->set, '\n'))) {
                re->matches_nl = 1;
                break;
            }
        }
    }

    if (errp)
        *errp = NULL;
    return re;
}

void
rx_free(rx_t *re)
{
    int i;

    if (!re)
        return;
    for (i = 0; i < re->nsets; i++)
        free(re->sets[i]);
    free(re->sets);
    free(re->prog);
    free(re);
}

int
rx_ngroups(const rx_t *re)
{
    return re ? re->ngroup : 0;
}

int
rx_exec(rx_t *re, const char *text, size_t len, size_t start,
        rx_match *m, int nmatch)
{
    rx_ctx ctx;
    long *sav;
    int nsav, i;
    long begin;

    if (!re)
        return ERR;
    nsav = 2 * (re->ngroup + 1) + re->nmark;    /* captures + scratch slots */
    sav = malloc((size_t)nsav * sizeof *sav);
    if (!sav)
        return ERR;

    ctx.prog = re->prog;
    ctx.text = text;
    ctx.len = (long)len;
    ctx.flags = re->flags;
    ctx.sav = sav;
    ctx.nsav = nsav;
    ctx.cs = NULL;
    ctx.cn = ctx.ccap = 0;
    ctx.ul = NULL;
    ctx.un = ctx.ucap = 0;

    for (begin = (long)start; begin <= (long)len; begin++) {
        long r;

        for (i = 0; i < nsav; i++)
            sav[i] = -1;
        ctx.steps = RX_STEP_LIMIT;
        ctx.aborted = 0;

        r = rx_run(&ctx, 0, begin);
        if (ctx.aborted) {
            free(ctx.cs);
            free(ctx.ul);
            free(sav);
            return ERR;
        }
        if (r >= 0) {
            int ng = re->ngroup + 1;

            for (i = 0; i < nmatch; i++) {
                if (i < ng) {
                    m[i].so = sav[2 * i];
                    m[i].eo = sav[2 * i + 1];
                } else {
                    m[i].so = m[i].eo = -1;
                }
            }
            free(ctx.cs);
            free(ctx.ul);
            free(sav);
            return 1;
        }
    }
    free(ctx.cs);
    free(ctx.ul);
    free(sav);
    return 0;
}

int
rx_matches_newline(const rx_t *re)
{
    return re ? re->matches_nl : 0;
}

/* Find the start offset of the last non-overlapping match that begins
 * strictly before `limit`, or -1 if there is none. Returns -2 on a
 * matcher error so the caller can propagate it. */
static long
last_start_before(rx_t *re, const char *text, size_t len, long limit)
{
    long pos = 0;
    long best = -1;
    rx_match m0;

    while (pos <= (long)len) {
        int r = rx_exec(re, text, len, (size_t)pos, &m0, 1);

        if (r < 0)
            return -2;
        if (r == 0 || m0.so >= limit)
            break;
        best = m0.so;
        pos = m0.eo > m0.so ? m0.eo : m0.so + 1;
    }
    return best;
}

int
rx_search(rx_t *re, const char *text, size_t len, size_t from,
          int sflags, rx_match *m, int nmatch)
{
    if (!re)
        return ERR;

    if (!(sflags & RX_BACKWARD)) {
        int r = rx_exec(re, text, len, from, m, nmatch);

        if (r != 0)                 /* a hit, or an error: done */
            return r;
        if ((sflags & RX_WRAP) && from > 0)
            return rx_exec(re, text, len, 0, m, nmatch);
        return 0;
    }

    /* Backward: the last match beginning before `from`. With wrap, fall
     * back to the last match anywhere, which is the one reached by
     * continuing past the top of the buffer. */
    {
        long start = last_start_before(re, text, len, (long)from);

        if (start == -2)
            return ERR;
        if (start < 0 && (sflags & RX_WRAP))
            start = last_start_before(re, text, len, (long)len + 1);
        if (start == -2)
            return ERR;
        if (start < 0)
            return 0;
        return rx_exec(re, text, len, (size_t)start, m, nmatch);
    }
}

/****************************************************************
 * Substitution
 ****************************************************************/

typedef struct {
    char *b;
    size_t len, cap;
    int oom;
} sbuf;

static void
sb_reserve(sbuf *s, size_t extra)
{
    if (s->oom)
        return;
    if (s->len + extra + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 64;
        char *nb;

        while (ncap < s->len + extra + 1)
            ncap *= 2;
        nb = realloc(s->b, ncap);
        if (!nb) {
            s->oom = 1;
            return;
        }
        s->b = nb;
        s->cap = ncap;
    }
}

static void
sb_putc(sbuf *s, int ch)
{
    sb_reserve(s, 1);
    if (s->oom)
        return;
    s->b[s->len++] = (char)ch;
}

static void
sb_put(sbuf *s, const char *p, size_t n)
{
    sb_reserve(s, n);
    if (s->oom)
        return;
    memcpy(s->b + s->len, p, n);
    s->len += n;
}

/* Case-conversion state for the replacement template. */
typedef struct {
    int mode;       /* 0 none, 'U' upper, 'L' lower (sticky)   */
    int once;       /* 0 none, 'u' upper, 'l' lower (one char) */
} cstate;

static void
put_cased(sbuf *out, cstate *cs, int ch)
{
    if (cs->once == 'u') {
        ch = toupper((unsigned char)ch);
        cs->once = 0;
    } else if (cs->once == 'l') {
        ch = tolower((unsigned char)ch);
        cs->once = 0;
    } else if (cs->mode == 'U') {
        ch = toupper((unsigned char)ch);
    } else if (cs->mode == 'L') {
        ch = tolower((unsigned char)ch);
    }
    sb_putc(out, ch);
}

static void
put_group(sbuf *out, cstate *cs, const char *text, rx_match *m,
          int ng, int g)
{
    long i;

    if (g < 0 || g >= ng)
        return;
    if (m[g].so < 0 || m[g].eo < 0)
        return;
    for (i = m[g].so; i < m[g].eo; i++)
        put_cased(out, cs, (unsigned char)text[i]);
}

/* Expand one replacement template against the current match. */
static void
expand(sbuf *out, const char *repl, const char *text, rx_match *m, int ng)
{
    const char *r = repl;
    cstate cs = { 0, 0 };

    while (*r) {
        if (*r == '&') {
            put_group(out, &cs, text, m, ng, 0);
            r++;
            continue;
        }
        if (*r != '\\') {
            put_cased(out, &cs, (unsigned char)*r);
            r++;
            continue;
        }
        r++;                    /* consume backslash */
        if (!*r) {
            sb_putc(out, '\\');
            break;
        }
        switch (*r) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            put_group(out, &cs, text, m, ng, *r - '0');
            break;
        case '&': put_cased(out, &cs, '&'); break;
        case '\\': put_cased(out, &cs, '\\'); break;
        case 'n': sb_putc(out, '\n'); break;
        case 't': sb_putc(out, '\t'); break;
        case 'r': sb_putc(out, '\r'); break;
        case 'f': sb_putc(out, '\f'); break;
        case 'v': sb_putc(out, '\v'); break;
        case 'a': sb_putc(out, '\a'); break;
        case 'x': {
            int h1, h2, v;

            if ((h1 = hexval((unsigned char)r[1])) < 0) {
                put_cased(out, &cs, 'x');   /* lone \x is literal x */
                break;
            }
            r++;                            /* consume first hex digit */
            v = h1;
            if ((h2 = hexval((unsigned char)r[1])) >= 0) {
                r++;                        /* consume second hex digit */
                v = v * 16 + h2;
            }
            sb_putc(out, v);
            break;
        }
        case 'U': cs.mode = 'U'; break;
        case 'L': cs.mode = 'L'; break;
        case 'E': cs.mode = 0; cs.once = 0; break;
        case 'u': cs.once = 'u'; break;
        case 'l': cs.once = 'l'; break;
        default:
            put_cased(out, &cs, (unsigned char)*r);
            break;
        }
        r++;
    }
}

char *
rx_replace(rx_t *re, const char *text, size_t len, const char *repl,
           int flags)
{
    sbuf out = { NULL, 0, 0, 0 };
    rx_match *m;
    int ng;
    long copied = 0;
    long pos = 0;
    long prev_end = -1;

    if (!re)
        return NULL;
    ng = re->ngroup + 1;
    m = malloc((size_t)ng * sizeof *m);
    if (!m)
        return NULL;

    while (pos <= (long)len) {
        int r = rx_exec(re, text, len, (size_t)pos, m, ng);

        if (r < 0) {            /* error (budget) */
            free(m);
            free(out.b);
            return NULL;
        }
        if (r == 0)
            break;

        /* Suppress an empty match abutting the previous match end. */
        if (m[0].so == m[0].eo && m[0].so == prev_end) {
            if (pos >= (long)len)
                break;
            pos++;
            continue;
        }

        sb_put(&out, text + copied, (size_t)(m[0].so - copied));
        expand(&out, repl, text, m, ng);
        copied = m[0].eo;
        prev_end = m[0].eo;

        if (m[0].eo > pos)
            pos = m[0].eo;
        else
            pos = m[0].eo + 1;  /* empty or non-advancing match */

        if (!(flags & RX_GLOBAL))
            break;
    }

    sb_put(&out, text + copied, (size_t)((long)len - copied));
    free(m);

    if (out.oom) {
        free(out.b);
        return NULL;
    }
    if (!out.b)                 /* empty result still returns a string */
        out.b = calloc(1, 1);
    else
        out.b[out.len] = '\0';
    return out.b;
}

/* ----- end vendored rx.c ----- */

/* rx defines OK/ERR with the same values the editor uses below; drop them
 * here so the editor's own definitions stand on their own after re-vendoring. */
#undef OK
#undef ERR

/* ==================== end vendored rx ==================== */

/****************************************************************
 * Configuration file (gitconfig-style key/value).
 *
 * The core never opens a file. The CLI front end, or an embedding host, parses
 * one with vedit_cfg_load() and hands it in with vedit_set_config(); the
 * startup resolvers consult it, ranked below the environment and flags. Keys
 * are dotted: a "[section]" header prefixes the names under it, "[section
 * \"sub\"]" adds a middle component, and "section.key = value" works without a
 * header. '#' and ';' start comments. A later value for a key wins.
 ****************************************************************/

typedef struct cfg Cfg;

typedef struct cfg_entry {
	char	*key;
	char	*value;
} Cfgent;

struct cfg {
	Cfgent	*entries;
	int	 count;
	int	 alloc;
};

static char *
cfg_dup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p)
		memcpy(p, s, n);
	return p;
}

Cfg *
vedit_cfg_new(void)
{
	return calloc(1, sizeof(Cfg));
}

void
vedit_cfg_free(Cfg *c)
{
	int i;

	if (!c)
		return;
	for (i = 0; i < c->count; i++) {
		free(c->entries[i].key);
		free(c->entries[i].value);
	}
	free(c->entries);
	free(c);
}

/* Append a normalized key/value. Like git config, every occurrence is kept in
 * file order: a scalar read (cfg_get) takes the last, and a multivar read walks
 * them in order. Silently drops the pair on an allocation failure. */
static void
cfg_set(Cfg *c, const char *key, const char *value)
{
	char *k, *v;

	if (c->count >= c->alloc) {
		int na = c->alloc ? c->alloc * 2 : 16;
		Cfgent *ne = realloc(c->entries, (size_t)na * sizeof(*ne));

		if (!ne)
			return;
		c->entries = ne;
		c->alloc = na;
	}
	k = cfg_dup(key);
	v = cfg_dup(value);
	if (!k || !v) {
		free(k);
		free(v);
		return;
	}
	c->entries[c->count].key = k;
	c->entries[c->count].value = v;
	c->count++;
}

/* The last value set for key (later occurrences win), or NULL. */
static const char *
cfg_get(const Cfg *c, const char *key)
{
	const char *hit = NULL;
	int i;

	if (!c)
		return NULL;
	for (i = 0; i < c->count; i++)
		if (strcmp(c->entries[i].key, key) == 0)
			hit = c->entries[i].value;
	return hit;
}

/* Interpret a string as a boolean, returning def when it is not recognized. */
static int
str_bool(const char *v, int def)
{
	if (!v)
		return def;
	if (strcmp(v, "on") == 0 || strcmp(v, "yes") == 0 ||
	    strcmp(v, "true") == 0 || strcmp(v, "1") == 0)
		return 1;
	if (strcmp(v, "off") == 0 || strcmp(v, "no") == 0 ||
	    strcmp(v, "false") == 0 || strcmp(v, "0") == 0)
		return 0;
	return def;
}

/* Interpret a config value as a boolean. Returns def when c has no such key or
 * the value is not recognized. */
static int
cfg_bool(const Cfg *c, const char *key, int def)
{
	return str_bool(cfg_get(c, key), def);
}

static char *
cfg_skip_ws(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static void
cfg_trim_end(char *s)
{
	char *end = s + strlen(s);

	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	    end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';
}

/* Join section + subsection + name into a dotted key written to out. */
static void
cfg_make_key(char *out, size_t outsz, const char *section,
    const char *subsect, const char *name)
{
	if (section[0] == '\0')
		snprintf(out, outsz, "%s", name);
	else if (subsect)
		snprintf(out, outsz, "%s.%s.%s", section, subsect, name);
	else
		snprintf(out, outsz, "%s.%s", section, name);
}

/* Parse config text (modified in place) into c, merging over anything already
 * there. Lines are split on '\n'; a blank or whitespace-only line is skipped.
 * Returns 0 on success, -1 on a syntax error. Shared by the file loader and the
 * built-in default grammars. */
static int
cfg_load_mem(Cfg *c, char *text)
{
	char section[256], key[2048];
	char *subsect = NULL, *cursor = text;
	int rc = 0;

	if (!c)
		return -1;
	section[0] = '\0';
	while (*cursor) {
		char *buf = cursor, *base, *tmp, *nl = strchr(cursor, '\n');

		if (nl) {
			*nl = '\0';
			cursor = nl + 1;
		} else {
			cursor += strlen(cursor);
		}
		buf[strcspn(buf, "\r")] = '\0';		/* drop a CR before the LF */
		/* strip a comment, honoring quoted regions */
		for (tmp = buf; *tmp; tmp++) {
			if (*tmp == '#' || *tmp == ';') {
				*tmp = '\0';
				break;
			}
			if (*tmp == '"') {
				tmp++;
				while (*tmp && *tmp != '"')
					tmp++;
				if (!*tmp)
					break;
			}
		}
		base = cfg_skip_ws(buf);
		if (*base == '\0')
			continue;
		if (*base == '[') {		/* [section] or [section "sub"] */
			char *end, *q;

			base++;
			end = strchr(base, ']');
			if (!end) {
				rc = -1;
				break;
			}
			*end = '\0';
			q = strchr(base, '"');
			free(subsect);
			subsect = NULL;
			if (q) {
				char *q2;

				*q = '\0';
				q2 = strchr(q + 1, '"');
				if (!q2) {
					rc = -1;
					break;
				}
				*q2 = '\0';
				subsect = cfg_dup(q + 1);
				if (!subsect) {	/* OOM: do not mismap the key */
					rc = -1;
					break;
				}
			}
			base = cfg_skip_ws(base);
			cfg_trim_end(base);
			snprintf(section, sizeof(section), "%s", base);
			continue;
		}
		tmp = strchr(base, '=');
		if (!tmp) {
			rc = -1;
			break;
		}
		*tmp = '\0';
		{
			char *name = base, *value = cfg_skip_ws(tmp + 1);
			size_t vlen;

			cfg_trim_end(name);
			cfg_trim_end(value);
			/* Strip surrounding quotes, so a value may hold a '#'
			 * (a hex color) or trailing spaces without being taken
			 * for a comment. */
			vlen = strlen(value);
			if (vlen >= 2 && value[0] == '"' &&
			    value[vlen - 1] == '"') {
				value[vlen - 1] = '\0';
				value++;
			}
			if (section[0] == '\0' && strchr(name, '.') != NULL) {
				/* shorthand: a trimmed name with a dot is already
				 * a full dotted key */
				cfg_set(c, name, value);
			} else {
				cfg_make_key(key, sizeof(key), section, subsect,
				    name);
				cfg_set(c, key, value);
			}
		}
	}
	free(subsect);
	return rc;
}

/* Parse a config file into c, merging over anything already there. Returns 0
 * on success (a missing file is a failure, so the caller can ignore it), -1 on
 * a read or syntax error. */
int
vedit_cfg_load(Cfg *c, const char *path)
{
	FILE *f;
	long sz;
	size_t got;
	char *buf;
	int rc;

	if (!c)
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
		fclose(f);
		return -1;
	}
	rewind(f);
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return -1;
	}
	got = fread(buf, 1, (size_t)sz, f);	/* text mode may read fewer */
	buf[got] = '\0';
	fclose(f);
	rc = cfg_load_mem(c, buf);
	free(buf);
	return rc;
}

/****************************************************************
 * UTF-8
 ****************************************************************/

#define UTF8_RUNE_ERROR 0xFFFDu
#define UTF8_RUNE_MAX   0x10FFFFu

int
utf8_decode(uint32_t *rune, const unsigned char *s, size_t len)
{
	uint32_t v;
	int need;

	if (len == 0) {
		*rune = UTF8_RUNE_ERROR;
		return 0;
	}
	if (s[0] < 0x80) {
		*rune = s[0];
		return 1;
	}
	if ((s[0] & 0xE0) == 0xC0) {
		v = s[0] & 0x1F;
		need = 2;
	} else if ((s[0] & 0xF0) == 0xE0) {
		v = s[0] & 0x0F;
		need = 3;
	} else if ((s[0] & 0xF8) == 0xF0) {
		v = s[0] & 0x07;
		need = 4;
	} else {
		*rune = UTF8_RUNE_ERROR;
		return 1;
	}
	if ((size_t)need > len) {
		*rune = UTF8_RUNE_ERROR;
		return 1;
	}
	for (int i = 1; i < need; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*rune = UTF8_RUNE_ERROR;
			return 1;
		}
		v = (v << 6) | (s[i] & 0x3F);
	}
	if (need == 2 && v < 0x80)
		goto bad;
	if (need == 3 && v < 0x800)
		goto bad;
	if (need == 4 && v < 0x10000)
		goto bad;
	if (v >= 0xD800 && v <= 0xDFFF)
		goto bad;
	if (v > UTF8_RUNE_MAX)
		goto bad;
	*rune = v;
	return need;
bad:
	*rune = UTF8_RUNE_ERROR;
	return 1;
}

int
utf8_encode(unsigned char *buf, uint32_t rune)
{
	if (rune <= 0x7F) {
		buf[0] = rune;
		return 1;
	}
	if (rune <= 0x7FF) {
		buf[0] = 0xC0 | (rune >> 6);
		buf[1] = 0x80 | (rune & 0x3F);
		return 2;
	}
	if (rune <= 0xFFFF) {
		if (rune >= 0xD800 && rune <= 0xDFFF)
			return 0;
		buf[0] = 0xE0 | (rune >> 12);
		buf[1] = 0x80 | ((rune >> 6) & 0x3F);
		buf[2] = 0x80 | (rune & 0x3F);
		return 3;
	}
	if (rune <= UTF8_RUNE_MAX) {
		buf[0] = 0xF0 | (rune >> 18);
		buf[1] = 0x80 | ((rune >> 12) & 0x3F);
		buf[2] = 0x80 | ((rune >> 6) & 0x3F);
		buf[3] = 0x80 | (rune & 0x3F);
		return 4;
	}
	return 0;
}

/****************************************************************
 * Character width -- a compact wcwidth. Combining marks are zero
 * width, the common CJK and emoji ranges are two, everything else
 * is one.
 ****************************************************************/

typedef struct wrange { uint32_t lo, hi; } Wrange;

static int
in_ranges(uint32_t cp, const Wrange *r, size_t n)
{
	size_t lo = 0, hi = n;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;

		if (cp < r[mid].lo)
			hi = mid;
		else if (cp > r[mid].hi)
			lo = mid + 1;
		else
			return 1;
	}
	return 0;
}

/* Zero-width combining marks (the ranges that matter in practice). */
static const Wrange zero_width[] = {
	{ 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD },
	{ 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 },
	{ 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 }, { 0x0901, 0x0903 },
	{ 0x093C, 0x093C }, { 0x0941, 0x0948 }, { 0x094D, 0x094D },
	{ 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0EB1, 0x0EB1 },
	{ 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x200B, 0x200F },
	{ 0x20D0, 0x20FF }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
};

/* Two-cell wide ranges (CJK, Hangul, fullwidth forms, common emoji). */
static const Wrange wide[] = {
	{ 0x1100, 0x115F }, { 0x2329, 0x232A }, { 0x2E80, 0x303E },
	{ 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF },
	{ 0xA000, 0xA4CF }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF },
	{ 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 },
	{ 0xFFE0, 0xFFE6 }, { 0x1F300, 0x1F64F }, { 0x1F900, 0x1F9FF },
	{ 0x20000, 0x3FFFD },
};

int rune_width_init(void) { return 0; }

int
rune_width(uint32_t cp)
{
	if (cp == 0)
		return 0;
	if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
		return -1;
	if (in_ranges(cp, zero_width,
	    sizeof(zero_width) / sizeof(zero_width[0])))
		return 0;
	if (in_ranges(cp, wide, sizeof(wide) / sizeof(wide[0])))
		return 2;
	return 1;
}

/****************************************************************
 * Terminal cell
 ****************************************************************/

typedef enum vt_attr {
	ATTR_BOLD      = 1 << 0,
	ATTR_UNDERLINE = 1 << 1,
	ATTR_REVERSE   = 1 << 2,
	ATTR_ITALIC    = 1 << 3,
	ATTR_BLINK     = 1 << 4,
	ATTR_UNDERCURL = 1 << 5,
	ATTR_DIM       = 1 << 6,
	ATTR_HIDDEN    = 1 << 7,
	ATTR_STRIKE    = 1 << 8,
	ATTR_PREDICTED = 1 << 9,
} Attr;

typedef enum vt_color_type {
	COLOR_DEFAULT,
	COLOR_INDEXED,
	COLOR_RGB,
} ColorType;

typedef struct vt_color {
	ColorType type;
	union {
		uint8_t index;
		struct { uint8_t r, g, b; } rgb;
	};
} Color;

typedef struct vt_cell {
	uint32_t	codepoint;
	Color	fg;
	Color	bg;
	uint16_t	attrs;
	uint8_t		width;
} Cell;

static void
vt_cell_clear(Cell *c)
{
	memset(c, 0, sizeof(*c));
	c->codepoint = ' ';
	c->width = 1;
}

/* Parse a color: "default", a 0-255 palette index, "#rrggbb", or one of the 16
 * ANSI names (with a "bright-" prefix for 8-15). Returns 1 on success. Used by
 * the config file for both chrome themes and syntax classes. */
static int
cfg_color(const char *s, Color *out)
{
	static const char *const names[8] = {
		"black", "red", "green", "yellow",
		"blue", "magenta", "cyan", "white"
	};
	int i;

	if (!s || !s[0])
		return 0;
	if (strcmp(s, "default") == 0) {
		out->type = COLOR_DEFAULT;
		return 1;
	}
	if (s[0] == '#' && strlen(s) == 7) {
		unsigned r, g, b;

		if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
			out->type = COLOR_RGB;
			out->rgb.r = (uint8_t)r;
			out->rgb.g = (uint8_t)g;
			out->rgb.b = (uint8_t)b;
			return 1;
		}
		return 0;
	}
	if (s[0] >= '0' && s[0] <= '9') {
		char *end;
		long n = strtol(s, &end, 10);

		if (*end == '\0' && n >= 0 && n <= 255) {
			out->type = COLOR_INDEXED;
			out->index = (uint8_t)n;
			return 1;
		}
		return 0;
	}
	{
		const char *base = s;
		int bright = 0;

		if (strncmp(s, "bright-", 7) == 0) {
			base = s + 7;
			bright = 8;
		}
		for (i = 0; i < 8; i++)
			if (strcmp(base, names[i]) == 0) {
				out->type = COLOR_INDEXED;
				out->index = (uint8_t)(i + bright);
				return 1;
			}
	}
	return 0;
}

/* Parse a color spec "COLOR [attr...]" (attrs: bold, underline, reverse, dim,
 * italic) into fg and attr. Returns 1 when the color parsed. */
static int
cfg_style(const char *spec, Color *fg, uint16_t *attr)
{
	char buf[64], *tok, *save = NULL;
	int first = 1, ok = 0;

	*attr = 0;
	if (!spec)
		return 0;
	snprintf(buf, sizeof(buf), "%s", spec);
	for (tok = strtok_r(buf, " \t", &save); tok;
	    tok = strtok_r(NULL, " \t", &save)) {
		if (first) {
			ok = cfg_color(tok, fg);
			first = 0;
		} else if (strcmp(tok, "bold") == 0)
			*attr |= ATTR_BOLD;
		else if (strcmp(tok, "underline") == 0)
			*attr |= ATTR_UNDERLINE;
		else if (strcmp(tok, "reverse") == 0)
			*attr |= ATTR_REVERSE;
		else if (strcmp(tok, "dim") == 0)
			*attr |= ATTR_DIM;
		else if (strcmp(tok, "italic") == 0)
			*attr |= ATTR_ITALIC;
	}
	return ok;
}

/****************************************************************
 * Box-drawing abstraction
 *
 * The frame, scrollbars, menus, and dialogs are drawn with logical glyphs that
 * render three ways depending on the client: UTF-8 box-drawing, DEC VT100
 * line-drawing (single bytes in the alternate charset, which many clients that
 * cannot do UTF-8 still support), or plain-ASCII substitutes. The editor stores
 * a glyph as a marker codepoint (BOX_CP) in a cell; scr_present turns it into
 * the right bytes for the current mode. The mode is per editor instance (stored
 * on Scrbuf), so two players on one server can differ.
 ****************************************************************/

typedef enum box_glyph {
	BG_TL, BG_TR, BG_BL, BG_BR, BG_H, BG_V,
	BG_UP, BG_DOWN, BG_LEFT, BG_RIGHT, BG_THUMB, BG_TRACK, BG_CHECK,
	BG_COUNT
} BoxGlyph;

/* For each glyph: its Unicode codepoint, its ASCII substitute, and the byte to
 * send in DEC line-drawing mode (0 means DEC has no equivalent, so the ASCII
 * substitute is used even in DEC mode). */
typedef struct box_def {
	uint32_t	uni;
	unsigned char	ascii;
	unsigned char	dec;
} BoxDef;

static const BoxDef box_tab[BG_COUNT] = {
	[BG_TL]    = { 0x250cu, '+', 'l' },	/* corners */
	[BG_TR]    = { 0x2510u, '+', 'k' },
	[BG_BL]    = { 0x2514u, '+', 'm' },
	[BG_BR]    = { 0x2518u, '+', 'j' },
	[BG_H]     = { 0x2500u, '-', 'q' },	/* edges */
	[BG_V]     = { 0x2502u, '|', 'x' },
	[BG_UP]    = { 0x25b2u, '^', 0 },	/* scrollbar arrows/thumb/track */
	[BG_DOWN]  = { 0x25bcu, 'v', 0 },
	[BG_LEFT]  = { 0x25c4u, '<', 0 },
	[BG_RIGHT] = { 0x25bau, '>', 0 },
	[BG_THUMB] = { 0x2588u, '#', 0 },
	[BG_TRACK] = { 0x2591u, ':', 'a' },	/* DEC 'a' is a checkerboard */
	[BG_CHECK] = { 0x2022u, '*', '`' },	/* DEC '`' is a diamond */
};

/* Glyph markers live in a private-use plane so they never collide with text. */
#define BOX_CP_BASE	0x0F0000u
#define BOX_CP(id)	(BOX_CP_BASE + (uint32_t)(id))
#define BOX_IS(cp)	((cp) >= BOX_CP_BASE && (cp) < BOX_CP_BASE + BG_COUNT)
#define BOX_ID(cp)	((BoxGlyph)((cp) - BOX_CP_BASE))

/* The configuration handed in by vedit_set_config(), or NULL. The startup
 * resolvers consult it below the environment and above auto-detection. */
static const Cfg *g_cfg;

/* Look up a per-project config key, letting an environment variable override the
 * config file. The variable name is VEDIT_ followed by the key uppercased with
 * every '.' turned into '_', so tags.file is VEDIT_TAGS_FILE and command.c.build
 * is VEDIT_COMMAND_C_BUILD. A set, non-empty variable wins over the config;
 * otherwise the config value is used. Returns NULL when neither is set. This is
 * reserved for keys that genuinely vary per project (an include or tags path, a
 * build command), not for editor preferences. */
static const char *
cfg_proj_get(const char *key)
{
	char env[160];
	char *p;
	const char *v;
	int n;

	n = snprintf(env, sizeof(env), "VEDIT_%s", key);
	if (n > 0 && (size_t)n < sizeof(env)) {
		for (p = env + 6; *p; p++)
			*p = (*p == '.') ? '_'
			    : (char)toupper((unsigned char)*p);
		v = getenv(env);
		if (v && v[0])
			return v;
	}
	return g_cfg ? cfg_get(g_cfg, key) : NULL;
}

/* A forced mode set from the command line, or -1 for "decide from the
 * environment". box_default() resolves it. */
static int g_box_force = -1;

static enum vedit_box_mode
box_default(void)
{
	const char *l;

	if (g_box_force >= 0)
		return (enum vedit_box_mode)g_box_force;
	if (getenv("VEDIT_ASCII"))
		return VEDIT_BOX_ASCII;
	l = getenv("VEDIT_BOX");
	if (l) {
		if (strcmp(l, "utf8") == 0 || strcmp(l, "utf-8") == 0)
			return VEDIT_BOX_UTF8;
		if (strcmp(l, "dec") == 0)
			return VEDIT_BOX_DEC;
		if (strcmp(l, "ascii") == 0)
			return VEDIT_BOX_ASCII;
	}
	l = cfg_get(g_cfg, "ui.box");
	if (l) {
		if (strcmp(l, "utf8") == 0 || strcmp(l, "utf-8") == 0)
			return VEDIT_BOX_UTF8;
		if (strcmp(l, "dec") == 0)
			return VEDIT_BOX_DEC;
		if (strcmp(l, "ascii") == 0)
			return VEDIT_BOX_ASCII;
	}
	l = getenv("LANG");
	if (!l || !*l)
		l = getenv("LC_ALL");
	if (l && (strstr(l, "UTF-8") || strstr(l, "utf-8") || strstr(l, "utf8")))
		return VEDIT_BOX_UTF8;
	return VEDIT_BOX_ASCII;	/* safe default; --dec opts into DEC */
}

/* Color depth the terminal is assumed to handle: 256 or 16. A forced value set
 * from the command line, or -1 to decide from the environment. On a 16-color
 * client the renderer maps 256-palette indices to the nearest ANSI color and
 * emits the classic 30-37/90-97 codes, so a very primitive MUD client stays
 * readable. An embedding host that negotiates capability (telnet MTTS) should
 * call vedit_set_colors() instead. */
static int g_colors_force = -1;

static int
color_default(void)
{
	const char *s;

	if (g_colors_force > 0)
		return g_colors_force;
	s = getenv("VEDIT_COLORS");
	if (s) {
		if (strcmp(s, "256") == 0)
			return 256;
		if (strcmp(s, "16") == 0 || strcmp(s, "8") == 0)
			return 16;
	}
	s = cfg_get(g_cfg, "ui.colors");
	if (s) {
		if (strcmp(s, "256") == 0)
			return 256;
		if (strcmp(s, "16") == 0 || strcmp(s, "8") == 0)
			return 16;
	}
	s = getenv("COLORTERM");
	if (s && (strstr(s, "truecolor") || strstr(s, "24bit")))
		return 256;
	s = getenv("TERM");
	if (s && strstr(s, "256color"))
		return 256;
	return 16;	/* conservative: assume a limited client */
}

/* Whether to use the terminal's scroll region (DECSTBM + IND/RI) to move the
 * text area when the viewport scrolls or a line is inserted, instead of
 * repainting every row. It turns a one-line scroll into a single control
 * sequence plus the one newly exposed row, a large saving on a slow link. It
 * is off by default because it relies on VT100 scroll-region support, which
 * the most primitive clients lack; --scroll or VEDIT_SCROLL turns it on, and
 * an embedding host that knows the client calls vedit_set_scroll(). */
static int g_scroll_force = -1;

static int
scroll_default(void)
{
	const char *s;

	if (g_scroll_force >= 0)
		return g_scroll_force;
	s = getenv("VEDIT_SCROLL");
	if (s) {
		if (strcmp(s, "1") == 0 || strcmp(s, "on") == 0 ||
		    strcmp(s, "yes") == 0)
			return 1;
		if (strcmp(s, "0") == 0 || strcmp(s, "off") == 0 ||
		    strcmp(s, "no") == 0)
			return 0;
	}
	if (cfg_get(g_cfg, "ui.scroll"))
		return cfg_bool(g_cfg, "ui.scroll", 0);
	return 0;	/* conservative: dumb full-row repaint */
}

/* Chrome color schemes, cycled by View > Color Scheme (MA_SCHEME). The black
 * scheme leaves the text-area background at the terminal default, which lets
 * scr_present clear trailing blanks with erase-to-EOL (see there). */
enum { SCHEME_DOS, SCHEME_BLACK, SCHEME_PLAIN, SCHEME_COUNT };

/****************************************************************
 * Syntax highlighting. A small, self-contained lexer: one generic C-family
 * tokenizer (C, C++, LPC) plus a '#'-comment shell variant. syn_line() styles
 * one line and returns a carry state so a block comment spans lines.
 ****************************************************************/

typedef struct jsf Jsf;		/* data-driven FSM highlighter from config */

/* A highlighter: just a named wrapper over an FSM grammar. */
typedef struct syntax {
	const char	*name;
	uint16_t	start;		/* initial FSM state */
	const Jsf	*fsm;
} Syntax;

/****************************************************************
 * Data-driven highlighter (joe-style FSM authored in the config file).
 *
 * A language is a set of states; each state has an ordered list of transition
 * rules keyed on a character set. A rule may recolor earlier bytes, not consume
 * the current byte (noeat), start buffering a token (buffer), and match that
 * token against keyword groups (kw=group:class). The carry state across lines
 * is just the current state index, which the line cache already stores. Built
 * from g_cfg by syntax_load_cfg(); the core opens no files.
 ****************************************************************/

#define JSF_CLASS_MAX	32
#define JSF_STATE_MAX	128
#define JSF_NAME	48
#define JSF_LANG_MAX	8

enum {
	JSF_F_NOEAT = 1,	/* do not consume the byte; re-dispatch in next */
	JSF_F_BUFFER = 2,	/* start a token here for a later keyword match */
	JSF_F_MARK = 4,		/* set the region mark to the current position */
	JSF_F_RECOLORMARK = 8	/* repaint [mark, here) with the target color */
};

typedef struct jsf_kw { int group; uint8_t klass; } Jsfkw;

typedef struct jsf_rule {
	uint8_t		set[32];	/* 256-bit charset bitmap */
	uint16_t	next;		/* target state index */
	uint8_t		flags;		/* JSF_F_* */
	uint8_t		recolor;	/* 0 none; 255 whole token; else N bytes */
	int		kw_first, kw_n;	/* slice into Jsf.kws */
} Jsfrule;

typedef struct jsf_state {
	char		name[JSF_NAME];
	uint8_t		klass;		/* default color class for this state */
	int		rule_first, rule_n;
	int		include;	/* fall back to this state's rules, or -1 */
} Jsfstate;

typedef struct jsf_word { const char *s; int len; } Jsfword;	/* into g_cfg */
typedef struct jsf_group { char name[JSF_NAME]; int first, n; } Jsfgroup;

struct jsf {
	char		name[JSF_NAME];
	Jsfstate	states[JSF_STATE_MAX];	int nstates;
	Jsfrule		*rules;	int nrules, rulecap;
	Jsfkw		*kws;	int nkws, kwcap;
	Jsfword		*words;	int nwords, wordcap;
	Jsfgroup	groups[JSF_CLASS_MAX];	int ngroups;
	char		classname[JSF_CLASS_MAX][24];	int nclasses;
	Color		fg[JSF_CLASS_MAX];
	uint16_t	attr[JSF_CLASS_MAX];
	uint16_t	start;
};

/* A registry of FSM grammars: the languages, a Syntax wrapper for each, and the
 * count. There are two, so user grammars can override the built-in defaults
 * without the two sets clobbering each other on a config reload. */
typedef struct jsf_reg {
	Jsf	lang[JSF_LANG_MAX];
	Syntax	syn[JSF_LANG_MAX];	/* Syntax wrappers over lang[] */
	int	count;
} Jsfreg;

static Jsfreg	g_user;		/* grammars from the user's config */
static Jsfreg	g_def;		/* built-in default grammars (C, shell) */

static int
jsf_find(const Jsfreg *r, const char *name)
{
	int i;

	for (i = 0; i < r->count; i++)
		if (strcmp(r->lang[i].name, name) == 0)
			return i;
	return -1;
}

/* Intern a class name, returning its index; class 0 is the default (terminal
 * color, no attrs). */
static int
jsf_class(Jsf *j, const char *name)
{
	int i;

	for (i = 0; i < j->nclasses; i++)
		if (strcmp(j->classname[i], name) == 0)
			return i;
	if (j->nclasses >= JSF_CLASS_MAX)
		return 0;
	i = j->nclasses++;
	snprintf(j->classname[i], sizeof(j->classname[i]), "%s", name);
	j->fg[i].type = COLOR_DEFAULT;
	j->attr[i] = 0;
	return i;
}

static int
jsf_state_idx(Jsf *j, const char *name)
{
	int i;

	for (i = 0; i < j->nstates; i++)
		if (strcmp(j->states[i].name, name) == 0)
			return i;
	if (j->nstates >= JSF_STATE_MAX)
		return 0;
	i = j->nstates++;
	snprintf(j->states[i].name, sizeof(j->states[i].name), "%s", name);
	j->states[i].klass = 0;
	j->states[i].rule_first = j->nrules;
	j->states[i].rule_n = 0;
	j->states[i].include = -1;
	return i;
}

static int
jsf_group_idx(Jsf *j, const char *name)
{
	int i;

	for (i = 0; i < j->ngroups; i++)
		if (strcmp(j->groups[i].name, name) == 0)
			return i;
	if (j->ngroups >= JSF_CLASS_MAX)
		return -1;
	i = j->ngroups++;
	snprintf(j->groups[i].name, sizeof(j->groups[i].name), "%s", name);
	j->groups[i].first = j->nwords;
	j->groups[i].n = 0;
	return i;
}

static int
jsf_group_has(const Jsf *j, int g, const char *s, int len)
{
	int i;

	if (g < 0)
		return 0;
	for (i = 0; i < j->groups[g].n; i++) {
		const Jsfword *w = &j->words[j->groups[g].first + i];

		if (w->len == len && memcmp(w->s, s, (size_t)len) == 0)
			return 1;
	}
	return 0;
}

/* Fill a 256-bit set from "*" or a quoted "a-z..." spec (escapes \n \t \r \\
 * \" \xHH, and X-Y ranges). Returns 1 on success. */
static int
jsf_charset(const char *tok, uint8_t set[32])
{
	const char *p;
	int prev = -1;

	memset(set, 0, 32);
	if (strcmp(tok, "*") == 0) {
		memset(set, 0xff, 32);
		return 1;
	}
	if (tok[0] != '"')
		return 0;
	p = tok + 1;
	while (*p && *p != '"') {
		int ch;

		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': ch = '\n'; break;
			case 't': ch = '\t'; break;
			case 'r': ch = '\r'; break;
			case 'x': {
				unsigned v = 0;
				int k;

				for (k = 0; k < 2 && isxdigit((unsigned char)p[1]);
				    k++) {
					p++;
					v = v * 16 + (isdigit((unsigned char)*p)
					    ? *p - '0'
					    : (tolower(*p) - 'a' + 10));
				}
				ch = (int)v;
				break;
			}
			default: ch = (unsigned char)*p; break;
			}
			p++;
		} else if (p[1] == '-' && p[2] && p[2] != '"' && prev < 0) {
			int lo = (unsigned char)p[0], hi = (unsigned char)p[2];
			int x;

			if (hi >= lo)
				for (x = lo; x <= hi; x++)
					set[x >> 3] |= (uint8_t)(1 << (x & 7));
			p += 3;
			continue;
		} else {
			ch = (unsigned char)*p;
			p++;
		}
		if (ch >= 0 && ch < 256)
			set[ch >> 3] |= (uint8_t)(1 << (ch & 7));
		prev = -1;
	}
	return 1;
}

static Jsfrule *
jsf_add_rule(Jsf *j)
{
	if (j->nrules >= j->rulecap) {
		int nc = j->rulecap ? j->rulecap * 2 : 32;
		Jsfrule *n = realloc(j->rules, (size_t)nc * sizeof(*n));

		if (!n)
			return NULL;
		j->rules = n;
		j->rulecap = nc;
	}
	return &j->rules[j->nrules];
}

/* Run the FSM over one line into out[] (class per byte); return the end state. */
static uint16_t
jsf_line(const Jsf *j, uint16_t state_in, const char *bytes, size_t n,
    uint8_t *out)
{
	size_t i = 0, tok = 0, markpos = 0;
	uint16_t st = state_in < j->nstates ? state_in : j->start;
	int buffering = 0, hops = 0;

	/* Iterate one past the line and feed a virtual '\n' there, as joe does,
	 * so a state can end a line (a "\n" rule returns a line comment to idle)
	 * and so a token flush at end of line matches a keyword. out[] is only
	 * written for real bytes (i < n). */
	while (i <= n) {
		unsigned char c = i < n ? (unsigned char)bytes[i] : '\n';
		const Jsfstate *S = &j->states[st];
		const Jsfrule *R = NULL;
		int r, cur = st, guard = 0;

		/* Match against this state's rules; if none fire, fall through
		 * the include chain (cur = that state's include) so a state can
		 * borrow another's transitions. The current state's color still
		 * applies, since S stays the real state. */
		while (cur >= 0 && guard++ < JSF_STATE_MAX) {
			const Jsfstate *CS = &j->states[cur];

			for (r = 0; r < CS->rule_n; r++) {
				const Jsfrule *cand = &j->rules[CS->rule_first + r];

				if (cand->set[c >> 3] & (1 << (c & 7))) {
					R = cand;
					break;
				}
			}
			if (R)
				break;
			cur = CS->include;
		}
		if (!R) {			/* no rule: color and consume */
			if (out && i < n)
				out[i] = S->klass;
			i++;
			continue;
		}
		/* keyword match on the buffered token at a transition */
		if (R->kw_n && buffering && out && i > tok) {
			int k;

			for (k = 0; k < R->kw_n; k++) {
				const Jsfkw *kw = &j->kws[R->kw_first + k];
				size_t p;

				if (!jsf_group_has(j, kw->group, bytes + tok,
				    (int)(i - tok)))
					continue;
				for (p = tok; p < i && p < n; p++)
					out[p] = kw->klass;
				break;
			}
		}
		if (R->flags & JSF_F_BUFFER) {
			tok = i;
			buffering = 1;
		}
		if (R->flags & JSF_F_MARK)
			markpos = i;
		/* color the consumed byte with this state's class first, then
		 * let recolor override it (and any earlier bytes) */
		if (out && i < n && !(R->flags & JSF_F_NOEAT))
			out[i] = S->klass;
		if ((R->flags & JSF_F_RECOLORMARK) && out) {
			uint8_t nk = j->states[R->next].klass;
			size_t end = i < n ? i : n;
			size_t p;

			for (p = markpos; p < end; p++)
				out[p] = nk;
		}
		if (R->recolor && out) {
			size_t last = i < n ? i : (n ? n - 1 : 0);
			size_t cnt = R->recolor;
			size_t from = cnt > last + 1 ? 0 : (last + 1 - cnt);
			uint8_t nk = j->states[R->next].klass;
			size_t p;

			if (i <= n)
				for (p = from; p <= last && p < n; p++)
					out[p] = nk;
		}
		if (R->flags & JSF_F_NOEAT) {
			st = R->next;
			if (++hops > 16) {	/* break a malformed noeat cycle */
				if (out && i < n)
					out[i] = j->states[st].klass;
				i++;
				hops = 0;
			}
			continue;
		}
		st = R->next;
		hops = 0;
		i++;
	}
	return st;
}

static void
jsf_reset(Jsfreg *r)
{
	int i;

	for (i = 0; i < r->count; i++) {
		free(r->lang[i].rules);
		free(r->lang[i].kws);
		free(r->lang[i].words);
	}
	memset(r->lang, 0, sizeof(r->lang));
	r->count = 0;
}

/* Intern a language slot by name in registry r. */
static Jsf *
jsf_lang(Jsfreg *r, const char *name)
{
	int i = jsf_find(r, name);

	if (i >= 0)
		return &r->lang[i];
	if (r->count >= JSF_LANG_MAX)
		return NULL;
	i = r->count++;
	snprintf(r->lang[i].name, sizeof(r->lang[i].name), "%s", name);
	r->lang[i].nclasses = 1;		/* class 0 = default */
	r->lang[i].classname[0][0] = '\0';
	r->lang[i].fg[0].type = COLOR_DEFAULT;
	r->lang[i].attr[0] = 0;
	return &r->lang[i];
}

/* Split "a.b.c" at the first dot: head into buf, return the tail (or NULL). */
static const char *
jsf_split(const char *s, char *buf, size_t bufsz)
{
	const char *dot = strchr(s, '.');
	size_t nl;

	if (!dot)
		return NULL;
	nl = (size_t)(dot - s);
	if (nl >= bufsz)
		return NULL;
	memcpy(buf, s, nl);
	buf[nl] = '\0';
	return dot + 1;
}

/* Append space-separated words from val into group g of j (slices into val,
 * which lives in the borrowed g_cfg). */
static void
jsf_add_words(Jsf *j, int g, const char *val)
{
	const char *p = val;

	while (*p) {
		const char *start;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (j->nwords >= j->wordcap) {
			int nc = j->wordcap ? j->wordcap * 2 : 32;
			Jsfword *n = realloc(j->words, (size_t)nc * sizeof(*n));

			if (!n)
				return;
			j->words = n;
			j->wordcap = nc;
		}
		j->words[j->nwords].s = start;
		j->words[j->nwords].len = (int)(p - start);
		j->nwords++;
		j->groups[g].n++;
	}
}

/* Parse one rule value "charset target [options]" into state st of j. */
static void
jsf_parse_rule(Jsf *j, int st, const char *val)
{
	char buf[512], *tok, *save = NULL;
	Jsfrule *R = jsf_add_rule(j);
	int field = 0;

	if (!R)
		return;
	memset(R, 0, sizeof(*R));
	R->kw_first = j->nkws;
	snprintf(buf, sizeof(buf), "%s", val);
	for (tok = strtok_r(buf, " \t", &save); tok;
	    tok = strtok_r(NULL, " \t", &save)) {
		if (field == 0) {
			if (!jsf_charset(tok, R->set))
				return;		/* drop a malformed rule */
		} else if (field == 1) {
			R->next = (uint16_t)jsf_state_idx(j, tok);
		} else if (strcmp(tok, "noeat") == 0) {
			R->flags |= JSF_F_NOEAT;
		} else if (strcmp(tok, "buffer") == 0) {
			R->flags |= JSF_F_BUFFER;
		} else if (strcmp(tok, "mark") == 0) {
			R->flags |= JSF_F_MARK;
		} else if (strcmp(tok, "recolormark") == 0) {
			R->flags |= JSF_F_RECOLORMARK;
		} else if (strcmp(tok, "recolor") == 0) {
			R->recolor = 1;
		} else if (strncmp(tok, "recolor=", 8) == 0) {
			int v = atoi(tok + 8);

			R->recolor = (uint8_t)(v < 1 ? 1 : v > 254 ? 254 : v);
		} else if (strncmp(tok, "kw=", 3) == 0) {
			char grp[JSF_NAME];
			const char *colon = strchr(tok + 3, ':');
			const char *cls = NULL;
			int g, cl;

			if (colon) {		/* kw=<group>:<class> */
				size_t gl = (size_t)(colon - (tok + 3));

				if (gl < sizeof(grp)) {
					memcpy(grp, tok + 3, gl);
					grp[gl] = '\0';
					cls = colon + 1;
				}
			}
			if (cls && j->nkws < JSF_CLASS_MAX * 4) {
				g = jsf_group_idx(j, grp);
				cl = jsf_class(j, cls);
				if (g >= 0) {
					if (j->nkws >= j->kwcap) {
						int nc = j->kwcap ? j->kwcap * 2
						    : 16;
						Jsfkw *nk = realloc(j->kws,
						    (size_t)nc * sizeof(*nk));

						if (nk) {
							j->kws = nk;
							j->kwcap = nc;
						}
					}
					if (j->nkws < j->kwcap) {
						j->kws[j->nkws].group = g;
						j->kws[j->nkws].klass =
						    (uint8_t)cl;
						j->nkws++;
						R->kw_n++;
					}
				}
			}
		}
		field++;
	}
	if (field < 2)			/* need at least charset + target */
		return;
	if (j->states[st].rule_n == 0)	/* first rule: anchor the slice here */
		j->states[st].rule_first = j->nrules;
	j->states[st].rule_n++;
	j->nrules++;
}

/* Build registry r from the config. Pass 1 interns languages, classes, groups,
 * and states; pass 2 parses rules (so forward references resolve) and the start
 * state, then builds the Syntax wrappers. The word slices point into c, so c
 * must outlive r. */
static void
syntax_load_cfg(Jsfreg *r, const Cfg *c)
{
	int pass, i;

	jsf_reset(r);
	if (!c)
		return;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < c->count; i++) {
			const char *key = c->entries[i].key;
			const char *val = c->entries[i].value;
			char lang[JSF_NAME];
			const char *rest, *tail;
			Jsf *j;

			if (strncmp(key, "color.", 6) == 0) {
				if (pass)
					continue;
				rest = jsf_split(key + 6, lang, sizeof(lang));
				if (!rest)
					continue;
				j = jsf_lang(r, lang);
				if (j)
					cfg_style(val,
					    &j->fg[jsf_class(j, rest)],
					    &j->attr[jsf_class(j, rest)]);
			} else if (strncmp(key, "words.", 6) == 0) {
				char grp[JSF_NAME];

				if (pass)
					continue;
				rest = jsf_split(key + 6, lang, sizeof(lang));
				if (!rest)
					continue;
				/* rest = "<group>.list"; keep "<lang>.<group>" */
				tail = jsf_split(rest, grp, sizeof(grp));
				if (!tail || strcmp(tail, "list") != 0)
					continue;
				j = jsf_lang(r, lang);
				if (j) {
					char full[JSF_NAME];

					if (snprintf(full, sizeof(full), "%s.%s",
					    lang, grp) >= (int)sizeof(full))
						continue;	/* name too long */
					jsf_add_words(j, jsf_group_idx(j, full),
					    val);
				}
			} else if (strncmp(key, "state.", 6) == 0) {
				char sname[JSF_NAME];

				rest = jsf_split(key + 6, lang, sizeof(lang));
				if (!rest)
					continue;
				tail = jsf_split(rest, sname, sizeof(sname));
				if (!tail)
					continue;
				j = jsf_lang(r, lang);
				if (!j)
					continue;
				if (pass == 0) {
					jsf_state_idx(j, sname);
				} else if (strcmp(tail, "color") == 0) {
					j->states[jsf_state_idx(j, sname)].klass =
					    (uint8_t)jsf_class(j, val);
				} else if (strcmp(tail, "include") == 0) {
					j->states[jsf_state_idx(j, sname)].include =
					    jsf_state_idx(j, val);
				} else if (strcmp(tail, "rule") == 0) {
					jsf_parse_rule(j,
					    jsf_state_idx(j, sname), val);
				}
			} else if (strncmp(key, "language.", 9) == 0) {
				if (pass)
					continue;
				rest = jsf_split(key + 9, lang, sizeof(lang));
				if (rest)
					jsf_lang(r, lang);
			}
		}
	}
	/* resolve start states and build Syntax wrappers */
	for (i = 0; i < r->count; i++) {
		char sk[JSF_NAME + 24];
		const char *sv = NULL;

		if (snprintf(sk, sizeof(sk), "language.%s.start",
		    r->lang[i].name) < (int)sizeof(sk))
			sv = cfg_get(c, sk);
		r->lang[i].start = sv
		    ? (uint16_t)jsf_state_idx(&r->lang[i], sv) : 0;
		memset(&r->syn[i], 0, sizeof(r->syn[i]));
		r->syn[i].name = r->lang[i].name;
		r->syn[i].start = r->lang[i].start;
		r->syn[i].fsm = &r->lang[i];
	}
}

/* The built-in default grammars (C family and shell), authored in the same
 * config format a user would write, so the FSM engine is the single highlighter
 * and there is no second hardcoded lexer. Colors are base-16 indices, which
 * render the same at 16 and 256 colors and stay legible on the DOS blue chrome.
 * A user config can define a language of the same name to override one. */
static const char g_default_grammar[] =
	"[language \"c\"]\n"
	"	start = idle\n"
	"[language \"sh\"]\n"
	"	start = idle\n"
	"[language \"md\"]\n"
	"	start = bol\n"
	"\n"
	"[syntax]\n"
	"	h = c\n"
	"	cc = c\n"
	"	cpp = c\n"
	"	cxx = c\n"
	"	hpp = c\n"
	"	hh = c\n"
	"	i = c\n"
	"	lpc = c\n"
	"	bash = sh\n"
	"	markdown = md\n"
	"	mdown = md\n"
	"	mkd = md\n"
	"\n"
	"[color \"c\"]\n"
	"	comment = 14\n"
	"	string = 13\n"
	"	char = 13\n"
	"	number = 13\n"
	"	keyword = 11\n"
	"	type = 10\n"
	"	preproc = 9\n"
	"[words \"c.keywords\"]\n"
	"	list = if else while do for switch case default break continue\n"
	"	list = return goto sizeof typedef struct union enum static const\n"
	"	list = extern volatile register inline auto restrict true false NULL\n"
	"	list = inherit nomask varargs private public protected nosave new delete\n"
	"	list = foreach catch efun in virtual namespace class try this operator\n"
	"	list = template using\n"
	"[words \"c.types\"]\n"
	"	list = void char short int long float double signed unsigned bool\n"
	"	list = size_t ssize_t wchar_t FILE va_list\n"
	"	list = int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t\n"
	"	list = intptr_t uintptr_t\n"
	"	list = object mapping mixed string function closure status buffer array\n"
	"	list = u8 u16 u32 u64 s8 s16 s32 s64 gfp_t loff_t atomic_t spinlock_t\n"
	"[state \"c.idle\"]\n"
	"	color = text\n"
	"	rule = \"/\" slash\n"
	"	rule = \"\\\"\" string recolor\n"
	"	rule = \"'\" char recolor\n"
	"	rule = \"0-9\" number recolor\n"
	"	rule = \"a-zA-Z_\" ident buffer\n"
	"	rule = \"#\" preproc recolor\n"
	"	rule = * idle\n"
	"[state \"c.slash\"]\n"
	"	color = text\n"
	"	rule = \"/\" lcomment recolor=2\n"
	"	rule = \"*\" bcomment recolor=2\n"
	"	rule = * idle noeat\n"
	"[state \"c.lcomment\"]\n"
	"	color = comment\n"
	"	rule = \"\\n\" idle\n"
	"	rule = * lcomment\n"
	"[state \"c.bcomment\"]\n"
	"	color = comment\n"
	"	rule = \"*\" bstar\n"
	"	rule = * bcomment\n"
	"[state \"c.bstar\"]\n"
	"	color = comment\n"
	"	rule = \"/\" idle\n"
	"	rule = \"*\" bstar\n"
	"	rule = * bcomment\n"
	"[state \"c.string\"]\n"
	"	color = string\n"
	"	rule = \"\\\\\" sesc\n"
	"	rule = \"\\\"\" idle\n"
	"	rule = \"\\n\" idle\n"
	"	rule = * string\n"
	"[state \"c.sesc\"]\n"
	"	color = string\n"
	"	rule = * string\n"
	"[state \"c.char\"]\n"
	"	color = char\n"
	"	rule = \"\\\\\" cesc\n"
	"	rule = \"'\" idle\n"
	"	rule = \"\\n\" idle\n"
	"	rule = * char\n"
	"[state \"c.cesc\"]\n"
	"	color = char\n"
	"	rule = * char\n"
	"[state \"c.number\"]\n"
	"	color = number\n"
	"	rule = \"0-9a-fA-F.xXuUlLeE\" number\n"
	"	rule = * idle noeat\n"
	"[state \"c.ident\"]\n"
	"	color = text\n"
	"	rule = \"a-zA-Z0-9_\" ident\n"
	"	rule = * idle noeat kw=c.keywords:keyword kw=c.types:type\n"
	"[state \"c.preproc\"]\n"
	"	color = preproc\n"
	"	rule = \"\\\"\" pstring recolor\n"
	"	rule = \"\\\\\" pcont\n"
	"	rule = \"\\n\" idle\n"
	"	rule = * preproc\n"
	"[state \"c.pstring\"]\n"
	"	color = string\n"
	"	rule = \"\\\\\" pstresc\n"
	"	rule = \"\\\"\" preproc\n"
	"	rule = \"\\n\" idle\n"
	"	rule = * pstring\n"
	"[state \"c.pstresc\"]\n"
	"	color = string\n"
	"	rule = * pstring\n"
	"[state \"c.pcont\"]\n"
	"	color = preproc\n"
	"	rule = \"\\n\" preproc\n"
	"	rule = * preproc noeat\n"
	"\n"
	"[color \"sh\"]\n"
	"	comment = 14\n"
	"	string = 13\n"
	"	keyword = 11\n"
	"	variable = 14\n"
	"[words \"sh.keywords\"]\n"
	"	list = if then else elif fi for while until do done case esac in\n"
	"	list = function select return break continue local export readonly\n"
	"	list = declare set unset echo exit shift test\n"
	"[state \"sh.idle\"]\n"
	"	color = text\n"
	"	rule = \"#\" comment recolor\n"
	"	rule = \"\\\"\" dquote recolor\n"
	"	rule = \"'\" squote recolor\n"
	"	rule = \"$\" var recolor\n"
	"	rule = \"a-zA-Z_\" ident buffer\n"
	"	rule = * idle\n"
	"[state \"sh.comment\"]\n"
	"	color = comment\n"
	"	rule = \"\\n\" idle\n"
	"	rule = * comment\n"
	"[state \"sh.dquote\"]\n"
	"	color = string\n"
	"	rule = \"\\\\\" desc\n"
	"	rule = \"\\\"\" idle\n"
	"	rule = \"$\" dvar recolor\n"
	"	rule = * dquote\n"
	"[state \"sh.dvar\"]\n"
	"	color = variable\n"
	"	rule = \"{\" dvarbrace\n"
	"	rule = \"a-zA-Z0-9_\" dvar\n"
	"	rule = * dquote noeat\n"
	"[state \"sh.dvarbrace\"]\n"
	"	color = variable\n"
	"	rule = \"}\" dquote\n"
	"	rule = * dvarbrace\n"
	"[state \"sh.desc\"]\n"
	"	color = string\n"
	"	rule = * dquote\n"
	"[state \"sh.squote\"]\n"
	"	color = string\n"
	"	rule = \"'\" idle\n"
	"	rule = * squote\n"
	"[state \"sh.var\"]\n"
	"	color = variable\n"
	"	rule = \"{\" varbrace\n"
	"	rule = \"a-zA-Z0-9_\" var\n"
	"	rule = * idle noeat\n"
	"[state \"sh.varbrace\"]\n"
	"	color = variable\n"
	"	rule = \"}\" idle\n"
	"	rule = * varbrace\n"
	"[state \"sh.ident\"]\n"
	"	color = text\n"
	"	rule = \"a-zA-Z0-9_\" ident\n"
	"	rule = * idle noeat kw=sh.keywords:keyword\n"
	"\n"
	"[color \"md\"]\n"
	"	heading = 11 bold\n"
	"	bold = 15 bold\n"
	"	italic = 13 italic\n"
	"	code = 10\n"
	"	codeblock = 10\n"
	"	quote = 14\n"
	"	link = 13 underline\n"
	"	url = 14\n"
	"	listmark = 11\n"
	"	rule = 14\n"
	/* bol: the start of a line, where block-level markers are recognized. The
	 * FSM carries its end-of-line state into the next line, so a line returns
	 * here on its trailing newline. Leading spaces and tabs are skipped so an
	 * indented marker is still seen. */
	"[state \"md.bol\"]\n"
	"	color = text\n"
	"	rule = \"\\x20\\t\" bol\n"
	"	rule = \"#\" heading recolor\n"
	"	rule = \">\" quote recolor\n"
	"	rule = \"`\" fence1\n"
	"	rule = \"-\" dash\n"
	"	rule = \"+\" plus\n"
	"	rule = \"*\" bstar\n"
	"	rule = \"0-9\" olist mark\n"
	"	rule = \"[\" link_text recolor\n"
	"	rule = \"\\\\\" escape\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n"
	/* inline: the body of a normal text line, where span-level markup lives. */
	"[state \"md.inline\"]\n"
	"	color = text\n"
	"	rule = \"`\" code recolor\n"
	"	rule = \"*\" star1\n"
	"	rule = \"_\" us1\n"
	"	rule = \"[\" link_text recolor\n"
	"	rule = \"\\\\\" escape\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline\n"
	/* ATX heading and blockquote both color the whole line. */
	"[state \"md.heading\"]\n"
	"	color = heading\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * heading\n"
	"[state \"md.quote\"]\n"
	"	color = quote\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * quote\n"
	/* A backslash escapes the next byte so it cannot open a span. */
	"[state \"md.escape\"]\n"
	"	color = text\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline\n"
	/* Inline code span: `code`. The backtick at a line start is disambiguated
	 * from a ``` fence by counting backticks (fence1/fence2). */
	"[state \"md.code\"]\n"
	"	color = code\n"
	"	rule = \"`\" inline\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * code\n"
	"[state \"md.fence1\"]\n"
	"	color = text\n"
	"	rule = \"`\" fence2\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * code noeat recolor=2\n"
	"[state \"md.fence2\"]\n"
	"	color = text\n"
	"	rule = \"`\" fence_open recolor=3\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * code noeat recolor=3\n"
	/* fence_open colors the opening fence and info string; its newline carries
	 * into codeblock, the per-line start state inside a fenced block. */
	"[state \"md.fence_open\"]\n"
	"	color = code\n"
	"	rule = \"\\n\" codeblock\n"
	"	rule = * fence_open\n"
	"[state \"md.codeblock\"]\n"
	"	color = codeblock\n"
	"	rule = \"`\" cb_fence1\n"
	"	rule = \"\\n\" codeblock\n"
	"	rule = * cb_line noeat\n"
	"[state \"md.cb_line\"]\n"
	"	color = codeblock\n"
	"	rule = \"\\n\" codeblock\n"
	"	rule = * cb_line\n"
	"[state \"md.cb_fence1\"]\n"
	"	color = codeblock\n"
	"	rule = \"`\" cb_fence2\n"
	"	rule = \"\\n\" codeblock\n"
	"	rule = * cb_line noeat\n"
	"[state \"md.cb_fence2\"]\n"
	"	color = codeblock\n"
	"	rule = \"`\" cb_fence3\n"
	"	rule = \"\\n\" codeblock\n"
	"	rule = * cb_line noeat\n"
	/* The closing fence consumes the rest of its line and exits to bol. */
	"[state \"md.cb_fence3\"]\n"
	"	color = codeblock\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * cb_fence3\n"
	/* Emphasis with '*': one star is italic, two are bold. The opening marker
	 * is repainted by recolor once the kind is known. */
	"[state \"md.star1\"]\n"
	"	color = text\n"
	"	rule = \"*\" bold recolor=2\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * italic noeat recolor=2\n"
	/* bstar is star1 at a line start, where '* ' is instead a list bullet. */
	"[state \"md.bstar\"]\n"
	"	color = text\n"
	"	rule = \"\\x20\\t\" afterbullet recolor=2\n"
	"	rule = \"*\" bold recolor=2\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * italic noeat recolor=2\n"
	"[state \"md.italic\"]\n"
	"	color = italic\n"
	"	rule = \"*\" inline\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * italic\n"
	"[state \"md.bold\"]\n"
	"	color = bold\n"
	"	rule = \"*\" bold_c1\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * bold\n"
	"[state \"md.bold_c1\"]\n"
	"	color = bold\n"
	"	rule = \"*\" inline\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * bold noeat\n"
	/* The same for '_' emphasis, which closes on '_' rather than '*'. */
	"[state \"md.us1\"]\n"
	"	color = text\n"
	"	rule = \"_\" ubold recolor=2\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * uitalic noeat recolor=2\n"
	"[state \"md.uitalic\"]\n"
	"	color = italic\n"
	"	rule = \"_\" inline\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * uitalic\n"
	"[state \"md.ubold\"]\n"
	"	color = bold\n"
	"	rule = \"_\" ubold_c1\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * ubold\n"
	"[state \"md.ubold_c1\"]\n"
	"	color = bold\n"
	"	rule = \"_\" inline\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * ubold noeat\n"
	/* Link: [text](url). A ']' not followed by '(' falls back to plain text. */
	"[state \"md.link_text\"]\n"
	"	color = link\n"
	"	rule = \"]\" link_rb\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * link_text\n"
	"[state \"md.link_rb\"]\n"
	"	color = link\n"
	"	rule = \"(\" link_url\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n"
	"[state \"md.link_url\"]\n"
	"	color = url\n"
	"	rule = \")\" inline\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * link_url\n"
	/* List bullets ('-', '+', '* ') and a '---' horizontal rule. afterbullet
	 * colors the marker, then hands the rest of the line to inline. */
	"[state \"md.dash\"]\n"
	"	color = text\n"
	"	rule = \"\\x20\\t\" afterbullet recolor=2\n"
	"	rule = \"-\" dash2\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n"
	"[state \"md.dash2\"]\n"
	"	color = text\n"
	"	rule = \"-\" hrline recolor=3\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n"
	"[state \"md.hrline\"]\n"
	"	color = rule\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * hrline\n"
	"[state \"md.plus\"]\n"
	"	color = text\n"
	"	rule = \"\\x20\\t\" afterbullet recolor=2\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n"
	/* Ordered list: digits then '.' or ')'. The run is marked in bol and
	 * repainted once the trailing space confirms it is a list marker. */
	"[state \"md.olist\"]\n"
	"	color = text\n"
	"	rule = \"0-9\" olist\n"
	"	rule = \".)\" afterdot\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n"
	"[state \"md.afterdot\"]\n"
	"	color = text\n"
	"	rule = \"\\x20\\t\" afterbullet recolormark\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n"
	"[state \"md.afterbullet\"]\n"
	"	color = listmark\n"
	"	rule = \"\\n\" bol\n"
	"	rule = * inline noeat\n";

/* The config that backs the default grammars; kept for the lifetime of the
 * process because the grammars' word slices point into it. */
static Cfg *g_def_cfg;

/* Parse and load the built-in grammars into g_def, once. */
static void
syntax_load_defaults(void)
{
	char *text;

	if (g_def_cfg)
		return;
	g_def_cfg = vedit_cfg_new();
	if (!g_def_cfg)
		return;
	text = cfg_dup(g_default_grammar);	/* cfg_load_mem mutates its input */
	if (text) {
		cfg_load_mem(g_def_cfg, text);
		free(text);
	}
	syntax_load_cfg(&g_def, g_def_cfg);
}

/* Style one line into out[0..n) (out may be NULL to carry state only). All
 * highlighting goes through the FSM now, so this just dispatches to it. Returns
 * the FSM carry state at end of line (0 when there is no grammar). */
static uint16_t
syn_line(const Syntax *sy, uint16_t state_in, const char *bytes,
    size_t n, uint8_t *out)
{
	if (!sy || !sy->fsm)
		return 0;
	return jsf_line(sy->fsm, state_in, bytes, n, out);
}

/* Find a grammar named name: a user grammar first (so a user config overrides a
 * built-in), then a default one. */
static const Syntax *
syn_reg_find(const char *name)
{
	int i = jsf_find(&g_user, name);

	if (i >= 0)
		return &g_user.syn[i];
	i = jsf_find(&g_def, name);
	if (i >= 0)
		return &g_def.syn[i];
	return NULL;
}

/* Map a file extension (or a :syntax language name) to a grammar. A language of
 * that exact name wins first (so :syntax <lang> and a matching extension both
 * work), then a "syntax.<ext> = <lang>" mapping from the user config, then the
 * same mapping from the built-in defaults. */
static const Syntax *
syn_for_ext(const char *ext)
{
	char key[JSF_NAME + 16];
	const char *mapped;
	const Syntax *s;

	if (!ext || !*ext)
		return NULL;
	syntax_load_defaults();
	s = syn_reg_find(ext);
	if (s)
		return s;
	snprintf(key, sizeof(key), "syntax.%s", ext);
	mapped = cfg_get(g_cfg, key);
	if (!mapped)
		mapped = cfg_get(g_def_cfg, key);
	if (mapped)
		return syn_reg_find(mapped);
	return NULL;
}


#define VEDIT_VERSION "vedit 1.0.0"

/****************************************************************
 * Keyboard input decoding -- It fills the same struct tkbd_seq the editor
 * reads, decoding UTF-8 text, control keys, arrows, Home/End/PgUp/PgDn,
 * Insert/Delete, function keys, CSI modifiers, Alt+letter, and bracketed
 * paste. Mouse decoding is intentionally omitted.
 ****************************************************************/

#define TKBD_SEQ_MAX 32
#define TKBD_CH_NONE 0x7FFFFFFFU

struct tkbd_seq {
	uint8_t		type;
	uint8_t		mod;
	uint16_t	key;
	uint32_t	ch;
	int32_t		x, y;
	size_t		len;
	char		data[TKBD_SEQ_MAX];
};

#define TKBD_KEY    1
#define TKBD_MOUSE  2

#define TKBD_MOD_NONE   0x00
#define TKBD_MOD_SHIFT  0x01
#define TKBD_MOD_ALT    0x02
#define TKBD_MOD_CTRL   0x04
#define TKBD_MOD_META   0x08
#define TKBD_MOD_MOTION 0x80

#define TKBD_KEY_NONE        0x00
#define TKBD_KEY_UNKNOWN     0xFFFF
#define TKBD_KEY_PASTE_BEGIN 0xFFF0
#define TKBD_KEY_PASTE_END   0xFFF1

#define TKBD_KEY_BACKSPACE   0x08
#define TKBD_KEY_TAB         0x09
#define TKBD_KEY_ENTER       0x0A
#define TKBD_KEY_ESC         0x1B
#define TKBD_KEY_SPACE       0x20
#define TKBD_KEY_BACKSPACE2  0x7F

#define TKBD_KEY_UP          0x10
#define TKBD_KEY_DOWN        0x11
#define TKBD_KEY_RIGHT       0x12
#define TKBD_KEY_LEFT        0x13
#define TKBD_KEY_INS         0x14
#define TKBD_KEY_DEL         0x15
#define TKBD_KEY_PGUP        0x16
#define TKBD_KEY_PGDN        0x17
#define TKBD_KEY_HOME        0x18
#define TKBD_KEY_END         0x19

/* Letters and digits map to their ASCII codes. */
#define TKBD_KEY_0 0x30
#define TKBD_KEY_1 0x31
#define TKBD_KEY_2 0x32
#define TKBD_KEY_3 0x33
#define TKBD_KEY_4 0x34
#define TKBD_KEY_5 0x35
#define TKBD_KEY_6 0x36
#define TKBD_KEY_7 0x37
#define TKBD_KEY_8 0x38
#define TKBD_KEY_9 0x39
#define TKBD_KEY_A 0x41
#define TKBD_KEY_B 0x42
#define TKBD_KEY_C 0x43
#define TKBD_KEY_D 0x44
#define TKBD_KEY_E 0x45
#define TKBD_KEY_F 0x46
#define TKBD_KEY_G 0x47
#define TKBD_KEY_H 0x48
#define TKBD_KEY_I 0x49
#define TKBD_KEY_J 0x4A
#define TKBD_KEY_K 0x4B
#define TKBD_KEY_L 0x4C
#define TKBD_KEY_M 0x4D
#define TKBD_KEY_N 0x4E
#define TKBD_KEY_O 0x4F
#define TKBD_KEY_P 0x50
#define TKBD_KEY_Q 0x51
#define TKBD_KEY_R 0x52
#define TKBD_KEY_S 0x53
#define TKBD_KEY_T 0x54
#define TKBD_KEY_U 0x55
#define TKBD_KEY_V 0x56
#define TKBD_KEY_W 0x57
#define TKBD_KEY_X 0x58
#define TKBD_KEY_Y 0x59
#define TKBD_KEY_Z 0x5A

/* Function keys map to the low alpha range. */
#define TKBD_KEY_F1  0x61
#define TKBD_KEY_F2  0x62
#define TKBD_KEY_F3  0x63
#define TKBD_KEY_F4  0x64
#define TKBD_KEY_F5  0x65
#define TKBD_KEY_F6  0x67
#define TKBD_KEY_F7  0x68
#define TKBD_KEY_F8  0x69
#define TKBD_KEY_F9  0x6A
#define TKBD_KEY_F10 0x6B
#define TKBD_KEY_F11 0x6C
#define TKBD_KEY_F12 0x6D

/* Mouse pseudo-keys. vedit does not decode mouse input, but the editor
 * references these constants; they stay so it compiles and the mouse paths
 * remain dead code. */
#define TKBD_MOUSE_LEFT       (0xFFFF - 1)
#define TKBD_MOUSE_RIGHT      (0xFFFF - 2)
#define TKBD_MOUSE_MIDDLE     (0xFFFF - 3)
#define TKBD_MOUSE_RELEASE    (0xFFFF - 4)
#define TKBD_MOUSE_WHEEL_UP   (0xFFFF - 5)
#define TKBD_MOUSE_WHEEL_DOWN (0xFFFF - 6)

/* The I/O vtable (struct vedit_io) and the embed API are declared in vedit.h,
 * included near the top of this file. */

/****************************************************************
 * The draw surface and its ANSI terminal backend. draw is the
 * cell grid the editor paints into; draw_term holds the grid, the
 * shadow copy for dirty-line diffing, the input decoder, and the
 * bound io vtable. scr_new wraps a draw_term, matching the two
 * objects the editor expects (e->d and e->term).
 ****************************************************************/

#define CELL_CONT 0xFFFEu

typedef enum draw_event_type {
	EVENT_NONE,
	EVENT_KEY,
	EVENT_RESIZE,
	EVENT_RESUME,
	EVENT_EOF,
	EVENT_IDLE,		/* input went quiet (poll timeout); no key */
} EventType;

typedef struct draw_event {
	EventType	type;
	struct tkbd_seq		key;
} Event;

typedef enum cursor_shape {
	CURSOR_DEFAULT,
	CURSOR_BLOCK,
	CURSOR_BAR,
	CURSOR_UNDERLINE,
} CursorShape;


typedef struct draw_term {
	struct vedit_io	io;

	int		rows, cols;
	Cell	*cur;		/* grid being painted */
	Cell	*shadow;	/* last grid presented */
	int		*rowdirty;	/* per-row dirty flag for the diff */

	int		cursor_r, cursor_c;
	int		cursor_vis;

	int		begun;
	int		want_resize;	/* a resize is pending for scr_wait */
	int		box_mode;	/* enum vedit_box_mode for the glyphs */
	int		colors;		/* 256 or 16 (downgrade palette) */
	int		scroll;		/* use the terminal scroll region */
	int		shadow_valid;	/* the shadow matches the screen (safe to scroll) */

	/* raw input bytes awaiting decode */
	unsigned char	inbuf[512];
	int		inlen;

	/* vi macro recording: the raw bytes of each event decoded while on */
	unsigned char	*rec;
	size_t		rec_len, rec_cap;
	int		rec_on;

	/* pending output bytes awaiting flush */
	char		*out;
	size_t		outlen, outcap;
} Scrbuf;

typedef struct draw { Scrbuf *t; } Screen;

/* The one process-wide window-change flag, set by the SIGWINCH handler the
 * command-line binding installs. An embedded host uses vedit_set_size()
 * instead and never touches this. */
static volatile sig_atomic_t g_winch;

static void
scr_reserve(Scrbuf *t, size_t need)
{
	if (t->outlen + need > t->outcap) {
		size_t cap = t->outcap ? t->outcap : 4096;
		char *p;

		while (cap < t->outlen + need)
			cap *= 2;
		p = realloc(t->out, cap);
		if (!p)
			return;
		t->out = p;
		t->outcap = cap;
	}
}

static void
scr_bytes(Scrbuf *t, const char *s, size_t n)
{
	scr_reserve(t, n);
	if (t->outlen + n <= t->outcap) {
		memcpy(t->out + t->outlen, s, n);
		t->outlen += n;
	}
}

static void
scr_str(Scrbuf *t, const char *s)
{
	scr_bytes(t, s, strlen(s));
}

static void
scr_flush(Scrbuf *t)
{
	size_t off = 0;

	while (off < t->outlen) {
		long w = t->io.write(t->io.ctx, t->out + off,
		    (long)(t->outlen - off));

		if (w <= 0)
			break;		/* host could not accept more; drop it */
		off += (size_t)w;
	}
	t->outlen = 0;
}

static int
grid_alloc(Scrbuf *t, int rows, int cols)
{
	size_t n = (size_t)rows * (size_t)cols;
	Cell *cur = calloc(n ? n : 1, sizeof(*cur));
	Cell *shadow = calloc(n ? n : 1, sizeof(*shadow));
	int *rd = calloc(rows ? (size_t)rows : 1, sizeof(*rd));
	size_t i;

	if (!cur || !shadow || !rd) {
		free(cur);
		free(shadow);
		free(rd);
		return -1;
	}
	for (i = 0; i < n; i++) {
		vt_cell_clear(&cur[i]);
		vt_cell_clear(&shadow[i]);
	}
	free(t->cur);
	free(t->shadow);
	free(t->rowdirty);
	t->cur = cur;
	t->shadow = shadow;
	t->rowdirty = rd;
	t->rows = rows;
	t->cols = cols;
	return 0;
}

static Scrbuf *
scr_new_io(const struct vedit_io *io)
{
	Scrbuf *t = calloc(1, sizeof(*t));
	int rows = 24, cols = 80;

	if (!t)
		return NULL;
	t->io = *io;
	t->cursor_vis = 1;
	t->box_mode = box_default();
	t->colors = color_default();
	t->scroll = scroll_default();
	if (t->io.getsize && t->io.getsize(t->io.ctx, &rows, &cols) == 0) {
		if (rows < 1)
			rows = 24;
		if (cols < 1)
			cols = 80;
	}
	if (grid_alloc(t, rows, cols) != 0) {
		free(t);
		return NULL;
	}
	return t;
}


/* ---- SGR emission ------------------------------------------------------- */

static int
color_eq(Color a, Color b)
{
	if (a.type != b.type)
		return 0;
	if (a.type == COLOR_INDEXED)
		return a.index == b.index;
	if (a.type == COLOR_RGB)
		return a.rgb.r == b.rgb.r && a.rgb.g == b.rgb.g &&
		    a.rgb.b == b.rgb.b;
	return 1;
}

/* Nearest of the 16 ANSI colors to an RGB triple, by squared distance. */
static int
rgb_to_ansi16(int r, int g, int b)
{
	static const unsigned char ansi[16][3] = {
		{   0,   0,   0 }, { 205,   0,   0 }, {   0, 205,   0 },
		{ 205, 205,   0 }, {   0,   0, 238 }, { 205,   0, 205 },
		{   0, 205, 205 }, { 229, 229, 229 }, { 127, 127, 127 },
		{ 255,   0,   0 }, {   0, 255,   0 }, { 255, 255,   0 },
		{  92,  92, 255 }, { 255,   0, 255 }, {   0, 255, 255 },
		{ 255, 255, 255 },
	};
	int i, best = 7;
	long bestd = -1;

	for (i = 0; i < 16; i++) {
		int dr = r - ansi[i][0];
		int dg = g - ansi[i][1];
		int db = b - ansi[i][2];
		long d = (long)dr * dr + (long)dg * dg + (long)db * db;

		if (bestd < 0 || d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

/* Map an xterm-256 palette index to the nearest ANSI 16 color. Indices 0-15
 * are already ANSI; 16-231 are the 6x6x6 cube; 232-255 the grayscale ramp. */
static int
color256_to_16(int idx)
{
	static const unsigned char cube[6] = { 0, 95, 135, 175, 215, 255 };
	int r, g, b;

	if (idx < 16)
		return idx < 0 ? 7 : idx;
	if (idx < 232) {
		idx -= 16;
		r = cube[(idx / 36) % 6];
		g = cube[(idx / 6) % 6];
		b = cube[idx % 6];
	} else if (idx < 256) {
		r = g = b = 8 + (idx - 232) * 10;
	} else {
		return 7;
	}
	return rgb_to_ansi16(r, g, b);
}

static void
scr_sgr_color(Scrbuf *t, Color c, int is_bg)
{
	char buf[32];

	if (c.type == COLOR_DEFAULT) {
		snprintf(buf, sizeof(buf), "\033[%dm", is_bg ? 49 : 39);
		scr_str(t, buf);
		return;
	}
	if (t->colors < 256) {		/* 16-color client: downgrade + classic SGR */
		int ci = (c.type == COLOR_INDEXED) ? color256_to_16(c.index)
		    : rgb_to_ansi16(c.rgb.r, c.rgb.g, c.rgb.b);
		int code = (ci < 8) ? (is_bg ? 40 : 30) + ci
		    : (is_bg ? 100 : 90) + (ci - 8);

		snprintf(buf, sizeof(buf), "\033[%dm", code);
		scr_str(t, buf);
		return;
	}
	if (c.type == COLOR_INDEXED)
		snprintf(buf, sizeof(buf), "\033[%d;5;%dm", is_bg ? 48 : 38,
		    c.index);
	else
		snprintf(buf, sizeof(buf), "\033[%d;2;%d;%d;%dm",
		    is_bg ? 48 : 38, c.rgb.r, c.rgb.g, c.rgb.b);
	scr_str(t, buf);
}

static void
scr_sgr_attrs(Scrbuf *t, uint16_t attrs)
{
	if (attrs & ATTR_BOLD)
		scr_str(t, "\033[1m");
	if (attrs & ATTR_DIM)
		scr_str(t, "\033[2m");
	if (attrs & ATTR_ITALIC)
		scr_str(t, "\033[3m");
	if (attrs & ATTR_UNDERLINE)
		scr_str(t, "\033[4m");
	if (attrs & ATTR_BLINK)
		scr_str(t, "\033[5m");
	if (attrs & ATTR_REVERSE)
		scr_str(t, "\033[7m");
	if (attrs & ATTR_STRIKE)
		scr_str(t, "\033[9m");
}

/* The pen tracks the SGR state already emitted, so a row paints only the
 * changes between adjacent cells instead of a full reset per cell. This matters
 * on a slow link: a uniformly colored line costs one escape, not one per
 * column. */
typedef struct pen {
	int		valid;
	Color	fg, bg;
	uint16_t	attrs;
} Pen;

static void
pen_reset(Pen *p)
{
	p->valid = 0;
}

/* Bring the terminal pen to (fg, bg, attrs), emitting only what changed. An
 * attribute turning off needs a full reset (SGR has no per-attribute off here),
 * so an attrs change re-emits the colors too. */
static void
scr_pen(Scrbuf *t, Pen *p, Color fg,
    Color bg, uint16_t attrs)
{
	int need_color;

	if (p->valid && p->attrs == attrs && color_eq(p->fg, fg) &&
	    color_eq(p->bg, bg))
		return;

	if (!p->valid || p->attrs != attrs) {
		scr_str(t, "\033[0m");
		scr_sgr_attrs(t, attrs);
		need_color = 1;		/* reset cleared the colors */
	} else {
		need_color = 0;
	}
	if (need_color || !color_eq(p->fg, fg)) {
		if (fg.type != COLOR_DEFAULT || !need_color)
			scr_sgr_color(t, fg, 0);
	}
	if (need_color || !color_eq(p->bg, bg)) {
		if (bg.type != COLOR_DEFAULT || !need_color)
			scr_sgr_color(t, bg, 1);
	}
	p->fg = fg;
	p->bg = bg;
	p->attrs = attrs;
	p->valid = 1;
}

static int
cell_eq(const Cell *a, const Cell *b)
{
	return a->codepoint == b->codepoint && a->attrs == b->attrs &&
	    a->width == b->width && color_eq(a->fg, b->fg) &&
	    color_eq(a->bg, b->bg);
}

/* A blank cell on the terminal default background: a run of these at the end of
 * a row can be produced with one erase-to-EOL (ESC [ K), which clears to the
 * default background on every client, with or without back-color-erase. (A
 * themed background such as the DOS blue is not default, so such a row falls
 * back to emitting the spaces; the black scheme keeps the text area default so
 * the saving applies there.) The glyph is a space, so its foreground does not
 * matter. */
static int
cell_is_blank_default(const Cell *c)
{
	return c->codepoint == ' ' && c->attrs == 0 &&
	    c->bg.type == COLOR_DEFAULT;
}

/* ---- the draw surface API the editor calls ------------------------------ */

static Screen *
scr_new(void *ctx)
{
	Screen *d;

	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->t = ctx;
	return d;
}

static void
scr_size(Screen *d, int *rows, int *cols)
{
	if (rows)
		*rows = d->t->rows;
	if (cols)
		*cols = d->t->cols;
}

static void
scr_resize(Screen *d, int rows, int cols)
{
	Scrbuf *t = d->t;
	int i;

	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;
	if (grid_alloc(t, rows, cols) != 0)
		return;
	for (i = 0; i < rows; i++)
		t->rowdirty[i] = 1;
	t->shadow_valid = 0;		/* grid reallocated; shadow is blank */
}

static void
scr_clear(Screen *d)
{
	Scrbuf *t = d->t;
	size_t n = (size_t)t->rows * (size_t)t->cols;
	size_t i;

	for (i = 0; i < n; i++)
		vt_cell_clear(&t->cur[i]);
}

static void
scr_cell(Screen *d, int r, int c, uint32_t cp, Color fg,
    Color bg, uint16_t attrs)
{
	Scrbuf *t = d->t;
	Cell *cell;
	int w;

	if (r < 0 || r >= t->rows || c < 0 || c >= t->cols)
		return;
	cell = &t->cur[(size_t)r * t->cols + c];
	w = rune_width(cp);
	if (w < 1)
		w = 1;
	cell->codepoint = cp;
	cell->fg = fg;
	cell->bg = bg;
	cell->attrs = attrs;
	cell->width = (uint8_t)w;
	if (w == 2 && c + 1 < t->cols) {
		Cell *cont = &t->cur[(size_t)r * t->cols + c + 1];

		cont->codepoint = CELL_CONT;
		cont->fg = fg;
		cont->bg = bg;
		cont->attrs = attrs;
		cont->width = 0;
	}
}

static int
scr_text(Screen *d, int r, int c, const char *utf8, Color fg,
    Color bg, uint16_t attrs)
{
	Scrbuf *t = d->t;
	const unsigned char *p = (const unsigned char *)utf8;
	size_t len = strlen(utf8), i = 0;

	while (i < len && c < t->cols) {
		uint32_t cp;
		int n = utf8_decode(&cp, p + i, len - i);
		int w;

		if (n <= 0)
			n = 1;
		if (cp < 0x20 || cp == 0x7f)
			cp = ' ';
		w = rune_width(cp);
		if (w < 1)
			w = 1;
		if (w == 2 && c + 1 >= t->cols)
			break;			/* a wide glyph would overflow */
		scr_cell(d, r, c, cp, fg, bg, attrs);
		c += w;
		i += (size_t)n;
	}
	return c;
}

static void
scr_fill(Screen *d, int r, int c, int n, uint32_t cp, Color fg,
    Color bg, uint16_t attrs)
{
	int i;

	for (i = 0; i < n; i++)
		scr_cell(d, r, c + i, cp, fg, bg, attrs);
}

static void
scr_cursor(Screen *d, int r, int c)
{
	d->t->cursor_r = r;
	d->t->cursor_c = c;
}

static void
scr_cursor_vis(Screen *d, int on)
{
	d->t->cursor_vis = on;
}

static void
scr_cursor_shape(Screen *d, CursorShape shape)
{
	(void)d; (void)shape;	/* primitive terminals ignore cursor shape */
}

/* Emit one cell's glyph at the cursor, tracking the pen and the DEC
 * line-drawing charset (*acs) across calls. The caller positions the cursor and
 * relies on the terminal to advance it. */
static void
scr_emit_cell(Scrbuf *t, Pen *pen, int *acs, const Cell *cell)
{
	uint32_t cp = cell->codepoint;
	unsigned char ub[4];
	int n;

	if (cp == CELL_CONT)
		return;			/* trailing half of a wide cell */
	scr_pen(t, pen, cell->fg, cell->bg, cell->attrs);

	if (BOX_IS(cp)) {
		const BoxDef *b = &box_tab[BOX_ID(cp)];

		if (t->box_mode == VEDIT_BOX_DEC && b->dec) {
			if (!*acs) {
				scr_str(t, "\033(0");
				*acs = 1;
			}
			ub[0] = b->dec;
			scr_bytes(t, (const char *)ub, 1);
			return;
		}
		if (*acs) {
			scr_str(t, "\033(B");
			*acs = 0;
		}
		cp = (t->box_mode == VEDIT_BOX_UTF8) ? b->uni
		    : (uint32_t)b->ascii;
		n = utf8_encode(ub, cp);
		scr_bytes(t, (const char *)ub, (size_t)n);
		return;
	}

	if (*acs) {			/* back to ASCII for ordinary text */
		scr_str(t, "\033(B");
		*acs = 0;
	}
	if (cp < 0x20 || cp == 0x7f)
		n = utf8_encode(ub, ' ');
	else
		n = utf8_encode(ub, cp);
	if (n <= 0) {
		ub[0] = ' ';
		n = 1;
	}
	scr_bytes(t, (const char *)ub, (size_t)n);
}

/* Shortest blank-default run (in columns) worth replacing with an erase-to-EOL
 * plus a repaint of whatever non-blank cells follow it. The EL path costs a
 * reset, the EL, and a reposition-and-redraw of the trailing non-blank cells
 * (in the framed editor that is just the border/scrollbar column), so it only
 * pays off once the run of spaces it saves is longer than that overhead. */
#define SCR_EL_MIN 16

/* Present the current grid: for every row that differs from the shadow, move
 * to its first changed column and repaint only the changed span (an in-row span
 * diff), then sync the shadow. On a row whose changed span holds a long run of
 * default-background blanks, that run is cleared with one erase-to-EOL and the
 * cells past it repainted, instead of emitting a column of spaces; this needs a
 * default background (the black scheme keeps one, the DOS blue does not). Both
 * savings matter on a slow link. */
static void
scr_present(Screen *d)
{
	Scrbuf *t = d->t;
	int r, c;
	char mv[32];
	Pen pen;

	scr_str(t, "\033[?25l");		/* hide cursor during the paint */
	for (r = 0; r < t->rows; r++) {
		Cell *row = &t->cur[(size_t)r * t->cols];
		Cell *srow = &t->shadow[(size_t)r * t->cols];
		int acs = 0;		/* DEC line-drawing charset is active */
		int first, last, g0 = 0, run, gap0 = 0, gap1 = 0;

		/* first and last columns that differ from the shadow */
		first = 0;
		while (first < t->cols && cell_eq(&row[first], &srow[first]))
			first++;
		if (first >= t->cols) {
			t->rowdirty[r] = 0;	/* nothing on this row changed */
			continue;
		}
		last = t->cols - 1;
		while (last > first && cell_eq(&row[last], &srow[last]))
			last--;

		/* never start or end in the trailing half of a wide glyph */
		while (first > 0 && row[first].codepoint == CELL_CONT)
			first--;
		if (last + 1 < t->cols && row[last + 1].codepoint == CELL_CONT)
			last++;

		/* longest run of default-background blanks within the changed
		 * span, as a candidate for erase-to-EOL */
		run = 0;
		for (c = first; c <= last; c++) {
			if (cell_is_blank_default(&row[c])) {
				if (run == 0)
					g0 = c;
				run++;
				if (run > gap1 - gap0) {
					gap0 = g0;
					gap1 = c + 1;
				}
			} else {
				run = 0;
			}
		}

		pen_reset(&pen);
		if (gap1 - gap0 >= SCR_EL_MIN) {
			/* paint up to the blank run, erase it (and everything to
			 * the row's end) with one EL, then repaint the non-blank
			 * cells that follow; EL clears to the default background,
			 * which is why this path needs one */
			snprintf(mv, sizeof(mv), "\033[%d;%dH", r + 1, first + 1);
			scr_str(t, mv);
			for (c = first; c < gap0; c++)
				scr_emit_cell(t, &pen, &acs, &row[c]);
			if (acs) {
				scr_str(t, "\033(B");
				acs = 0;
			}
			scr_str(t, "\033[0m");	/* default bg, so EL clears right */
			scr_str(t, "\033[K");
			pen_reset(&pen);
			for (c = gap1; c < t->cols; c++) {
				if (cell_is_blank_default(&row[c]) ||
				    row[c].codepoint == CELL_CONT)
					continue;
				snprintf(mv, sizeof(mv), "\033[%d;%dH", r + 1,
				    c + 1);
				scr_str(t, mv);
				pen_reset(&pen);
				while (c < t->cols &&
				    !cell_is_blank_default(&row[c])) {
					scr_emit_cell(t, &pen, &acs, &row[c]);
					c++;
				}
				if (acs) {
					scr_str(t, "\033(B");
					acs = 0;
				}
				c--;		/* the for loop re-increments */
			}
		} else {
			snprintf(mv, sizeof(mv), "\033[%d;%dH", r + 1, first + 1);
			scr_str(t, mv);
			for (c = first; c <= last; c++)
				scr_emit_cell(t, &pen, &acs, &row[c]);
			if (acs) {	/* never leave a row in line-drawing mode */
				scr_str(t, "\033(B");
				acs = 0;
			}
			scr_str(t, "\033[0m");
		}
		t->rowdirty[r] = 0;
		memcpy(srow, row, (size_t)t->cols * sizeof(*row));
	}

	snprintf(mv, sizeof(mv), "\033[%d;%dH", t->cursor_r + 1,
	    t->cursor_c + 1);
	scr_str(t, mv);
	scr_str(t, t->cursor_vis ? "\033[?25h" : "\033[?25l");
	t->shadow_valid = 1;
	scr_flush(t);
}

/* Scroll a band of rows [top, top+count) in the terminal by `delta` lines
 * (positive moves content up, exposing new rows at the bottom; negative moves
 * it down) using the VT100 scroll region, then shift the shadow to match so
 * scr_present repaints only the newly exposed rows. The caller gates this on
 * t->scroll and on the shadow being valid; see render_body. */
static void
scr_blank_row(Cell *row, int cols)
{
	int c;

	for (c = 0; c < cols; c++)
		vt_cell_clear(&row[c]);
}

static void
scr_scroll(Screen *d, int top, int count, int delta)
{
	Scrbuf *t = d->t;
	int cols = t->cols;
	int bot = top + count;		/* exclusive, 0-based */
	int k = delta < 0 ? -delta : delta;
	char buf[32];
	int i, r;

	if (k < 1 || k >= count || top < 0 || bot > t->rows)
		return;

	scr_str(t, "\033[0m");		/* scrolled-in lines take the default bg */
	snprintf(buf, sizeof(buf), "\033[%d;%dr", top + 1, bot);
	scr_str(t, buf);		/* set the scroll region (1-based) */
	if (delta > 0) {		/* content up: IND at the bottom margin */
		snprintf(buf, sizeof(buf), "\033[%d;1H", bot);
		scr_str(t, buf);
		for (i = 0; i < k; i++)
			scr_str(t, "\033D");
	} else {			/* content down: RI at the top margin */
		snprintf(buf, sizeof(buf), "\033[%d;1H", top + 1);
		scr_str(t, buf);
		for (i = 0; i < k; i++)
			scr_str(t, "\033M");
	}
	scr_str(t, "\033[r");		/* restore the full-screen region */

	if (delta > 0) {
		for (r = top; r < bot - k; r++)
			memcpy(&t->shadow[(size_t)r * cols],
			    &t->shadow[(size_t)(r + k) * cols],
			    (size_t)cols * sizeof(Cell));
		for (r = bot - k; r < bot; r++)
			scr_blank_row(&t->shadow[(size_t)r * cols], cols);
	} else {
		for (r = bot - 1; r >= top + k; r--)
			memcpy(&t->shadow[(size_t)r * cols],
			    &t->shadow[(size_t)(r - k) * cols],
			    (size_t)cols * sizeof(Cell));
		for (r = top; r < top + k; r++)
			scr_blank_row(&t->shadow[(size_t)r * cols], cols);
	}
}

static void
scr_begin(Screen *d)
{
	Scrbuf *t = d->t;
	int i;

	if (t->io.begin)
		t->io.begin(t->io.ctx);
	t->begun = 1;
	scr_str(t, "\033[?1049h");	/* alt screen, if supported */
	scr_str(t, "\033[?2004h");	/* bracketed paste */
	scr_str(t, "\033[2J");		/* clear */
	scr_str(t, "\033[H");
	for (i = 0; i < t->rows; i++)
		t->rowdirty[i] = 1;
	t->shadow_valid = 0;		/* screen just cleared; do not scroll yet */
	scr_flush(t);
}

static void
scr_end(Screen *d)
{
	Scrbuf *t = d->t;

	char mv[32];

	scr_str(t, "\033[0m");
	scr_str(t, "\033[?2004l");
	scr_str(t, "\033[?25h");
	/* Drop the cursor to the bottom and scroll up one line, so on a client
	 * without the alternate screen the shell prompt lands on a fresh line
	 * below the editor instead of in the middle of the chrome. A client that
	 * honors the alt screen discards this when it restores below. */
	snprintf(mv, sizeof(mv), "\033[%d;1H", t->rows > 0 ? t->rows : 1);
	scr_str(t, mv);
	scr_str(t, "\r\n");
	scr_str(t, "\033[?1049l");	/* leave alt screen */
	scr_flush(t);
	t->begun = 0;
	if (t->io.end)
		t->io.end(t->io.ctx);
}

static void
scr_free(Screen *d)
{
	Scrbuf *t;

	if (!d)
		return;
	t = d->t;
	if (t) {
		free(t->cur);
		free(t->shadow);
		free(t->rowdirty);
		free(t->out);
		free(t->rec);
		free(t);
	}
	free(d);
}

/* Base64-encode n bytes of in into out, which must hold ((n + 2) / 3) * 4 + 1
 * bytes. Returns the encoded length (excluding the trailing NUL). */
static size_t
b64_encode(const unsigned char *in, size_t n, char *out)
{
	static const char tab[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;

	for (i = 0; i + 3 <= n; i += 3) {
		unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 |
		    in[i + 2];

		out[o++] = tab[(v >> 18) & 0x3f];
		out[o++] = tab[(v >> 12) & 0x3f];
		out[o++] = tab[(v >> 6) & 0x3f];
		out[o++] = tab[v & 0x3f];
	}
	if (n - i == 1) {
		unsigned v = (unsigned)in[i] << 16;

		out[o++] = tab[(v >> 18) & 0x3f];
		out[o++] = tab[(v >> 12) & 0x3f];
		out[o++] = '=';
		out[o++] = '=';
	} else if (n - i == 2) {
		unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8;

		out[o++] = tab[(v >> 18) & 0x3f];
		out[o++] = tab[(v >> 12) & 0x3f];
		out[o++] = tab[(v >> 6) & 0x3f];
		out[o++] = '=';
	}
	out[o] = '\0';
	return o;
}

/* Copy n bytes to the terminal's selection buffer with OSC 52. A terminal that
 * does not implement it ignores the unknown control string, so this is safe to
 * emit; whether it takes effect is "on supported terminals". */
static void
scr_set_clipboard(Screen *d, const char *utf8, size_t n)
{
	size_t b64cap = ((n + 2) / 3) * 4 + 1, b64len;
	char *b64;

	if (n == 0 || !d || !d->t)
		return;
	b64 = malloc(b64cap);
	if (!b64)
		return;
	b64len = b64_encode((const unsigned char *)utf8, n, b64);
	scr_str(d->t, "\033]52;c;");	/* OSC 52, clipboard "c" */
	scr_bytes(d->t, b64, b64len);
	scr_str(d->t, "\033\\");	/* ST */
	scr_flush(d->t);
	free(b64);
}

/* ---- input decoding ----------------------------------------------------- */

/* Translate a CSI modifier parameter (1 + bitmask) into TKBD_MOD_* flags. */
static uint8_t
csi_mods(int param)
{
	uint8_t m = 0;
	int bits = param - 1;

	if (bits & 1)
		m |= TKBD_MOD_SHIFT;
	if (bits & 2)
		m |= TKBD_MOD_ALT;
	if (bits & 4)
		m |= TKBD_MOD_CTRL;
	return m;
}

static void
seq_simple(struct tkbd_seq *seq, uint16_t key, uint8_t mod)
{
	seq->type = TKBD_KEY;
	seq->key = key;
	seq->mod = mod;
	seq->ch = TKBD_CH_NONE;
}

/* Try to decode one sequence from the front of buf[0,len). Returns the bytes
 * consumed and fills seq, 0 when the byte is unusable (skip it), or -1 when
 * more bytes are needed to complete a sequence. */
static int
tkbd_decode(struct tkbd_seq *seq, const unsigned char *buf, int len)
{
	memset(seq, 0, sizeof(*seq));
	seq->ch = TKBD_CH_NONE;
	if (len <= 0)
		return -1;

	if (buf[0] != 0x1B) {
		unsigned char b = buf[0];

		/* control keys */
		if (b == 0x09) {
			seq_simple(seq, TKBD_KEY_TAB, 0);
			return 1;
		}
		if (b == 0x0D || b == 0x0A) {
			seq_simple(seq, TKBD_KEY_ENTER, 0);
			return 1;
		}
		if (b == 0x08) {
			seq_simple(seq, TKBD_KEY_BACKSPACE, 0);
			return 1;
		}
		if (b == 0x7F) {
			seq_simple(seq, TKBD_KEY_BACKSPACE2, 0);
			return 1;
		}
		if (b < 0x20) {
			/* Ctrl + letter (Ctrl-A == 0x01) */
			seq_simple(seq, (uint16_t)(0x40 + b), TKBD_MOD_CTRL);
			return 1;
		}
		/* a printable character. Lowercase maps to the uppercase key
		 * code so letters never collide with the function-key codes,
		 * which share the 0x61-0x6d range; the glyph itself is carried
		 * in ch. */
		{
			uint32_t cp;
			int n = utf8_decode(&cp, buf, len);

			if (n <= 0)
				return -1;	/* need more bytes */
			if (cp == UTF8_RUNE_ERROR && n == 1 && (buf[0] & 0x80))
				return 1;	/* skip a stray byte via consume */
			seq->type = TKBD_KEY;
			seq->ch = cp;
			if (cp >= 'a' && cp <= 'z') {
				seq->key = (uint16_t)(TKBD_KEY_A + (cp - 'a'));
			} else if (cp >= '0' && cp <= '9') {
				seq->key = (uint16_t)cp;
			} else if (cp >= 'A' && cp <= 'Z') {
				seq->mod |= TKBD_MOD_SHIFT;
				seq->key = (uint16_t)cp;
			} else if (cp <= 0x7E) {
				seq->key = (uint16_t)cp;
				if (!strchr(" `-=[]\\;',./", (int)cp))
					seq->mod |= TKBD_MOD_SHIFT;
			} else {
				seq->key = TKBD_KEY_NONE;
			}
			return n;
		}
	}

	/* an escape sequence */
	if (len == 1)
		return -1;		/* lone ESC: wait for more or time out */

	/* Alt + key: ESC followed by a non-'[' non-'O' byte */
	if (buf[1] != '[' && buf[1] != 'O') {
		uint32_t cp;
		int n = utf8_decode(&cp, buf + 1, len - 1);

		if (n <= 0)
			return -1;
		seq->type = TKBD_KEY;
		seq->mod = TKBD_MOD_ALT;
		seq->ch = cp;
		seq->key = (cp < 128) ? (uint16_t)toupper((int)cp)
		    : TKBD_KEY_NONE;
		return 1 + n;
	}

	/* SS3: ESC O x  (application-mode arrows and F1-F4) */
	if (buf[1] == 'O') {
		if (len < 3)
			return -1;
		switch (buf[2]) {
		case 'A': seq_simple(seq, TKBD_KEY_UP, 0); return 3;
		case 'B': seq_simple(seq, TKBD_KEY_DOWN, 0); return 3;
		case 'C': seq_simple(seq, TKBD_KEY_RIGHT, 0); return 3;
		case 'D': seq_simple(seq, TKBD_KEY_LEFT, 0); return 3;
		case 'H': seq_simple(seq, TKBD_KEY_HOME, 0); return 3;
		case 'F': seq_simple(seq, TKBD_KEY_END, 0); return 3;
		case 'P': seq_simple(seq, TKBD_KEY_F1, 0); return 3;
		case 'Q': seq_simple(seq, TKBD_KEY_F2, 0); return 3;
		case 'R': seq_simple(seq, TKBD_KEY_F3, 0); return 3;
		case 'S': seq_simple(seq, TKBD_KEY_F4, 0); return 3;
		default: return 0;
		}
	}

	/* CSI: ESC [ ... final */
	{
		int i = 2;
		int params[4] = { 0, 0, 0, 0 };
		int np = 0, have = 0;
		unsigned char fin;
		uint8_t mod;

		while (i < len && buf[i] >= '0' && buf[i] <= '9') {
			have = 1;
			params[np < 4 ? np : 3] =
			    params[np < 4 ? np : 3] * 10 + (buf[i] - '0');
			i++;
			if (i < len && buf[i] == ';') {
				np++;
				i++;
				have = 0;
			}
		}
		if (have || np > 0)
			np++;
		if (i >= len)
			return -1;		/* incomplete */
		fin = buf[i];
		mod = (np >= 2) ? csi_mods(params[1]) : 0;

		switch (fin) {
		case 'A': seq_simple(seq, TKBD_KEY_UP, mod); return i + 1;
		case 'B': seq_simple(seq, TKBD_KEY_DOWN, mod); return i + 1;
		case 'C': seq_simple(seq, TKBD_KEY_RIGHT, mod); return i + 1;
		case 'D': seq_simple(seq, TKBD_KEY_LEFT, mod); return i + 1;
		case 'H': seq_simple(seq, TKBD_KEY_HOME, mod); return i + 1;
		case 'F': seq_simple(seq, TKBD_KEY_END, mod); return i + 1;
		/* Modified F1-F4 arrive as CSI 1;mod P..S (xterm). The
		 * unmodified forms come through SS3 above. */
		case 'P': seq_simple(seq, TKBD_KEY_F1, mod); return i + 1;
		case 'Q': seq_simple(seq, TKBD_KEY_F2, mod); return i + 1;
		case 'R': seq_simple(seq, TKBD_KEY_F3, mod); return i + 1;
		case 'S': seq_simple(seq, TKBD_KEY_F4, mod); return i + 1;
		case 'Z': seq_simple(seq, TKBD_KEY_TAB, TKBD_MOD_SHIFT);
			return i + 1;
		case '~':
			switch (params[0]) {
			case 1: seq_simple(seq, TKBD_KEY_HOME, mod); break;
			case 2: seq_simple(seq, TKBD_KEY_INS, mod); break;
			case 3: seq_simple(seq, TKBD_KEY_DEL, mod); break;
			case 4: seq_simple(seq, TKBD_KEY_END, mod); break;
			case 5: seq_simple(seq, TKBD_KEY_PGUP, mod); break;
			case 6: seq_simple(seq, TKBD_KEY_PGDN, mod); break;
			case 7: seq_simple(seq, TKBD_KEY_HOME, mod); break;
			case 8: seq_simple(seq, TKBD_KEY_END, mod); break;
			case 11: seq_simple(seq, TKBD_KEY_F1, mod); break;
			case 12: seq_simple(seq, TKBD_KEY_F2, mod); break;
			case 13: seq_simple(seq, TKBD_KEY_F3, mod); break;
			case 14: seq_simple(seq, TKBD_KEY_F4, mod); break;
			case 15: seq_simple(seq, TKBD_KEY_F5, mod); break;
			case 17: seq_simple(seq, TKBD_KEY_F6, mod); break;
			case 18: seq_simple(seq, TKBD_KEY_F7, mod); break;
			case 19: seq_simple(seq, TKBD_KEY_F8, mod); break;
			case 20: seq_simple(seq, TKBD_KEY_F9, mod); break;
			case 21: seq_simple(seq, TKBD_KEY_F10, mod); break;
			case 23: seq_simple(seq, TKBD_KEY_F11, mod); break;
			case 24: seq_simple(seq, TKBD_KEY_F12, mod); break;
			case 200: seq_simple(seq, TKBD_KEY_PASTE_BEGIN, 0); break;
			case 201: seq_simple(seq, TKBD_KEY_PASTE_END, 0); break;
			default: return i + 1;	/* consume unknown ~ seq */
			}
			return i + 1;
		default:
			return i + 1;		/* consume an unknown CSI */
		}
	}
}

/* Pull more raw bytes into the decode buffer. Returns 1 when bytes arrived,
 * 0 on timeout, -1 on EOF or error. */
static int
in_refill(Scrbuf *t, int timeout_ms)
{
	long r;

	if (t->inlen >= (int)sizeof(t->inbuf))
		return 1;		/* buffer full; decode what we have */
	if (t->io.poll) {
		int pr = t->io.poll(t->io.ctx, timeout_ms);

		if (pr == 0)
			return 0;
		if (pr < 0)
			return -1;
	}
	r = t->io.read(t->io.ctx, t->inbuf + t->inlen,
	    (long)(sizeof(t->inbuf) - t->inlen));
	if (r > 0) {
		t->inlen += (int)r;
		return 1;
	}
	if (r == 0)
		return t->io.poll ? -1 : 0;	/* EOF when poll said ready */
	return -1;
}

/* Append the raw bytes of one decoded event to the macro recording, when it is
 * on. A failed grow just drops bytes, so the recording truncates rather than
 * aborting the edit. */
static void
scr_rec_append(Scrbuf *t, const unsigned char *b, int n)
{
	if (!t->rec_on || n <= 0)
		return;
	if (t->rec_len + (size_t)n > t->rec_cap) {
		size_t nc = t->rec_cap ? t->rec_cap * 2 : 64;
		unsigned char *p;

		while (nc < t->rec_len + (size_t)n)
			nc *= 2;
		p = realloc(t->rec, nc);
		if (!p)
			return;
		t->rec = p;
		t->rec_cap = nc;
	}
	memcpy(t->rec + t->rec_len, b, (size_t)n);
	t->rec_len += (size_t)n;
}

/* Remove and return the next decoded event. Returns 1 when out is filled,
 * 0 on timeout with no event, or -1 on EOF or error. */
static int
scr_next_event(Screen *d, int timeout_ms, struct tkbd_seq *out)
{
	Scrbuf *t = d->t;

	for (;;) {
		if (t->inlen > 0) {
			int n = tkbd_decode(out, t->inbuf, t->inlen);

			if (n > 0) {
				scr_rec_append(t, t->inbuf, n);
				memmove(t->inbuf, t->inbuf + n, t->inlen - n);
				t->inlen -= n;
				return 1;
			}
			if (n == 0) {
				/* unusable lead byte: drop it and retry */
				memmove(t->inbuf, t->inbuf + 1, t->inlen - 1);
				t->inlen--;
				continue;
			}
			/* n < 0: incomplete. If it is a lone ESC, give a brief
			 * grace for the rest, then treat it as the ESC key. */
			if (t->inbuf[0] == 0x1B && t->inlen >= 1) {
				int got = in_refill(t, 50);

				if (got == 1)
					continue;
				if (got <= 0) {
					out->type = TKBD_KEY;
					out->key = TKBD_KEY_ESC;
					out->mod = 0;
					out->ch = TKBD_CH_NONE;
					scr_rec_append(t, t->inbuf, 1);
					memmove(t->inbuf, t->inbuf + 1,
					    t->inlen - 1);
					t->inlen--;
					return 1;
				}
			}
		}
		{
			int got = in_refill(t, timeout_ms);

			if (got == 0)
				return 0;
			if (got < 0)
				return t->inlen > 0 ? 1 : -1;
		}
	}
}

/* Wait for the next event, folding in resize handling. */
static int
scr_wait(Screen *d, Event *ev)
{
	Scrbuf *t = d->t;

	for (;;) {
		int rc;

		if (g_winch || t->want_resize) {
			int rows = t->rows, cols = t->cols;

			g_winch = 0;
			t->want_resize = 0;
			if (t->io.getsize &&
			    t->io.getsize(t->io.ctx, &rows, &cols) == 0) {
				if (rows >= 1 && cols >= 1 &&
				    (rows != t->rows || cols != t->cols))
					scr_resize(d, rows, cols);
			}
			ev->type = EVENT_RESIZE;
			return EVENT_RESIZE;
		}
		rc = scr_next_event(d, 200, &ev->key);
		if (rc == 1) {
			ev->type = EVENT_KEY;
			return EVENT_KEY;
		}
		if (rc < 0) {
			ev->type = EVENT_EOF;
			return EVENT_EOF;
		}
		/* rc == 0: input is quiet. Surface an idle tick so the main loop
		 * can refresh the swap file; nested modal loops ignore it. */
		ev->type = EVENT_IDLE;
		return EVENT_IDLE;
	}
}

/* Begin capturing raw input bytes for a vi macro recording (discarding any
 * previous capture). Every event decoded by scr_next_event is appended until
 * scr_record_stop. */
static void
scr_record_start(Screen *d)
{
	d->t->rec_len = 0;
	d->t->rec_on = 1;
}

/* Stop capturing and return the recorded bytes, setting *len. The buffer stays
 * owned by the screen; the caller copies what it needs before the next event. */
static const unsigned char *
scr_record_stop(Screen *d, size_t *len)
{
	d->t->rec_on = 0;
	*len = d->t->rec_len;
	return d->t->rec;
}

/****************************************************************
 * Editor core types
 ****************************************************************/
/* The editor core shared between the modeless and vi personalities.
 *
 * The modeless personality holds the buffer, rendering, chrome, menus,
 * dialogs, and editing; the vi personality is layered on top. These types are
 * the state both share (Editor and its enums), the core helpers the vi code
 * calls, and the vi entry points the modeless code calls. */



/* Mark slots. 'a'..'z' occupy 0..25; the rest are special read-only marks that
 * commands set automatically. mark_index() maps a mark character to a slot. */
enum {
	MARK_LETTERS = 26,		/* 'a'..'z' -> 0..25 */
	MARK_PREV = MARK_LETTERS,	/* ` and ' : position before the last jump */
	MARK_CHANGE,			/* . : last change */
	MARK_INSERT,			/* ^ : where insert mode last stopped */
	MARK_VISLT,			/* < : start of the last visual selection */
	MARK_VISGT,			/* > : end of the last visual selection */
	MARK_SLOTS
};

/* A yank/delete register: owned bytes, their length, and whether the content
 * is whole lines (put restores it as new lines). */
typedef struct vi_reg {
	char	*bytes;
	size_t	len;
	int	linewise;
} Reg;

/* A recorded key sequence, used by the '.' repeat to replay the last change. */
typedef struct vi_keylog {
	struct tkbd_seq	*ev;
	int		len;
	int		cap;
} Keylog;

/* One open file. The editor keeps a list of these; the active buffer's fields
 * are mirrored into the flat Editor for editing and copied back here on
 * a switch. Only genuinely per-file state lives here -- the draw surface,
 * chrome, and global vi state (registers, the dot register, the clipboard, the
 * last search) stay in Editor and are shared across all buffers. */
typedef struct text Text;
typedef struct ebuf {
	Text	*t;
	char		path[PATH_MAX];
	int		has_name;
	size_t		cy, cx, top, left;
	int		sel_active;
	size_t		ay, ax;
	const Syntax *syn;
	uint16_t	*line_state;
	size_t		line_state_cap;
	size_t		hl_valid;
	int		hex_view;
	size_t		hex_top;
	int		expand_tabs;	/* indent with spaces in this buffer */
	size_t		vi_mark_y[MARK_SLOTS];
	size_t		vi_mark_x[MARK_SLOTS];
	uint64_t	vi_marks_set;
	char		swap_path[PATH_MAX];	/* this buffer's swap file, or "" */
	int		swap_on;	/* a swap file exists on disk for it */
	size_t		swap_rev;	/* text rev at the last swap write */
	time_t		load_mtime;	/* file mtime at load (swap staleness check) */
} Buf;

/* Referenced only by pointer here; the users include the real headers. */
struct tkbd_seq;

#define TAB_WIDTH 8

/* Fixed-size text buffers carried on the Editor struct. */
#define STATUS_MAX	160	/* transient status/message line */
#define FIND_MAX	256	/* last search and replacement strings */
#define TOOLTITLE_MAX	64	/* label of the last build/shell command */
#define HEXPAT_MAX	64	/* last searched byte pattern (hex view) */

/* Editing personality. The default is a modeless (nano-style) editor;
 * MODE_NORMAL/MODE_INSERT are the vi personality, toggled with F2. */
typedef enum edit_mode {
	MODE_MODELESS,		/* value 0, so a zeroed editor starts modeless */
	MODE_NORMAL,		/* vi command mode */
	MODE_INSERT,		/* vi insert mode */
} Mode;

/* Marker stored in e->vi_visual for a Ctrl-V block selection (the control code
 * Ctrl-V itself), alongside 'v' charwise and 'V' linewise. */
#define VI_VBLOCK 0x16

/* What the main loop must do after a command; save and quit may prompt, so
 * they are carried out there where the input stream is available. */
typedef enum req {
	REQ_CONTINUE,
	REQ_FIND,
	REQ_REPLACE,
	REQ_GOTO,
	REQ_HELP,
	REQ_SAVE,
	REQ_QUIT,		/* modeless quit: prompts if the buffer is dirty */
	REQ_VI_COLON,		/* vi ':' ex command line */
	REQ_VI_SEARCH,		/* vi '/' search prompt */
	REQ_FORCE_QUIT,		/* vi decided quitting is allowed: leave now */
	REQ_QUIT_ERR,		/* vi ':cq' -- leave with a nonzero exit code */
} Req;

#ifndef VEDIT_NO_TOOLS
/* Diagnostic severity, ascending so a higher value is more severe. An
 * unlabeled "file:line: msg" (a linker line, a custom tool) counts as an
 * error, so it is still a navigation stop. */
enum toolsev { TSEV_NOTE, TSEV_WARN, TSEV_ERROR };

/* A parsed compiler diagnostic: where it points, and which captured output
 * line it came from (so the pane can highlight that line). */
typedef struct toolerr {
	char	file[PATH_MAX];
	size_t	line, col;		/* 1-based; col is 0 when absent */
	int	outline;		/* index into the captured output lines */
	int	sev;			/* enum toolsev */
} Toolerr;
#endif

/* One saved location on the tag stack: where a tag jump started, so a pop can
 * return there. Only named buffers are recorded (a pop reopens by path). */
typedef struct tagloc {
	char	path[PATH_MAX];
	size_t	cy, cx;
} Tagloc;

typedef struct editor {
	Buf	*bufs;		/* open buffers; active mirrors into flat */
	int		nbuf;		/* number of open buffers */
	int		bufs_cap;	/* allocated slots in bufs */
	int		cur;		/* index of the active buffer */
	Text	*t;
	char		path[PATH_MAX];
	int		has_name;
	size_t		cy;		/* cursor line */
	size_t		cx;		/* cursor byte offset within the line */
	size_t		top;		/* first visible line */
	size_t		left;		/* horizontal scroll, display columns */
	size_t		prev_top;	/* e->top at the last text-view paint */
	size_t		prev_left;	/* e->left at the last text-view paint */
	int		prev_text_view;	/* the last paint was the scrollable text */
	int		rows;
	int		cols;
	Screen	*d;		/* drawing surface and input source */
	Scrbuf *term;		/* terminal driver (box mode, resize) */
	int		sel_active;	/* a selection is being extended */
	int		sel_block;	/* the selection is a rectangle (draw mode) */
	size_t		ay;		/* selection anchor line */
	size_t		ax;		/* selection anchor byte column */
	char		*clip;		/* internal clipboard bytes */
	size_t		clip_len;	/* length of clip in bytes */
	int		clip_linewise;	/* clip holds whole lines (vi p/P) */
	int		clip_block;	/* clip holds a rectangle (draw mode) */
	int		clip_osc52;	/* mirror every copy/yank through OSC 52 */
#ifndef VEDIT_NO_TOOLS
	const struct vedit_tool_api *tools;	/* borrowed; NULL = no building */
	char	**tool_lines;		/* captured output, one line per entry */
	int		tool_nlines, tool_lines_cap;
	char	*tool_raw;		/* byte accumulator during a capture */
	size_t		tool_rawlen, tool_rawcap;
	Toolerr	*tool_errs;		/* parsed diagnostics */
	int		tool_nerr, tool_errs_cap;
	int		tool_curerr;		/* selected error (F4/Shift+F4), -1 */
	char	tool_title[TOOLTITLE_MAX];	/* label of the last command, for the pane */
	char	tool_dir[PATH_MAX];	/* directory the last command ran in */
#endif
	int		draw_mode;	/* 2D/block draw mode: free cursor + overtype */
	char		last_find[FIND_MAX];	/* last search string, for repeat */
	char		last_replace[FIND_MAX]; /* last replacement string */
	int		vi_search_dir;	/* last search direction: 1 fwd, -1 back */
	int		search_icase;	/* match searches case-insensitively */
	int		vi_want_col;	/* display column j/k aim for (INT_MAX=EOL) */
	int		vi_vert_run;	/* this command was a vertical j/k/$ move */
	int		vi_vert_prev;	/* the previous command was one */
	char		status[STATUS_MAX];
	int		scheme;		/* chrome color scheme (SCHEME_*) */
	int		show_lineno;	/* draw the line-number gutter */
	int		wrap;		/* soft-wrap long lines to the window width */
	int		show_tabs;	/* draw a guide glyph at each hard tab */
	int		auto_indent;	/* a new line copies the previous indent */
	int		expand_tabs;	/* Tab and auto-indent use spaces (per buffer) */
	int		shiftwidth;	/* >> / << shift size in columns; 0 = a tab stop */
	int		swap_enabled;	/* write .swp crash-recovery snapshots */
	int		backup_enabled;	/* keep the previous version on save */
	char		swap_path[PATH_MAX];	/* active buffer's swap file, or "" */
	int		swap_on;	/* a swap file exists on disk for it */
	size_t		swap_rev;	/* text rev at the last swap write */
	time_t		load_mtime;	/* file mtime at load (swap staleness check) */
	Tagloc		*tagstack;	/* positions to return to after tag jumps */
	int		tag_sp, tag_cap;	/* stack depth and capacity */
	Tagloc		*jumps;		/* jump list for Ctrl-O / Ctrl-I and `` / '' */
	int		jump_n, jump_cap;	/* entries and capacity */
	int		jump_cur;	/* position in the list; == jump_n means live */
	int		hex_view;	/* render the buffer as a hex dump */
	size_t		hex_top;	/* first visible hex row (byte offset >> 4) */
	int		hex_ascii;	/* editing the ascii column, not the hex */
	int		hex_pending;	/* a typed high nibble 0-15, or -1 */
	int		hex_insert;	/* insert bytes instead of overwriting */
	unsigned char	hex_pat[HEXPAT_MAX];	/* last searched byte pattern */
	size_t		hex_pat_len;	/* its length, 0 when none searched yet */
	int		hex_pat_dir;	/* last search direction: 1 fwd, -1 back */
	int		hex_cols;	/* dump bytes per row: 8, 16, or 32 */
	int		hex_inspect;	/* show the data-inspector footer line */
	int		hex_sel;	/* a byte selection is being extended */
	size_t		hex_anchor;	/* byte offset the selection anchors at */
	/* syntax highlighting */
	const Syntax *syn;	/* language, or NULL when none */
	int		hl_on;		/* highlighting enabled */
	uint16_t	*line_state;	/* tokenizer state at each line's start */
	size_t		line_state_cap;
	size_t		hl_valid;	/* line_state[0..hl_valid) are current */
	uint8_t		*hl_buf;	/* scratch styles for one rendered line */
	size_t		hl_buf_cap;

	/* vi personality state */
	Mode	mode;
	char		vi_visual;	/* 0, 'v' charwise, or 'V' linewise */
	char		vi_last_vis;	/* kind of the last selection, for gv (0 none) */
	size_t		vi_lv_ay, vi_lv_ax;	/* its anchor */
	size_t		vi_lv_cy, vi_lv_cx;	/* its cursor end */
	int		vi_count;	/* pending motion count, 0 = none */
	char		vi_op;		/* pending operator: 0, 'd', 'c', 'y' */
	int		vi_op_count;	/* count typed before the operator */
	int		vi_gpending;	/* a leading 'g' is awaiting its pair */
	char		vi_charsearch;	/* f/F/t/T awaiting its target char */
	char		vi_textobj;	/* i/a awaiting a text-object char */
	char		vi_markcmd;	/* m/`/' awaiting its mark letter */
	char		vi_mark_norec;	/* the pending mark jump is g`/g': no jump */
	char		vi_rpending;	/* r typed, awaiting the new character */
	char		vi_atpending;	/* @ typed, awaiting the register name */
	char		vi_last_macro;	/* last register played with @, for @@ */
	int		vi_macro_depth;	/* @ recursion guard */
	char		vi_qpending;	/* q typed, awaiting the record register */
	char		vi_recording;	/* register being recorded (a-z), 0 = none */
	int		vi_rec_append;	/* recording was armed with A-Z: append */
	int		vi_overtype;	/* R Replace mode: typing overwrites */
	size_t		vi_mark_y[MARK_SLOTS];	/* line of each mark slot */
	size_t		vi_mark_x[MARK_SLOTS];	/* byte column of the mark */
	uint64_t	vi_marks_set;	/* bit i set: mark slot i is defined */
	Reg	vi_regs[26];	/* named registers "a..z */
	char		vi_reg;		/* selected register a-z/A-Z, 0 = none */
	char		vi_reg_fresh;	/* vi_reg was just armed by " */
	char		vi_regpending;	/* " typed, awaiting the register name */
	Keylog vi_dot;	/* keys of the last change, for '.' */
	Keylog vi_rec;	/* the command being recorded now */
	int		vi_replaying;	/* replaying '.': do not record */
	int		vi_cmd_open;	/* a command is mid-record */
	int		vi_suppress_dot;/* this command must not become '.' */
	size_t		vi_cmd_rev;	/* text revision when it started */
	char		vi_last_fT;	/* last f/F/t/T, for ; and , */
	uint32_t	vi_last_fT_ch;	/* the target char it searched for */
	int		vi_zpending;	/* a leading 'Z' is awaiting its pair */
	/* visual-block (Ctrl-V) insert/append in progress */
	int		vi_block_insert;	/* a block I/A insert is active */
	int		vi_bi_append;	/* that insert appends (A) vs inserts (I) */
	size_t		vi_bi_col;	/* block column the lower rows receive */
	size_t		vi_bi_start;	/* column the top-row insert began at */
	size_t		vi_bi_y1, vi_bi_y2;	/* block row range to replicate over */

	/* runtime config reload (set by the standalone CLI) */
	char		cfg_path[PATH_MAX];	/* config file to re-read, or "" */
	Cfg		*cfg_owned;	/* a config this editor reloaded and owns */
} Editor;

/* Set the one-line status message (printf-style). The single choke point for
 * e->status, so every message is bounded by its size the same way. */
static void set_status(Editor *e, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void
set_status(Editor *e, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(e->status, sizeof(e->status), fmt, ap);
	va_end(ap);
}

/* Core helpers the vi personality relies on, defined in edit.c. */
int text_height(const Editor *e);
int disp_cols(const char *s, size_t nbytes);
size_t rune_len_at(const char *s, size_t len, size_t cx);
size_t prev_rune_len(const char *s, size_t cx);
void clamp_col(Editor *e);
void move_left(Editor *e);
void move_right(Editor *e);
void hl_touch(Editor *e, size_t line);
void ed_insert(Editor *e, const char *bytes, size_t n);
void ed_newline(Editor *e);
void ed_delete(Editor *e);
void ed_backspace(Editor *e);
void ed_find(Editor *e, const char *q);
void ed_find_dir(Editor *e, const char *q, int dir);

/* Multi-buffer management (edit.c). The active buffer's per-file state lives
 * in the flat struct editor; these swap it with the saved buffers. */
int buf_open(Editor *e, const char *path);	/* open/switch; index or -1 */
void buf_switch(Editor *e, int i);
int buf_cycle(Editor *e, int dir);		/* next/prev; new index */
int buf_close(Editor *e, int i);			/* 0 ok, -1 refused */
void buf_list(Editor *e);			/* summarize into status */
static int dlg_save_file(Editor *e, char *out, size_t outsz);
static void dlg_symbol_pick(Editor *e);
void insert_clip(Editor *e);
void insert_bytes(Editor *e, const char *bytes, size_t len);
char *region_text(Editor *e, size_t y1, size_t x1, size_t y2, size_t x2,
    size_t *outlen);
void delete_region(Editor *e, size_t y1, size_t x1, size_t y2,
    size_t x2);
void clip_set(Editor *e, char *bytes, size_t len);
int dlg_prompt_line(Editor *e, const char *q, char *buf, size_t bufsz);

/* Hex view helpers, defined in hex.c. The byte stream is the buffer's line
 * bytes joined by an implied newline (a trailing one only when the content
 * ends with a newline), so these operate over the same Text as the
 * text view. */
size_t hex_total(const Text *t);
size_t hex_offset_of(const Text *t, size_t cy, size_t cx);
void hex_pos_at(const Text *t, size_t off, size_t *cy, size_t *cx);
size_t hex_gather(const Text *t, size_t start, unsigned char *buf,
    size_t count);
void hex_format_row(char *out, size_t out_sz, size_t offset,
    const unsigned char *buf, size_t n, int cols);
int hex_hexcol(int j);		/* row column of byte j's hex pair */
int hex_ascii_start(int cols);	/* row column where the ascii gutter begins */
int hex_asciicol(int j, int cols);	/* row column of byte j's ascii cell */
int hex_row_width(int cols);	/* display width of a cols-wide dump row */
int hex_parse_bytes(const char *s, unsigned char *out, size_t max,
    size_t *outlen);		/* parse "de ad be ef" hex pairs; 0 ok, -1 bad */
int hex_find(const Text *t, const unsigned char *pat, size_t plen,
    size_t from, int dir, size_t *found);	/* wrapping byte search; 1 = hit */
void hex_inspect_line(char *out, size_t out_sz, const unsigned char *b,
    size_t n);				/* decode up to 4 bytes at the cursor */

/* The vi personality, defined in vi.c. */
Req vi_dispatch(Editor *e, const struct tkbd_seq *seq);
Req vi_colon(Editor *e);
void vi_search(Editor *e);
void vi_clamp(Editor *e);
void vi_reset_pending(Editor *e);
size_t vi_col_to_byte(Editor *e, size_t y, int target_col);


/****************************************************************
 * Text buffer
 ****************************************************************/
/* An editable text buffer with a line index. */



#define OK	0
#define ERR	(-1)

typedef struct estack Estack;
static void estack_clear(Estack *s);
static void estack_free(Estack *s);

typedef struct line {
	char	*buf;		/* NUL-terminated line bytes, no newline */
	size_t	len;		/* bytes excluding the NUL */
	size_t	cap;		/* allocated bytes including room for NUL */
} Line;

/* A reversible primitive. Applying one mutates the buffer and yields the
 * primitive that reverses it, which is how undo and redo stay symmetric. */
typedef enum eop {
	OP_INSERT,		/* insert bytes at (line, col) */
	OP_DELETE,		/* delete n bytes at (line, col) */
	OP_SPLIT,		/* split line at col */
	OP_JOIN,		/* join line with the one after it */
} Eop;

typedef struct erec {
	Eop	op;
	size_t		line;
	size_t		col;
	char		*bytes;		/* owned; the payload for OP_INSERT */
	size_t		n;		/* byte count for INSERT and DELETE */
	unsigned	group;		/* group id; 0 = a standalone step */
} Erec;

struct estack {
	Erec	*v;
	size_t		n;
	size_t		cap;
};

/* Line-ending style: how lines are separated on disk. The in-memory lines
 * never hold the terminator; load splits it off and save writes it back. */
enum eol {
	EOL_LF,			/* "\n" (Unix) */
	EOL_CRLF,		/* "\r\n" (DOS) */
	EOL_NUL,		/* "\0" (NUL-separated records) */
};

struct text {
	Line	*lines;
	size_t		nlines;
	size_t		cap;
	int		final_newline;	/* source ended with a line terminator */
	int		eol;		/* enum eol: the line-ending style */
	int		dirty;
	size_t		rev;		/* bumped on every primitive mutation */

	Estack	undo;
	Estack	redo;
	int		can_coalesce;	/* a typing run may extend the top */

	/* Undo grouping: while a group is open every recorded primitive is
	 * tagged with cur_group, and undo/redo replay the whole run as one
	 * step. group_depth counts nested begin/end pairs; group_seq hands
	 * out fresh ids. */
	unsigned	cur_group;
	unsigned	group_depth;
	unsigned	group_seq;
};

/****************************************************************
 * Growable storage
 ****************************************************************/

static int
line_reserve(Line *l, size_t need)
{
	/* need is bytes excluding the NUL */
	if (need + 1 > l->cap) {
		size_t cap = l->cap ? l->cap : 16;
		char *p;

		while (cap < need + 1)
			cap *= 2;
		p = realloc(l->buf, cap);
		if (!p)
			return ERR;
		l->buf = p;
		l->cap = cap;
	}
	return OK;
}

static int
line_init(Line *l, const char *s, size_t n)
{
	l->buf = NULL;
	l->len = 0;
	l->cap = 0;
	if (line_reserve(l, n) != OK)
		return ERR;
	if (n)
		memcpy(l->buf, s, n);
	l->buf[n] = '\0';
	l->len = n;
	return OK;
}

static int
lines_reserve(Text *t, size_t need)
{
	if (need > t->cap) {
		size_t cap = t->cap ? t->cap : 32;
		Line *p;

		while (cap < need)
			cap *= 2;
		p = realloc(t->lines, cap * sizeof(*p));
		if (!p)
			return ERR;
		t->lines = p;
		t->cap = cap;
	}
	return OK;
}

/* Insert a fresh line initialized from s[0..n) at index idx. */
static int
lines_insert_at(Text *t, size_t idx, const char *s, size_t n)
{
	if (idx > t->nlines)
		return ERR;
	if (lines_reserve(t, t->nlines + 1) != OK)
		return ERR;
	if (line_init(&t->lines[t->nlines], s, n) != OK)
		return ERR;
	/* the new line was built at the end; rotate it into place */
	if (idx < t->nlines) {
		Line tmp = t->lines[t->nlines];

		memmove(&t->lines[idx + 1], &t->lines[idx],
		    (t->nlines - idx) * sizeof(Line));
		t->lines[idx] = tmp;
	}
	t->nlines++;
	return OK;
}

static void
lines_remove_at(Text *t, size_t idx)
{
	if (idx >= t->nlines)
		return;
	free(t->lines[idx].buf);
	memmove(&t->lines[idx], &t->lines[idx + 1],
	    (t->nlines - idx - 1) * sizeof(Line));
	t->nlines--;
}

static void
text_clear(Text *t)
{
	size_t i;

	for (i = 0; i < t->nlines; i++)
		free(t->lines[i].buf);
	t->nlines = 0;
}

/****************************************************************
 * Lifecycle
 ****************************************************************/

Text *
text_new(void)
{
	Text *t = calloc(1, sizeof(*t));

	if (!t)
		return NULL;
	if (lines_insert_at(t, 0, "", 0) != OK) {
		free(t->lines);
		free(t);
		return NULL;
	}
	t->final_newline = 0;
	t->eol = EOL_LF;		/* a new buffer defaults to Unix line endings */
	t->dirty = 0;
	return t;
}

void
text_free(Text *t)
{
	if (!t)
		return;
	text_clear(t);
	estack_free(&t->undo);
	estack_free(&t->redo);
	free(t->lines);
	free(t);
}

/****************************************************************
 * Load and save
 ****************************************************************/

/* Read a whole stream into t, replacing its contents and resetting history.
 * The caller owns fp (this never closes it); fp must be positioned at the
 * start of the bytes to load. Detects the line-ending style from the bytes,
 * so it serves both a real file and a recovered swap body. Returns OK, or ERR
 * with errno set on a read or allocation failure. */
int
text_load_fp(Text *t, FILE *fp)
{
	char *data = NULL;
	size_t len = 0, cap = 0;
	int c;
	size_t start, i;
	int saved_errno;

	while ((c = fgetc(fp)) != EOF) {
		if (len + 1 > cap) {
			size_t ncap = cap ? cap * 2 : 4096;
			char *p = realloc(data, ncap);

			if (!p) {
				saved_errno = errno;
				free(data);
				errno = saved_errno;
				return ERR;
			}
			data = p;
			cap = ncap;
		}
		data[len++] = (char)c;
	}
	if (ferror(fp)) {
		saved_errno = errno;
		free(data);
		errno = saved_errno;
		return ERR;
	}

	text_clear(t);
	estack_clear(&t->undo);		/* history does not span a reload */
	estack_clear(&t->redo);
	t->can_coalesce = 0;
	t->group_depth = 0;
	t->cur_group = 0;

	/* Detect the line-ending style: a NUL byte means NUL-separated records,
	 * otherwise a "\r\n" before the first newline means DOS, else Unix. The
	 * separator is one byte ('\n' or '\0'); the trailing '\r' of a DOS line
	 * is stripped from each line below. */
	t->eol = EOL_LF;
	{
		char sep = '\n';

		for (i = 0; i < len; i++) {
			if (data[i] == '\0') {
				t->eol = EOL_NUL;
				sep = '\0';
				break;
			}
			if (data[i] == '\n') {
				if (i > 0 && data[i - 1] == '\r')
					t->eol = EOL_CRLF;
				break;
			}
		}
		t->final_newline = (len > 0 && data[len - 1] == sep);

		start = 0;
		for (i = 0; i < len; i++) {
			size_t llen;

			if (data[i] != sep)
				continue;
			llen = i - start;
			if (t->eol == EOL_CRLF && llen > 0 &&
			    data[start + llen - 1] == '\r')
				llen--;		/* drop the DOS line's trailing CR */
			if (lines_insert_at(t, t->nlines, data + start, llen)
			    != OK) {
				free(data);
				errno = ENOMEM;
				return ERR;
			}
			start = i + 1;
		}
		/* trailing bytes with no terminator form a final line */
		if (start < len) {
			size_t llen = len - start;

			if (t->eol == EOL_CRLF && data[start + llen - 1] == '\r')
				llen--;
			if (lines_insert_at(t, t->nlines, data + start, llen)
			    != OK) {
				free(data);
				errno = ENOMEM;
				return ERR;
			}
		}
	}
	free(data);

	if (t->nlines == 0 &&
	    lines_insert_at(t, 0, "", 0) != OK) {	/* empty file: one line */
		errno = ENOMEM;			/* keep the >=1 line invariant */
		return ERR;
	}
	t->dirty = 0;
	return OK;
}

/* Load a file by path into t. Opens it read-only and delegates to
 * text_load_fp. Returns OK, or ERR with errno set (ENOENT for a missing file,
 * which callers treat as a new buffer). */
int
text_load(Text *t, const char *path)
{
	FILE *fp = fopen(path, "rb");
	int rc, saved_errno;

	if (!fp)
		return ERR;
	rc = text_load_fp(t, fp);
	saved_errno = errno;
	fclose(fp);
	errno = saved_errno;
	return rc;
}

/* Write every line of t to fp, with the terminator chosen from t->eol. The
 * trailing terminator is emitted only when the source carried one. The caller
 * owns fp. Returns OK, or ERR with errno set on a write failure. */
int
text_write_fp(const Text *t, FILE *fp)
{
	size_t i;
	const char *term = (t->eol == EOL_CRLF) ? "\r\n" :
	    (t->eol == EOL_NUL) ? "\0" : "\n";
	size_t termlen = (t->eol == EOL_CRLF) ? 2 : 1;

	for (i = 0; i < t->nlines; i++) {
		if (t->lines[i].len &&
		    fwrite(t->lines[i].buf, 1, t->lines[i].len, fp)
		    != t->lines[i].len)
			return ERR;
		/* a terminator between lines, and after the last only when the
		 * source carried a trailing terminator */
		if (i + 1 < t->nlines || t->final_newline) {
			if (fwrite(term, 1, termlen, fp) != termlen)
				return ERR;
		}
	}
	return OK;
}

/* Save t to path atomically: write the bytes to a temporary file in the same
 * directory, flush them to disk, then rename it over the target so a crash or
 * a full disk never leaves the file half-written. The target's permissions are
 * preserved when it already exists. Returns OK, or ERR with errno set. */
int
text_save(Text *t, const char *path)
{
	char tmp[PATH_MAX];
	const char *slash = strrchr(path, '/');
	int fd, saved_errno;
	FILE *fp;
	struct stat st;
	mode_t mode;

	/* The temp file must share the target's directory so the rename stays
	 * on one filesystem (an atomic replace, never an EXDEV copy). */
	if (slash) {
		int dlen = (int)(slash - path);

		if (snprintf(tmp, sizeof(tmp), "%.*s/.vedit-save-XXXXXX",
		    dlen, path) >= (int)sizeof(tmp)) {
			errno = ENAMETOOLONG;
			return ERR;
		}
	} else if (snprintf(tmp, sizeof(tmp), ".vedit-save-XXXXXX") >=
	    (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return ERR;
	}
	fd = mkstemp(tmp);
	if (fd < 0) {
		/* The directory is not writable (so no sibling temp is possible),
		 * but the file itself might be. Fall back to a direct, in-place
		 * write: not atomic, but it still saves the common case of a
		 * writable file in a read-only directory. */
		fp = fopen(path, "wb");
		if (!fp)
			return ERR;
		if (text_write_fp(t, fp) != OK) {
			saved_errno = errno;
			fclose(fp);
			errno = saved_errno;
			return ERR;
		}
		if (fclose(fp) != 0)
			return ERR;
		t->dirty = 0;
		return OK;
	}
	fp = fdopen(fd, "wb");
	if (!fp) {
		saved_errno = errno;
		close(fd);
		goto fail;
	}
	if (text_write_fp(t, fp) != OK) {
		saved_errno = errno;
		fclose(fp);
		goto fail;
	}
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
		saved_errno = errno;
		fclose(fp);
		goto fail;
	}
	if (fclose(fp) != 0) {
		saved_errno = errno;
		goto fail;
	}
	/* Keep the existing file's permission bits; mkstemp made tmp 0600. */
	if (stat(path, &st) == 0)
		mode = st.st_mode & 07777;
	else {
		mode_t um = umask(0);

		umask(um);
		mode = 0666 & ~um;
	}
	(void)chmod(tmp, mode);
	if (rename(tmp, path) != 0) {
		saved_errno = errno;
		goto fail;
	}
	t->dirty = 0;
	return OK;

fail:
	unlink(tmp);
	errno = saved_errno;
	return ERR;
}

/****************************************************************
 * Queries
 ****************************************************************/

size_t
text_lines(const Text *t)
{
	return t->nlines;
}

const char *
text_line(const Text *t, size_t line, size_t *len)
{
	if (line >= t->nlines)
		return NULL;
	if (len)
		*len = t->lines[line].len;
	return t->lines[line].buf;
}

size_t
text_line_len(const Text *t, size_t line)
{
	if (line >= t->nlines)
		return 0;
	return t->lines[line].len;
}

int
text_dirty(const Text *t)
{
	return t->dirty;
}

int
text_final_newline(const Text *t)
{
	return t->final_newline;
}

/* Whether a fresh buffer of language `lang` indents with spaces. Resolved from
 * the config: a per-language `indent.<lang>.expand`, then a global
 * `indent.expand`, else off (hard tabs). lang may be NULL. */
static int
indent_expand_default(const char *lang)
{
	char key[128];

	if (lang && lang[0] && g_cfg) {
		snprintf(key, sizeof(key), "indent.%s.expand", lang);
		if (cfg_get(g_cfg, key))
			return cfg_bool(g_cfg, key, 0);
	}
	return cfg_bool(g_cfg, "indent.expand", 0);
}

/* The buffer's line-ending style (enum eol). */
int
text_eol(const Text *t)
{
	return t->eol;
}

/* Set the line-ending style. Marks the buffer dirty when it changes, since the
 * bytes written to disk will differ. */
void
text_set_eol(Text *t, int eol)
{
	if (eol != t->eol) {
		t->eol = eol;
		t->dirty = 1;
	}
}

/* A short name for a line-ending style, for the status bar and messages. */
static const char *
eol_name(int eol)
{
	switch (eol) {
	case EOL_CRLF:	return "CRLF";
	case EOL_NUL:	return "NUL";
	default:	return "LF";
	}
}

size_t
text_revision(const Text *t)
{
	return t->rev;
}

/****************************************************************
 * Undo primitives
 ****************************************************************/

static void
estack_clear(Estack *s)
{
	size_t i;

	for (i = 0; i < s->n; i++)
		free(s->v[i].bytes);
	s->n = 0;
}

static void
estack_free(Estack *s)
{
	estack_clear(s);
	free(s->v);
	s->v = NULL;
	s->cap = 0;
}

static int
estack_push(Estack *s, const Erec *rec)
{
	if (s->n >= s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 32;
		Erec *p = realloc(s->v, cap * sizeof(*p));

		if (!p)
			return ERR;
		s->v = p;
		s->cap = cap;
	}
	s->v[s->n++] = *rec;
	return OK;
}

/* Apply one primitive to the buffer and fill inv with the primitive that
 * reverses it. inv->bytes, when set, is owned by the caller. */
static int
apply_op(Text *t, const Erec *in, Erec *inv)
{
	inv->bytes = NULL;
	inv->n = 0;
	inv->col = 0;
	inv->group = 0;

	switch (in->op) {
	case OP_INSERT: {
		Line *l;

		if (in->line >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		if (in->col > l->len)
			return ERR;
		if (line_reserve(l, l->len + in->n) != OK)
			return ERR;
		memmove(l->buf + in->col + in->n, l->buf + in->col,
		    l->len - in->col);
		memcpy(l->buf + in->col, in->bytes, in->n);
		l->len += in->n;
		l->buf[l->len] = '\0';
		inv->op = OP_DELETE;
		inv->line = in->line;
		inv->col = in->col;
		inv->n = in->n;
		break;
	}
	case OP_DELETE: {
		Line *l;
		size_t nn;
		char *cap;

		if (in->line >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		if (in->col > l->len)
			return ERR;
		nn = in->n;
		if (nn > l->len - in->col)
			nn = l->len - in->col;
		cap = malloc(nn ? nn : 1);
		if (!cap)
			return ERR;
		memcpy(cap, l->buf + in->col, nn);
		memmove(l->buf + in->col, l->buf + in->col + nn,
		    l->len - in->col - nn);
		l->len -= nn;
		l->buf[l->len] = '\0';
		inv->op = OP_INSERT;
		inv->line = in->line;
		inv->col = in->col;
		inv->n = nn;
		inv->bytes = cap;
		break;
	}
	case OP_SPLIT: {
		Line *l;

		if (in->line >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		if (in->col > l->len)
			return ERR;
		if (lines_insert_at(t, in->line + 1, l->buf + in->col,
		    l->len - in->col) != OK)
			return ERR;
		l = &t->lines[in->line];	/* array may have moved */
		l->len = in->col;
		l->buf[in->col] = '\0';
		inv->op = OP_JOIN;
		inv->line = in->line;
		break;
	}
	case OP_JOIN: {
		Line *l, *next;
		size_t boundary;

		if (in->line + 1 >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		next = &t->lines[in->line + 1];
		boundary = l->len;
		if (next->len) {
			if (line_reserve(l, l->len + next->len) != OK)
				return ERR;
			memcpy(l->buf + l->len, next->buf, next->len);
			l->len += next->len;
			l->buf[l->len] = '\0';
		}
		lines_remove_at(t, in->line + 1);
		inv->op = OP_SPLIT;
		inv->line = in->line;
		inv->col = boundary;
		break;
	}
	default:
		return ERR;
	}
	t->dirty = 1;
	t->rev++;
	return OK;
}

/* Record an inverse on the undo stack, extending the top record when a
 * typing run continues, and drop the now-stale redo stack. */
static void
record_undo(Text *t, Erec *inv)
{
	estack_clear(&t->redo);
	inv->group = t->cur_group;

	if (t->can_coalesce && inv->op == OP_DELETE && t->undo.n > 0) {
		Erec *top = &t->undo.v[t->undo.n - 1];

		if (top->op == OP_DELETE && top->group == inv->group &&
		    top->line == inv->line && top->col + top->n == inv->col) {
			top->n += inv->n;	/* inv carries no bytes */
			return;
		}
	}
	if (estack_push(&t->undo, inv) != OK)
		free(inv->bytes);	/* drop the record rather than leak */
}

/* Fill the cursor location a change should move to, given the primitive
 * that was applied and the inverse it produced. */
static void
cursor_after(const Erec *applied, const Erec *inv,
    size_t *line, size_t *col)
{
	size_t cl = applied->line, cc = applied->col;

	switch (applied->op) {
	case OP_INSERT:
		cc = applied->col + applied->n;
		break;
	case OP_DELETE:
		cc = applied->col;
		break;
	case OP_SPLIT:
		cl = applied->line + 1;
		cc = 0;
		break;
	case OP_JOIN:
		cc = inv->col;		/* the boundary the join merged at */
		break;
	}
	if (line)
		*line = cl;
	if (col)
		*col = cc;
}

/****************************************************************
 * Editing
 ****************************************************************/

int
text_insert(Text *t, size_t line, size_t col,
    const char *s, size_t n)
{
	Erec in, inv;

	if (line >= t->nlines || col > t->lines[line].len)
		return ERR;
	if (n == 0)
		return OK;

	in.op = OP_INSERT;
	in.line = line;
	in.col = col;
	in.bytes = (char *)s;
	in.n = n;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	t->can_coalesce = 1;
	return OK;
}

int
text_delete(Text *t, size_t line, size_t col, size_t n)
{
	Erec in, inv;

	if (line >= t->nlines || col > t->lines[line].len)
		return ERR;
	if (n == 0 || col == t->lines[line].len)
		return OK;

	in.op = OP_DELETE;
	in.line = line;
	in.col = col;
	in.bytes = NULL;
	in.n = n;
	t->can_coalesce = 0;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	return OK;
}

int
text_split(Text *t, size_t line, size_t col)
{
	Erec in, inv;

	if (line >= t->nlines || col > t->lines[line].len)
		return ERR;

	in.op = OP_SPLIT;
	in.line = line;
	in.col = col;
	in.bytes = NULL;
	in.n = 0;
	t->can_coalesce = 0;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	return OK;
}

int
text_join(Text *t, size_t line)
{
	Erec in, inv;

	if (line + 1 >= t->nlines)
		return ERR;

	in.op = OP_JOIN;
	in.line = line;
	in.col = 0;
	in.bytes = NULL;
	in.n = 0;
	t->can_coalesce = 0;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	return OK;
}

/****************************************************************
 * Undo and redo
 ****************************************************************/

int
text_can_undo(const Text *t)
{
	return t->undo.n > 0;
}

int
text_can_redo(const Text *t)
{
	return t->redo.n > 0;
}

void
text_undo_boundary(Text *t)
{
	t->can_coalesce = 0;
}

void
text_undo_group_begin(Text *t)
{
	if (t->group_depth == 0)
		t->cur_group = ++t->group_seq;
	t->group_depth++;
	t->can_coalesce = 0;	/* do not merge into a pre-group record */
}

void
text_undo_group_end(Text *t)
{
	if (t->group_depth > 0)
		t->group_depth--;
	if (t->group_depth == 0)
		t->cur_group = 0;
	t->can_coalesce = 0;	/* the next edit starts a fresh record */
}

/* Apply one recorded primitive during undo/redo: mutate the buffer, push the
 * inverse onto `to` (preserving its group so the reverse direction regroups),
 * and update the cursor location. */
static int
undo_apply_one(Text *t, Estack *from, Estack *to,
    size_t *line, size_t *col)
{
	Erec rec, inv;

	rec = from->v[--from->n];
	if (apply_op(t, &rec, &inv) != OK) {
		free(rec.bytes);
		return ERR;
	}
	inv.group = rec.group;
	if (estack_push(to, &inv) != OK)
		free(inv.bytes);
	cursor_after(&rec, &inv, line, col);
	free(rec.bytes);
	return OK;
}

/* Shared engine for undo and redo: reverse one step. A grouped step spans
 * every record at the top of `from` sharing the same non-zero group id, so a
 * multi-primitive edit undoes and redoes as a unit. */
static int
undo_step(Text *t, Estack *from, Estack *to,
    size_t *line, size_t *col)
{
	unsigned group;

	t->can_coalesce = 0;
	if (from->n == 0)
		return ERR;

	group = from->v[from->n - 1].group;
	if (undo_apply_one(t, from, to, line, col) != OK)
		return ERR;
	while (group != 0 && from->n > 0 &&
	    from->v[from->n - 1].group == group)
		if (undo_apply_one(t, from, to, line, col) != OK)
			break;
	return OK;
}

int
text_undo(Text *t, size_t *line, size_t *col)
{
	return undo_step(t, &t->undo, &t->redo, line, col);
}

int
text_redo(Text *t, size_t *line, size_t *col)
{
	return undo_step(t, &t->redo, &t->undo, line, col);
}

/****************************************************************
 * Hex view
 ****************************************************************/
/* Hex-dump view helpers.
 *
 * The editor can render the current buffer as a hex dump instead of as text.
 * Both views share one Text, so these helpers reconstruct the byte
 * stream that view walks: each line's bytes in order, joined by an implied
 * newline, with a trailing newline only when the content ends with one. The
 * functions are pure over struct text; the rendering and key handling that use
 * them live in edit.c. */




/* Whether an implied newline follows line i (a separator before the next line,
 * or the trailing newline after the last line when the file carried one). */
static int
line_has_newline(const Text *t, size_t i, size_t nlines)
{
	return (i + 1 < nlines) || text_final_newline(t);
}

/* Total bytes the hex view shows: every line's bytes plus one for each implied
 * newline. */
size_t
hex_total(const Text *t)
{
	size_t n = text_lines(t), i, tot = 0;

	for (i = 0; i < n; i++) {
		tot += text_line_len(t, i);
		if (line_has_newline(t, i, n))
			tot++;
	}
	return tot;
}

/* Byte offset of the text position (cy, cx). cx is a byte column within its
 * line and may equal the line length (the cursor sitting on the newline). */
size_t
hex_offset_of(const Text *t, size_t cy, size_t cx)
{
	size_t n = text_lines(t), i, off = 0;

	if (cy >= n)
		cy = n ? n - 1 : 0;
	for (i = 0; i < cy; i++) {
		off += text_line_len(t, i);
		if (line_has_newline(t, i, n))
			off++;
	}
	return off + cx;
}

/* Inverse of hex_offset_of: the text position holding byte "off". An offset on
 * an implied newline maps to (line, line_len); at or past the end it clamps to
 * the last position. */
void
hex_pos_at(const Text *t, size_t off, size_t *cy, size_t *cx)
{
	size_t n = text_lines(t), i, base = 0;

	for (i = 0; i < n; i++) {
		size_t len = text_line_len(t, i);

		if (off <= base + len) {	/* within the line or on its \n */
			*cy = i;
			*cx = off - base;
			return;
		}
		base += len + (line_has_newline(t, i, n) ? 1 : 0);
	}
	*cy = n ? n - 1 : 0;
	*cx = text_line_len(t, *cy);
}

/* Copy up to "count" bytes of the reconstructed stream starting at byte offset
 * "start" into buf. Returns how many were copied (fewer than count only near
 * the end of the buffer). */
size_t
hex_gather(const Text *t, size_t start, unsigned char *buf,
    size_t count)
{
	size_t n = text_lines(t), cy, cx, got = 0;

	hex_pos_at(t, start, &cy, &cx);
	while (got < count && cy < n) {
		size_t len = 0;
		const char *b = text_line(t, cy, &len);

		while (cx < len && got < count)
			buf[got++] = (unsigned char)b[cx++];
		if (got >= count)
			break;
		if (cx >= len) {		/* reached the implied newline */
			if (line_has_newline(t, cy, n)) {
				if (got >= count)
					break;
				buf[got++] = '\n';
			}
			cy++;
			cx = 0;
		}
	}
	return got;
}

/* Column of byte j's two-character hex pair within a dump row. A wider gap
 * falls after each group of eight bytes, so this is independent of the row's
 * total column count. */
int
hex_hexcol(int j)
{
	return 10 + j * 3 + j / 8;
}

/* Column where the ascii gutter begins for a row of "cols" bytes: past the
 * offset field, the hex pairs, their group gaps, and two spaces. */
int
hex_ascii_start(int cols)
{
	return 10 + cols * 3 + (cols - 1) / 8 + 2;
}

/* Column of byte j's character in the ascii gutter of a "cols"-wide row. */
int
hex_asciicol(int j, int cols)
{
	return hex_ascii_start(cols) + j;
}

/* Total display width of a "cols"-wide dump row (through the closing bar). */
int
hex_row_width(int cols)
{
	return hex_ascii_start(cols) + cols + 1;
}

/* Format one dump row into out: "OFFSET  hex...  |ascii|" for cols bytes per
 * row. buf holds n valid bytes (1..cols); columns past n are left blank. out_sz
 * must be at least hex_row_width(cols) + 1. */
void
hex_format_row(char *out, size_t out_sz, size_t offset,
    const unsigned char *buf, size_t n, int cols)
{
	static const char hexd[] = "0123456789abcdef";
	size_t width = (size_t)hex_row_width(cols);
	char off[24];
	int ol;
	size_t j;

	if (out_sz < width + 1) {
		if (out_sz)
			out[0] = '\0';
		return;
	}
	memset(out, ' ', width);
	out[width] = '\0';

	ol = snprintf(off, sizeof(off), "%08zx", offset);
	if (ol > 8)			/* an offset past 2^32: keep the low digits */
		memcpy(out, off + (ol - 8), 8);
	else
		memcpy(out, off, (size_t)ol);

	out[hex_asciicol(0, cols) - 1] = '|';
	out[hex_asciicol(cols - 1, cols) + 1] = '|';
	for (j = 0; j < (size_t)cols && j < n; j++) {
		unsigned char b = buf[j];
		int hc = hex_hexcol((int)j);

		out[hc] = hexd[b >> 4];
		out[hc + 1] = hexd[b & 0x0f];
		out[hex_asciicol((int)j, cols)] =
		    (b >= 0x20 && b < 0x7f) ? (char)b : '.';
	}
}

/* The value 0-15 of a hex digit, or -1 if c is not one. */
static int
hex_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parse a hex-pair pattern such as "de ad be ef" (spaces and tabs ignored)
 * into out, storing at most max bytes. Returns 0 with *outlen set on success,
 * or -1 for a non-hex character, an odd trailing nibble, or an overflow. */
int
hex_parse_bytes(const char *s, unsigned char *out, size_t max, size_t *outlen)
{
	size_t n = 0;
	int hi = -1;

	for (; *s; s++) {
		int d;

		if (*s == ' ' || *s == '\t')
			continue;
		d = hex_val(*s);
		if (d < 0)
			return -1;
		if (hi < 0) {
			hi = d;
		} else {
			if (n >= max)
				return -1;
			out[n++] = (unsigned char)((hi << 4) | d);
			hi = -1;
		}
	}
	if (hi >= 0)
		return -1;		/* a lone trailing nibble */
	*outlen = n;
	return 0;
}

/* Format a data-inspector line describing the up-to-four bytes b[0..n) that
 * start at the cursor: the first byte as unsigned and signed 8-bit and as a
 * character, then the 16- and 32-bit values in little-endian and big-endian.
 * Fields without enough bytes show "-". */
void
hex_inspect_line(char *out, size_t out_sz, const unsigned char *b, size_t n)
{
	char su8[12], si8[12], sch[8], su16[24], su32[40];

	if (n >= 1) {
		snprintf(su8, sizeof(su8), "%u", (unsigned)b[0]);
		snprintf(si8, sizeof(si8), "%d", (int)(signed char)b[0]);
		if (b[0] >= 0x20 && b[0] < 0x7f)
			snprintf(sch, sizeof(sch), "'%c'", (char)b[0]);
		else
			snprintf(sch, sizeof(sch), "'.'");
	} else {
		snprintf(su8, sizeof(su8), "-");
		snprintf(si8, sizeof(si8), "-");
		snprintf(sch, sizeof(sch), "-");
	}
	if (n >= 2) {
		unsigned le = (unsigned)b[0] | ((unsigned)b[1] << 8);
		unsigned be = ((unsigned)b[0] << 8) | (unsigned)b[1];

		snprintf(su16, sizeof(su16), "%u/%u", le, be);
	} else {
		snprintf(su16, sizeof(su16), "-");
	}
	if (n >= 4) {
		unsigned long le = (unsigned long)b[0] |
		    ((unsigned long)b[1] << 8) | ((unsigned long)b[2] << 16) |
		    ((unsigned long)b[3] << 24);
		unsigned long be = ((unsigned long)b[0] << 24) |
		    ((unsigned long)b[1] << 16) | ((unsigned long)b[2] << 8) |
		    (unsigned long)b[3];

		snprintf(su32, sizeof(su32), "%lu/%lu", le, be);
	} else {
		snprintf(su32, sizeof(su32), "-");
	}
	snprintf(out, out_sz, " u8 %s  i8 %s  %s  u16 %s  u32 %s  (le/be)",
	    su8, si8, sch, su16, su32);
}

/* Search the reconstructed byte stream for the plen-byte pattern pat. The scan
 * starts one byte off the cursor position "from" in direction dir (>=0 forward,
 * <0 backward) and wraps around the buffer once, so the match under the cursor
 * is skipped but every other position is tried. Returns 1 and the match offset
 * in *found, or 0 when the pattern does not occur (or on allocation failure). */
int
hex_find(const Text *t, const unsigned char *pat, size_t plen,
    size_t from, int dir, size_t *found)
{
	size_t total = hex_total(t), maxstart, i;
	unsigned char *buf;
	int hit = 0;

	if (plen == 0 || plen > total)
		return 0;
	buf = malloc(total);
	if (buf == NULL)
		return 0;
	hex_gather(t, 0, buf, total);
	maxstart = total - plen;		/* last position a match can start */

	if (dir >= 0) {
		for (i = from + 1; i <= maxstart; i++)
			if (memcmp(buf + i, pat, plen) == 0)
				goto found;
		for (i = 0; i <= from && i <= maxstart; i++)	/* wrap */
			if (memcmp(buf + i, pat, plen) == 0)
				goto found;
	} else {
		i = from;			/* positions below the cursor */
		while (i-- > 0)
			if (i <= maxstart && memcmp(buf + i, pat, plen) == 0)
				goto found;
		i = maxstart + 1;		/* wrap: from the end down to from+1 */
		while (i-- > from + 1)
			if (memcmp(buf + i, pat, plen) == 0)
				goto found;
	}
	free(buf);
	return 0;
found:
	*found = i;
	hit = 1;
	free(buf);
	return hit;
}

/****************************************************************
 * Modeless editor / MS-EDIT personality
 ****************************************************************/
/* The modeless (nano/MS-EDIT-style) text editor. */




static const char *progname = "vedit";


/* One parsed compiler/make diagnostic (see the build section). */
/****************************************************************
 * Keymap -- the table a modal (vi) personality would swap out
 ****************************************************************/

typedef enum cmd {
	CMD_NONE,
	CMD_INSERT,		/* self-insert the typed character */
	CMD_TAB,
	CMD_NEWLINE,
	CMD_BACKSPACE,
	CMD_DELETE,
	CMD_LEFT,
	CMD_RIGHT,
	CMD_UP,
	CMD_DOWN,
	CMD_HOME,
	CMD_END,
	CMD_TOP,		/* jump to the start of the file */
	CMD_BOTTOM,		/* jump to the end of the file */
	CMD_PGUP,
	CMD_PGDN,
	CMD_UNDO,
	CMD_REDO,
	CMD_COPY,
	CMD_CUT,
	CMD_PASTE,
	CMD_FIND,		/* prompt for a string and jump to it */
	CMD_REPLACE,		/* prompt for a pattern and a replacement */
	CMD_SYMBOL,		/* pick a definition in the buffer and jump to it */
	CMD_GOTO,		/* prompt for a line number and jump to it */
	CMD_HELP,		/* show the key bindings */
	CMD_SAVE,
	CMD_QUIT,
#ifndef VEDIT_NO_TOOLS
	CMD_COMPILE,		/* compile the current file (Alt+F9) */
	CMD_MAKE,		/* build the project (F9) */
	CMD_RUN,		/* run the program (Ctrl+F9) */
	CMD_VIEW_OUTPUT,	/* reopen the last output pane (Alt+F5) */
	CMD_ERR_NEXT,		/* jump to the next diagnostic (F4) */
	CMD_ERR_PREV,		/* jump to the previous diagnostic (Shift+F4) */
#endif
} Cmd;

typedef struct keybind {
	uint16_t	key;		/* TKBD_KEY_* to match */
	uint8_t		ctrl;		/* require the Ctrl modifier */
	Cmd	cmd;
} Keybind;

static const Keybind keymap[] = {
	{ TKBD_KEY_Q,		1, CMD_QUIT },
	{ TKBD_KEY_S,		1, CMD_SAVE },
	{ TKBD_KEY_Z,		1, CMD_UNDO },
	{ TKBD_KEY_Y,		1, CMD_REDO },
	{ TKBD_KEY_C,		1, CMD_COPY },
	{ TKBD_KEY_X,		1, CMD_CUT },
	{ TKBD_KEY_V,		1, CMD_PASTE },
	{ TKBD_KEY_F,		1, CMD_FIND },
	{ TKBD_KEY_R,		1, CMD_REPLACE },
	{ TKBD_KEY_T,		1, CMD_SYMBOL },
	{ TKBD_KEY_L,		1, CMD_GOTO },
	{ TKBD_KEY_F1,		0, CMD_HELP },
	{ TKBD_KEY_LEFT,	0, CMD_LEFT },
	{ TKBD_KEY_RIGHT,	0, CMD_RIGHT },
	{ TKBD_KEY_UP,		0, CMD_UP },
	{ TKBD_KEY_DOWN,	0, CMD_DOWN },
	{ TKBD_KEY_HOME,	0, CMD_HOME },
	{ TKBD_KEY_END,		0, CMD_END },
	{ TKBD_KEY_HOME,	1, CMD_TOP },	/* Ctrl+Home: start of file */
	{ TKBD_KEY_END,		1, CMD_BOTTOM },	/* Ctrl+End: end of file */
	{ TKBD_KEY_PGUP,	0, CMD_PGUP },
	{ TKBD_KEY_PGDN,	0, CMD_PGDN },
	{ TKBD_KEY_ENTER,	0, CMD_NEWLINE },
	{ TKBD_KEY_BACKSPACE,	0, CMD_BACKSPACE },
	{ TKBD_KEY_BACKSPACE2,	0, CMD_BACKSPACE },
	{ TKBD_KEY_DEL,		0, CMD_DELETE },
	{ TKBD_KEY_TAB,		0, CMD_TAB },
};

#define KEYMAP_COUNT ((int)(sizeof(keymap) / sizeof(keymap[0])))

static Cmd
key_to_cmd(const struct tkbd_seq *seq)
{
	int i;

	if (seq->type != TKBD_KEY)
		return CMD_NONE;

	for (i = 0; i < KEYMAP_COUNT; i++) {
		int want_ctrl = keymap[i].ctrl;
		int has_ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;

		if (keymap[i].key == seq->key && want_ctrl == has_ctrl)
			return keymap[i].cmd;
	}

	/* an ordinary printable character self-inserts */
	if (!(seq->mod & TKBD_MOD_CTRL) && seq->ch != TKBD_CH_NONE &&
	    seq->ch >= 0x20 && seq->ch != 0x7f)
		return CMD_INSERT;

	return CMD_NONE;
}

/****************************************************************
 * UTF-8 and display-column helpers
 ****************************************************************/

/* Display columns spanned by the first nbytes of s, expanding tabs to the
 * next TAB_WIDTH stop and honoring rune widths. */
int
disp_cols(const char *s, size_t nbytes)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;

	while (i < nbytes) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, nbytes - i);
		int w;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if (r < 0x20 || r == 0x7f)
			w = 1;
		else {
			w = rune_width(r);
			if (w < 0)
				w = 1;
		}
		col += w;
		i += (size_t)n;
	}
	return col;
}

/* Byte length of the UTF-8 rune ending just before byte offset cx. */
size_t
prev_rune_len(const char *s, size_t cx)
{
	size_t n = 1;

	while (n < cx && ((unsigned char)s[cx - n] & 0xc0) == 0x80)
		n++;
	return n;
}

/* Byte length of the UTF-8 rune starting at byte offset cx. */
size_t
rune_len_at(const char *s, size_t len, size_t cx)
{
	uint32_t r;
	int n;

	if (cx >= len)
		return 0;
	n = utf8_decode(&r, (const unsigned char *)s + cx, len - cx);
	return n > 0 ? (size_t)n : 1;
}

/****************************************************************
 * Syntax highlighting
 ****************************************************************/

/* The extension of a path (after the last '.'), or "" when there is none. */
static const char *
file_ext(const char *path)
{
	const char *dot = strrchr(path, '.');
	const char *slash = strrchr(path, '/');

	if (!dot || (slash && dot < slash) || dot[1] == '\0')
		return "";
	return dot + 1;
}

/* Mark the highlighter's cached start-states stale from `line` onward: the
 * state entering `line` is unchanged, but everything after it may differ. */
void
hl_touch(Editor *e, size_t line)
{
	if (e->hl_valid > line + 1)
		e->hl_valid = line + 1;
}

/* Ensure line_state[0..upto) hold correct start-states, tokenizing forward
 * from the last valid line. Cheap once warm; after an edit it recomputes only
 * from the change down to the bottom of the view. */
static void
hl_ensure(Editor *e, size_t upto)
{
	size_t nl = text_lines(e->t);

	if (!e->syn || !e->hl_on)
		return;
	if (upto > nl)
		upto = nl;
	if (e->line_state_cap < nl) {
		size_t cap = e->line_state_cap ? e->line_state_cap : 64;
		uint16_t *p;

		while (cap < nl)
			cap *= 2;
		p = realloc(e->line_state, cap * sizeof(*p));
		if (!p)
			return;			/* skip highlighting this frame */
		e->line_state = p;
		e->line_state_cap = cap;
	}
	if (e->hl_valid == 0) {
		e->line_state[0] = e->syn->start;
		e->hl_valid = 1;
	}
	if (e->hl_valid > nl)
		e->hl_valid = nl;		/* the buffer lost lines */
	while (e->hl_valid < upto) {
		size_t i = e->hl_valid - 1;
		size_t llen = 0;
		const char *s = text_line(e->t, i, &llen);

		e->line_state[e->hl_valid] =
		    syn_line(e->syn, e->line_state[i], s ? s : "", llen, NULL);
		e->hl_valid++;
	}
}

/* Compute the per-byte styles for line idx into e->hl_buf, or return NULL when
 * highlighting is off or the line has no cached start-state. */
static const uint8_t *
hl_line(Editor *e, size_t idx, const char *s, size_t llen)
{
	if (!e->syn || !e->hl_on || idx >= e->hl_valid)
		return NULL;
	if (e->hl_buf_cap < llen) {
		size_t cap = e->hl_buf_cap ? e->hl_buf_cap : 128;
		uint8_t *p;

		while (cap < llen)
			cap *= 2;
		p = realloc(e->hl_buf, cap);
		if (!p)
			return NULL;
		e->hl_buf = p;
		e->hl_buf_cap = cap;
	}
	syn_line(e->syn, e->line_state[idx], s ? s : "", llen, e->hl_buf);
	return e->hl_buf;
}

/****************************************************************
 * DOS-style chrome: menu bar, framed window, scrollbars, status
 *
 * The chrome frames the text area, so the editable region is inset by a
 * menu bar and top border above, a bottom border and status bar below, and
 * a border column on each side (the right one doubles as the vertical
 * scrollbar). Geometry is derived from e->rows/e->cols through the helpers
 * below; the text origin is fixed at (CHROME_TOP, CHROME_LEFT).
 ****************************************************************/

#define CHROME_TOP	2	/* first text row (menu bar 0, top border 1) */
#define CHROME_LEFT	1	/* first text column (left border is column 0) */
#define CHROME_BOTTOM	2	/* rows below the text (bottom border + status) */
#define CHROME_RIGHT	1	/* columns right of the text (border/scrollbar) */

static void ui_field(Screen *d, int row, int col, int width,
    const char *s, Color fg, Color bg, uint16_t attrs);

/* Frame and scrollbar glyphs. GL_* are logical markers (see the box-drawing
 * section near the top of the file); scr_present renders each one per the
 * editor's box mode as UTF-8 box-drawing, DEC line-drawing, or an ASCII
 * substitute. Keeping them as markers means the many call sites below are
 * unchanged. */
#define GL_TL		BOX_CP(BG_TL)
#define GL_TR		BOX_CP(BG_TR)
#define GL_BL		BOX_CP(BG_BL)
#define GL_BR		BOX_CP(BG_BR)
#define GL_H		BOX_CP(BG_H)
#define GL_V		BOX_CP(BG_V)
#define GL_UP		BOX_CP(BG_UP)
#define GL_DOWN		BOX_CP(BG_DOWN)
#define GL_LEFT		BOX_CP(BG_LEFT)
#define GL_RIGHT	BOX_CP(BG_RIGHT)
#define GL_THUMB	BOX_CP(BG_THUMB)
#define GL_TRACK	BOX_CP(BG_TRACK)
#define GL_CHECK	BOX_CP(BG_CHECK)

/* Editor chrome palette. Three presets: the DOS look (blue text area, gray
 * bars), a black look (text area left on the terminal default background, which
 * lets scr_present use erase-to-EOL for trailing blanks), and a monochrome
 * fallback that leans on reverse video. */
typedef struct chrome_pal {
	Color	content_fg, content_bg;	/* the text area */
	Color	frame_fg, frame_bg;	/* window border + scrollbars */
	Color	title_fg;		/* filename in the top border */
	Color	bar_fg, bar_bg;		/* menu bar + status bar */
	int		reverse_bars;		/* draw the bars in reverse video */
} Pal;

#define CIDX(n) { .type = COLOR_INDEXED, { .index = (n) } }
#define CDEF	{ .type = COLOR_DEFAULT }
/* The default DOS look (the chrome palette the editor paints with). */
static Pal chrome_dos = {
	.content_fg = CIDX(15), .content_bg = CIDX(4),
	.frame_fg = CIDX(15), .frame_bg = CIDX(4),
	.title_fg = CIDX(15),
	.bar_fg = CIDX(0), .bar_bg = CIDX(7),
	.reverse_bars = 0,
};
/* Black look: the text area stays on the terminal default background, so
 * scr_present can clear trailing blanks with erase-to-EOL on any client. The
 * bars keep the DOS gray so the chrome still reads as chrome. */
static const Pal chrome_black = {
	.content_fg = CIDX(15), .content_bg = CDEF,
	.frame_fg = CIDX(6), .frame_bg = CDEF,
	.title_fg = CIDX(15),
	.bar_fg = CIDX(0), .bar_bg = CIDX(7),
	.reverse_bars = 0,
};
static const Pal chrome_plain = {
	.content_fg = CDEF, .content_bg = CDEF,
	.frame_fg = CDEF, .frame_bg = CDEF,
	.title_fg = CDEF,
	.bar_fg = CDEF, .bar_bg = CDEF,
	.reverse_bars = 1,
};
#undef CIDX
#undef CDEF

/* User color themes defined in the config file as [theme "name"] sections.
 * They extend the three built-in schemes: e->scheme holds SCHEME_COUNT + index
 * to select one. */
#define USER_THEME_MAX 8

typedef struct user_theme {
	char	name[32];
	Pal	pal;
	int	borderless;	/* drop the right border, like the black scheme */
	int	used;
} Usertheme;

static Usertheme	g_user_themes[USER_THEME_MAX];
static int		g_user_theme_count;

static int
theme_by_name(const char *name)
{
	int i;

	for (i = 0; i < g_user_theme_count; i++)
		if (g_user_themes[i].used &&
		    strcmp(g_user_themes[i].name, name) == 0)
			return i;
	return -1;
}

/* Find or create a slot for a theme name, initialized to the DOS preset. */
static int
theme_slot(const char *name)
{
	int i = theme_by_name(name);

	if (i >= 0)
		return i;
	if (g_user_theme_count >= USER_THEME_MAX ||
	    strlen(name) >= sizeof(g_user_themes[0].name))
		return -1;
	i = g_user_theme_count++;
	snprintf(g_user_themes[i].name, sizeof(g_user_themes[i].name), "%s",
	    name);
	g_user_themes[i].pal = chrome_dos;
	g_user_themes[i].borderless = 0;
	g_user_themes[i].used = 1;
	return i;
}

/* Load the [theme "name"] sections from the config into g_user_themes. Pass 0
 * creates the slots and applies each theme's "base" preset; pass 1 overlays the
 * individual color fields, so field and base order in the file does not matter.
 */
static void
themes_load_cfg(const Cfg *c)
{
	int pass, i;

	g_user_theme_count = 0;
	memset(g_user_themes, 0, sizeof(g_user_themes));
	if (!c)
		return;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < c->count; i++) {
			const char *key = c->entries[i].key;
			const char *val = c->entries[i].value;
			const char *rest, *dot, *field;
			char name[32];
			size_t nl;
			int slot;
			Pal *p;

			if (strncmp(key, "theme.", 6) != 0)
				continue;
			rest = key + 6;
			dot = strchr(rest, '.');
			if (!dot)
				continue;
			nl = (size_t)(dot - rest);
			if (nl == 0 || nl >= sizeof(name))
				continue;
			memcpy(name, rest, nl);
			name[nl] = '\0';
			field = dot + 1;
			slot = theme_slot(name);
			if (slot < 0)
				continue;
			p = &g_user_themes[slot].pal;

			if (pass == 0) {
				if (strcmp(field, "base") != 0)
					continue;
				if (strcmp(val, "black") == 0) {
					*p = chrome_black;
					g_user_themes[slot].borderless = 1;
				} else if (strcmp(val, "plain") == 0) {
					*p = chrome_plain;
				} else {
					*p = chrome_dos;
				}
				continue;
			}
			if (strcmp(field, "base") == 0)
				continue;		/* applied in pass 0 */
			else if (strcmp(field, "content.fg") == 0)
				cfg_color(val, &p->content_fg);
			else if (strcmp(field, "content.bg") == 0)
				cfg_color(val, &p->content_bg);
			else if (strcmp(field, "frame.fg") == 0)
				cfg_color(val, &p->frame_fg);
			else if (strcmp(field, "frame.bg") == 0)
				cfg_color(val, &p->frame_bg);
			else if (strcmp(field, "title.fg") == 0)
				cfg_color(val, &p->title_fg);
			else if (strcmp(field, "bar.fg") == 0)
				cfg_color(val, &p->bar_fg);
			else if (strcmp(field, "bar.bg") == 0)
				cfg_color(val, &p->bar_bg);
			else if (strcmp(field, "reverse-bars") == 0)
				p->reverse_bars = str_bool(val, p->reverse_bars);
			else if (strcmp(field, "borderless") == 0)
				g_user_themes[slot].borderless =
				    str_bool(val, g_user_themes[slot].borderless);
		}
	}
}

static const Pal *
ed_chrome(const Editor *e)
{
	if (e->scheme >= SCHEME_COUNT) {
		int i = e->scheme - SCHEME_COUNT;

		if (i < g_user_theme_count)
			return &g_user_themes[i].pal;
	}
	switch (e->scheme) {
	case SCHEME_BLACK:
		return &chrome_black;
	case SCHEME_PLAIN:
		return &chrome_plain;
	default:
		return &chrome_dos;
	}
}

/* Height of the framed text area (rows minus menu, two borders, status). */
int
text_height(const Editor *e)
{
	int h = e->rows - CHROME_TOP - CHROME_BOTTOM;

	return h < 1 ? 1 : h;
}

/* Columns reserved to the right of the text. The black scheme drops the right
 * border and vertical scrollbar so the text reaches the last column, which lets
 * scr_present clear trailing blanks with erase-to-EOL (see there). */
static int
chrome_right(const Editor *e)
{
	if (e->scheme == SCHEME_BLACK)
		return 0;
	if (e->scheme >= SCHEME_COUNT) {
		int i = e->scheme - SCHEME_COUNT;

		if (i < g_user_theme_count && g_user_themes[i].borderless)
			return 0;
	}
	return CHROME_RIGHT;
}

/* Width of the framed text area (cols minus the border columns). */
static int
text_width(const Editor *e)
{
	int w = e->cols - CHROME_LEFT - chrome_right(e);

	return w < 1 ? 1 : w;
}

/* Columns of the line-number gutter at the left of the text, or 0 when off. The
 * width tracks the buffer's digit count (minimum three) plus a trailing space,
 * so it is the text-mode counterpart of the hex view's address column. It is
 * sized from the total line count, not the visible lines, so it does not jitter
 * as the view scrolls. */
static int
gutter_width(const Editor *e)
{
	size_t n = text_lines(e->t);
	int d = 1;

	if (!e->show_lineno)
		return 0;
	while (n >= 10) {
		n /= 10;
		d++;
	}
	if (d < 3)
		d = 3;
	return d + 1;
}

/* Paint the gutter cells for one screen row: the 1-based number of line `idx`
 * right-aligned, or blanks for a virtual row past the end of the buffer or a
 * soft-wrap continuation row (number == 0). The cursor's own line is drawn
 * bright, the rest dim. */
static void
render_gutter(Screen *d, const Editor *e, int row, int gutter, size_t idx,
    int number, Color fg, Color bg)
{
	char num[24];
	uint16_t at = (number && idx == e->cy) ? 0 : ATTR_DIM;
	int i, n = 0;

	if (gutter <= 0)
		return;
	if (number && idx < text_lines(e->t))
		n = snprintf(num, sizeof(num), "%*zu ", gutter - 1, idx + 1);
	for (i = 0; i < gutter; i++)
		scr_cell(d, row, CHROME_LEFT + i,
		    (uint32_t)(unsigned char)(i < n ? num[i] : ' '), fg, bg, at);
}

/* Paint one scrollbar cell, choosing the thumb where pos falls in [0,span). */
static void
scrollbar_cell(Screen *d, int row, int col, int idx, int span,
    int thumb, uint32_t arrow_a, uint32_t arrow_b,
    Color fg, Color bg)
{
	uint32_t cp;

	if (idx == 0)
		cp = arrow_a;
	else if (idx == span - 1)
		cp = arrow_b;
	else
		cp = (idx == thumb) ? GL_THUMB : GL_TRACK;
	scr_cell(d, row, col, cp, fg, bg, 0);
}

/* Thumb index within a track of tspan interior cells for a scroll offset. */
static int
thumb_index(size_t off, size_t max_off, int span)
{
	int inner = span - 2;		/* interior between the two arrows */
	int t;

	if (inner < 1 || max_off == 0)
		return 1;
	t = 1 + (int)((off * (size_t)(inner - 1)) / max_off);
	if (t < 1)
		t = 1;
	if (t > span - 2)
		t = span - 2;
	return t;
}

/* Menu bar model: a fixed set of pull-down menus. Each item names an action
 * the main loop carries out; a separator (MA_SEP) is a non-selectable rule. */
typedef enum menu_act {
	MA_NONE, MA_SEP,
	MA_NEW, MA_OPEN, MA_SAVE, MA_SAVE_AS,
	MA_BUF_NEXT, MA_BUF_PREV, MA_BUF_LIST, MA_EXIT,
	MA_UNDO, MA_REDO, MA_CUT, MA_COPY, MA_PASTE, MA_OSC_COPY, MA_OSC_COPY_FILE,
	MA_FIND, MA_FIND_NEXT, MA_REPLACE, MA_SYMBOL, MA_TAG_POP, MA_OPEN_HEADER,
	MA_GOTO,
	MA_SYNTAX, MA_SCHEME, MA_LINENO, MA_WRAP, MA_EOL, MA_HEX, MA_DRAW,
	MA_SHOW_TABS, MA_AUTO_INDENT, MA_EXPAND_TABS,
	MA_TABS_TO_SPACES, MA_SPACES_TO_TABS,
	MA_VI_MODE, MA_RELOAD_CONFIG,
#ifndef VEDIT_NO_TOOLS
	MA_COMPILE, MA_MAKE, MA_RUN, MA_VIEW_OUTPUT, MA_ERR_NEXT, MA_ERR_PREV,
#endif
	MA_HELP, MA_TUTORIAL, MA_ABOUT,
} Menuact;

/* draw mode toggle, defined with the draw-mode module further down */
static void draw_toggle(Editor *e);
static void ed_reload_config(Editor *e);

typedef struct menu_item {
	const char	*label;
	const char	*accel;		/* modeless shortcut, right-aligned, or "" */
	const char	*vaccel;	/* vi-personality shortcut, or "" to reuse accel */
	Menuact	act;
} Menuitem;

typedef struct menu_def {
	const char	*title;
	int		col;		/* start column on the bar (Help: dynamic) */
	const Menuitem *items;
	int		n;
} Menu;

static const Menuitem mi_file[] = {
	{ "&New",	"",		"",	MA_NEW },
	{ "&Open...",	"",		"",	MA_OPEN },
	{ "&Save",	"Ctrl+S",	":w",	MA_SAVE },
	{ "Save &As...","",		"",	MA_SAVE_AS },
	{ "",		"",		"",	MA_SEP },
	{ "Next &Buffer","F8",		":bn",	MA_BUF_NEXT },
	{ "&Prev Buffer","Shift+F8",	":bp",	MA_BUF_PREV },
	{ "Buffer &List","",		"",	MA_BUF_LIST },
	{ "",		"",		"",	MA_SEP },
	{ "E&xit",	"Ctrl+Q",	":q",	MA_EXIT },
};
static const Menuitem mi_edit[] = {
	{ "&Undo",	"Ctrl+Z",	"u",		MA_UNDO },
	{ "&Redo",	"Ctrl+Y",	"Ctrl+R",	MA_REDO },
	{ "",		"",		"",		MA_SEP },
	{ "Cu&t",	"Ctrl+X",	"dd",		MA_CUT },
	{ "&Copy",	"Ctrl+C",	"yy",		MA_COPY },
	{ "&Paste",	"Ctrl+V",	"p",		MA_PASTE },
	{ "",		"",		"",		MA_SEP },
	{ "Copy to T&erminal",	 "",	"",	MA_OSC_COPY },
	{ "Copy &File to Terminal","",	"",	MA_OSC_COPY_FILE },
	{ "",		"",		"",		MA_SEP },
	{ "Tabs to &Spaces",	"",	":retab",	MA_TABS_TO_SPACES },
	{ "Spaces to &Tabs",	"",	"",	MA_SPACES_TO_TABS },
};
static const Menuitem mi_search[] = {
	{ "&Find...",		"Ctrl+F",	"/",	MA_FIND },
	{ "&Repeat Find",	"",		"n",	MA_FIND_NEXT },
	{ "&Replace...",	"Ctrl+R",	":s",	MA_REPLACE },
	{ "Go to S&ymbol...",	"Ctrl+T",	"",	MA_SYMBOL },
	{ "&Pop Tag",		"",		":pop",	MA_TAG_POP },
	{ "Open &Header",	"",		"gf",	MA_OPEN_HEADER },
	{ "&Go to Line...",	"Ctrl+L",	"G",	MA_GOTO },
};
static const Menuitem mi_view[] = {
	{ "&Syntax Highlight",	"",	"",	MA_SYNTAX },
	{ "&Color Scheme",	"",	"",	MA_SCHEME },
	{ "&Line Numbers",	"",	":set nu",	MA_LINENO },
	{ "&Word Wrap",		"",	":set wrap",	MA_WRAP },
	{ "Line &Endings",	"",	":set ff",	MA_EOL },
	{ "Show &Tabs",		"",	":set list",	MA_SHOW_TABS },
	{ "&Auto Indent",	"",	":set ai",	MA_AUTO_INDENT },
	{ "&Indent with Spaces","",	":set et",	MA_EXPAND_TABS },
	{ "&Hex Dump",		"",	"",	MA_HEX },
};
static const Menuitem mi_options[] = {
	{ "&Draw Mode",		"Ins",	"",		MA_DRAW },
	{ "&Vi Keys",		"F2",	"",		MA_VI_MODE },
	{ "&Reload Config",	"",	":reload",	MA_RELOAD_CONFIG },
};
#ifndef VEDIT_NO_TOOLS
static const Menuitem mi_compile[] = {
	{ "&Compile",	"Alt+F9",	"",	MA_COMPILE },
	{ "&Make",	"F9",		"",	MA_MAKE },
};
static const Menuitem mi_run[] = {
	{ "&Run",		"Ctrl+F9",	"",	MA_RUN },
	{ "&View Output",	"Alt+F5",	"",	MA_VIEW_OUTPUT },
	{ "",			"",		"",	MA_SEP },
	{ "&Next Error",	"F4",		"",	MA_ERR_NEXT },
	{ "&Prev Error",	"Shift+F4",	"",	MA_ERR_PREV },
};
#endif
static const Menuitem mi_help[] = {
	{ "&Key Bindings",	"F1",	"",	MA_HELP },
	{ "&Tutorial",		"",	"",	MA_TUTORIAL },
	{ "&About",		"",	"",	MA_ABOUT },
};

#define MENU_ITEMS(a) (a), (int)(sizeof(a) / sizeof((a)[0]))
static const Menu MENUS[] = {
	{ "&File",	1,	MENU_ITEMS(mi_file) },
	{ "&Edit",	7,	MENU_ITEMS(mi_edit) },
	{ "&Search",	13,	MENU_ITEMS(mi_search) },
	{ "&View",	21,	MENU_ITEMS(mi_view) },
	{ "&Options",	27,	MENU_ITEMS(mi_options) },
#ifndef VEDIT_NO_TOOLS
	{ "&Compile",	36,	MENU_ITEMS(mi_compile) },
	{ "&Run",	45,	MENU_ITEMS(mi_run) },
#endif
	{ "&Help",	0,	MENU_ITEMS(mi_help) },	/* col set dynamically */
};
#undef MENU_ITEMS
#define MENU_COUNT ((int)(sizeof(MENUS) / sizeof(MENUS[0])))
#define MENU_HELP (MENU_COUNT - 1)

/* A menu title or item label may mark its mnemonic with '&' before the chosen
 * letter (DOS style: the highlighted key that selects the entry). A literal
 * ampersand is written "&&". These helpers read such a string. */

/* Display width of a label, not counting the '&' mnemonic markers. */
static int
menu_disp_w(const char *s)
{
	int w = 0;

	while (*s) {
		if (*s == '&' && s[1] == '&') {
			s += 2;
			w++;
		} else if (*s == '&' && s[1]) {
			s++;		/* marker: no column of its own */
		} else {
			s++;
			w++;
		}
	}
	return w;
}

/* The lowercased mnemonic letter of a label, or 0 when it has none. */
static int
menu_mnemonic(const char *s)
{
	for (; *s; s++) {
		if (*s == '&' && s[1] == '&')
			s++;			/* literal "&&", skip both */
		else if (*s == '&' && s[1])
			return tolower((unsigned char)s[1]);
	}
	return 0;
}

/* Draw a label, dropping the '&' markers and underlining the mnemonic letter.
 * Returns the column after the last cell written. */
static int
ui_menu_label(Screen *d, int r, int c, const char *s,
    Color fg, Color bg, uint16_t at)
{
	char buf[2] = { 0, 0 };

	for (; *s; s++) {
		uint16_t a = at;

		if (*s == '&' && s[1] == '&')
			s++;			/* "&&" -> literal '&' */
		else if (*s == '&' && s[1]) {
			a |= ATTR_UNDERLINE;
			s++;
		}
		buf[0] = *s;
		c = scr_text(d, r, c, buf, fg, bg, a);
	}
	return c;
}

/* Top-level menu whose title mnemonic is lc (a lowercased letter), or -1. */
static int
menu_title_by_mnemonic(int lc)
{
	int i;

	for (i = 0; i < MENU_COUNT; i++)
		if (menu_mnemonic(MENUS[i].title) == lc)
			return i;
	return -1;
}

/* Selectable item of menu m whose mnemonic is lc, or -1. Separators never
 * match. */
static int
menu_item_by_mnemonic(int m, int lc)
{
	int i;

	for (i = 0; i < MENUS[m].n; i++)
		if (MENUS[m].items[i].act != MA_SEP &&
		    menu_mnemonic(MENUS[m].items[i].label) == lc)
			return i;
	return -1;
}

/* Bar column of menu i; Help is right-aligned. */
static int
menu_col(const Editor *e, int i)
{
	if (i == MENU_HELP)
		return e->cols - 5;
	return MENUS[i].col;
}

/* Which top-level menu title column x falls on, or -1. */
static int
menu_hit(const Editor *e, int x)
{
	int i;

	for (i = 0; i < MENU_COUNT; i++) {
		int c = menu_col(e, i);

		if (x >= c && x < c + menu_disp_w(MENUS[i].title))
			return i;
	}
	return -1;
}

static void
ui_menubar(Editor *e, const Pal *p, int active)
{
	uint16_t at = p->reverse_bars ? ATTR_REVERSE : 0;
	int i;

	scr_fill(e->d, 0, 0, e->cols, ' ', p->bar_fg, p->bar_bg, at);
	for (i = 0; i < MENU_COUNT; i++) {
		uint16_t a = (i == active) ? (at ^ ATTR_REVERSE) : at;
		int col = menu_col(e, i);

		if (col < 0 || col >= e->cols)
			continue;
		ui_menu_label(e->d, 0, col, MENUS[i].title, p->bar_fg,
		    p->bar_bg, a);
	}
}

/* Clamped left column of menu i's drop-down box (kept on screen). */
static int
dropdown_x(const Editor *e, int mi, int boxw)
{
	int x = menu_col(e, mi);

	if (x + boxw > e->cols)
		x = e->cols - boxw;
	if (x < 0)
		x = 0;
	return x;
}

/* Accelerator to display for an item under the active personality. The
 * modeless Ctrl+ chords do not reach the editor in the vi personalities, so
 * show the vi keys that carry out the same action instead. An empty vaccel
 * means the modeless accel applies in both (e.g. the F1/F2 function keys). */
static const char *
item_accel(const Editor *e, const Menuitem *it)
{
	if (e->mode != MODE_MODELESS && it->vaccel[0])
		return it->vaccel;
	return it->accel;
}

/* Inner width of menu i's drop-down (widest "label  accel"). */
static int
dropdown_width(const Editor *e, int mi)
{
	const Menu *m = &MENUS[mi];
	int i, w = 0;

	for (i = 0; i < m->n; i++) {
		const Menuitem *it = &m->items[i];
		const char *accel = item_accel(e, it);
		int lw = menu_disp_w(it->label);

		if (accel[0])
			lw += 2 + (int)strlen(accel);
		if (lw > w)
			w = lw;
	}
	return w + 2;		/* one space of padding on each side */
}

/* Toggle state of a menu action: 1 on, 0 off, -1 when it is not a toggle. */
static int
menu_checked(const Editor *e, Menuact act)
{
	switch (act) {
	case MA_SYNTAX:
		return e->hl_on ? 1 : 0;
	case MA_SCHEME:
		return -1;		/* a three-way cycle, not a checkbox */
	case MA_LINENO:
		return e->show_lineno ? 1 : 0;
	case MA_WRAP:
		return e->wrap ? 1 : 0;
	case MA_DRAW:
		return e->draw_mode ? 1 : 0;
	case MA_VI_MODE:
		return e->mode != MODE_MODELESS ? 1 : 0;
	default:
		return -1;
	}
}

static void
ui_dropdown(Editor *e, int mi, int sel)
{
	const Pal *p = ed_chrome(e);
	const Menu *m = &MENUS[mi];
	Screen *d = e->d;
	Color fg = p->bar_fg, bg = p->bar_bg;
	uint16_t base = p->reverse_bars ? ATTR_REVERSE : 0;
	int w = dropdown_width(e, mi);
	int boxw = w + 2;
	int x = dropdown_x(e, mi, boxw);
	int y = 1;			/* top border sits under the bar */
	int i;

	/* top and bottom border */
	scr_cell(d, y, x, GL_TL, fg, bg, base);
	scr_cell(d, y, x + boxw - 1, GL_TR, fg, bg, base);
	scr_cell(d, y + m->n + 1, x, GL_BL, fg, bg, base);
	scr_cell(d, y + m->n + 1, x + boxw - 1, GL_BR, fg, bg, base);
	for (i = 0; i < w; i++) {
		scr_cell(d, y, x + 1 + i, GL_H, fg, bg, base);
		scr_cell(d, y + m->n + 1, x + 1 + i, GL_H, fg, bg, base);
	}

	for (i = 0; i < m->n; i++) {
		const Menuitem *it = &m->items[i];
		const char *accel;
		int row = y + 1 + i;
		uint16_t at = base;

		scr_cell(d, row, x, GL_V, fg, bg, base);
		scr_cell(d, row, x + boxw - 1, GL_V, fg, bg, base);
		if (it->act == MA_SEP) {
			int c;

			for (c = 0; c < w; c++)
				scr_cell(d, row, x + 1 + c, GL_H, fg, bg,
				    base);
			continue;
		}
		if (i == sel)
			at = base ^ ATTR_REVERSE;	/* highlight bar */
		scr_fill(d, row, x + 1, w, ' ', fg, bg, at);
		if (menu_checked(e, it->act) == 1)
			scr_cell(d, row, x + 1, GL_CHECK, fg, bg, at);
		ui_menu_label(d, row, x + 2, it->label, fg, bg, at);
		accel = item_accel(e, it);
		if (accel[0])
			scr_text(d, row, x + 1 + w - 1 - (int)strlen(accel),
			    accel, fg, bg, at);
	}
}

/* Draw a bordered, filled box of w by h cells at (x, y). Used by the modal
 * dialogs. */
static void
scr_box(Screen *d, int x, int y, int w, int h,
    Color fg, Color bg, uint16_t at)
{
	int r, c;

	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++) {
			uint32_t cp = ' ';

			if (r == 0)
				cp = c == 0 ? GL_TL :
				    c == w - 1 ? GL_TR : GL_H;
			else if (r == h - 1)
				cp = c == 0 ? GL_BL :
				    c == w - 1 ? GL_BR : GL_H;
			else if (c == 0 || c == w - 1)
				cp = GL_V;
			scr_cell(d, y + r, x + c, cp, fg, bg, at);
		}
}

/* Top-left corner that centers a boxw by boxh overlay in the screen, clamped
 * to stay on screen. Used by the modal dialogs at open and on resize. */
static void
dlg_center(const Editor *e, int boxw, int boxh, int *x, int *y)
{
	*x = (e->cols - boxw) / 2;
	*y = (e->rows - boxh) / 2;
	if (*x < 0)
		*x = 0;
	if (*y < 0)
		*y = 0;
}

/* The bar palette the modal overlays draw with (menu bar colors, reverse
 * video in the plain scheme). */
static void
dlg_palette(const Editor *e, Color *fg, Color *bg,
    uint16_t *base)
{
	const Pal *p = ed_chrome(e);

	*fg = p->bar_fg;
	*bg = p->bar_bg;
	*base = p->reverse_bars ? ATTR_REVERSE : 0;
}

static void
ui_frame(Editor *e, const Pal *p)
{
	Screen *d = e->d;
	Color fg = p->frame_fg, bg = p->frame_bg;
	int top = CHROME_TOP - 1;	/* top border row, under the menu bar */
	int bot = e->rows - CHROME_BOTTOM;	/* bottom border row */
	int sb = e->cols - chrome_right(e);	/* right border / vertical bar
						 * (off-screen when borderless) */
	int th = text_height(e);
	int i;
	size_t nlines = text_lines(e->t);
	size_t max_top = nlines > (size_t)th ? nlines - (size_t)th : 0;
	size_t curlen = 0;
	const char *cur = text_line(e->t, e->cy, &curlen);
	int curw = cur ? disp_cols(cur, curlen) : 0;
	int tw = text_width(e);
	size_t max_left = curw > tw ? (size_t)(curw - tw) : 0;
	int vthumb = thumb_index(e->top, max_top, th);
	int hthumb;
	const char *name = e->has_name ? e->path : "Untitled";
	char title[80];
	int tlen, tstart;

	if (bot < top)
		bot = top;

	/* top and bottom borders */
	scr_fill(d, top, 0, e->cols, GL_H, fg, bg, 0);
	scr_fill(d, bot, 0, e->cols, GL_H, fg, bg, 0);
	scr_cell(d, top, 0, GL_TL, fg, bg, 0);
	scr_cell(d, top, sb, GL_TR, fg, bg, 0);
	scr_cell(d, bot, 0, GL_BL, fg, bg, 0);
	scr_cell(d, bot, sb, GL_BR, fg, bg, 0);

	/* centered filename on the top border, bracketed by spaces; a buffer
	 * index is prefixed when more than one file is open */
	if (e->nbuf > 1)
		tlen = snprintf(title, sizeof(title), " [%d/%d] %s ",
		    e->cur + 1, e->nbuf, name);
	else
		tlen = snprintf(title, sizeof(title), " %s ", name);
	if (tlen > e->cols - 4)
		tlen = e->cols - 4;
	if (tlen > 0) {
		tstart = (e->cols - tlen) / 2;
		if (tstart < 1)
			tstart = 1;
		ui_field(d, top, tstart, tlen, title, p->title_fg, bg,
		    ATTR_BOLD);
	}

	/* left border column and the vertical scrollbar column */
	for (i = 0; i < th; i++) {
		int r = CHROME_TOP + i;

		scr_cell(d, r, 0, GL_V, fg, bg, 0);
		if (th >= 3)
			scrollbar_cell(d, r, sb, i, th, vthumb,
			    GL_UP, GL_DOWN, fg, bg);
		else
			scr_cell(d, r, sb, GL_V, fg, bg, 0);
	}

	/* horizontal scrollbar embedded in the bottom border */
	if (e->cols >= 6) {
		int hspan = e->cols - CHROME_LEFT - chrome_right(e); /* corners off */

		hthumb = thumb_index(e->left, max_left, hspan);
		for (i = 0; i < hspan; i++)
			scrollbar_cell(d, bot, 1 + i, i, hspan, hthumb,
			    GL_LEFT, GL_RIGHT, fg, bg);
	}
}

static void
ui_statusbar(Editor *e, const Pal *p, int cur_col)
{
	int row = e->rows - 1;
	uint16_t at = p->reverse_bars ? ATTR_REVERSE : 0;
	char right[80];
	char flags[32];
	int rlen;

	scr_fill(e->d, row, 0, e->cols, ' ', p->bar_fg, p->bar_bg, at);

	/* compact indicators for the sticky display toggles, then the line-ending
	 * style, which is always shown */
	flags[0] = '\0';
	if (e->wrap)
		strcat(flags, "WRAP ");
	if (e->show_lineno)
		strcat(flags, "NUM ");
	strcat(flags, eol_name(text_eol(e->t)));
	strcat(flags, "  ");

	if (e->status[0]) {
		scr_text(e->d, row, 1, e->status, p->bar_fg, p->bar_bg, at);
	} else {
		const char *mode = "";

		if (e->draw_mode)
			mode = "-- DRAW --  ";
		else if (e->vi_visual == 'v')
			mode = "-- VISUAL --  ";
		else if (e->vi_visual == 'V')
			mode = "-- VISUAL LINE --  ";
		else if (e->vi_visual == VI_VBLOCK)
			mode = "-- VISUAL BLOCK --  ";
		else if (e->mode == MODE_NORMAL)
			mode = "-- NORMAL --  ";
		else if (e->mode == MODE_INSERT)
			mode = "-- INSERT --  ";
		scr_text(e->d, row, 1, mode, p->bar_fg, p->bar_bg, at);
		scr_text(e->d, row, 1 + (int)strlen(mode), "F1=Help",
		    p->bar_fg, p->bar_bg, at);
	}

	rlen = snprintf(right, sizeof(right), "%sLine:%zu  Col:%zu%s",
	    flags, e->cy + 1, (size_t)cur_col + 1,
	    text_dirty(e->t) ? "  *" : "");
	if (rlen > 0 && rlen < e->cols - 1)
		scr_text(e->d, row, e->cols - rlen - 1, right,
		    p->bar_fg, p->bar_bg, at);
}

/****************************************************************
 * Rendering
 ****************************************************************/

/* Draw one text line clipped to the display window [left, left+width),
 * expanding tabs. Trailing space pads the field to width. Display columns in
 * [hl_start, hl_end) are shown in reverse video for the selection; pass
 * hl_start >= hl_end for no highlight. When show_tabs is set, each hard tab's
 * first column carries a dim guide glyph. */
static void
scr_line(Screen *d, int row, int col0, const char *s, size_t len,
    int left, int width, int hl_start, int hl_end, const uint8_t *sty,
    const Color *pal, const uint16_t *pal_attr, int npal,
    Color base_fg, Color base_bg, int show_tabs)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;		/* display column at the start of this rune */
	int drawn = 0;		/* columns emitted into the window */
	uint32_t tabmark = 0;	/* guide glyph for a hard tab, 0 when hidden */

	if (show_tabs)
		tabmark = (d->t->box_mode == VEDIT_BOX_UTF8) ? 0x2192 : '>';

	while (i < len && drawn < width) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, len - i);
		int w;
		int rev;
		uint16_t attrs;
		Color fg;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if (r < 0x20 || r == 0x7f)
			w = 1;
		else {
			w = rune_width(r);
			if (w < 0)
				w = 1;
		}

		if (col + w <= left) {
			/* wholly left of the window */
			col += w;
			i += (size_t)n;
			continue;
		}

		/* reverse video marks the selection (its bounds fall on rune
		 * edges, so a whole rune is in or out); syntax colors the fg */
		rev = (hl_start < hl_end && col >= hl_start && col < hl_end);
		attrs = rev ? ATTR_REVERSE : 0;
		if (sty && pal && sty[i] < npal) {
			fg = pal[sty[i]];
			if (pal_attr && !rev)
				attrs |= pal_attr[sty[i]];
		} else {
			fg = base_fg;
		}

		if (r == '\t' || r < 0x20 || r == 0x7f) {
			/* render as spaces, clipped at both edges; a hard tab's
			 * first column shows the dim guide glyph when enabled */
			int c;

			for (c = 0; c < w; c++) {
				uint32_t ch = ' ';
				uint16_t a = attrs;

				if (col + c < left)
					continue;
				if (drawn >= width)
					break;
				if (r == '\t' && tabmark && c == 0) {
					ch = tabmark;
					if (!rev)
						a |= ATTR_DIM;
				}
				scr_cell(d, row, col0 + drawn, ch, fg, base_bg, a);
				drawn++;
			}
		} else if (col < left) {
			/* a wide rune straddling the left edge: pad */
			int c;

			for (c = 0; c < w && drawn < width; c++) {
				scr_cell(d, row, col0 + drawn, ' ', fg, base_bg,
				    attrs);
				drawn++;
			}
		} else if (drawn + w > width) {
			break;			/* would overflow right edge */
		} else if (w > 0) {
			scr_cell(d, row, col0 + drawn, r, fg, base_bg, attrs);
			drawn += w;
		}
		col += w;
		i += (size_t)n;
	}
	while (drawn < width) {
		/* highlight blank cells too, so a block selection or a selection
		 * that runs past a short line still shows as a solid rectangle */
		int dcol = left + drawn;
		int rev = (hl_start < hl_end && dcol >= hl_start && dcol < hl_end);

		scr_cell(d, row, col0 + drawn, ' ', base_fg, base_bg,
		    rev ? ATTR_REVERSE : 0);
		drawn++;
	}
}

/* Display column of the cursor, honoring draw-mode virtual space: a cursor past
 * the end of a line (or on a virtual row below the buffer) sits that many blank
 * columns further right. */
static int
cursor_dispcol(const Editor *e)
{
	size_t len = 0;
	const char *line;

	if (e->cy >= text_lines(e->t))
		return (int)e->cx;		/* virtual row: all blanks */
	line = text_line(e->t, e->cy, &len);
	if (!line)
		return (int)e->cx;
	if (e->cx <= len)
		return disp_cols(line, e->cx);
	return disp_cols(line, len) + (int)(e->cx - len);
}

/* Adjust top/left so the cursor stays on screen. */
static void
scroll_to_cursor(Editor *e, int text_h, int text_w)
{
	int cur_col = cursor_dispcol(e);

	if (e->cy < e->top)
		e->top = e->cy;
	if (text_h > 0 && e->cy >= e->top + (size_t)text_h)
		e->top = e->cy - (size_t)text_h + 1;

	if ((size_t)cur_col < e->left)
		e->left = (size_t)cur_col;
	if (text_w > 0 && (size_t)cur_col >= e->left + (size_t)text_w)
		e->left = (size_t)cur_col - (size_t)text_w + 1;
}

/* Order the anchor and cursor so (*y1,*x1) is the start of the selection and
 * (*y2,*x2) the end. */
static void
sel_bounds(const Editor *e, size_t *y1, size_t *x1,
    size_t *y2, size_t *x2)
{
	if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
		*y1 = e->ay;
		*x1 = e->ax;
		*y2 = e->cy;
		*x2 = e->cx;
	} else {
		*y1 = e->cy;
		*x1 = e->cx;
		*y2 = e->ay;
		*x2 = e->ax;
	}
}

/* Fill row [col, col+width) with a color, then write s over the start of it,
 * so the text sits in a uniformly colored field. */
static void
ui_field(Screen *d, int row, int col, int width, const char *s,
    Color fg, Color bg, uint16_t attrs)
{
	scr_fill(d, row, col, width, ' ', fg, bg, attrs);
	scr_text(d, row, col, s, fg, bg, attrs);
}

/* Prompt for a byte offset (hex, an optional 0x accepted) and move the cursor
 * there. */
static void
hex_goto(Editor *e)
{
	char buf[32];
	size_t total = hex_total(e->t), off;

	buf[0] = '\0';
	if (!dlg_prompt_line(e, "Go to offset: ", buf, sizeof(buf)))
		return;
	off = (size_t)strtoull(buf, NULL, 16);
	if (total == 0)
		off = 0;
	else if (off >= total)
		off = total - 1;
	hex_pos_at(e->t, off, &e->cy, &e->cx);
}

/* The value 0-15 of a hex digit, or -1 if c is not one. */
static int
hex_digit(uint32_t c)
{
	if (c >= '0' && c <= '9')
		return (int)(c - '0');
	if (c >= 'a' && c <= 'f')
		return (int)(c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (int)(c - 'A' + 10);
	return -1;
}

/* Overwrite the byte under the cursor with v, as one undo step. Refuses when
 * the cursor is on an implied newline or v would introduce one, since changing
 * the line structure is deferred. Returns 1 when a byte was written. */
static int
hex_overwrite(Editor *e, unsigned char v)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	size_t total = hex_total(e->t);
	size_t cy, cx, len;
	char b = (char)v;

	if (off >= total) {		/* an empty buffer has nothing to edit */
		set_status(e, "no byte to overwrite");
		return 0;
	}
	hex_pos_at(e->t, off, &cy, &cx);
	len = text_line_len(e->t, cy);
	if (cx >= len || v == '\n') {
		set_status(e,
		    "newline bytes are structural (not editable yet)");
		return 0;
	}
	text_undo_group_begin(e->t);
	text_delete(e->t, cy, cx, 1);
	text_insert(e->t, cy, cx, &b, 1);
	text_undo_group_end(e->t);
	e->cy = cy;
	e->cx = cx;
	e->hl_valid = 0;		/* the text view must recolor on return */
	return 1;
}

/* Move the cursor to the next byte after an overwrite, clamped to the end. */
static void
hex_advance(Editor *e)
{
	size_t total = hex_total(e->t);
	size_t off = hex_offset_of(e->t, e->cy, e->cx) + 1;

	if (total == 0)
		return;
	if (off >= total)
		off = total - 1;
	hex_pos_at(e->t, off, &e->cy, &e->cx);
}

/* Insert the byte v at the cursor, as one undo step, then land on the byte
 * after it. A newline byte splits the line, growing the buffer by a line;
 * any other byte is inserted into the current line. Inserting at an implied
 * newline (cx == line length) appends to the end of the line, which is what
 * the offset there means. */
static void
hex_insert_byte(Editor *e, unsigned char v)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	size_t cy, cx;
	char b = (char)v;

	hex_pos_at(e->t, off, &cy, &cx);
	text_undo_group_begin(e->t);
	if (v == '\n')
		text_split(e->t, cy, cx);
	else
		text_insert(e->t, cy, cx, &b, 1);
	text_undo_group_end(e->t);
	e->hl_valid = 0;		/* the text view must recolor on return */
	hex_pos_at(e->t, off + 1, &e->cy, &e->cx);
}

/* Delete the byte under the cursor, as one undo step. A data byte is removed
 * from its line; an implied interior newline joins the following line onto
 * this one. The sole trailing newline of the whole buffer is left alone: it
 * is the final_newline flag, which the structural keys do not touch. Returns
 * 1 when a byte was removed. */
static int
hex_delete_at(Editor *e)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	size_t total = hex_total(e->t);
	size_t cy, cx, len, nlines;

	if (off >= total) {
		set_status(e, "no byte to delete");
		return 0;
	}
	hex_pos_at(e->t, off, &cy, &cx);
	len = text_line_len(e->t, cy);
	nlines = text_lines(e->t);
	if (cx < len) {
		text_undo_group_begin(e->t);
		text_delete(e->t, cy, cx, 1);
		text_undo_group_end(e->t);
	} else if (cy + 1 < nlines) {	/* an implied newline: join the lines */
		text_undo_group_begin(e->t);
		text_join(e->t, cy);
		text_undo_group_end(e->t);
	} else {
		set_status(e,
		    "the trailing newline is not a deletable byte");
		return 0;
	}
	e->hl_valid = 0;
	total = hex_total(e->t);
	if (total == 0) {
		e->cy = e->cx = 0;
	} else {
		if (off >= total)
			off = total - 1;
		hex_pos_at(e->t, off, &e->cy, &e->cx);
	}
	return 1;
}

/* Delete the byte before the cursor (Backspace): step back one offset and
 * delete there, which leaves the cursor on the byte that shifted into place. */
static void
hex_delete_prev(Editor *e)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);

	if (off == 0)
		return;
	hex_pos_at(e->t, off - 1, &e->cy, &e->cx);
	hex_delete_at(e);
}

/* Route a typed byte to the insert or overwrite editor, per the current mode,
 * and advance past it. Overwrite refuses structural positions; insert accepts
 * them (a newline byte splits the line). */
static void
hex_put(Editor *e, unsigned char v)
{
	if (e->hex_insert)
		hex_insert_byte(e, v);
	else if (hex_overwrite(e, v))
		hex_advance(e);
}

/* Repeat the stored search in direction dir, moving the cursor to the match. */
static void
hex_do_search(Editor *e, int dir)
{
	size_t from = hex_offset_of(e->t, e->cy, e->cx);
	size_t found;

	if (e->hex_pat_len == 0) {
		set_status(e, "no previous search");
		return;
	}
	if (hex_find(e->t, e->hex_pat, e->hex_pat_len, from, dir, &found)) {
		hex_pos_at(e->t, found, &e->cy, &e->cx);
		set_status(e, "found at %08zx", found);
	} else {
		set_status(e, "pattern not found");
	}
}

/* Prompt for a search pattern and jump to the first match. When ascii is set
 * the query's bytes are searched literally; otherwise it is parsed as hex byte
 * pairs (for example "0a 0d"). The pattern is remembered for n and N. */
static void
hex_search_prompt(Editor *e, int ascii)
{
	char buf[160];

	buf[0] = '\0';
	e->hex_pending = -1;
	if (ascii) {
		size_t len;

		if (!dlg_prompt_line(e, "Find text: ", buf, sizeof(buf)))
			return;
		len = strlen(buf);
		if (len == 0)
			return;
		if (len > sizeof(e->hex_pat))
			len = sizeof(e->hex_pat);
		memcpy(e->hex_pat, buf, len);
		e->hex_pat_len = len;
	} else {
		unsigned char pat[sizeof(e->hex_pat)];
		size_t plen;

		if (!dlg_prompt_line(e, "Find hex bytes: ", buf, sizeof(buf)))
			return;
		if (hex_parse_bytes(buf, pat, sizeof(pat), &plen) != 0 ||
		    plen == 0) {
			set_status(e,
			    "enter hex byte pairs, e.g. 0a 0d");
			return;
		}
		memcpy(e->hex_pat, pat, plen);
		e->hex_pat_len = plen;
	}
	e->hex_pat_dir = 1;
	hex_do_search(e, 1);
}

/* The inclusive byte range the selection covers, low to high. With no active
 * selection this is just the byte under the cursor. */
static void
hex_sel_range(Editor *e, size_t *lo, size_t *hi)
{
	size_t cur = hex_offset_of(e->t, e->cy, e->cx);

	if (e->hex_sel && e->hex_anchor < cur) {
		*lo = e->hex_anchor;
		*hi = cur;
	} else if (e->hex_sel) {
		*lo = cur;
		*hi = e->hex_anchor;
	} else {
		*lo = *hi = cur;
	}
}

/* Copy the selected byte range (or the single byte under the cursor) to the
 * clipboard, which also mirrors to the system clipboard, then clear the
 * selection. */
static void
hex_yank(Editor *e)
{
	size_t total = hex_total(e->t), lo, hi, n;
	unsigned char *buf;

	if (total == 0) {
		set_status(e, "nothing to yank");
		e->hex_sel = 0;
		return;
	}
	hex_sel_range(e, &lo, &hi);
	if (hi >= total)
		hi = total - 1;
	if (lo > hi)
		lo = hi;
	n = hi - lo + 1;
	buf = malloc(n);
	if (buf == NULL) {
		set_status(e, "out of memory");
		return;
	}
	hex_gather(e->t, lo, buf, n);
	clip_set(e, (char *)buf, n);
	e->clip_linewise = 0;
	e->hex_sel = 0;
	set_status(e, "yanked %zu byte%s", n,
	    n == 1 ? "" : "s");
}

/* Insert the clipboard bytes at the cursor as one undo step. Newlines in the
 * clipboard split lines, matching how the hex view treats a 0a byte. */
static void
hex_paste(Editor *e)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);

	if (e->clip == NULL || e->clip_len == 0) {
		set_status(e, "clipboard is empty");
		return;
	}
	hex_pos_at(e->t, off, &e->cy, &e->cx);	/* land on a real position */
	text_undo_group_begin(e->t);
	insert_bytes(e, e->clip, e->clip_len);	/* advances the cursor past it */
	text_undo_group_end(e->t);
	e->hl_valid = 0;
	e->hex_sel = 0;
	set_status(e, "pasted %zu byte%s", e->clip_len,
	    e->clip_len == 1 ? "" : "s");
}

/* One key in the hex view. Arrows and paging move the cursor byte; Tab switches
 * the hex and ascii sub-columns; Insert toggles overwrite and insert modes;
 * Delete removes the byte under the cursor and Backspace the one before it;
 * typed hex digits (in the hex column) or printable characters (in the ascii
 * column) overwrite or insert the byte at the cursor per the current mode; g
 * prompts for an offset; / and \ search for a text or a hex-byte pattern and n
 * or N repeat it; w cycles the row width and i toggles the data inspector; v
 * starts or clears a byte selection, y yanks it (or the byte under the cursor)
 * and p pastes the clipboard; and q or Esc returns to the text view (Esc first
 * clears an active selection). Ctrl-S and Ctrl-Q request a save and a quit,
 * which the main loop carries out through the same handling as the text view.
 * The command letters act in the hex column, where they are not byte data.
 * Returns the request for the caller to act on, usually REQ_CONTINUE. */
static Req
hex_key(Editor *e, const struct tkbd_seq *seq)
{
	size_t total = hex_total(e->t);
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	uint32_t ch;
	size_t pg, cols = e->hex_cols ? (size_t)e->hex_cols : 16;
	int page = e->rows - 2, moved = 1;

	if (seq->type != TKBD_KEY)
		return REQ_CONTINUE;
	if (page < 1)
		page = 1;
	pg = (size_t)page * cols;

	/* Save and quit go through the editor's shared request handling, so
	 * the hex view saves and quits exactly as the text view does. */
	if ((seq->mod & TKBD_MOD_CTRL) && seq->key == TKBD_KEY_S) {
		e->hex_pending = -1;
		return REQ_SAVE;
	}
	if ((seq->mod & TKBD_MOD_CTRL) && seq->key == TKBD_KEY_Q) {
		e->hex_pending = -1;
		return REQ_QUIT;
	}

	switch (seq->key) {
	case TKBD_KEY_LEFT:	off = off ? off - 1 : 0; break;
	case TKBD_KEY_RIGHT:	off++; break;
	case TKBD_KEY_UP:	if (off >= cols) off -= cols; break;
	case TKBD_KEY_DOWN:	off += cols; break;
	case TKBD_KEY_PGUP:	off = off >= pg ? off - pg : off % cols; break;
	case TKBD_KEY_PGDN:	off += pg; break;
	case TKBD_KEY_HOME:	off = 0; break;
	case TKBD_KEY_END:	off = total ? total - 1 : 0; break;
	case TKBD_KEY_TAB:
		e->hex_ascii = !e->hex_ascii;
		e->hex_pending = -1;
		return REQ_CONTINUE;
	case TKBD_KEY_INS:
		e->hex_pending = -1;
		e->hex_insert = !e->hex_insert;
		return REQ_CONTINUE;
	case TKBD_KEY_DEL:
		e->hex_pending = -1;
		hex_delete_at(e);
		return REQ_CONTINUE;
	case TKBD_KEY_BACKSPACE:
	case TKBD_KEY_BACKSPACE2:
		e->hex_pending = -1;
		hex_delete_prev(e);
		return REQ_CONTINUE;
	case TKBD_KEY_ESC:
		if (e->hex_sel) {	/* first Esc drops the selection */
			e->hex_sel = 0;
			e->hex_pending = -1;
			return REQ_CONTINUE;
		}
		e->hex_view = 0;
		return REQ_CONTINUE;
	default:
		moved = 0;
		break;
	}
	if (moved) {			/* any move discards a half-typed byte */
		e->hex_pending = -1;
		if (total == 0)
			off = 0;
		else if (off >= total)
			off = total - 1;
		hex_pos_at(e->t, off, &e->cy, &e->cx);
		return REQ_CONTINUE;
	}

	ch = seq->ch;
	if (e->hex_ascii) {		/* the ascii column takes any printable */
		if (ch >= 0x20 && ch < 0x7f)
			hex_put(e, (unsigned char)ch);
		return REQ_CONTINUE;
	}

	/* the hex column: two digits make a byte; the letters are commands here */
	if (hex_digit(ch) >= 0) {
		if (e->hex_pending < 0) {
			e->hex_pending = hex_digit(ch);
		} else {
			unsigned char v = (unsigned char)
			    ((e->hex_pending << 4) | hex_digit(ch));

			e->hex_pending = -1;
			hex_put(e, v);
		}
		return REQ_CONTINUE;
	}
	if (ch == 'q' || ch == 'Q') {
		e->hex_view = 0;
		return REQ_CONTINUE;
	}
	if (ch == 'g' || ch == 'G') {
		e->hex_pending = -1;
		hex_goto(e);
		return REQ_CONTINUE;
	}
	if (ch == '/') {
		hex_search_prompt(e, 1);
		return REQ_CONTINUE;
	}
	if (ch == '\\') {
		hex_search_prompt(e, 0);
		return REQ_CONTINUE;
	}
	if (ch == 'n') {
		e->hex_pending = -1;
		hex_do_search(e, e->hex_pat_dir);
		return REQ_CONTINUE;
	}
	if (ch == 'N') {
		e->hex_pending = -1;
		hex_do_search(e, -e->hex_pat_dir);
		return REQ_CONTINUE;
	}
	if (ch == 'w') {		/* cycle the row width 8 -> 16 -> 32 */
		e->hex_pending = -1;
		e->hex_cols = e->hex_cols == 8 ? 16 :
		    e->hex_cols == 16 ? 32 : 8;
		set_status(e, "%d bytes per row",
		    e->hex_cols);
		return REQ_CONTINUE;
	}
	if (ch == 'i') {		/* toggle the data-inspector footer */
		e->hex_pending = -1;
		e->hex_inspect = !e->hex_inspect;
		set_status(e, "inspector %s",
		    e->hex_inspect ? "on" : "off");
		return REQ_CONTINUE;
	}
	if (ch == 'v') {		/* start or clear a byte selection */
		e->hex_pending = -1;
		if (e->hex_sel) {
			e->hex_sel = 0;
			set_status(e,
			    "selection cleared");
		} else {
			e->hex_sel = 1;
			e->hex_anchor = off;
			set_status(e,
			    "selecting from %08zx", off);
		}
		return REQ_CONTINUE;
	}
	if (ch == 'y') {		/* yank the selection or the cursor byte */
		e->hex_pending = -1;
		hex_yank(e);
		return REQ_CONTINUE;
	}
	if (ch == 'p') {		/* paste the clipboard bytes at the cursor */
		e->hex_pending = -1;
		hex_paste(e);
	}
	return REQ_CONTINUE;
}

/* Draw the buffer as a hex dump: a menu bar, offset/hex/ascii rows following
 * the cursor byte, and a status line with the offset and total size. */
static void
hex_render(Editor *e, Screen *d)
{
	const Pal *p = ed_chrome(e);
	uint16_t barat = p->reverse_bars ? ATTR_REVERSE : 0;
	int content_h = e->rows - 2 - (e->hex_inspect ? 1 : 0);
	size_t cols = e->hex_cols ? (size_t)e->hex_cols : 16;
	size_t total = hex_total(e->t);
	size_t curoff = hex_offset_of(e->t, e->cy, e->cx);
	size_t currow = curoff / cols;
	size_t sello = 0, selhi = 0;
	int i, curj = (int)(curoff % cols);
	char st[160];

	if (content_h < 1)
		content_h = 1;
	if (currow < e->hex_top)
		e->hex_top = currow;
	else if (currow >= e->hex_top + (size_t)content_h)
		e->hex_top = currow - (size_t)content_h + 1;

	if (e->hex_sel)
		hex_sel_range(e, &sello, &selhi);

	scr_clear(d);
	for (i = 0; i < content_h; i++) {
		size_t rowoff = (e->hex_top + (size_t)i) * cols;
		unsigned char bytes[32];
		char line[192];
		size_t got, j;

		if (rowoff >= total && !(rowoff == 0 && total == 0)) {
			ui_field(d, 1 + i, 0, e->cols, "", p->content_fg,
			    p->content_bg, 0);
			continue;
		}
		got = hex_gather(e->t, rowoff, bytes, cols);
		hex_format_row(line, sizeof(line), rowoff, bytes, got, (int)cols);
		ui_field(d, 1 + i, 0, e->cols, line, p->content_fg,
		    p->content_bg, 0);
		for (j = 0; e->hex_sel && j < got; j++) {	/* selected bytes */
			size_t boff = rowoff + j;
			char hp[3], ac[2];
			int hc;

			if (boff < sello || boff > selhi)
				continue;
			hc = hex_hexcol((int)j);
			hp[0] = line[hc];
			hp[1] = line[hc + 1];
			hp[2] = '\0';
			ac[0] = line[hex_asciicol((int)j, (int)cols)];
			ac[1] = '\0';
			ui_field(d, 1 + i, hc, 2, hp, p->content_fg,
			    p->content_bg, ATTR_REVERSE);
			ui_field(d, 1 + i, hex_asciicol((int)j, (int)cols), 1,
			    ac, p->content_fg, p->content_bg, ATTR_REVERSE);
		}
		if (e->hex_top + (size_t)i == currow && (size_t)curj < got) {
			char hp[3], ac[2];
			int hc = hex_hexcol(curj);

			hp[0] = e->hex_pending >= 0 && !e->hex_ascii ?
			    "0123456789abcdef"[e->hex_pending] : line[hc];
			hp[1] = line[hc + 1];
			hp[2] = '\0';
			ac[0] = line[hex_asciicol(curj, (int)cols)];
			ac[1] = '\0';
			ui_field(d, 1 + i, hc, 2, hp, p->content_fg,
			    p->content_bg, ATTR_REVERSE);
			ui_field(d, 1 + i, hex_asciicol(curj, (int)cols), 1, ac,
			    p->content_fg, p->content_bg, ATTR_REVERSE);
		}
	}

	if (e->hex_inspect) {		/* decode the bytes under the cursor */
		unsigned char ins[4];
		size_t got = hex_gather(e->t, curoff, ins, sizeof(ins));
		char line[160];

		hex_inspect_line(line, sizeof(line), ins, got);
		scr_fill(d, e->rows - 2, 0, e->cols, ' ', p->bar_fg, p->bar_bg,
		    barat);
		scr_text(d, e->rows - 2, 1, line, p->bar_fg, p->bar_bg, barat);
	}

	ui_menubar(e, p, -1);
	if (e->status[0])		/* a transient message (search, errors) */
		snprintf(st, sizeof(st),
		    " HEX%s  %08zx / %08zx  [%s %s]  %.60s",
		    text_dirty(e->t) ? "*" : "", curoff, total,
		    e->hex_ascii ? "ascii" : "hex",
		    e->hex_insert ? "INS" : "OVR", e->status);
	else if (e->hex_sel)		/* a live selection extent */
		snprintf(st, sizeof(st),
		    " HEX%s  %08zx / %08zx  [%s %s]  SEL %08zx-%08zx (%zu)",
		    text_dirty(e->t) ? "*" : "", curoff, total,
		    e->hex_ascii ? "ascii" : "hex",
		    e->hex_insert ? "INS" : "OVR", sello, selhi,
		    selhi - sello + 1);
	else
		snprintf(st, sizeof(st),
		    " HEX%s  %08zx / %08zx  [%s %s]  %.22s  (Tab, Ins, /, w, q)",
		    text_dirty(e->t) ? "*" : "", curoff, total,
		    e->hex_ascii ? "ascii" : "hex",
		    e->hex_insert ? "INS" : "OVR",
		    e->has_name ? e->path : "[No Name]");
	scr_fill(d, e->rows - 1, 0, e->cols, ' ', p->bar_fg, p->bar_bg, barat);
	scr_text(d, e->rows - 1, 1, st, p->bar_fg, p->bar_bg, barat);

	scr_cursor_shape(d, CURSOR_DEFAULT);
	scr_cursor_vis(d, 1);
	if (currow >= e->hex_top && currow < e->hex_top + (size_t)content_h)
		scr_cursor(d, 1 + (int)(currow - e->hex_top),
		    e->hex_ascii ? hex_asciicol(curj, (int)cols) : hex_hexcol(curj));
}

/* Selection highlight bounds for one buffer line, as display columns [*hs,*he)
 * within the line, or -1/-1 when nothing on the line is selected. Shared by the
 * plain and soft-wrap renderers. */
static void
sel_cols(const Editor *e, size_t idx, const char *s, size_t llen,
    int *hs, int *he)
{
	*hs = -1;
	*he = -1;
	if (e->sel_active && e->sel_block) {
		size_t ry1 = e->ay < e->cy ? e->ay : e->cy;
		size_t ry2 = e->ay > e->cy ? e->ay : e->cy;
		size_t rx1 = e->ax < e->cx ? e->ax : e->cx;
		size_t rx2 = e->ax > e->cx ? e->ax : e->cx;

		if (idx >= ry1 && idx <= ry2) {
			*hs = (int)rx1;
			*he = (int)rx2 + 1;
		}
	} else if (e->sel_active && s) {
		size_t y1, x1, y2, x2;

		sel_bounds(e, &y1, &x1, &y2, &x2);
		if (idx >= y1 && idx <= y2) {
			size_t a = (idx == y1) ? x1 : 0;
			size_t b = (idx == y2) ? x2 : llen;

			if (e->vi_visual == 'V') {
				a = 0;
				b = llen;
			} else if (e->vi_visual == 'v' && idx == y2 && b < llen) {
				b += rune_len_at(s, llen, b);
			}
			*hs = disp_cols(s, a);
			*he = disp_cols(s, b);
		}
	}
}

/* One soft-wrap step. For the segment of line s (llen bytes) that starts at byte
 * `a`, whose first column is `acol`, and a wrap width of W display columns:
 * return the byte at which to stop drawing (a trailing break space is left out),
 * set *next to the first byte of the following segment (past the break spaces),
 * and *nextcol to its first display column. Breaks at the last space that fits,
 * or mid-rune when a single word is wider than W. */
static size_t
wrap_next(const char *s, size_t llen, size_t a, int acol, int W,
    size_t *next, int *nextcol)
{
	size_t i = a, sp = (size_t)-1;
	int col = acol, spcol = 0;

	while (i < llen) {
		uint32_t r;
		int n = utf8_decode(&r, (const unsigned char *)s + i, llen - i);
		int w;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if ((w = rune_width(r)) < 1)
			w = 1;
		if (col - acol + w > W && i > a) {
			if (sp != (size_t)-1) {	/* break at the last space */
				size_t ns = sp;

				while (ns < llen && s[ns] == ' ')
					ns++;
				*next = ns;
				*nextcol = spcol + (int)(ns - sp);
				return sp;
			}
			*next = i;		/* a word wider than the window */
			*nextcol = col;
			return i;
		}
		if (r == ' ') {
			sp = i;
			spcol = col;
		}
		col += w;
		i += (size_t)n;
	}
	*next = llen;
	*nextcol = col;
	return llen;
}

/* Number of screen rows a buffer line occupies at wrap width W (at least one). */
static int
line_rows(const char *s, size_t llen, int W)
{
	size_t a = 0;
	int acol = 0, rows = 0;

	if (llen == 0)
		return 1;
	while (a < llen) {
		size_t next;
		int nextcol;

		wrap_next(s, llen, a, acol, W, &next, &nextcol);
		rows++;
		if (next <= a)
			break;
		a = next;
		acol = nextcol;
	}
	return rows ? rows : 1;
}

/* Which wrap segment (0-based) of line s holds byte offset cx. */
static int
seg_index_of(const char *s, size_t llen, size_t cx, int W)
{
	size_t a = 0;
	int acol = 0, seg = 0;

	while (a < llen) {
		size_t next;
		int nextcol;

		wrap_next(s, llen, a, acol, W, &next, &nextcol);
		if (cx < next || next >= llen || next <= a)
			break;
		a = next;
		acol = nextcol;
		seg++;
	}
	return seg;
}

/* scroll_to_cursor for soft-wrap mode: keep the cursor's wrapped row on screen
 * by dropping whole buffer lines from the top. The view always starts at a
 * buffer-line boundary, and horizontal scrolling is off. */
static void
scroll_to_cursor_wrap(Editor *e, int text_h, int W)
{
	e->left = 0;
	if (e->cy < e->top)
		e->top = e->cy;
	for (;;) {
		int rows = 0;
		size_t idx;

		for (idx = e->top; idx < e->cy; idx++) {
			size_t l = 0;
			const char *s = text_line(e->t, idx, &l);

			rows += s ? line_rows(s, l, W) : 1;
		}
		{
			size_t l = 0;
			const char *s = text_line(e->t, e->cy, &l);

			rows += s ? seg_index_of(s, l, e->cx, W) : 0;
		}
		if (rows < text_h || e->top >= e->cy)
			break;
		e->top++;		/* drop a whole line from the top */
	}
}

/* Resolve the syntax palette for the active language: the FSM's per-class colors
 * and attributes. With no active grammar the palette is empty, and scr_line
 * leaves every cell at the base color. */
static void
syn_palette(const Editor *e, const Screen *d, const Color **pal,
    const uint16_t **pa, int *np)
{
	(void)d;
	if (e->hl_on && e->syn && e->syn->fsm) {
		*pal = e->syn->fsm->fg;
		*pa = e->syn->fsm->attr;
		*np = e->syn->fsm->nclasses;
	} else {
		*pal = NULL;
		*pa = NULL;
		*np = 0;
	}
}

/* Paint the text area with soft wrap: each buffer line flows across as many
 * screen rows as it needs. Returns the cursor's screen row/col through the out
 * params (col0-based column). */
static void
render_body_wrapped(Editor *e, Screen *d, const Pal *p, int text_h,
    int text_w, int gutter, int col0, int *cur_row, int *cur_col)
{
	int i = 0;
	size_t idx = e->top;
	const Color *pal;
	const uint16_t *pa;
	int np;

	syn_palette(e, d, &pal, &pa, &np);
	*cur_row = 0;
	*cur_col = col0;
	while (i < text_h) {
		size_t llen = 0;
		const char *s = text_line(e->t, idx, &llen);
		const uint8_t *sty;
		int hs, he, row = CHROME_TOP + i;
		size_t a;
		int acol, seg;

		if (!s) {			/* a virtual row past the last line */
			render_gutter(d, e, row, gutter, idx, 0, p->content_fg,
			    p->content_bg);
			scr_line(d, row, col0, "", 0, 0, text_w, -1, -1, NULL,
			    pal, pa, np, p->content_fg, p->content_bg,
			    e->show_tabs);
			if (idx == e->cy) {
				*cur_row = i;
				*cur_col = col0;
			}
			idx++;
			i++;
			continue;
		}

		sel_cols(e, idx, s, llen, &hs, &he);
		sty = hl_line(e, idx, s, llen);

		if (llen == 0) {		/* an empty line is one blank row */
			render_gutter(d, e, row, gutter, idx, 1, p->content_fg,
			    p->content_bg);
			scr_line(d, row, col0, "", 0, 0, text_w, hs, he, NULL,
			    pal, pa, np, p->content_fg, p->content_bg,
			    e->show_tabs);
			if (idx == e->cy) {
				*cur_row = i;
				*cur_col = col0;
			}
			idx++;
			i++;
			continue;
		}

		a = 0;
		acol = 0;
		seg = 0;
		while (a < llen && i < text_h) {
			size_t next, end;
			int nextcol;

			end = wrap_next(s, llen, a, acol, text_w, &next, &nextcol);
			if (end <= a)
				end = a + 1;		/* guarantee progress */
			row = CHROME_TOP + i;
			render_gutter(d, e, row, gutter, idx, seg == 0,
			    p->content_fg, p->content_bg);
			scr_line(d, row, col0, s, end, acol, text_w, hs, he,
			    sty, pal, pa, np, p->content_fg, p->content_bg,
			    e->show_tabs);
			if (idx == e->cy && (e->cx < next || next >= llen)) {
				int cc = disp_cols(s, e->cx) - acol;

				if (cc < 0)
					cc = 0;
				if (cc > text_w)
					cc = text_w;
				*cur_row = i;
				*cur_col = col0 + cc;
			}
			seg++;
			a = next;
			acol = nextcol;
			i++;
		}
		idx++;
	}
}

static void
render_body(Editor *e, Screen *d)
{
	const Pal *p = ed_chrome(e);
	int text_h = text_height(e);
	int gutter = gutter_width(e);
	int text_w = text_width(e) - gutter;
	int col0 = CHROME_LEFT + gutter;
	int wrap = e->wrap && !e->draw_mode;
	int i;
	size_t len = 0;
	const char *cur = text_line(e->t, e->cy, &len);
	int cur_col, cur_row = 0, cur_scol = col0;

	if (text_w < 1)
		text_w = 1;

	if (e->hex_view) {
		e->prev_text_view = 0;	/* hex uses hex_top, not e->top */
		hex_render(e, d);
		return;
	}

	if (wrap)
		scroll_to_cursor_wrap(e, text_h, text_w);
	else
		scroll_to_cursor(e, text_h, text_w);
	cur_col = cursor_dispcol(e);
	(void)cur;

	/* When only the vertical offset moved by a few lines, scroll the text
	 * region in the terminal instead of repainting every row; scr_present
	 * then paints just the newly exposed lines. Gated on t->scroll (VT100
	 * scroll region) and on a valid, like-for-like previous text frame.
	 * Disabled under soft wrap, where a line spans a variable row count. */
	if (!wrap && e->term->scroll && e->term->shadow_valid &&
	    e->prev_text_view && e->left == e->prev_left &&
	    e->top != e->prev_top) {
		long dv = (long)e->top - (long)e->prev_top;

		if (dv > -text_h && dv < text_h)
			scr_scroll(d, CHROME_TOP, text_h, (int)dv);
	}

	hl_ensure(e, e->top + (size_t)text_h);

	scr_clear(d);

	if (wrap) {
		render_body_wrapped(e, d, p, text_h, text_w, gutter, col0,
		    &cur_row, &cur_scol);
	} else {
		const Color *pal;
		const uint16_t *pa;
		int np;

		syn_palette(e, d, &pal, &pa, &np);
		for (i = 0; i < text_h; i++) {
			size_t idx = e->top + (size_t)i;
			size_t llen = 0;
			const char *s = text_line(e->t, idx, &llen);
			int hs, he, row = CHROME_TOP + i;
			const uint8_t *sty = NULL;

			sel_cols(e, idx, s, llen, &hs, &he);
			render_gutter(d, e, row, gutter, idx, 1, p->content_fg,
			    p->content_bg);
			if (s) {
				sty = hl_line(e, idx, s, llen);
				scr_line(d, row, col0, s, llen, (int)e->left,
				    text_w, hs, he, sty, pal, pa, np,
				    p->content_fg, p->content_bg, e->show_tabs);
			} else {
				scr_line(d, row, col0, "", 0, (int)e->left,
				    text_w, hs, he, NULL, pal, pa, np,
				    p->content_fg, p->content_bg, e->show_tabs);
			}
		}
		cur_row = (int)(e->cy - e->top);
		cur_scol = col0 + cur_col - (int)e->left;
	}

	ui_menubar(e, p, -1);
	ui_frame(e, p);
	ui_statusbar(e, p, cur_col);

	/* cursor shape follows the mode: a block in normal mode, a bar while
	 * inserting; leave the modeless editor's cursor at its default */
	if (e->mode == MODE_NORMAL)
		scr_cursor_shape(d, CURSOR_BLOCK);
	else if (e->mode == MODE_INSERT)
		scr_cursor_shape(d, CURSOR_BAR);
	else
		scr_cursor_shape(d, CURSOR_DEFAULT);

	scr_cursor_vis(d, 1);		/* a menu overlay may have hidden it */
	scr_cursor(d, CHROME_TOP + cur_row, cur_scol);

	e->prev_top = e->top;		/* for the next frame's scroll decision */
	e->prev_left = e->left;
	e->prev_text_view = 1;
}

static void
ed_render(Editor *e, Screen *d)
{
	render_body(e, d);
	scr_present(d);
}

/* Geometry and palette of a centered modal overlay, handed to its draw and
 * key callbacks each frame. */
typedef struct modal {
	int		x, y, w, h;
	Color	fg, bg;
	uint16_t	base;
} Modal;

/* Run a centered modal box of w by h until its key handler closes it. draw
 * paints the box interior each frame; on_key handles one input event and
 * returns nonzero to close. Both receive the box geometry and palette, plus
 * the caller's ctx. EOF and the editor frame behind the box are handled here.
 */
static void
dlg_run(Editor *e, int w, int h, void *ctx,
    void (*draw)(Editor *, const Modal *, void *),
    int (*on_key)(Editor *, const Modal *,
        const Event *, void *))
{
	Modal m;

	dlg_palette(e, &m.fg, &m.bg, &m.base);
	m.w = w > e->cols ? e->cols : w;
	m.h = h > e->rows ? e->rows : h;
	dlg_center(e, m.w, m.h, &m.x, &m.y);

	for (;;) {
		Event ev;

		render_body(e, e->d);
		scr_box(e->d, m.x, m.y, m.w, m.h, m.fg, m.bg, m.base);
		scr_cursor_vis(e->d, 0);	/* hidden unless draw shows it */
		draw(e, &m, ctx);
		scr_present(e->d);

		switch (scr_wait(e->d, &ev)) {
		case EVENT_EOF:
			return;
		case EVENT_KEY:
			if (on_key(e, &m, &ev, ctx))
				return;
			break;
		case EVENT_RESIZE:
		case EVENT_RESUME:
			scr_size(e->d, &e->rows, &e->cols);
			dlg_center(e, m.w, m.h, &m.x, &m.y);
			break;
		default:
			break;
		}
	}
}

/****************************************************************
 * Reusable list picker
 *
 * A modal panel that shows a scrollable list and lets the user pick a row.
 * The panel owns the box, scrolling, selection and keys; a Picksrc supplies
 * the rows and decides what choosing one means. The file browser below is the
 * first client; the same control is meant to serve mailbox lists (a mail
 * reader) and module/function lists (a script IDE) later, so it knows nothing
 * about files.
 ****************************************************************/

/* What the panel does after the source handles a chosen row or typed entry. */
enum { PICK_STAY, PICK_DONE, PICK_CANCEL };

typedef struct picksrc Picksrc;
struct picksrc {
	void		*ctx;
	const char	*(*title)(void *ctx);		/* panel title */
	int		 (*count)(void *ctx);		/* number of rows */
	const char	*(*label)(void *ctx, int i);	/* text of row i */
	/* Row i was activated. Return PICK_DONE to close, PICK_CANCEL to
	 * abort, or PICK_STAY to stay open after the source replaced its
	 * listing (for example on descending into a directory). count/label
	 * are re-queried after a PICK_STAY. */
	int		 (*choose)(void *ctx, int i);
	/* Optional entry line. When entry_label is non-NULL the panel shows an
	 * editable field; submit is called with the typed text on Enter and
	 * returns the same tri-state as choose. entry_init pre-fills it, and
	 * focus_entry starts focus there (for a save-style prompt). */
	const char	*entry_label;
	const char	*entry_init;
	int		 focus_entry;
	int		 (*submit)(void *ctx, const char *text);
};

typedef struct picker {
	const Picksrc	*src;
	int		 sel;		/* selected row */
	int		 top;		/* first visible row */
	int		 vis;		/* visible list rows (set by draw) */
	int		 listy;		/* first list row on screen (set by draw) */
	int		 focus;		/* 0 list, 1 entry */
	int		 ecurx;		/* entry cursor column on screen (set by draw) */
	int		 result;	/* 1 chosen, 0 cancelled (set by the key cb) */
	char		 entry[PATH_MAX];
} Picker;

/* Choose a horizontal scroll offset (a byte index into s) so the end of s
 * stays visible within avail columns. Editing the entry only appends or
 * backspaces, so the cursor is always at the end; this hides whole chunks of
 * the left side at a time rather than shifting one column per keystroke, so a
 * long path does not reflow the whole field (and resend it over a slow link)
 * on every key. Returns the byte offset of the first visible rune. */
static int
entry_scroll_off(const char *s, int avail)
{
	size_t i = 0, slen = strlen(s);
	int total = disp_cols(s, slen);
	int hide, chunk, acc = 0;

	if (avail < 1 || total <= avail)
		return 0;
	chunk = avail / 2 < 1 ? 1 : avail / 2;
	hide = total - avail;			/* least we must hide on the left */
	hide = ((hide + chunk - 1) / chunk) * chunk;	/* round up to a jump */
	while (i < slen && acc < hide) {	/* walk past the hidden columns */
		uint32_t cp;
		int n = utf8_decode(&cp, (const unsigned char *)s + i, slen - i);
		int w;

		if (n <= 0)
			n = 1;
		w = rune_width(cp);
		if (w < 1)
			w = 1;
		acc += w;
		i += (size_t)n;
	}
	return (int)i;
}

/* Copy src into dst keeping at most w display columns; return columns used. */
static int
pick_fit(char *dst, size_t dstsz, const char *src, int w)
{
	size_t si = 0, di = 0, slen = strlen(src);
	int col = 0;

	while (si < slen && col < w) {
		uint32_t cp;
		int n = utf8_decode(&cp, (const unsigned char *)src + si,
		    slen - si);
		int cw;

		if (n <= 0)
			n = 1;
		cw = rune_width(cp);
		if (cw < 1)
			cw = 1;
		if (col + cw > w || di + (size_t)n + 1 > dstsz)
			break;
		memcpy(dst + di, src + si, (size_t)n);
		di += (size_t)n;
		si += (size_t)n;
		col += cw;
	}
	dst[di] = '\0';
	return col;
}

/* Keep sel on a valid row and scroll so it stays visible. */
static void
pick_clamp(Picker *pk, int n)
{
	if (pk->sel >= n)
		pk->sel = n - 1;
	if (pk->sel < 0)
		pk->sel = 0;
	if (pk->vis > 0) {
		if (pk->sel < pk->top)
			pk->top = pk->sel;
		if (pk->sel >= pk->top + pk->vis)
			pk->top = pk->sel - pk->vis + 1;
	}
	if (pk->top > n - pk->vis)
		pk->top = n - pk->vis;
	if (pk->top < 0)
		pk->top = 0;
}

/* Jump the selection to the next row whose label starts with ch. */
static void
pick_jump(Picker *pk, int n, int ch)
{
	int i, lc = tolower(ch);

	for (i = 1; i <= n; i++) {
		int r = (pk->sel + i) % n;
		const char *s = pk->src->label(pk->src->ctx, r);

		if (s && tolower((unsigned char)s[0]) == lc) {
			pk->sel = r;
			return;
		}
	}
}

static void
pick_draw(Editor *e, const Modal *m, void *ctx)
{
	Picker *pk = ctx;
	const Picksrc *src = pk->src;
	Screen *d = e->d;
	const char *title = src->title ? src->title(src->ctx) : "";
	int n = src->count(src->ctx);
	int inner = m->w - 4;		/* interior minus borders and one pad */
	int listx = m->x + 2;
	int row, i;
	uint16_t sel_at = m->base ^ ATTR_REVERSE;
	char buf[PATH_MAX];

	/* title centered in the top border */
	if (title && title[0]) {
		int tw = pick_fit(buf, sizeof(buf), title, m->w - 4);

		scr_cell(d, m->y, m->x + (m->w - tw - 2) / 2, ' ',
		    m->fg, m->bg, m->base);
		scr_text(d, m->y, m->x + (m->w - tw - 2) / 2 + 1, buf,
		    m->fg, m->bg, m->base);
		scr_cell(d, m->y, m->x + (m->w - tw - 2) / 2 + 1 + tw, ' ',
		    m->fg, m->bg, m->base);
	}

	row = m->y + 1;
	if (src->entry_label) {
		int lw, avail, used, eoff;

		scr_fill(d, row, m->x + 1, m->w - 2, ' ', m->fg, m->bg, m->base);
		lw = scr_text(d, row, listx, src->entry_label,
		    m->fg, m->bg, m->base);
		avail = m->x + m->w - 2 - lw;	/* columns left for the text */
		if (avail < 1)
			avail = 1;
		eoff = entry_scroll_off(pk->entry, avail);	/* scroll the tail in */
		used = pick_fit(buf, sizeof(buf), pk->entry + eoff, avail);
		scr_text(d, row, lw, buf, m->fg, m->bg, m->base);
		pk->ecurx = lw + used;		/* cursor sits after the text */
		row++;
		scr_fill(d, row, m->x + 1, m->w - 2, GL_H, m->fg, m->bg,
		    m->base);
		row++;
	}

	pk->listy = row;
	pk->vis = m->y + m->h - 1 - row;
	if (pk->vis < 1)
		pk->vis = 1;
	pick_clamp(pk, n);

	for (i = 0; i < pk->vis; i++) {
		int r = pk->top + i;
		uint16_t at = (r == pk->sel && pk->focus == 0) ?
		    sel_at : m->base;

		scr_fill(d, row + i, m->x + 1, m->w - 2, ' ',
		    m->fg, m->bg, at);
		if (r < n) {
			pick_fit(buf, sizeof(buf),
			    src->label(src->ctx, r), inner);
			scr_text(d, row + i, listx, buf, m->fg, m->bg, at);
		}
	}

	if (pk->focus == 1 && src->entry_label) {
		scr_cursor(d, m->y + 1, pk->ecurx);	/* kept within the field */
		scr_cursor_vis(d, 1);
	} else {
		scr_cursor_vis(d, 0);
	}
}

/* Edit the entry line in response to one key. */
static void
pick_entry_key(Picker *pk, const struct tkbd_seq *k)
{
	size_t len = strlen(pk->entry);

	if (k->key == TKBD_KEY_BACKSPACE || k->key == TKBD_KEY_BACKSPACE2) {
		while (len > 0 && ((unsigned char)pk->entry[len - 1] & 0xc0)
		    == 0x80)
			len--;
		if (len > 0)
			len--;
		pk->entry[len] = '\0';
		return;
	}
	if (!(k->mod & TKBD_MOD_CTRL) && k->ch != TKBD_CH_NONE &&
	    k->ch >= 0x20 && k->ch != 0x7f) {
		unsigned char enc[8];
		int el = utf8_encode(enc, k->ch);

		if (el > 0 && len + (size_t)el < sizeof(pk->entry)) {
			memcpy(pk->entry + len, enc, (size_t)el);
			pk->entry[len + (size_t)el] = '\0';
		}
	}
}

/* Handle one picker key. Returns nonzero to close the panel (pk->result says
 * whether a row or entry was chosen); zero to stay open. */
static int
pick_key(Editor *e, const Modal *m, const Event *ev, void *ctx)
{
	Picker *pk = ctx;
	const Picksrc *src = pk->src;
	const struct tkbd_seq *k;
	int act = PICK_STAY;
	int n;

	(void)e;
	(void)m;
	if (ev->key.type != TKBD_KEY)
		return 0;		/* ignore mouse and the like */
	k = &ev->key;
	n = src->count(src->ctx);

	if (k->key == TKBD_KEY_ESC) {
		pk->result = 0;
		return 1;
	}
	if (k->key == TKBD_KEY_TAB && src->entry_label) {
		pk->focus = !pk->focus;
		return 0;
	}
	if (pk->focus == 1) {		/* entry line has focus */
		if (k->key == TKBD_KEY_ENTER) {
			act = src->submit(src->ctx, pk->entry);
		} else if (k->key == TKBD_KEY_UP || k->key == TKBD_KEY_DOWN) {
			pk->focus = 0;
			return 0;
		} else {
			pick_entry_key(pk, k);
			return 0;
		}
	} else {			/* list has focus */
		switch (k->key) {
		case TKBD_KEY_UP:
			pk->sel--;
			pick_clamp(pk, n);
			return 0;
		case TKBD_KEY_DOWN:
			pk->sel++;
			pick_clamp(pk, n);
			return 0;
		case TKBD_KEY_PGUP:
			pk->sel -= pk->vis > 0 ? pk->vis : 1;
			pick_clamp(pk, n);
			return 0;
		case TKBD_KEY_PGDN:
			pk->sel += pk->vis > 0 ? pk->vis : 1;
			pick_clamp(pk, n);
			return 0;
		case TKBD_KEY_HOME:
			pk->sel = 0;
			pick_clamp(pk, n);
			return 0;
		case TKBD_KEY_END:
			pk->sel = n - 1;
			pick_clamp(pk, n);
			return 0;
		case TKBD_KEY_ENTER:
			if (n > 0)
				act = src->choose(src->ctx, pk->sel);
			break;
		default:
			if (!(k->mod & TKBD_MOD_CTRL) &&
			    k->ch != TKBD_CH_NONE && k->ch >= 0x20 &&
			    k->ch < 0x7f && n > 0) {
				pick_jump(pk, n, (int)k->ch);
				pick_clamp(pk, n);
			}
			return 0;
		}
	}

	if (act == PICK_DONE) {
		pk->result = 1;
		return 1;
	}
	if (act == PICK_CANCEL) {
		pk->result = 0;
		return 1;
	}
	/* PICK_STAY: listing may have changed, reset the view */
	pk->sel = 0;
	pk->top = 0;
	pk->entry[0] = '\0';
	return 0;
}

/* Run the picker panel through the shared modal loop. Returns 1 if a row or
 * entry was chosen, 0 if cancelled. */
static int
dlg_pick(Editor *e, const Picksrc *src)
{
	Picker pk;
	int boxw, boxh, maxw = 0, n, i;

	memset(&pk, 0, sizeof(pk));
	pk.src = src;
	if (src->entry_label && src->entry_init)
		snprintf(pk.entry, sizeof(pk.entry), "%s", src->entry_init);
	if (src->entry_label && src->focus_entry)
		pk.focus = 1;

	n = src->count(src->ctx);
	for (i = 0; i < n; i++) {
		const char *s = src->label(src->ctx, i);
		int w = s ? disp_cols(s, strlen(s)) : 0;

		if (w > maxw)
			maxw = w;
	}
	if (src->title) {
		int w = disp_cols(src->title(src->ctx),
		    strlen(src->title(src->ctx)));

		if (w > maxw)
			maxw = w;
	}
	boxw = maxw + 6;			/* borders + two spaces of pad */
	if (boxw < 40)
		boxw = 40;
	if (boxw > e->cols - 2)
		boxw = e->cols - 2;
	boxh = n + 2;				/* list rows + top/bottom border */
	if (src->entry_label)
		boxh += 2;			/* entry line + separator */
	if (boxh < 7)
		boxh = 7;
	if (boxh > e->rows - 2)
		boxh = e->rows - 2;

	dlg_run(e, boxw, boxh, &pk, pick_draw, pick_key);
	return pk.result;
}

/****************************************************************
 * Editing operations
 ****************************************************************/

void
move_left(Editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx > 0) {
		e->cx -= prev_rune_len(line, e->cx);
	} else if (e->cy > 0) {
		e->cy--;
		e->cx = text_line_len(e->t, e->cy);
	}
}

void
move_right(Editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx < len) {
		e->cx += rune_len_at(line, len, e->cx);
	} else if (e->cy + 1 < text_lines(e->t)) {
		e->cy++;
		e->cx = 0;
	}
}

/* Clamp the cursor to a valid line and column. */
void
clamp_col(Editor *e)
{
	size_t len;

	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
	len = text_line_len(e->t, e->cy);
	if (e->cx > len)
		e->cx = len;
}

void
ed_insert(Editor *e, const char *bytes, size_t n)
{
	hl_touch(e, e->cy);
	if (text_insert(e->t, e->cy, e->cx, bytes, n) == 0)
		e->cx += n;
}

void
ed_backspace(Editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	hl_touch(e, e->cy > 0 ? e->cy - 1 : 0);
	if (e->cx > 0) {
		size_t rl = prev_rune_len(line, e->cx);

		text_delete(e->t, e->cy, e->cx - rl, rl);
		e->cx -= rl;
	} else if (e->cy > 0) {
		size_t plen = text_line_len(e->t, e->cy - 1);

		text_join(e->t, e->cy - 1);
		e->cy--;
		e->cx = plen;
	}
}

void
ed_delete(Editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	hl_touch(e, e->cy);
	if (e->cx < len)
		text_delete(e->t, e->cy, e->cx, rune_len_at(line, len, e->cx));
	else if (e->cy + 1 < text_lines(e->t))
		text_join(e->t, e->cy);
}

void
ed_newline(Editor *e)
{
	hl_touch(e, e->cy);
	if (text_split(e->t, e->cy, e->cx) == 0) {
		e->cy++;
		e->cx = 0;
	}
}

/* Insert one indent step at the cursor: a hard tab, or, when the buffer indents
 * with spaces, enough spaces to reach the next TAB_WIDTH stop. */
static void
ed_indent_tab(Editor *e)
{
	size_t llen = 0;
	const char *s;
	int col;

	if (!e->expand_tabs) {
		ed_insert(e, "\t", 1);
		return;
	}
	s = text_line(e->t, e->cy, &llen);
	col = s ? disp_cols(s, e->cx) : 0;
	{
		int n = TAB_WIDTH - (col % TAB_WIDTH);
		char spaces[TAB_WIDTH];

		memset(spaces, ' ', (size_t)n);
		ed_insert(e, spaces, (size_t)n);
	}
}

/* Leading whitespace (spaces and tabs) of line y, written to buf. Returns the
 * byte count, capped at cap. */
static size_t
line_indent(Editor *e, size_t y, char *buf, size_t cap)
{
	size_t llen = 0;
	const char *s = text_line(e->t, y, &llen);
	size_t n = 0;

	if (!s)
		return 0;
	while (n < llen && n < cap && (s[n] == ' ' || s[n] == '\t'))
		n++;
	memcpy(buf, s, n);
	return n;
}

/* Split the line at the cursor and, when auto-indent is on, start the new line
 * with the same leading whitespace as the line the cursor left. */
static void
ed_newline_indent(Editor *e)
{
	char indent[256];
	size_t n = 0;

	if (e->auto_indent)
		n = line_indent(e, e->cy, indent, sizeof(indent));
	ed_newline(e);
	if (n)
		ed_insert(e, indent, n);
}

/* Rewrite the whitespace of lines [lo, hi]. to_spaces expands every hard tab in
 * the line to spaces at the TAB_WIDTH stops; otherwise the leading indent is
 * repacked into tabs plus a spaces remainder. The whole range is one undo step.
 * Returns the number of lines changed. */
static int
ed_retab_range(Editor *e, size_t lo, size_t hi, int to_spaces)
{
	size_t y;
	int changed = 0;

	text_undo_group_begin(e->t);
	for (y = lo; y <= hi && y < text_lines(e->t); y++) {
		size_t len = 0;
		const char *s = text_line(e->t, y, &len);
		char *out;
		size_t olen = 0, cap;

		if (!s || len == 0)
			continue;
		cap = len * TAB_WIDTH + 1;	/* a tab expands to at most 8 */
		out = malloc(cap);
		if (!out)
			break;

		if (to_spaces) {
			size_t i = 0;
			int col = 0;

			while (i < len) {
				uint32_t r;
				int n = utf8_decode(&r,
				    (const unsigned char *)s + i, len - i);

				if (n <= 0)
					n = 1;
				if (r == '\t') {
					int w = TAB_WIDTH - (col % TAB_WIDTH);

					while (w-- > 0)
						out[olen++] = ' ';
					col += TAB_WIDTH - (col % TAB_WIDTH);
				} else {
					int rw = rune_width(r);

					if (rw < 0)
						rw = 1;
					memcpy(out + olen, s + i, (size_t)n);
					olen += (size_t)n;
					col += rw;
				}
				i += (size_t)n;
			}
		} else {
			size_t i = 0;
			int width = 0, tabs, sp, k;

			while (i < len && (s[i] == ' ' || s[i] == '\t')) {
				if (s[i] == '\t')
					width += TAB_WIDTH - (width % TAB_WIDTH);
				else
					width++;
				i++;
			}
			tabs = width / TAB_WIDTH;
			sp = width % TAB_WIDTH;
			for (k = 0; k < tabs; k++)
				out[olen++] = '\t';
			for (k = 0; k < sp; k++)
				out[olen++] = ' ';
			memcpy(out + olen, s + i, len - i);
			olen += len - i;
		}

		if (olen != len || memcmp(out, s, len) != 0) {
			text_delete(e->t, y, 0, len);
			if (olen)
				text_insert(e->t, y, 0, out, olen);
			hl_touch(e, y);
			changed++;
		}
		free(out);
	}
	text_undo_group_end(e->t);
	return changed;
}

/* Convert tabs to spaces (to_spaces) or indentation to tabs over the selection
 * if there is one, otherwise the whole buffer. */
static void
ed_retab(Editor *e, int to_spaces)
{
	size_t lo = 0, hi = text_lines(e->t) ? text_lines(e->t) - 1 : 0;
	int n;

	if (e->sel_active) {
		size_t y1, x1, y2, x2;

		sel_bounds(e, &y1, &x1, &y2, &x2);
		lo = y1;
		hi = y2;
	}
	n = ed_retab_range(e, lo, hi, to_spaces);
	e->cx = 0;
	clamp_col(e);
	set_status(e, "%s: %d line%s changed",
	    to_spaces ? "tabs to spaces" : "spaces to tabs", n,
	    n == 1 ? "" : "s");
}


/* Consume a bracketed-paste payload (PASTE_BEGIN was just read) and insert
 * it literally, so control bytes in the paste never fire editor commands.
 * CR, LF, and CRLF all become one newline. Undo boundaries fence the paste
 * off from the surrounding edits. */
static void
paste_input(Editor *e)
{
	int saw_cr = 0;

	if (e->sel_active) {		/* a paste replaces the selection */
		size_t y1, x1, y2, x2;

		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
	}
	text_undo_boundary(e->t);	/* separate the paste from prior typing */
	for (;;) {
		Event ev;
		struct tkbd_seq seq;
		unsigned char buf[8];
		int n;

		if (scr_wait(e->d, &ev) == EVENT_EOF)
			break;			/* input closed ends the paste */
		if (ev.type != EVENT_KEY) {
			scr_size(e->d, &e->rows, &e->cols);	/* resize/resume */
			continue;
		}
		seq = ev.key;
		if (seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_PASTE_END)
			break;

		if (seq.key == TKBD_KEY_ENTER) {
			/* collapse a CR immediately followed by LF */
			if (seq.ch == 0x0a && saw_cr) {
				saw_cr = 0;
				continue;
			}
			ed_newline(e);
			saw_cr = (seq.ch == 0x0d);
			continue;
		}
		saw_cr = 0;
		if (seq.key == TKBD_KEY_TAB) {
			ed_insert(e, "\t", 1);
			continue;
		}
		if (seq.ch != TKBD_CH_NONE && seq.ch >= 0x20 &&
		    seq.ch != 0x7f) {
			n = utf8_encode(buf, seq.ch);
			if (n > 0)
				ed_insert(e, (char *)buf, (size_t)n);
		}
		/* other control and function keys are dropped inside a paste */
	}
	text_undo_boundary(e->t);
}

/* Consume and discard a bracketed-paste payload without inserting it. Used in
 * vi normal mode, where a paste is not text input. */
static void
paste_discard(Editor *e)
{
	for (;;) {
		Event ev;
		struct tkbd_seq seq;

		if (scr_wait(e->d, &ev) == EVENT_EOF)
			break;
		if (ev.type != EVENT_KEY) {
			scr_size(e->d, &e->rows, &e->cols);	/* resize/resume */
			continue;
		}
		seq = ev.key;
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_PASTE_END)
			break;
	}
}

static int
is_movement(Cmd cmd)
{
	switch (cmd) {
	case CMD_LEFT:
	case CMD_RIGHT:
	case CMD_UP:
	case CMD_DOWN:
	case CMD_HOME:
	case CMD_END:
	case CMD_PGUP:
	case CMD_PGDN:
		return 1;
	default:
		return 0;
	}
}

/* Copy the selection [y1,x1)-(y2,x2) into a fresh buffer, joining lines with
 * '\n'. Returns NULL on allocation failure. Sets *outlen to the byte count. */
char *
region_text(Editor *e, size_t y1, size_t x1, size_t y2, size_t x2,
    size_t *outlen)
{
	size_t cap = 0, len = 0, y;
	char *buf = NULL;

	for (y = y1; y <= y2; y++) {
		size_t llen = 0;
		const char *s = text_line(e->t, y, &llen);
		size_t a = (y == y1) ? x1 : 0;
		size_t b = (y == y2) ? x2 : llen;
		size_t seg;

		if (b > llen)			/* guard a stale/over-long end */
			b = llen;
		seg = (b > a) ? b - a : 0;
		size_t need = len + seg + 1;	/* room for a joining newline */

		if (need > cap) {
			char *nb = realloc(buf, need + 64);

			if (!nb) {
				free(buf);
				return NULL;
			}
			buf = nb;
			cap = need + 64;
		}
		if (seg && s) {
			memcpy(buf + len, s + a, seg);
			len += seg;
		}
		if (y < y2)
			buf[len++] = '\n';
	}
	if (!buf)
		buf = malloc(1);	/* empty selection: a valid 0-byte clip */
	*outlen = len;
	return buf;
}

/* Delete the selection [y1,x1)-(y2,x2) and leave the cursor at its start. */
void
delete_region(Editor *e, size_t y1, size_t x1, size_t y2, size_t x2)
{
	hl_touch(e, y1);
	if (y1 == y2) {
		text_delete(e->t, y1, x1, x2 - x1);
	} else {
		size_t first_len = 0;
		size_t k;

		text_line(e->t, y1, &first_len);
		text_delete(e->t, y1, x1, first_len - x1);
		text_delete(e->t, y2, 0, x2);
		for (k = y1 + 1; k < y2; k++) {	/* clear fully-covered lines */
			size_t ml = 0;

			text_line(e->t, k, &ml);
			text_delete(e->t, k, 0, ml);
		}
		for (k = y1; k < y2; k++)	/* pull each later line up */
			text_join(e->t, y1);
	}
	e->cy = y1;
	e->cx = x1;
}

/* Insert bytes at the cursor, breaking lines on embedded newlines. */
void
insert_bytes(Editor *e, const char *bytes, size_t len)
{
	size_t i = 0;

	while (i < len) {
		size_t j = i;

		while (j < len && bytes[j] != '\n')
			j++;
		if (j > i)
			ed_insert(e, bytes + i, j - i);
		if (j < len)
			ed_newline(e);		/* the newline itself */
		i = (j < len) ? j + 1 : j;
	}
}

/* Insert the clipboard (the unnamed register) at the cursor. */
void
insert_clip(Editor *e)
{
	insert_bytes(e, e->clip, e->clip_len);
}

/* Cap on the raw byte count mirrored to the system clipboard. OSC 52 rides
 * the terminal's input path, and many terminals cap or drop very long
 * sequences, so keep the payload modest; the internal clipboard is unbounded.
 */
#define OSC52_MAX 100000

void
clip_set(Editor *e, char *bytes, size_t len)
{
	free(e->clip);
	e->clip = bytes;
	e->clip_len = len;
	e->clip_block = 0;	/* a plain copy; block copies set this themselves */
	/* Mirror to the terminal's clipboard via OSC 52 only when the config
	 * opted in; by default a copy stays in the internal clipboard so the
	 * common case never depends on OSC 52 working. */
	if (e->clip_osc52 && len > 0 && len <= OSC52_MAX)
		scr_set_clipboard(e->d, bytes, len);
}

/* Allocated text of the active selection, with its byte length in *len. The
 * caller owns the buffer. Returns NULL when nothing is selected or on an
 * allocation failure. */
static char *
current_selection_text(Editor *e, size_t *len)
{
	size_t y1, x1, y2, x2;

	*len = 0;
	if (!e->sel_active)
		return NULL;
	sel_bounds(e, &y1, &x1, &y2, &x2);
	return region_text(e, y1, x1, y2, x2, len);
}

/* Send len bytes to the terminal's clipboard via OSC 52, reporting what
 * happened. Unlike clip_set, this always emits (it is the point of the
 * command) and does not touch the internal clipboard. */
static void
osc_copy(Editor *e, const char *bytes, size_t len, const char *what)
{
	if (len == 0) {
		set_status(e, "nothing to copy");
		return;
	}
	if (len > OSC52_MAX) {
		set_status(e,
		    "%s is too large for the terminal clipboard (%d bytes max)",
		    what, OSC52_MAX);
		return;
	}
	scr_set_clipboard(e->d, bytes, len);
	set_status(e,
	    "copied %s to the terminal clipboard (%zu bytes)", what, len);
}

/* Copy the selection, or the current line when nothing is selected, to the
 * terminal clipboard with OSC 52. */
static void
osc_copy_selection(Editor *e)
{
	char *text;
	size_t len = 0;

	if (e->sel_active) {
		text = current_selection_text(e, &len);
	} else {
		size_t ll = 0;
		const char *s = text_line(e->t, e->cy, &ll);

		text = malloc(ll ? ll : 1);
		if (text && ll)
			memcpy(text, s, ll);
		len = ll;
	}
	if (!text) {
		set_status(e, "out of memory");
		return;
	}
	osc_copy(e, text, len, e->sel_active ? "selection" : "line");
	free(text);
}

/* Copy the whole buffer to the terminal clipboard with OSC 52. */
static void
osc_copy_file(Editor *e)
{
	size_t nlines = text_lines(e->t), lastlen = 0, len = 0;
	char *text;

	if (nlines == 0) {
		set_status(e, "nothing to copy");
		return;
	}
	text_line(e->t, nlines - 1, &lastlen);
	text = region_text(e, 0, 0, nlines - 1, lastlen, &len);
	if (!text) {
		set_status(e, "out of memory");
		return;
	}
	osc_copy(e, text, len, "file");
	free(text);
}

/* Carry out one command, returning what the main loop must do next. */
static Req
ed_dispatch(Editor *e, Cmd cmd, const struct tkbd_seq *seq)
{
	int page = text_height(e) - 1;
	unsigned char buf[8];
	int n;
	int grouped = 0;		/* an undo group is open for this command */

	if (page < 1)
		page = 1;

	/* any command other than typing ends the current undo run */
	if (cmd != CMD_INSERT && cmd != CMD_TAB)
		text_undo_boundary(e->t);

	/* Shift + a movement key extends a selection from an anchor; an
	 * unshifted movement clears it. */
	if (is_movement(cmd)) {
		if (seq && (seq->mod & TKBD_MOD_SHIFT)) {
			if (!e->sel_active) {
				e->sel_active = 1;
				e->ay = e->cy;
				e->ax = e->cx;
			}
		} else {
			e->sel_active = 0;
		}
	}

	/* Editing over a selection replaces it: drop the selected text first,
	 * then let insertion proceed. Backspace and Delete are satisfied by
	 * that removal alone. */
	if (e->sel_active && (cmd == CMD_INSERT || cmd == CMD_TAB ||
	    cmd == CMD_NEWLINE || cmd == CMD_BACKSPACE || cmd == CMD_DELETE)) {
		size_t y1, x1, y2, x2;

		/* the deletion and any replacement typing are one undo step */
		text_undo_group_begin(e->t);
		grouped = 1;
		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
		if (cmd == CMD_BACKSPACE || cmd == CMD_DELETE) {
			text_undo_group_end(e->t);
			return REQ_CONTINUE;
		}
	}

	switch (cmd) {
	case CMD_QUIT:
		return REQ_QUIT;
	case CMD_SAVE:
		return REQ_SAVE;
	case CMD_UNDO:
		e->sel_active = 0;	/* the buffer shifts under the anchor */
		if (text_undo(e->t, &e->cy, &e->cx) != 0)
			set_status(e,
			    "nothing to undo");
		else {
			hl_touch(e, 0);	/* an undo may touch any lines */
			clamp_col(e);
		}
		break;
	case CMD_REDO:
		e->sel_active = 0;
		if (text_redo(e->t, &e->cy, &e->cx) != 0)
			set_status(e,
			    "nothing to redo");
		else {
			hl_touch(e, 0);
			clamp_col(e);
		}
		break;
	case CMD_COPY: {
		size_t rl = 0;
		char *r;

		if (e->sel_active) {
			r = current_selection_text(e, &rl);
			if (r) {
				clip_set(e, r, rl);
				set_status(e,
				    "copied %zu bytes", rl);
			}
		} else {
			size_t ll = 0;
			const char *s = text_line(e->t, e->cy, &ll);

			r = malloc(ll + 1);	/* the line plus its newline */
			if (r) {
				if (ll && s)
					memcpy(r, s, ll);
				r[ll] = '\n';
				clip_set(e, r, ll + 1);
				set_status(e,
				    "copied line");
			}
		}
		break;
	}
	case CMD_CUT: {
		size_t y1, x1, y2, x2, rl = 0;
		char *r;

		if (!e->sel_active) {
			set_status(e,
			    "select text first (Shift+arrows)");
			break;
		}
		text_undo_group_begin(e->t);
		grouped = 1;
		r = current_selection_text(e, &rl);
		if (r)
			clip_set(e, r, rl);
		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
		set_status(e, "cut %zu bytes", rl);
		break;
	}
	case CMD_PASTE:
		if (!e->clip || e->clip_len == 0) {
			set_status(e,
			    "clipboard is empty");
			break;
		}
		text_undo_group_begin(e->t);
		grouped = 1;
		if (e->sel_active) {		/* paste replaces the selection */
			size_t y1, x1, y2, x2;

			sel_bounds(e, &y1, &x1, &y2, &x2);
			delete_region(e, y1, x1, y2, x2);
			e->sel_active = 0;
		}
		insert_clip(e);
		break;
	case CMD_FIND:
		return REQ_FIND;
	case CMD_REPLACE:
		return REQ_REPLACE;
	case CMD_SYMBOL:
		dlg_symbol_pick(e);
		break;
	case CMD_GOTO:
		return REQ_GOTO;
	case CMD_HELP:
		return REQ_HELP;
	case CMD_INSERT:
		n = utf8_encode(buf, seq->ch);
		if (n > 0)
			ed_insert(e, (char *)buf, (size_t)n);
		break;
	case CMD_TAB:
		ed_indent_tab(e);
		break;
	case CMD_NEWLINE:
		ed_newline_indent(e);
		break;
	case CMD_BACKSPACE:
		ed_backspace(e);
		break;
	case CMD_DELETE:
		ed_delete(e);
		break;
	case CMD_LEFT:
		move_left(e);
		break;
	case CMD_RIGHT:
		move_right(e);
		break;
	case CMD_UP:
		if (e->cy > 0) {
			e->cy--;
			clamp_col(e);
		}
		break;
	case CMD_DOWN:
		if (e->cy + 1 < text_lines(e->t)) {
			e->cy++;
			clamp_col(e);
		}
		break;
	case CMD_HOME:
		e->cx = 0;
		break;
	case CMD_END:
		e->cx = text_line_len(e->t, e->cy);
		break;
	case CMD_TOP:
		e->cy = 0;
		e->cx = 0;
		break;
	case CMD_BOTTOM:
		e->cy = text_lines(e->t) ? text_lines(e->t) - 1 : 0;
		e->cx = text_line_len(e->t, e->cy);
		break;
	case CMD_PGUP:
		e->cy = e->cy > (size_t)page ? e->cy - (size_t)page : 0;
		clamp_col(e);
		break;
	case CMD_PGDN:
		e->cy += (size_t)page;
		if (e->cy >= text_lines(e->t))
			e->cy = text_lines(e->t) - 1;
		clamp_col(e);
		break;
	case CMD_NONE:
		break;
#ifndef VEDIT_NO_TOOLS
	case CMD_COMPILE:
	case CMD_MAKE:
	case CMD_RUN:
	case CMD_VIEW_OUTPUT:
	case CMD_ERR_NEXT:
	case CMD_ERR_PREV:
		/* tool commands are dispatched from the main loop, not here */
		break;
#endif
	}
	if (grouped)
		text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/****************************************************************
 * Status-line prompts
 ****************************************************************/

/* Draw the editor frame, then overlay a prompt on the status row and leave
 * the cursor at the end of the typed text. */
static void
ui_prompt(Editor *e, const char *q, const char *buf)
{
	const Pal *p = ed_chrome(e);
	uint16_t at = (p->reverse_bars ? ATTR_REVERSE : 0) | ATTR_BOLD;
	char line[512];
	int col;
	int status_row = (e->rows > 0 ? e->rows : 24) - 1;

	ed_render(e, e->d);		/* paint the frame under the prompt */
	col = snprintf(line, sizeof(line), "%s%s", q, buf ? buf : "");
	ui_field(e->d, status_row, 0, e->cols, line, p->bar_fg, p->bar_bg, at);
	if (col >= e->cols)
		col = e->cols - 1;
	scr_cursor(e->d, status_row, col);
	scr_present(e->d);
}

/* Read a line of text. buf is edited in place, so a caller may pre-fill it
 * with a default. Returns 1 with buf filled, or 0 if cancelled; when
 * allow_empty is 0 an empty line reads as a cancel, else it is accepted. */
static int
prompt_line(Editor *e, const char *q, char *buf, size_t bufsz, int allow_empty)
{
	size_t len = strlen(buf);

	for (;;) {
		Event ev;
		struct tkbd_seq seq;

		ui_prompt(e, q, buf);
		if (scr_wait(e->d, &ev) == EVENT_EOF)
			return 0;
		if (ev.type != EVENT_KEY) {
			scr_size(e->d, &e->rows, &e->cols);	/* resize/resume */
			continue;			/* loop redraws the prompt */
		}
		seq = ev.key;
		if (seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_ENTER)
			return allow_empty ? 1 : (len > 0);
		if (seq.key == TKBD_KEY_ESC ||
		    ((seq.mod & TKBD_MOD_CTRL) && seq.key == TKBD_KEY_C))
			return 0;
		if (seq.key == TKBD_KEY_BACKSPACE ||
		    seq.key == TKBD_KEY_BACKSPACE2) {
			while (len > 0 &&
			    ((unsigned char)buf[len - 1] & 0xc0) == 0x80)
				len--;		/* drop UTF-8 continuation */
			if (len > 0)
				len--;
			buf[len] = '\0';
			continue;
		}
		if (!(seq.mod & TKBD_MOD_CTRL) && seq.ch != TKBD_CH_NONE &&
		    seq.ch >= 0x20 && seq.ch != 0x7f) {
			unsigned char enc[8];
			int el = utf8_encode(enc, seq.ch);

			if (el > 0 && len + (size_t)el < bufsz) {
				memcpy(buf + len, enc, (size_t)el);
				len += (size_t)el;
				buf[len] = '\0';
			}
		}
	}
}

int
dlg_prompt_line(Editor *e, const char *q, char *buf, size_t bufsz)
{
	return prompt_line(e, q, buf, bufsz, 0);
}

/* Draw q on the status row and return the next character typed (lowercased),
 * or 0 on ESC, Ctrl-C or end of input. Used for a one-key y/n/a/q prompt. */
static int
dlg_prompt_key(Editor *e, const char *q)
{
	for (;;) {
		Event ev;
		struct tkbd_seq seq;

		ui_prompt(e, q, "");
		if (scr_wait(e->d, &ev) == EVENT_EOF)
			return 0;
		if (ev.type != EVENT_KEY) {
			scr_size(e->d, &e->rows, &e->cols);
			continue;
		}
		seq = ev.key;
		if (seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_ESC ||
		    ((seq.mod & TKBD_MOD_CTRL) && seq.key == TKBD_KEY_C))
			return 0;
		if (seq.key == TKBD_KEY_ENTER)
			return '\n';
		if (seq.ch != TKBD_CH_NONE && seq.ch >= 0x20 && seq.ch < 0x7f)
			return tolower((unsigned char)seq.ch);
	}
}

/* ---- regex search adapters over the vendored rx engine -----------------
 *
 * Search and replace treat the pattern as a regular expression (POSIX ERE
 * with the vi/sed conveniences rx supports) and match within a single line:
 * each line's bytes are handed to rx as the whole subject, so ^ and $ anchor
 * to the line and a match never crosses a newline. The callers compile the
 * pattern once and pass the compiled rx_t down to the per-line helpers. */

/* First match of re in s[0, slen) beginning at or after column `from`. Returns
 * 1 with the byte span in *so, *eo, or 0 for no match. */
static int
re_match_from(rx_t *re, const char *s, size_t slen, size_t from,
    size_t *so, size_t *eo)
{
	rx_match m[1];

	if (from > slen)
		return 0;
	if (rx_exec(re, s, slen, from, m, 1) != 1)
		return 0;
	*so = (size_t)m[0].so;
	*eo = (size_t)m[0].eo;
	return 1;
}

/* Last match of re in s whose start lies in [lo, hi). Returns 1 with the byte
 * span in *so, *eo, or 0 for no match. Used by the backward search to take the
 * match nearest the end of a scanned range. An empty match advances by one so
 * the scan makes progress. */
static int
re_match_last(rx_t *re, const char *s, size_t slen, size_t lo, size_t hi,
    size_t *so, size_t *eo)
{
	size_t from = lo;
	int got = 0;

	if (lo > slen)
		return 0;
	while (from <= slen) {
		rx_match m[1];
		size_t ms, me;

		if (rx_exec(re, s, slen, from, m, 1) != 1)
			break;
		ms = (size_t)m[0].so;
		me = (size_t)m[0].eo;
		if (ms >= hi)
			break;
		*so = ms;
		*eo = me;
		got = 1;
		from = (me > ms) ? me : ms + 1;
	}
	return got;
}

/* Find the next match of re at or after (fy, fx), scanning to the end of the
 * buffer without wrapping. Matches never cross a line. Returns 1 with the
 * position in *my, *mx and the match length in *mlen, or 0 if there is none. */
static int
replace_next(Editor *e, rx_t *re, size_t fy, size_t fx,
    size_t *my, size_t *mx, size_t *mlen)
{
	size_t nlines = text_lines(e->t), ln;

	for (ln = fy; ln < nlines; ln++) {
		size_t llen = 0, so, eo;
		const char *s = text_line(e->t, ln, &llen);
		size_t from = (ln == fy) ? fx : 0;

		if (!s || from > llen)
			continue;
		if (re_match_from(re, s, llen, from, &so, &eo)) {
			*my = ln;
			*mx = so;
			*mlen = eo - so;
			return 1;
		}
	}
	return 0;
}

/* Overwrite the patlen bytes at (ln, col) with repl. The caller guarantees the
 * match lies within the line. */
static void
replace_at(Editor *e, size_t ln, size_t col, size_t patlen,
    const char *repl, size_t repllen)
{
	hl_touch(e, ln);
	text_delete(e->t, ln, col, patlen);
	if (repllen)
		text_insert(e->t, ln, col, repl, repllen);
}

/* rx_compile flags for a user-initiated search or replace: case-insensitive
 * when :set ignorecase is on. Build-error grammars compile without it. */
static int
search_flags(const Editor *e)
{
	return e->search_icase ? RX_ICASE : 0;
}

/* Prompt for a regex pattern and a replacement template, then walk the matches
 * from the cursor to the end of the buffer. At each one, y replaces, n skips, a
 * replaces it and all that follow, and q (or Esc) stops. The template honours
 * rx's &, \1..\9, and \U \L \u \l \E conveniences. */
static void
replace_prompt(Editor *e)
{
	char pat[256], repl[256];
	size_t y, x, my, mx, mlen, count = 0;
	int all = 0;
	rx_t *re;
	const char *err;

	snprintf(pat, sizeof(pat), "%s", e->last_find);
	if (!prompt_line(e, "Replace (regex): ", pat, sizeof(pat), 0)) {
		set_status(e, "replace cancelled");
		return;
	}
	snprintf(repl, sizeof(repl), "%s", e->last_replace);
	if (!prompt_line(e, "Replace with: ", repl, sizeof(repl), 1)) {
		set_status(e, "replace cancelled");
		return;
	}
	if (pat[0] == '\0')
		return;
	re = rx_compile(pat, search_flags(e), &err);
	if (!re) {
		set_status(e, "bad pattern: %.80s", err);
		return;
	}
	snprintf(e->last_find, sizeof(e->last_find), "%s", pat);
	snprintf(e->last_replace, sizeof(e->last_replace), "%s", repl);

	y = e->cy;
	x = e->cx;
	text_undo_group_begin(e->t);
	while (replace_next(e, re, y, x, &my, &mx, &mlen)) {
		size_t llen = 0, outlen;
		const char *s;
		char *out;

		e->cy = my;
		e->cx = mx;
		e->sel_active = 0;
		if (!all) {
			int k = dlg_prompt_key(e, "Replace this one? "
			    "(y)es (n)o (a)ll (q)uit ");

			if (k == 0 || k == 'q')
				break;
			if (k == 'a')
				all = 1;
			else if (k != 'y' && k != '\n') {
				y = my;		/* skip: resume just past it */
				x = mx + (mlen ? mlen : 1);
				continue;
			}
		}
		/* Expand the template against this match by re-running rx over the
		 * matched slice, which is exactly one match. */
		s = text_line(e->t, my, &llen);
		out = s ? rx_replace(re, s + mx, mlen, repl, 0) : NULL;
		outlen = out ? strlen(out) : 0;
		replace_at(e, my, mx, mlen, out ? out : "", outlen);
		free(out);
		count++;
		y = my;
		x = mx + outlen;
		if (mlen == 0)		/* empty match: step past to make progress */
			x++;
		e->cx = mx + outlen;
	}
	text_undo_group_end(e->t);
	rx_free(re);
	set_status(e, "replaced %zu occurrence%s",
	    count, count == 1 ? "" : "s");
}

/****************************************************************
 * Swap and backup files (optional crash recovery, Vim-style).
 *
 * A swap file is a full snapshot of a dirty buffer, rewritten whenever the
 * editor goes idle (editor_loop's tick), so a crash or a dropped connection
 * leaves the last idle state on disk. Opening a file that has a swap beside it
 * offers to recover. A clean save or quit removes the swap. Backups keep the
 * previous on-disk version as a "~" file across a save. Both are off unless
 * configured (swap on by default, backup off); both locations are
 * configurable. The snapshot reuses text_write_fp/text_load_fp, so there is no
 * second serializer.
 ****************************************************************/

#define SWAP_MAGIC "VEDIT-SWAP"

enum {
	SWAP_NONE,		/* no swap file beside the target */
	SWAP_FOREIGN,		/* a file is there but not ours; leave it alone */
	SWAP_OPEN,		/* user chose to open the file anyway */
	SWAP_RECOVERED,		/* user recovered; the buffer now holds the swap */
	SWAP_DELETED,		/* user deleted the swap; the file is loaded */
	SWAP_ABORT		/* user declined to open the file at all */
};

/* Copy a path into out with every '/' turned into '%', for a collision-free
 * name when swap or backup files share one directory (Vim's convention). */
static void
path_mangle(const char *full, char *out, size_t sz)
{
	size_t i;

	for (i = 0; full[i] && i + 1 < sz; i++)
		out[i] = (full[i] == '/') ? '%' : full[i];
	out[i < sz ? i : sz - 1] = '\0';
}

/* Resolve the directory named by config key (edit.swapdir / edit.backupdir)
 * into out. A leading "~/" expands against $HOME. Returns 1 when a usable,
 * writable directory is set, 0 for "beside the file" (unset, empty, ".", or a
 * directory that does not exist or cannot be written). */
static int
resolve_dir_opt(const char *key, char *out, size_t sz)
{
	const char *v = cfg_proj_get(key);

	if (!v || !v[0] || strcmp(v, ".") == 0)
		return 0;
	if (v[0] == '~' && v[1] == '/') {
		const char *home = getenv("HOME");

		if (!home || !home[0])
			return 0;
		if ((size_t)snprintf(out, sz, "%s%s", home, v + 1) >= sz)
			return 0;
	} else if ((size_t)snprintf(out, sz, "%s", v) >= sz) {
		return 0;
	}
	if (access(out, W_OK | X_OK) != 0)
		return 0;
	return 1;
}

/* Build the swap or backup path for file_path into out. dir_key selects the
 * directory option; sep is the beside-the-file prefix ('.' for swap, 0 for
 * backup) and suffix is appended (".swp" or "~"). Returns 1 on success. */
static int
aux_path_for(const char *file_path, const char *dir_key, int dotted,
    const char *suffix, char *out, size_t sz)
{
	char dir[PATH_MAX];
	const char *slash = strrchr(file_path, '/');
	int n;

	if (resolve_dir_opt(dir_key, dir, sizeof(dir))) {
		char mangled[PATH_MAX];

		path_mangle(file_path, mangled, sizeof(mangled));
		n = snprintf(out, sz, "%s/%s%s", dir, mangled, suffix);
	} else if (dotted && slash) {
		n = snprintf(out, sz, "%.*s/.%s%s", (int)(slash - file_path),
		    file_path, slash + 1, suffix);
	} else if (dotted) {
		n = snprintf(out, sz, ".%s%s", file_path, suffix);
	} else {
		n = snprintf(out, sz, "%s%s", file_path, suffix);
	}
	return n > 0 && (size_t)n < sz;
}

static int
swap_path_for(const char *file_path, char *out, size_t sz)
{
	return aux_path_for(file_path, "edit.swapdir", 1, ".swp", out, sz);
}

static int
backup_path_for(const char *file_path, char *out, size_t sz)
{
	return aux_path_for(file_path, "edit.backupdir", 0, "~", out, sz);
}

/* Copy src's bytes to dst, giving dst the mode bits. Returns 0, or -1 with
 * errno set. Used to keep the previous version as a backup before a save. */
static int
file_copy(const char *src, const char *dst, mode_t mode)
{
	FILE *in = fopen(src, "rb"), *out;
	char buf[8192];
	size_t n;
	int saved;

	if (!in)
		return -1;
	out = fopen(dst, "wb");
	if (!out) {
		saved = errno;
		fclose(in);
		errno = saved;
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			saved = errno;
			fclose(in);
			fclose(out);
			errno = saved;
			return -1;
		}
	}
	saved = ferror(in);
	fclose(in);
	if (fclose(out) != 0 || saved) {
		errno = saved ? EIO : errno;
		return -1;
	}
	(void)chmod(dst, mode);
	return 0;
}

/* Set e->swap_path from the active buffer's name when swap is enabled and the
 * buffer has one, otherwise clear it (which makes swap_write a no-op). */
static void
swap_set_path(Editor *e)
{
	e->swap_path[0] = '\0';
	if (e->swap_enabled && e->has_name)
		swap_path_for(e->path, e->swap_path, sizeof(e->swap_path));
}

/* Write a snapshot of the active buffer to its swap file (atomically, via a
 * sibling temp and a rename). Best effort: a failure never disturbs editing,
 * it just leaves the swap stale. */
static void
swap_write(Editor *e)
{
	char tmp[PATH_MAX];
	FILE *fp;
	int ok;

	if (!e->swap_enabled || !e->has_name)
		return;
	if (!e->swap_path[0])
		swap_set_path(e);
	if (!e->swap_path[0])
		return;
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.new", e->swap_path) >=
	    sizeof(tmp))
		return;
	fp = fopen(tmp, "wb");
	if (!fp)
		return;
	ok = fprintf(fp, "%s\t1\npath\t%s\nmtime\t%ld\neol\t%d\n"
	    "final_newline\t%d\npid\t%ld\n\n", SWAP_MAGIC, e->path,
	    (long)e->load_mtime, e->t->eol, e->t->final_newline,
	    (long)getpid()) >= 0 && text_write_fp(e->t, fp) == OK;
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0)
		ok = 0;
	if (fclose(fp) != 0)
		ok = 0;
	if (!ok || rename(tmp, e->swap_path) != 0) {
		unlink(tmp);
		return;
	}
	e->swap_on = 1;
	e->swap_rev = e->t->rev;
}

/* Remove the active buffer's swap file, if one exists. */
static void
swap_remove(Editor *e)
{
	if (e->swap_on && e->swap_path[0])
		unlink(e->swap_path);
	e->swap_on = 0;
}

/* The idle tick: refresh the swap only when the buffer is dirty and has
 * changed since the last snapshot. Called when the event loop goes quiet. */
static void
swap_maybe_write(Editor *e)
{
	if (e->swap_enabled && e->has_name && text_dirty(e->t) &&
	    e->t->rev != e->swap_rev)
		swap_write(e);
}

/* Record swap state on the active buffer after an open or recovery decision:
 * the swap path for e->path, whether a swap file is now on disk, and the rev
 * it reflects. Called once e->path is set to the opened file. */
static void
swap_adopt(Editor *e, time_t orig_mtime, int action)
{
	swap_set_path(e);
	e->load_mtime = orig_mtime;
	e->swap_on = (action == SWAP_RECOVERED || action == SWAP_OPEN);
	e->swap_rev = e->t->rev;
	if (action == SWAP_FOREIGN) {	/* someone else's .swp: never touch it */
		e->swap_path[0] = '\0';
		e->swap_on = 0;
	}
}

/* Check for a swap file beside path. When one of ours is found, prompt and act
 * on the choice: recover into t (marking it dirty), open anyway, delete the
 * swap, or abort the open. Returns a SWAP_* action for the caller to adopt. */
static int
swap_recover(Editor *e, const char *path, Text *t, time_t orig_mtime)
{
	char swap[PATH_MAX], line[PATH_MAX + 64], msg[256];
	FILE *fp;
	long body = 0, hdr_mtime = -1, hdr_pid = -1;
	int other = 0, key;

	if (!e->swap_enabled || !swap_path_for(path, swap, sizeof(swap)))
		return SWAP_NONE;
	fp = fopen(swap, "rb");
	if (!fp)
		return SWAP_NONE;
	if (!fgets(line, sizeof(line), fp) ||
	    strncmp(line, SWAP_MAGIC, strlen(SWAP_MAGIC)) != 0) {
		fclose(fp);		/* not our file; leave it untouched */
		return SWAP_FOREIGN;
	}
	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '\n')
			break;		/* blank line ends the header */
		if (strncmp(line, "mtime\t", 6) == 0)
			hdr_mtime = atol(line + 6);
		else if (strncmp(line, "pid\t", 4) == 0)
			hdr_pid = atol(line + 4);
	}
	body = ftell(fp);
	if (hdr_pid > 0 && hdr_pid != (long)getpid() &&
	    (kill((pid_t)hdr_pid, 0) == 0 || errno == EPERM))
		other = 1;
	snprintf(msg, sizeof(msg),
	    "Swap file found%s%s. (r)ecover (o)pen (d)elete (q)uit? ",
	    other ? ", maybe open elsewhere" : "",
	    (hdr_mtime >= 0 && hdr_mtime != (long)orig_mtime) ?
	    ", file changed since" : "");
	key = dlg_prompt_key(e, msg);
	switch (key) {
	case 'r':
		if (body >= 0 && fseek(fp, body, SEEK_SET) == 0 &&
		    text_load_fp(t, fp) == OK) {
			t->dirty = 1;
			fclose(fp);
			set_status(e,
			    "recovered from swap; not yet saved");
			return SWAP_RECOVERED;
		}
		fclose(fp);
		set_status(e, "swap recovery failed");
		return SWAP_OPEN;
	case 'd':
		fclose(fp);
		unlink(swap);
		return SWAP_DELETED;
	case 'o':
		fclose(fp);
		return SWAP_OPEN;
	default:			/* q, Esc, Ctrl-C, EOF */
		fclose(fp);
		return SWAP_ABORT;
	}
}

/* Save the active buffer to e->path, keeping the previous version as a backup
 * first when backups are enabled. On success the swap becomes redundant and is
 * removed. Returns OK, or ERR with errno set (as text_save). */
static int
ed_save_file(Editor *e)
{
	int rc;

	if (e->backup_enabled && e->has_name) {
		struct stat st;
		char backup[PATH_MAX];

		if (stat(e->path, &st) == 0 && S_ISREG(st.st_mode) &&
		    backup_path_for(e->path, backup, sizeof(backup)))
			(void)file_copy(e->path, backup, st.st_mode & 07777);
	}
	rc = text_save(e->t, e->path);
	if (rc == OK) {
		swap_remove(e);
		e->swap_rev = e->t->rev;
	}
	return rc;
}

/* Save the buffer, prompting for a name if it has none. Returns 0 on a
 * successful save, -1 on failure or when the save was cancelled. */
static int
save_editor(Editor *e)
{
	if (!e->has_name) {
		char name[PATH_MAX];

		name[0] = '\0';
		if (!dlg_save_file(e, name, sizeof(name))) {
			set_status(e, "save cancelled");
			return -1;
		}
		snprintf(e->path, sizeof(e->path), "%s", name);
		e->has_name = 1;
		e->syn = syn_for_ext(file_ext(e->path));
		e->hl_valid = 0;
	}

	if (ed_save_file(e) < 0) {
		set_status(e, "save failed: %s",
		    strerror(errno));
		return -1;
	}
	set_status(e, "wrote %.120s", e->path);
	return 0;
}

/* Search for the regex q from the cursor in direction dir (1 forward, -1
 * backward), wrapping around the buffer, and move the cursor to the match. An
 * invalid pattern leaves the cursor put and reports the error. */
void
ed_find_dir(Editor *e, const char *q, int dir)
{
	size_t nlines = text_lines(e->t);
	size_t i;
	rx_t *re;
	const char *err;

	if (!q[0])
		return;
	re = rx_compile(q, search_flags(e), &err);
	if (!re) {
		set_status(e, "bad pattern: %.80s", err);
		return;
	}

	if (dir >= 0) {
		/* Current line after the cursor, then each following line, then
		 * wrap and finish the start of the current line. */
		for (i = 0; i <= nlines; i++) {
			size_t ln = (e->cy + i) % nlines;
			size_t llen = 0, so, eo;
			const char *s = text_line(e->t, ln, &llen);
			size_t from = (i == 0) ? e->cx + 1 : 0;

			if (!s || from > llen)
				continue;
			if (re_match_from(re, s, llen, from, &so, &eo)) {
				e->cy = ln;
				e->cx = so;
				e->sel_active = 0;
				set_status(e,
				    "found '%.80s' (line %zu)", q, ln + 1);
				rx_free(re);
				return;
			}
		}
	} else {
		/* Current line before the cursor, then each preceding line, then
		 * wrap and finish the tail of the current line at/after it. */
		for (i = 0; i <= nlines; i++) {
			size_t ln = (e->cy + 2 * nlines - i) % nlines;
			size_t llen = 0, so, eo, lo = 0, hi;
			const char *s = text_line(e->t, ln, &llen);

			if (!s)
				continue;
			if (i == 0)
				hi = e->cx;		/* strictly before cursor */
			else if (i == nlines) {
				lo = e->cx;		/* wrap: tail of this line */
				hi = llen + 1;
			} else {
				hi = llen + 1;		/* the whole line */
			}
			if (re_match_last(re, s, llen, lo, hi, &so, &eo)) {
				e->cy = ln;
				e->cx = so;
				e->sel_active = 0;
				set_status(e,
				    "found '%.80s' (line %zu)", q, ln + 1);
				rx_free(re);
				return;
			}
		}
	}
	rx_free(re);
	set_status(e, "not found: %.80s", q);
}

void
ed_find(Editor *e, const char *q)
{
	ed_find_dir(e, q, 1);
}

/* Direction-aware incremental scan for the regex q. Forward finds the first
 * match at or after (oy, ox), wrapping once back to the origin; backward finds
 * the nearest match before the origin, wrapping once to the end of the buffer.
 * Matches never cross a line. Returns 1 with the position in *my, *mx. An
 * invalid pattern (often a half-typed one) is treated as no match. */
static int
isearch_scan_dir(Editor *e, const char *q, size_t oy, size_t ox, int dir,
    size_t *my, size_t *mx)
{
	size_t nlines = text_lines(e->t), i;
	rx_t *re;
	int hit = 0;

	if (!q[0] || nlines == 0)
		return 0;
	re = rx_compile(q, search_flags(e), NULL);
	if (!re)
		return 0;
	if (dir >= 0) {
		for (i = 0; i <= nlines; i++) {
			size_t ln = (oy + i) % nlines;
			size_t llen = 0, so, eo;
			const char *s = text_line(e->t, ln, &llen);
			size_t from = (i == 0) ? ox : 0;

			if (!s || from > llen)
				continue;
			if (!re_match_from(re, s, llen, from, &so, &eo))
				continue;
			/* On the final wrap back to the origin line, only a
			 * match before the origin column is new. */
			if (i == nlines && so >= ox)
				break;
			*my = ln;
			*mx = so;
			hit = 1;
			break;
		}
	} else {
		for (i = 0; i <= nlines; i++) {
			size_t ln = (oy + 2 * nlines - i) % nlines;
			size_t llen = 0, so, eo, lo = 0, hi;
			const char *s = text_line(e->t, ln, &llen);

			if (!s)
				continue;
			if (i == 0)
				hi = ox;	/* strictly before the origin */
			else if (i == nlines) {
				lo = ox;	/* wrap: the tail of the origin line */
				hi = llen + 1;
			} else
				hi = llen + 1;
			if (re_match_last(re, s, llen, lo, hi, &so, &eo)) {
				*my = ln;
				*mx = so;
				hit = 1;
				break;
			}
		}
	}
	rx_free(re);
	return hit;
}

/* Paint the editor with the cursor already moved to the live match, then
 * overlay the query on the status row after label. ed_render leaves the
 * hardware cursor at the match, and ui_field does not move it, so the cursor
 * sits on the match. */
static void
isearch_draw(Editor *e, const char *label, const char *q, int found)
{
	const Pal *p = ed_chrome(e);
	uint16_t at = (p->reverse_bars ? ATTR_REVERSE : 0) | ATTR_BOLD;
	char line[512];
	int status_row = (e->rows > 0 ? e->rows : 24) - 1;

	ed_render(e, e->d);
	snprintf(line, sizeof(line), "%s%s%s",
	    found ? "" : "(failing) ", label, q);
	ui_field(e->d, status_row, 0, e->cols, line, p->bar_fg, p->bar_bg, at);
	scr_present(e->d);
}

/* Incremental search in direction dir (1 forward, -1 backward). The cursor
 * follows the first match from the starting position as the query is typed.
 * Enter accepts (storing the query and direction for repeats); an empty query
 * repeats the last search. Esc or Ctrl-C cancels and restores the start. label
 * is the status-line prompt ("/" or "?" for vi, "ISearch: " for modeless). */
static void
incsearch(Editor *e, int dir, const char *label)
{
	size_t oy = e->cy, ox = e->cx, otop = e->top, oleft = e->left;
	char q[256];
	size_t len = 0;
	int found = 1;

	q[0] = '\0';
	for (;;) {
		Event ev;
		struct tkbd_seq seq;
		size_t my = oy, mx = ox;

		if (len > 0 &&
		    (found = isearch_scan_dir(e, q, oy, ox, dir, &my, &mx))) {
			e->cy = my;
			e->cx = mx;
			e->sel_active = 0;
		} else {			/* empty or failing: rest at origin */
			e->cy = oy;
			e->cx = ox;
			e->top = otop;
			e->left = oleft;
			if (len == 0)
				found = 1;
		}
		isearch_draw(e, label, q, found);

		if (scr_wait(e->d, &ev) == EVENT_EOF) {
			e->cy = oy, e->cx = ox, e->top = otop, e->left = oleft;
			return;
		}
		if (ev.type != EVENT_KEY) {
			scr_size(e->d, &e->rows, &e->cols);
			continue;
		}
		seq = ev.key;
		if (seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_ENTER) {
			if (len == 0) {
				if (e->last_find[0])
					ed_find_dir(e, e->last_find, dir);
				return;
			}
			snprintf(e->last_find, sizeof(e->last_find), "%s", q);
			e->vi_search_dir = dir;
			if (found)
				set_status(e,
				    "found '%.80s'", q);
			else
				set_status(e,
				    "not found: %.80s", q);
			return;
		}
		if (seq.key == TKBD_KEY_ESC ||
		    ((seq.mod & TKBD_MOD_CTRL) && seq.key == TKBD_KEY_C)) {
			e->cy = oy, e->cx = ox, e->top = otop, e->left = oleft;
			set_status(e, "search cancelled");
			return;
		}
		if (seq.key == TKBD_KEY_BACKSPACE ||
		    seq.key == TKBD_KEY_BACKSPACE2) {
			while (len > 0 &&
			    ((unsigned char)q[len - 1] & 0xc0) == 0x80)
				len--;
			if (len > 0)
				len--;
			q[len] = '\0';
			continue;
		}
		if (!(seq.mod & TKBD_MOD_CTRL) && seq.ch != TKBD_CH_NONE &&
		    seq.ch >= 0x20 && seq.ch != 0x7f) {
			unsigned char enc[8];
			int el = utf8_encode(enc, seq.ch);

			if (el > 0 && len + (size_t)el < sizeof(q)) {
				memcpy(q + len, enc, (size_t)el);
				len += (size_t)el;
				q[len] = '\0';
			}
		}
	}
}

/* Modeless find: forward incremental search. */
static void
find_prompt(Editor *e)
{
	incsearch(e, 1, "ISearch: ");
}

/* Prompt for a 1-based line number and move the cursor to that line. A number
 * past the end clamps to the last line. */
static void
goto_prompt(Editor *e)
{
	char buf[32], *end;
	long ln;

	buf[0] = '\0';
	if (!dlg_prompt_line(e, "Go to line: ", buf, sizeof(buf))) {
		set_status(e, "goto cancelled");
		return;
	}
	ln = strtol(buf, &end, 10);
	if (end == buf || ln < 1) {
		set_status(e, "bad line number");
		return;
	}
	if ((size_t)ln > text_lines(e->t))
		ln = (long)text_lines(e->t);
	e->cy = (size_t)ln - 1;
	e->cx = 0;
	e->sel_active = 0;
	clamp_col(e);
	set_status(e, "line %ld", ln);
}

/* The key bindings, as shown by the help screen. Kept next to the keymap so
 * the two stay in step. */
static const struct {
	const char	*keys;
	const char	*desc;
} help_entries[] = {
	{ "arrows",		"Move the cursor" },
	{ "Home / End",		"Start / end of line" },
	{ "Ctrl+Home / End",	"Start / end of the file" },
	{ "PgUp / PgDn",	"Scroll by a screen" },
	{ "Shift+arrows",	"Extend a selection" },
	{ "Enter",		"Split the line" },
	{ "Backspace / Del",	"Delete before / after the cursor" },
	{ "Ctrl-F",		"Incremental find (Enter repeats the last)" },
	{ "Ctrl-R",		"Replace, confirming each match (y/n/a/q)" },
	{ "Ctrl-T",		"Go to a symbol (buffer, plus a tags file if any)" },
	{ "Ctrl-]",		"Jump to the tag under the cursor (tags file)" },
	{ "Ctrl-L",		"Go to a line number" },
	{ "Ctrl-C / Ctrl-X",	"Copy (line if none selected) / cut" },
	{ "Ctrl-V",		"Paste the clipboard" },
	{ "Ctrl-Z / Ctrl-Y",	"Undo / redo" },
	{ "Ctrl-S",		"Save (asks for a name if none)" },
	{ "Ctrl-Q",		"Quit (asks if there are unsaved changes)" },
	{ "F1",			"Show this help" },
	{ "F2",			"Toggle vi keys (modal editing)" },
	{ "F8 / Shift+F8",	"Next / previous open buffer" },
#ifndef VEDIT_NO_TOOLS
	{ "Alt+F9 / F9",	"Compile the file / make the project" },
	{ "Ctrl+F9 / Alt+F5",	"Run the program / view the last output" },
	{ "F4 / Shift+F4",	"Next / previous build error (wraps around)" },
#endif
};

#define HELP_COUNT ((int)(sizeof(help_entries) / sizeof(help_entries[0])))

/* The vi-personality bindings, shown by the help screen while modal editing
 * is on. Kept next to vi_normal_key and vi_colon so the three stay in step. */
static const struct {
	const char	*keys;
	const char	*desc;
} help_entries_vi[] = {
	{ "h j k l / arrows",	"Move the cursor" },
	{ "0 ^ $",		"Line start / first word / line end" },
	{ "w b e / W B E",	"Word forward / back / end" },
	{ "gg / G",		"First / last line" },
	{ "Ctrl+Home / End",	"First / last line (same as gg / G)" },
	{ "{ } ( )",		"Paragraph / sentence motion" },
	{ "% H M L |",		"Match pair, screen high/mid/low, column" },
	{ "f F t T ; ,",	"Find a char in the line, then repeat" },
	{ "Ctrl-F/B Ctrl-D/U",	"Scroll a page / half a page" },
	{ "i a o I A O",	"Enter insert mode (Esc returns to normal)" },
	{ "x  dd cc yy",	"Delete char, cut / change / yank a line" },
	{ "d c y + motion",	"Operate over a motion" },
	{ "v  V  Ctrl-V",	"Visual char / line / block select" },
	{ "block: d y I A",	"Delete, yank, insert at left, append at right" },
	{ "p / P",		"Paste after / before the cursor" },
	{ "\"a  qa  @a",	"Register a: prefix yank/put, record keys, replay" },
	{ "ma  `a  'a",		"Set mark a, jump to it (exact / line)" },
	{ "Ctrl-O / Ctrl-I",	"Jump list: older / newer (`` toggles ends)" },
	{ "u / Ctrl-R",		"Undo / redo" },
	{ "/ text  n",		"Search forward, repeat the last search" },
	{ ":w  :q  :wq / :x",	"Write, quit, write and quit" },
	{ ":q!  ZZ  ZQ",	"Quit discarding, save and quit, quit" },
	{ ":N  :cq",		"Go to a line, quit with an error code" },
	{ "Ctrl-]  :tag",	"Jump to a tag (under cursor / by name)" },
	{ "Ctrl-T  :pop",	"Pop the tag stack back to the last jump" },
	{ "gf",			"Open the header or file named under the cursor" },
	{ ":reload",		"Re-read the config file (also Options menu)" },
	{ "F1 / F2",		"Show this help / back to modeless keys" },
};

#define HELP_VI_COUNT \
	((int)(sizeof(help_entries_vi) / sizeof(help_entries_vi[0])))

/* Paint a full-screen scrollable view: a bold header row, body lines drawn
 * from `top` (get_line returns the text for an absolute index, or NULL past
 * the end), and a bold footer row. Used by the help and build-output screens.
 */
static void
ui_scroll_view(Editor *e, const char *header, const char *footer,
    int top, const char *(*get_line)(Editor *, void *, int), void *ctx)
{
	const Pal *p = ed_chrome(e);
	uint16_t barat = (p->reverse_bars ? ATTR_REVERSE : 0) | ATTR_BOLD;
	Screen *d = e->d;
	int rows = e->rows > 0 ? e->rows : 24;
	int row;

	scr_clear(d);
	ui_field(d, 0, 0, e->cols, header, p->bar_fg, p->bar_bg, barat);
	for (row = 1; row < rows - 1; row++) {
		const char *s = get_line(e, ctx, top + row - 1);

		ui_field(d, row, 0, e->cols, s ? s : "", p->content_fg,
		    p->content_bg, 0);
	}
	ui_field(d, rows - 1, 0, e->cols, footer, p->bar_fg, p->bar_bg, barat);
	scr_present(d);
}

/* Line provider for the help view: one formatted "keys  description" row from
 * the active personality's table. */
static const char *
help_line(Editor *e, void *ctx, int idx)
{
	static char line[128];
	int vi = e->mode != MODE_MODELESS;
	int n = vi ? HELP_VI_COUNT : HELP_COUNT;

	(void)ctx;
	if (idx < 0 || idx >= n)
		return NULL;
	snprintf(line, sizeof(line), "  %-20s %s",
	    vi ? help_entries_vi[idx].keys : help_entries[idx].keys,
	    vi ? help_entries_vi[idx].desc : help_entries[idx].desc);
	return line;
}

/* ---- tutorials (Help > Tutorial, and 't' on the key-bindings screen) ----
 *
 * A tutorial is a title and an array of body lines, shown in a scrollable
 * full-screen view. Each line is pre-laid-out plain text (keep it within about
 * 72 columns so it fits a narrow terminal). Add a tutorial by writing its line
 * array and appending an entry to g_tutorials. */

static const char *const tut_draw[] = {
	"Draw mode turns the editor into a 2D canvas for box diagrams,",
	"maps, and block art. The blanks you draw are real spaces, so a",
	"drawing drops into any text file as-is.",
	"",
	"Turning it on and off",
	"",
	"  - Press Insert to toggle draw mode. It works in both the",
	"    modeless and vi personalities. Options > Draw Mode and the",
	"    vi :draw command do the same.",
	"  - The status line shows -- DRAW -- while draw mode is on.",
	"  - Press Insert again to return to normal editing.",
	"",
	"Moving and typing",
	"",
	"  - The cursor moves freely over a virtual grid. The arrows (or",
	"    vi h j k l) step one cell in any direction, even past the",
	"    end of a line or below the last line. The edges do not wrap.",
	"  - Typing overwrites the cell under the cursor; insert is off.",
	"    Drawing in empty space pads the line with spaces and adds",
	"    blank lines as needed, so you can draw anywhere.",
	"  - Enter moves down one row and back to column 0.",
	"  - Backspace and Delete erase a cell to a space. They never",
	"    join lines the way they do in normal editing.",
	"",
	"Boxes and lines",
	"",
	"  - Hold Shift and press the arrows to mark a rectangle.",
	"  - Ctrl-B draws a border around the marked rectangle: + at the",
	"    corners, - and | along the edges.",
	"  - A selection one cell wide or one cell tall collapses to a",
	"    straight line, so Ctrl-B draws lines as well as boxes.",
	"  - The glyphs are plain ASCII, so the drawing shows on any",
	"    terminal.",
	"",
	"Moving blocks around",
	"",
	"  - With a rectangle marked, Ctrl-C copies it and Ctrl-X cuts",
	"    it, blanking the rectangle in place.",
	"  - Ctrl-V overlays the copied block at the cursor, drawing over",
	"    whatever cells are there.",
	"",
	"Your first drawing",
	"",
	"  1. Press Insert to enter draw mode.",
	"  2. Move to some open space.",
	"  3. Hold Shift and arrow right, then down, to mark a rectangle.",
	"  4. Press Ctrl-B to draw its border.",
	"  5. Move inside the box and type a label.",
	"  6. Press Insert to leave draw mode, then Ctrl-S to save.",
	"",
	"Because a drawing is made of real spaces, draw mode does not",
	"trim trailing whitespace when it saves.",
};

typedef struct tutorial {
	const char	*title;
	const char *const *lines;
	int		 n;
} Tutorial;

#define TUT(arr) (arr), (int)(sizeof(arr) / sizeof((arr)[0]))
static const Tutorial g_tutorials[] = {
	{ "Line Draw Mode",	TUT(tut_draw) },
};
#undef TUT
#define TUTORIAL_COUNT ((int)(sizeof(g_tutorials) / sizeof(g_tutorials[0])))

/* Line provider for dlg_text_view: one body line by absolute index. */
static const char *
tutorial_line(Editor *e, void *ctx, int idx)
{
	const Tutorial *t = ctx;

	(void)e;
	if (idx < 0 || idx >= t->n)
		return NULL;
	return t->lines[idx];
}

/* Show one tutorial in a scrollable view: Up/Down and PgUp/PgDn scroll,
 * Home/End jump, Esc or q returns. */
static void
dlg_tutorial_show(Editor *e, const Tutorial *t)
{
	char hdr[128];
	int top = 0;

	snprintf(hdr, sizeof(hdr), " vedit tutorial -- %s", t->title);
	for (;;) {
		int rows = e->rows > 0 ? e->rows : 24;
		int body = rows - 2;		/* header and footer rows */
		int maxtop = (t->n > body) ? t->n - body : 0;
		Event ev;

		if (top > maxtop)
			top = maxtop;
		if (top < 0)
			top = 0;
		ui_scroll_view(e, hdr,
		    " Up/Down PgUp/PgDn Home/End scroll   Esc or q returns",
		    top, tutorial_line, (void *)t);
		if (scr_wait(e->d, &ev) == EVENT_EOF)
			return;
		if (ev.type == EVENT_RESIZE || ev.type == EVENT_RESUME) {
			scr_size(e->d, &e->rows, &e->cols);
			continue;
		}
		if (ev.type != EVENT_KEY || ev.key.type != TKBD_KEY)
			continue;
		switch (ev.key.key) {
		case TKBD_KEY_UP:	top -= 1; break;
		case TKBD_KEY_DOWN:	top += 1; break;
		case TKBD_KEY_PGUP:	top -= body; break;
		case TKBD_KEY_PGDN:	top += body; break;
		case TKBD_KEY_HOME:	top = 0; break;
		case TKBD_KEY_END:	top = maxtop; break;
		case TKBD_KEY_ESC:	return;
		default:
			if (ev.key.ch == 'q' || ev.key.ch == 'Q')
				return;
			break;
		}
	}
}

/* Open the tutorials. With a single tutorial it opens straight away. */
static void
dlg_tutorial(Editor *e)
{
	dlg_tutorial_show(e, &g_tutorials[0]);
}

/* Show the help screen and wait for a key. Any key returns to editing, except
 * 't', which opens the tutorial. */
static void
dlg_help(Editor *e)
{
	const char *hdr = e->mode != MODE_MODELESS ?
	    " vedit -- vi key bindings" : " vedit -- key bindings";

	for (;;) {
		Event ev;

		ui_scroll_view(e, hdr,
		    " Press t for the tutorial, any other key to return", 0,
		    help_line, NULL);
		switch (scr_wait(e->d, &ev)) {
		case EVENT_KEY:
			if (ev.key.type != TKBD_KEY)
				break;
			if (ev.key.ch == 't' || ev.key.ch == 'T') {
				dlg_tutorial(e);
				break;		/* back to the key list */
			}
			return;			/* any other key returns */
		case EVENT_RESIZE:
		case EVENT_RESUME:
			scr_size(e->d, &e->rows, &e->cols);
			break;
		case EVENT_EOF:
			return;
		default:
			break;
		}
	}
}

/****************************************************************
 * Menu bar actions and the pull-down modal loop
 ****************************************************************/

typedef enum dlg_result { DLG_CANCEL = -1, DLG_NO = 0, DLG_YES = 1 } Dlgresult;

/* Result chosen by button index 0/1/2. */
static Dlgresult
dlg_btn_result(int i)
{
	return i == 0 ? DLG_YES : i == 1 ? DLG_NO : DLG_CANCEL;
}

static const char *const dlg_confirm_btn[3] = {
	"[ &Yes ]", "[ &No ]", "[ &Cancel ]"
};

/* State of the save-confirm dialog across its modal frames. */
typedef struct confirm_ctx {
	const char	*msg;
	int		focus;		/* 0 Yes, 1 No, 2 Cancel */
	Dlgresult	result;		/* what to return; Cancel by default */
	int		bx[3];		/* button columns, set by the draw pass */
	int		brow;		/* button row, set by the draw pass */
} Confirmctx;

static void
dlg_confirm_draw(Editor *e, const Modal *m, void *ctx)
{
	Confirmctx *c = ctx;
	Screen *d = e->d;
	int msglen = (int)strlen(c->msg);
	int brow_w = 4;			/* two 2-space gaps between 3 buttons */
	int i, cx;

	for (i = 0; i < 3; i++)
		brow_w += menu_disp_w(dlg_confirm_btn[i]);
	c->brow = m->y + m->h - 2;
	cx = m->x + (m->w - brow_w) / 2;
	scr_text(d, m->y + 1, m->x + (m->w - msglen) / 2, c->msg,
	    m->fg, m->bg, m->base);
	for (i = 0; i < 3; i++) {
		uint16_t at = (i == c->focus) ?
		    m->base ^ ATTR_REVERSE : m->base;

		c->bx[i] = cx;
		ui_menu_label(d, c->brow, cx, dlg_confirm_btn[i], m->fg, m->bg,
		    at);
		cx += menu_disp_w(dlg_confirm_btn[i]) + 2;
	}
}

static int
dlg_confirm_key(Editor *e, const Modal *m,
    const Event *ev, void *ctx)
{
	Confirmctx *c = ctx;
	int i;

	(void)e;
	(void)m;
	if (ev->key.type == TKBD_MOUSE) {
		if (ev->key.key == TKBD_MOUSE_LEFT &&
		    !(ev->key.mod & TKBD_MOD_MOTION) && ev->key.y == c->brow)
			for (i = 0; i < 3; i++)
				if (ev->key.x >= c->bx[i] && ev->key.x <
				    c->bx[i] + menu_disp_w(dlg_confirm_btn[i])) {
					c->result = dlg_btn_result(i);
					return 1;
				}
		return 0;		/* ignore other mouse events */
	}
	if (ev->key.type != TKBD_KEY)
		return 0;
	if (ev->key.ch != TKBD_CH_NONE && ev->key.ch < 128) {
		int lc = tolower((int)ev->key.ch);

		for (i = 0; i < 3; i++)
			if (menu_mnemonic(dlg_confirm_btn[i]) == lc) {
				c->result = dlg_btn_result(i);
				return 1;
			}
	}
	switch (ev->key.key) {
	case TKBD_KEY_ESC:
		c->result = DLG_CANCEL;
		return 1;
	case TKBD_KEY_LEFT:
		c->focus = (c->focus + 2) % 3;
		return 0;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_TAB:
		c->focus = (c->focus + 1) % 3;
		return 0;
	case TKBD_KEY_ENTER:
		c->result = dlg_btn_result(c->focus);
		return 1;
	default:
		return 0;
	}
}

/* A centered modal asking whether to save, with Yes/No/Cancel buttons. The
 * arrow keys or Tab move focus, Enter picks the focused button, an underlined
 * mnemonic letter (Y/N/C) chooses directly, Esc cancels, and a click selects a
 * button. */
static Dlgresult
dlg_confirm_save_dialog(Editor *e, const char *msg)
{
	Confirmctx c = { msg, 0, DLG_CANCEL, { 0, 0, 0 }, 0 };
	int msglen = (int)strlen(msg);
	int brow_w = 4;			/* two 2-space gaps between 3 buttons */
	int i, boxw;

	for (i = 0; i < 3; i++)
		brow_w += menu_disp_w(dlg_confirm_btn[i]);
	boxw = (msglen > brow_w ? msglen : brow_w) + 4 + 2;
	dlg_run(e, boxw, 5, &c, dlg_confirm_draw, dlg_confirm_key);
	return c.result;
}

/* Offer to save a dirty buffer before it is replaced or the editor exits.
 * Returns 1 to proceed (saved or discarded), 0 to abort the operation. */
static int
dlg_confirm_save(Editor *e, const char *msg)
{
	if (!text_dirty(e->t))
		return 1;
	switch (dlg_confirm_save_dialog(e, msg)) {
	case DLG_CANCEL:
		return 0;
	case DLG_YES:
		return save_editor(e) == 0;
	case DLG_NO:
	default:
		return 1;		/* discard */
	}
}

/* Confirm before replacing the current buffer (New/Open). */
static int
dlg_confirm_discard(Editor *e)
{
	return dlg_confirm_save(e, "Save changes to the current file?");
}

/* Reset syntax and cursor state after the buffer is swapped. */
static void
buffer_reset(Editor *e)
{
	e->cy = e->cx = e->top = e->left = 0;
	e->sel_active = 0;
	e->hl_valid = 0;
}

/* ---- multi-buffer management ------------------------------------------- *
 * The active buffer's per-file state lives in the flat Editor, which
 * is authoritative for it; the parked buffers live in e->bufs. buf_save mirrors
 * the flat state into a slot, buf_load mirrors a slot back. Only the active
 * buffer is ever read from the flat fields, so a parked slot may lag until the
 * next save. */

/* The per-buffer fields mirrored between the flat Editor and a Buf slot, listed
 * once so buf_save and buf_load cannot fall out of sync. Adding a mirrored
 * field means declaring it in both struct ebuf and struct editor, then adding
 * one line here (scalars assign; fixed-size arrays copy by value). */
#define BUF_STATE_SCALARS(X) \
	X(t) X(has_name) X(cy) X(cx) X(top) X(left) X(sel_active) X(ay) X(ax) \
	X(syn) X(line_state) X(line_state_cap) X(hl_valid) X(hex_view) \
	X(hex_top) X(expand_tabs) X(vi_marks_set) X(swap_on) X(swap_rev) \
	X(load_mtime)
#define BUF_STATE_ARRAYS(X) \
	X(path) X(vi_mark_y) X(vi_mark_x) X(swap_path)

/* Copy the active buffer's per-file fields into a slot. */
static void
buf_save(Editor *e, Buf *b)
{
#define CP(f) b->f = e->f;
	BUF_STATE_SCALARS(CP)
#undef CP
#define CP(f) memcpy(b->f, e->f, sizeof(b->f));
	BUF_STATE_ARRAYS(CP)
#undef CP
}

/* Mirror a slot into the flat editor and drop any in-flight vi command. */
static void
buf_load(Editor *e, const Buf *b)
{
#define CP(f) e->f = b->f;
	BUF_STATE_SCALARS(CP)
#undef CP
#define CP(f) memcpy(e->f, b->f, sizeof(e->f));
	BUF_STATE_ARRAYS(CP)
#undef CP
	e->vi_visual = 0;
	e->vi_want_col = e->vi_vert_run = e->vi_vert_prev = 0;
	e->hex_ascii = 0;
	e->hex_pending = -1;
	e->hex_insert = 0;
	e->hex_sel = 0;
	vi_reset_pending(e);
}

/* Free the file resources a slot owns (its text and syntax scratch). Used
 * both for a parked slot and, with the flat editor's own pointers, for the
 * active buffer. The diagnostics list is global, not per-buffer, and is
 * freed once at teardown. */
static void
buf_free_fields(Text *t, uint16_t *line_state)
{
	text_free(t);
	free(line_state);
}

/* Ensure room for one more buffer and return its index (nbuf grows). */
static int
buf_slot(Editor *e)
{
	if (e->nbuf >= e->bufs_cap) {
		int nc = e->bufs_cap ? e->bufs_cap * 2 : 4;
		Buf *nb = realloc(e->bufs, (size_t)nc * sizeof(*nb));

		if (!nb)
			return -1;
		e->bufs = nb;
		e->bufs_cap = nc;
	}
	return e->nbuf++;
}

void
buf_switch(Editor *e, int i)
{
	if (i < 0 || i >= e->nbuf || i == e->cur)
		return;
	swap_maybe_write(e);		/* flush the outgoing buffer's swap */
	buf_save(e, &e->bufs[e->cur]);
	e->cur = i;
	buf_load(e, &e->bufs[i]);
}

int
buf_cycle(Editor *e, int dir)
{
	int i;

	if (e->nbuf <= 1)
		return e->cur;
	i = ((e->cur + dir) % e->nbuf + e->nbuf) % e->nbuf;
	buf_switch(e, i);
	return e->cur;
}

/* Whether a and b name the same file for buffer de-duplication. When both exist
 * on disk, they match by device and inode, so a symlink, a "./" or "../" detour,
 * and a hard link all resolve to one buffer. A plain byte comparison is the
 * fallback, covering a name that is not on disk yet (a new file). */
static int
buf_same_file(const char *a, const char *b)
{
	struct stat sa, sb;

	if (strcmp(a, b) == 0)
		return 1;
	if (stat(a, &sa) == 0 && stat(b, &sb) == 0)
		return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
	return 0;
}

int
buf_open(Editor *e, const char *path)
{
	Text *nt;
	int i, swap_action = SWAP_NONE;
	time_t swap_mtime = 0;

	if (path && path[0]) {			/* already open? just switch */
		for (i = 0; i < e->nbuf; i++) {
			const char *bp = (i == e->cur) ? e->path :
			    e->bufs[i].path;
			int named = (i == e->cur) ? e->has_name :
			    e->bufs[i].has_name;

			if (named && buf_same_file(bp, path)) {
				buf_switch(e, i);
				return i;
			}
		}
	}
	nt = text_new();
	if (!nt) {
		set_status(e, "out of memory");
		return -1;
	}
	if (path && path[0] && text_load(nt, path) < 0 && errno != ENOENT) {
		set_status(e, "open failed: %s",
		    strerror(errno));
		text_free(nt);
		return -1;
	}
	if (path && path[0]) {			/* offer swap recovery */
		struct stat st;
		time_t mt = (stat(path, &st) == 0) ? st.st_mtime : 0;

		swap_action = swap_recover(e, path, nt, mt);
		swap_mtime = mt;
		if (swap_action == SWAP_ABORT) {
			text_free(nt);
			return -1;
		}
	}
	i = buf_slot(e);
	if (i < 0) {
		text_free(nt);
		set_status(e, "out of memory");
		return -1;
	}
	buf_save(e, &e->bufs[e->cur]);		/* park the current buffer */
	e->cur = i;
	e->t = nt;				/* set up the flat new buffer */
	if (path && path[0]) {
		snprintf(e->path, sizeof(e->path), "%s", path);
		e->has_name = 1;
		e->syn = syn_for_ext(file_ext(e->path));
	} else {
		e->path[0] = '\0';
		e->has_name = 0;
		e->syn = NULL;
	}
	e->expand_tabs = indent_expand_default(e->syn ? e->syn->name : NULL);
	e->cy = e->cx = e->top = e->left = 0;
	e->sel_active = 0;
	e->line_state = NULL;
	e->line_state_cap = 0;
	e->hl_valid = 0;
	memset(e->vi_mark_y, 0, sizeof(e->vi_mark_y));
	memset(e->vi_mark_x, 0, sizeof(e->vi_mark_x));
	e->vi_marks_set = 0;
	e->vi_visual = 0;
	vi_reset_pending(e);
	if (e->has_name)
		swap_adopt(e, swap_mtime, swap_action);
	else {
		e->swap_path[0] = '\0';
		e->swap_on = 0;
		e->swap_rev = e->t->rev;
		e->load_mtime = 0;
	}
	buf_save(e, &e->bufs[i]);		/* keep the slot consistent */
	if (swap_action != SWAP_RECOVERED)
		set_status(e, "%.120s [%d/%d]",
		    e->has_name ? e->path : "new buffer", e->cur + 1, e->nbuf);
	return i;
}

int
buf_close(Editor *e, int i)
{
	if (e->nbuf <= 1 || i < 0 || i >= e->nbuf)
		return -1;

	if (i == e->cur) {
		swap_remove(e);			/* closed cleanly: drop its swap */
		buf_free_fields(e->t, e->line_state);
		memmove(&e->bufs[i], &e->bufs[i + 1],
		    (size_t)(e->nbuf - i - 1) * sizeof(*e->bufs));
		e->nbuf--;
		e->cur = i < e->nbuf ? i : e->nbuf - 1;
		buf_load(e, &e->bufs[e->cur]);
	} else {
		if (e->bufs[i].swap_on && e->bufs[i].swap_path[0])
			unlink(e->bufs[i].swap_path);
		buf_free_fields(e->bufs[i].t, e->bufs[i].line_state);
		memmove(&e->bufs[i], &e->bufs[i + 1],
		    (size_t)(e->nbuf - i - 1) * sizeof(*e->bufs));
		e->nbuf--;
		if (e->cur > i)
			e->cur--;
	}
	return 0;
}

void
buf_list(Editor *e)
{
	char *p = e->status;
	size_t rem = sizeof(e->status);
	int i;

	for (i = 0; i < e->nbuf && rem > 1; i++) {
		int active = (i == e->cur);
		const char *name = active ?
		    (e->has_name ? e->path : "[No Name]") :
		    (e->bufs[i].has_name ? e->bufs[i].path : "[No Name]");
		const char *slash = strrchr(name, '/');
		int n;

		if (slash)
			name = slash + 1;
		n = snprintf(p, rem, "%s%d:%s%s", i ? "  " : "", i + 1,
		    active ? "*" : "", name);
		if (n < 0 || (size_t)n >= rem)
			break;
		p += n;
		rem -= (size_t)n;
	}
}

/****************************************************************
 * Buffer switcher (a picker client)
 ****************************************************************/

typedef struct bufpick {
	Editor	*e;
	char	 line[PATH_MAX + 64];	/* reused by label() for one row */
	int	 chosen;		/* selected buffer index on PICK_DONE */
} Bufpick;

static const char *
bufpick_title(void *ctx)
{
	(void)ctx;
	return "Buffers";
}

static int
bufpick_count(void *ctx)
{
	return ((Bufpick *)ctx)->e->nbuf;
}

static const char *
bufpick_label(void *ctx, int i)
{
	Bufpick *bp = ctx;
	Editor *e = bp->e;
	int active = (i == e->cur);
	const char *name, *slash;
	const Text *t;

	if (i < 0 || i >= e->nbuf)
		return "";
	/* The active buffer's live state is in the flat editor; a parked one's
	 * is in its slot. Its Text pointer is shared, so dirty and line counts
	 * read correctly from either. */
	name = active ? (e->has_name ? e->path : "[No Name]")
	    : (e->bufs[i].has_name ? e->bufs[i].path : "[No Name]");
	slash = strrchr(name, '/');
	if (slash)
		name = slash + 1;
	t = active ? e->t : e->bufs[i].t;
	snprintf(bp->line, sizeof(bp->line), "%d%s  %s%s  %zuL", i + 1,
	    active ? " *" : "", name, (t && text_dirty(t)) ? " [+]" : "",
	    t ? text_lines(t) : 0);
	return bp->line;
}

static int
bufpick_choose(void *ctx, int i)
{
	Bufpick *bp = ctx;

	if (i < 0 || i >= bp->e->nbuf)
		return PICK_STAY;
	bp->chosen = i;
	return PICK_DONE;
}

/* Open a modal list of the buffers and switch to the chosen one. */
static void
dlg_buffer_pick(Editor *e)
{
	Picksrc s = {
		.title = bufpick_title, .count = bufpick_count,
		.label = bufpick_label, .choose = bufpick_choose,
	};
	Bufpick bp;

	if (e->nbuf < 1)
		return;
	memset(&bp, 0, sizeof(bp));
	bp.e = e;
	bp.chosen = e->cur;
	s.ctx = &bp;
	if (dlg_pick(e, &s))
		buf_switch(e, bp.chosen);
}

/****************************************************************
 * ctags "tags" file: a parsed index of definitions across the
 * project, feeding the symbol picker and the :tag / Ctrl-] jumps.
 * The format is Exuberant/Universal ctags (also classic vi tags):
 *   name <TAB> file <TAB> address[;" fields...]
 * where address is a line number or a /pattern/ search, and the
 * optional fields carry the kind. See https://ctags.io/ and
 * https://ctags.sourceforge.net/.
 ****************************************************************/

/* Directory part of path (everything up to the last '/'), or "." when none. */
static void
path_dir(const char *path, char *out, size_t outsz)
{
	const char *slash = strrchr(path, '/');

	if (slash) {
		size_t n = (size_t)(slash - path);

		if (n >= outsz)
			n = outsz - 1;
		memcpy(out, path, n);
		out[n] = '\0';
	} else {
		snprintf(out, outsz, ".");
	}
}

/* Whether a and b name the same file on disk (realpath), else a byte compare. */
static int
same_path(const char *a, const char *b)
{
	char ra[PATH_MAX], rb[PATH_MAX];

	if (realpath(a, ra) && realpath(b, rb))
		return strcmp(ra, rb) == 0;
	return strcmp(a, b) == 0;
}

/* Collapse ".", ".." and duplicate slashes in a path, lexically: no disk
 * access and no symlink resolution, so it works on paths that do not exist and
 * never blocks. A leading '/' is kept, and an empty result becomes ".". 0 on
 * success, -1 if the result would not fit or the path has too many segments
 * (the caller then falls back to the raw path). */
static int
path_normalize(const char *in, char *out, size_t outsz)
{
	int starts[256];		/* out-offset where each kept segment begins */
	int nseg = 0, absolute = (in[0] == '/');
	const char *p = in;
	size_t o = 0, base;

	if (outsz == 0)
		return -1;
	if (absolute)
		out[o++] = '/';
	base = o;
	while (*p) {
		const char *s;
		size_t n;

		while (*p == '/')
			p++;
		s = p;
		while (*p && *p != '/')
			p++;
		n = (size_t)(p - s);
		if (n == 0)
			break;			/* trailing slash */
		if (n == 1 && s[0] == '.')
			continue;		/* "." : drop */
		if (n == 2 && s[0] == '.' && s[1] == '.') {
			if (nseg > 0) {
				size_t ls = (size_t)starts[nseg - 1];

				if (!(o - ls == 2 && out[ls] == '.' &&
				    out[ls + 1] == '.')) {
					nseg--;		/* pop a real segment */
					o = ls;
					if (o > base)
						o--;	/* and its separator */
					continue;
				}
			}
			if (absolute)
				continue;	/* ".." above root: drop */
			/* relative with nothing to pop: keep ".." literally */
		}
		if (nseg >= (int)(sizeof(starts) / sizeof(starts[0])))
			return -1;
		if (o > base) {			/* separator before a non-first seg */
			if (o + 1 >= outsz)
				return -1;
			out[o++] = '/';
		}
		starts[nseg++] = (int)o;
		if (o + n >= outsz)
			return -1;
		memcpy(out + o, s, n);
		o += n;
	}
	if (o == 0)
		out[o++] = '.';
	out[o] = '\0';
	return 0;
}

typedef struct tagent {
	char	name[64];
	char	kind;		/* ctags kind letter, or 0 when unknown */
	int	file_idx;	/* index into Tagdb.files */
	size_t	line;		/* 1-based line address, 0 when a pattern is used */
	char	pattern[160];	/* unescaped search text, empty when line > 0 */
} Tagent;

typedef struct tagdb {
	Tagent	*ent;
	int	 n, cap;
	char	**files;	/* interned file names, as written in the tags file */
	int	 nfiles, files_cap;
	char	 dir[PATH_MAX];	/* directory holding the tags file */
} Tagdb;

static void
tagdb_free(Tagdb *db)
{
	int i;

	free(db->ent);
	for (i = 0; i < db->nfiles; i++)
		free(db->files[i]);
	free(db->files);
	memset(db, 0, sizeof(*db));
}

/* Intern a file name, returning its index, or -1 on allocation failure. */
static int
tagdb_file_idx(Tagdb *db, const char *name)
{
	int i;
	size_t len;
	char *copy;

	for (i = 0; i < db->nfiles; i++)
		if (strcmp(db->files[i], name) == 0)
			return i;
	if (db->nfiles == db->files_cap) {
		int nc = db->files_cap ? db->files_cap * 2 : 16;
		char **nf = realloc(db->files, (size_t)nc * sizeof(*nf));

		if (!nf)
			return -1;
		db->files = nf;
		db->files_cap = nc;
	}
	len = strlen(name) + 1;
	copy = malloc(len);
	if (!copy)
		return -1;
	memcpy(copy, name, len);
	db->files[db->nfiles] = copy;
	return db->nfiles++;
}

/* Parse the address and extension fields of a tag line into te. The address is
 * a line number or a /pat/ or ?pat? search ending at the ';"' field marker or
 * the line end; the kind comes from a "kind:X" or a bare single-letter field. */
static void
tagdb_parse_addr(char *addr, Tagent *te)
{
	char *fields = NULL;

	while (*addr == ' ' || *addr == '\t')
		addr++;
	if (isdigit((unsigned char)*addr)) {
		te->line = (size_t)strtoul(addr, NULL, 10);
	} else if (*addr == '/' || *addr == '?') {
		char delim = *addr;
		char *p = addr + 1, *out = te->pattern;
		size_t cap = sizeof(te->pattern) - 1;

		if (*p == '^')			/* drop the line-start anchor */
			p++;
		while (*p && *p != delim) {
			if (*p == '\\' && p[1]) {	/* \/ \\ \t etc. */
				p++;
				if (out < te->pattern + cap)
					*out++ = *p;
				p++;
				continue;
			}
			if (*p == '$' && (p[1] == delim || p[1] == '\0'))
				break;		/* drop the line-end anchor */
			if (out < te->pattern + cap)
				*out++ = *p;
			p++;
		}
		*out = '\0';
	}
	/* extension fields begin at the ';"' marker */
	fields = strstr(addr, ";\"");
	if (fields) {
		char *tok;

		fields += 2;
		for (tok = strtok(fields, "\t"); tok; tok = strtok(NULL, "\t")) {
			if (strncmp(tok, "kind:", 5) == 0 && tok[5])
				te->kind = tok[5];
			else if (strncmp(tok, "line:", 5) == 0)
				te->line = (size_t)strtoul(tok + 5, NULL, 10);
			else if (tok[0] && tok[1] == '\0' && !te->kind)
				te->kind = tok[0];	/* bare single-letter kind */
		}
	}
}

/* Parse one tags-file line (modified in place) into db. */
static int
tagdb_parse_line(Tagdb *db, char *line)
{
	char *name, *file, *addr, *tab;
	Tagent *te;
	int fidx;

	if (line[0] == '\0' || line[0] == '!')	/* blank or !_TAG_ pseudo-tag */
		return 0;
	name = line;
	tab = strchr(name, '\t');
	if (!tab)
		return 0;
	*tab = '\0';
	file = tab + 1;
	tab = strchr(file, '\t');
	if (!tab)
		return 0;
	*tab = '\0';
	addr = tab + 1;
	if (!name[0] || !file[0])
		return 0;

	fidx = tagdb_file_idx(db, file);
	if (fidx < 0)
		return -1;
	if (db->n == db->cap) {
		int nc = db->cap ? db->cap * 2 : 256;
		Tagent *ne = realloc(db->ent, (size_t)nc * sizeof(*ne));

		if (!ne)
			return -1;
		db->ent = ne;
		db->cap = nc;
	}
	te = &db->ent[db->n];
	memset(te, 0, sizeof(*te));
	snprintf(te->name, sizeof(te->name), "%s", name);
	te->file_idx = fidx;
	tagdb_parse_addr(addr, te);
	db->n++;
	return 0;
}

/* Locate the tags file: an explicit tags.file config path, else a "tags" file
 * in the current buffer's directory. Writes the path to out; returns 0, or -1
 * when none is readable. */
static int
tags_locate(Editor *e, char *out, size_t outsz)
{
	const char *cfg = cfg_proj_get("tags.file");
	char dir[PATH_MAX];

	if (cfg && cfg[0]) {
		snprintf(out, outsz, "%s", cfg);
		return access(out, R_OK) == 0 ? 0 : -1;
	}
	if (!e->has_name)
		return -1;
	path_dir(e->path, dir, sizeof(dir));
	if (snprintf(out, outsz, "%s/tags", dir) >= (int)outsz)
		return -1;
	return access(out, R_OK) == 0 ? 0 : -1;
}

/* Read and parse the tags file at path into db. Returns 0, or -1 on error. */
static int
tags_load(Tagdb *db, const char *path)
{
	FILE *fp = fopen(path, "rb");
	char *data = NULL;
	size_t len = 0, cap = 0, start, i;
	int c;

	if (!fp)
		return -1;
	while ((c = fgetc(fp)) != EOF) {
		if (len + 2 > cap) {
			size_t nc = cap ? cap * 2 : 8192;
			char *p = realloc(data, nc);

			if (!p) {
				free(data);
				fclose(fp);
				return -1;
			}
			data = p;
			cap = nc;
		}
		data[len++] = (char)c;
	}
	fclose(fp);
	if (!data)				/* empty file */
		return 0;
	data[len] = '\0';
	path_dir(path, db->dir, sizeof(db->dir));

	for (start = 0, i = 0; i < len; i++) {
		if (data[i] == '\n') {
			data[i] = '\0';
			tagdb_parse_line(db, data + start);
			start = i + 1;
		}
	}
	if (start < len)			/* last line without a newline */
		tagdb_parse_line(db, data + start);
	free(data);
	return 0;
}

/* Resolve tag file index idx against the tags-file directory. */
static void
tag_resolve(const Tagdb *db, int idx, char *out, size_t outsz)
{
	const char *rel;

	if (idx < 0 || idx >= db->nfiles) {
		out[0] = '\0';
		return;
	}
	rel = db->files[idx];
	if (rel[0] == '/')
		snprintf(out, outsz, "%s", rel);
	else if (snprintf(out, outsz, "%s/%s", db->dir, rel) >= (int)outsz)
		snprintf(out, outsz, "%s", rel);
}

/* The first buffer line containing pat (a plain substring), or (size_t)-1. */
static size_t
buf_find_line(Editor *e, const char *pat)
{
	size_t n = text_lines(e->t), i, pl = strlen(pat);

	if (pl == 0)
		return (size_t)-1;
	for (i = 0; i < n; i++) {
		size_t len = 0, j;
		const char *s = text_line(e->t, i, &len);

		if (!s || len < pl)
			continue;
		for (j = 0; j + pl <= len; j++)
			if (memcmp(s + j, pat, pl) == 0)
				return i;
	}
	return (size_t)-1;
}

/* Open or switch to file and move the cursor to line (1-based; 0 means use the
 * pattern), or to the first line matching pattern. Returns 0, or -1. */
static int
ed_goto_target(Editor *e, const char *file, size_t line, const char *pattern)
{
	if (buf_open(e, file) < 0)
		return -1;
	if (text_lines(e->t) == 0)
		return -1;
	if (line == 0 && pattern && pattern[0]) {
		size_t ly = buf_find_line(e, pattern);

		if (ly != (size_t)-1)
			line = ly + 1;
	}
	e->cy = line ? line - 1 : 0;
	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
	e->cx = 0;
	e->sel_active = 0;
	clamp_col(e);
	return 0;
}

/****************************************************************
 * Header navigation (compile_commands.json include paths)
 *
 * A clang compilation database is a JSON array of objects, each with
 * "directory", "file", and either a "command" string or an "arguments"
 * array. vedit reads only the include search directories from the entry
 * that matches the current file, so an #include line can open its header.
 * This parses just that shape; it is not a general JSON reader.
 ****************************************************************/

#define CC_MAX_INC 64

typedef struct ccincludes {
	char	dir[PATH_MAX];			/* the entry's compile directory */
	char	inc[CC_MAX_INC][PATH_MAX];	/* -I / -isystem search dirs */
	char	quote[CC_MAX_INC][PATH_MAX];	/* -iquote dirs (for "" form) */
	int	ninc;
	int	nquote;
	int	found;
} CcIncludes;

static char *
cc_skip_ws(char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

static int
cc_hex4(const char *p)
{
	int v = 0, i;

	for (i = 0; i < 4; i++) {
		char c = p[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= c - '0';
		else if (c >= 'a' && c <= 'f')
			v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v |= c - 'A' + 10;
		else
			return -1;
	}
	return v;
}

/* Unescape a JSON string in place and advance *pp past the closing quote.
 * Returns the (nul-terminated) start, or NULL when *pp is not a string. */
static char *
cc_string(char **pp)
{
	char *p = *pp, *start, *dst;

	if (*p != '"')
		return NULL;
	p++;
	start = dst = p;
	while (*p && *p != '"') {
		if (*p != '\\') {
			*dst++ = *p++;
			continue;
		}
		p++;
		switch (*p) {
		case 'b': *dst++ = '\b'; p++; break;
		case 'f': *dst++ = '\f'; p++; break;
		case 'n': *dst++ = '\n'; p++; break;
		case 'r': *dst++ = '\r'; p++; break;
		case 't': *dst++ = '\t'; p++; break;
		case 'u': {
			int cp = cc_hex4(p + 1);

			if (cp < 0) {
				*dst++ = *p++;
				break;
			}
			p += 5;
			if (cp <= 0x7f) {
				*dst++ = (char)cp;
			} else if (cp <= 0x7ff) {
				*dst++ = (char)(0xc0 | (cp >> 6));
				*dst++ = (char)(0x80 | (cp & 0x3f));
			} else {
				*dst++ = (char)(0xe0 | (cp >> 12));
				*dst++ = (char)(0x80 | ((cp >> 6) & 0x3f));
				*dst++ = (char)(0x80 | (cp & 0x3f));
			}
			break;
		}
		case '\0': break;
		default: *dst++ = *p++; break;
		}
	}
	if (*p == '"')
		p++;
	*dst = '\0';
	*pp = p;
	return start;
}

/* Skip one JSON value (string, array, object, or primitive), advancing *pp. */
static void
cc_skip_value(char **pp)
{
	char *p = cc_skip_ws(*pp);

	if (*p == '"') {
		cc_string(&p);
	} else if (*p == '[' || *p == '{') {
		char open = *p, close = open == '[' ? ']' : '}';
		int depth = 0;

		do {
			if (*p == '"') {		/* skip strings whole */
				cc_string(&p);
				continue;
			}
			if (*p == open)
				depth++;
			else if (*p == close)
				depth--;
			p++;
		} while (*p && depth > 0);
	} else {
		while (*p && *p != ',' && *p != '}' && *p != ']')
			p++;
	}
	*pp = p;
}

/* Append a search dir, resolving a relative one against the compile dir. */
static void
cc_add_dir(char arr[][PATH_MAX], int *n, const char *base, const char *d)
{
	if (*n >= CC_MAX_INC || !d || !d[0])
		return;
	if (d[0] == '/')
		snprintf(arr[*n], PATH_MAX, "%s", d);
	else
		snprintf(arr[*n], PATH_MAX, "%.2040s/%.2040s", base, d);
	(*n)++;
}

/* Pull -I / -isystem / -iquote dirs from one entry's argument list. */
static void
cc_extract(CcIncludes *out, const char *dir, char **argv, int argc)
{
	int i;

	snprintf(out->dir, sizeof(out->dir), "%s", dir);
	for (i = 0; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] == '-' && a[1] == 'I') {
			if (a[2])
				cc_add_dir(out->inc, &out->ninc, dir, a + 2);
			else if (i + 1 < argc)
				cc_add_dir(out->inc, &out->ninc, dir, argv[++i]);
		} else if (strcmp(a, "-isystem") == 0 && i + 1 < argc) {
			cc_add_dir(out->inc, &out->ninc, dir, argv[++i]);
		} else if (strcmp(a, "-iquote") == 0 && i + 1 < argc) {
			cc_add_dir(out->quote, &out->nquote, dir, argv[++i]);
		}
	}
	out->found = 1;
}

/* Split a shell-style "command" string in place into argv (pointers into s).
 * Handles double/single quotes and backslash escapes well enough to carry a
 * path with spaces. Returns a malloc'd, NULL-terminated array, or NULL. */
static char **
cc_split(char *s, int *argc_out)
{
	char **argv = NULL;
	int argc = 0, cap = 0;
	char *p = s;

	for (;;) {
		char *dst, *arg;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		arg = dst = p;
		while (*p && *p != ' ' && *p != '\t') {
			if (*p == '"' || *p == '\'') {
				char q = *p++;

				while (*p && *p != q) {
					if (q == '"' && *p == '\\' && p[1])
						p++;
					*dst++ = *p++;
				}
				if (*p == q)
					p++;
			} else if (*p == '\\' && p[1]) {
				p++;
				*dst++ = *p++;
			} else {
				*dst++ = *p++;
			}
		}
		if (*p)
			p++;		/* step over the separator before nul */
		*dst = '\0';
		if (argc + 1 >= cap) {
			int nc = cap ? cap * 2 : 16;
			char **na = realloc(argv, sizeof(*na) * nc);

			if (!na) {
				free(argv);
				return NULL;
			}
			argv = na;
			cap = nc;
		}
		argv[argc++] = arg;
	}
	if (argv)
		argv[argc] = NULL;
	*argc_out = argc;
	return argv;
}

/* Parse one object (*pp at '{'); if its file matches e->path, fill out and set
 * out->found. Advances *pp past the closing '}'. */
static void
cc_object(Editor *e, char **pp, CcIncludes *out)
{
	char *p = *pp + 1;			/* past '{' */
	char *dir = NULL, *file = NULL, *command = NULL;
	char **args = NULL;
	int nargs = 0;

	while (*p && *p != '}') {
		char *key;

		p = cc_skip_ws(p);
		if (*p == ',') { p++; continue; }
		if (*p != '"')
			break;
		key = cc_string(&p);
		p = cc_skip_ws(p);
		if (*p != ':')
			break;
		p++;
		p = cc_skip_ws(p);
		if (key && strcmp(key, "directory") == 0)
			dir = cc_string(&p);
		else if (key && strcmp(key, "file") == 0)
			file = cc_string(&p);
		else if (key && strcmp(key, "command") == 0)
			command = cc_string(&p);
		else if (key && strcmp(key, "arguments") == 0 && *p == '[') {
			int cap = 0;

			p++;
			while (*p && *p != ']') {
				p = cc_skip_ws(p);
				if (*p == ',') { p++; continue; }
				if (*p != '"') { cc_skip_value(&p); continue; }
				if (nargs + 1 >= cap) {
					int nc = cap ? cap * 2 : 16;
					char **na = realloc(args,
					    sizeof(*na) * nc);

					if (!na)
						break;
					args = na;
					cap = nc;
				}
				args[nargs++] = cc_string(&p);
			}
			if (*p == ']')
				p++;
		} else {
			cc_skip_value(&p);
		}
	}
	if (*p == '}')
		p++;
	*pp = p;

	if (file && file[0]) {			/* does this entry name our file? */
		char full[PATH_MAX];

		if (file[0] == '/')
			snprintf(full, sizeof(full), "%s", file);
		else if (dir)
			snprintf(full, sizeof(full), "%.2040s/%.2040s",
			    dir, file);
		else
			full[0] = '\0';
		if (full[0] && same_path(full, e->path)) {
			if (!args && command)
				args = cc_split(command, &nargs);
			cc_extract(out, dir ? dir : ".", args, nargs);
		}
	}
	free(args);
}

/* Resolve the current file's include search dirs from the compile database
 * named by the cc.file config key. Returns 0 on a match, -1 otherwise.
 *
 * The result (hit or miss) is cached for one file, so repeated gf on the same
 * buffer does not re-read and re-parse the whole database. The cache is keyed
 * by the buffer path and the database path, and invalidated when the database's
 * size or mtime changes, so an edited database is picked up. */
static int
cc_resolve(Editor *e, CcIncludes *out)
{
	static CcIncludes cache;
	static char cache_key[PATH_MAX];	/* buffer path */
	static char cache_db[PATH_MAX];		/* cc.file path */
	static time_t cache_mtime;
	static off_t cache_size;
	static int cache_valid;

	const char *cfg = cfg_proj_get("cc.file");
	struct stat st;
	char *json = NULL, *p;
	size_t len = 0, cap = 0;
	FILE *fp;
	int c;

	out->found = 0;
	out->ninc = out->nquote = 0;
	out->dir[0] = '\0';
	if (!cfg || !cfg[0] || !e->has_name)
		return -1;
	if (stat(cfg, &st) != 0)
		return -1;

	if (cache_valid && cache_mtime == st.st_mtime &&
	    cache_size == st.st_size && strcmp(cache_key, e->path) == 0 &&
	    strcmp(cache_db, cfg) == 0) {
		*out = cache;
		return out->found ? 0 : -1;
	}

	fp = fopen(cfg, "rb");
	if (!fp)
		return -1;
	while ((c = fgetc(fp)) != EOF) {
		if (len + 2 > cap) {
			size_t nc = cap ? cap * 2 : 8192;
			char *np = realloc(json, nc);

			if (!np) {
				free(json);
				fclose(fp);
				return -1;	/* allocation failure: do not cache */
			}
			json = np;
			cap = nc;
		}
		json[len++] = (char)c;
	}
	fclose(fp);

	if (json) {
		json[len] = '\0';
		p = cc_skip_ws(json);
		if (*p == '[') {
			p++;
			while (*p && *p != ']') {
				p = cc_skip_ws(p);
				if (*p == ',') { p++; continue; }
				if (*p != '{')
					break;
				cc_object(e, &p, out);
				if (out->found)
					break;
			}
		}
		free(json);
	}

	cache = *out;				/* remember this outcome */
	snprintf(cache_key, sizeof(cache_key), "%s", e->path);
	snprintf(cache_db, sizeof(cache_db), "%s", cfg);
	cache_mtime = st.st_mtime;
	cache_size = st.st_size;
	cache_valid = 1;
	return out->found ? 0 : -1;
}

/* Characters that make up a filename path word under the cursor. */
static int
cc_path_ch(int c)
{
	return isalnum((unsigned char)c) || c == '.' || c == '_' ||
	    c == '-' || c == '/' || c == '+';
}

/* The path-like word under the cursor, for a bare filename (not #include). */
static int
cursor_path(Editor *e, char *out, size_t outsz)
{
	size_t len = 0, a, b;
	const char *s = text_line(e->t, e->cy, &len);

	out[0] = '\0';
	if (!s || e->cx > len)
		return 0;
	a = e->cx;
	while (a > 0 && cc_path_ch((unsigned char)s[a - 1]))
		a--;
	b = e->cx;
	while (b < len && cc_path_ch((unsigned char)s[b]))
		b++;
	if (b <= a || b - a >= outsz)
		return 0;
	memcpy(out, s + a, b - a);
	out[b - a] = '\0';
	return b - a;
}

/* Extract the header named on an #include line: "name" or <name>. Sets *angle
 * for the <...> form. Returns the length, or 0 when the line is not #include. */
static size_t
include_target(const char *s, size_t len, char *out, size_t outsz, int *angle)
{
	size_t i = 0, start, n;
	char close;

	out[0] = '\0';
	*angle = 0;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (i >= len || s[i] != '#')
		return 0;
	i++;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (len - i < 7 || memcmp(s + i, "include", 7) != 0)
		return 0;
	i += 7;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (i < len && s[i] == '"')
		close = '"';
	else if (i < len && s[i] == '<') {
		close = '>';
		*angle = 1;
	} else {
		return 0;
	}
	i++;
	start = i;
	while (i < len && s[i] != close)
		i++;
	if (i >= len)
		return 0;
	n = i - start;
	if (n == 0 || n >= outsz)
		return 0;
	memcpy(out, s + start, n);
	out[n] = '\0';
	return n;
}

/* Open path if it is readable, placing the cursor at its top. 0 on success. */
static int
cc_try_open(Editor *e, const char *path)
{
	char norm[PATH_MAX];

	if (path_normalize(path, norm, sizeof(norm)) != 0)
		snprintf(norm, sizeof(norm), "%.4094s", path);	/* fall back */
	if (access(norm, R_OK) != 0)
		return -1;
	if (ed_goto_target(e, norm, 0, NULL) != 0)
		return -1;
	set_status(e, "opened %.100s", norm);
	return 0;
}

/* gf: open the header named on an #include line, or the filename under the
 * cursor, searching the compile database's include dirs. */
static void
ed_open_header(Editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);
	char name[PATH_MAX], cand[PATH_MAX], dir[PATH_MAX];
	int angle = 0, i;
	CcIncludes cc;

	if (!line)
		return;
	if (include_target(line, len, name, sizeof(name), &angle) == 0 &&
	    cursor_path(e, name, sizeof(name)) == 0) {
		set_status(e,
		    "no include or filename under cursor");
		return;
	}
	if (cc_try_open(e, name) == 0)		/* absolute or cwd-relative */
		return;

	cc_resolve(e, &cc);			/* -1 just leaves cc empty */

	if (!angle && e->has_name) {		/* "": the file's own dir first */
		path_dir(e->path, dir, sizeof(dir));
		snprintf(cand, sizeof(cand), "%.2040s/%.2040s", dir, name);
		if (cc_try_open(e, cand) == 0)
			return;
	}
	if (!angle)
		for (i = 0; i < cc.nquote; i++) {
			snprintf(cand, sizeof(cand), "%.2040s/%.2040s",
			    cc.quote[i], name);
			if (cc_try_open(e, cand) == 0)
				return;
		}
	for (i = 0; i < cc.ninc; i++) {
		snprintf(cand, sizeof(cand), "%.2040s/%.2040s", cc.inc[i], name);
		if (cc_try_open(e, cand) == 0)
			return;
	}
	if (cc.found) {				/* last resort: the compile dir */
		snprintf(cand, sizeof(cand), "%.2040s/%.2040s", cc.dir, name);
		if (cc_try_open(e, cand) == 0)
			return;
	}
	set_status(e, "file not found: %.80s", name);
}

/****************************************************************
 * Symbol picker (a picker client)
 ****************************************************************/

typedef struct sym {
	size_t	line;		/* 0-based line the definition is on (buffer) */
	char	kind;		/* 'f' func, 's' struct/union/enum, 'c' class,
				 * 'd' macro, 't' typedef alias, or a ctags kind */
	char	name[80];
	int	tag;		/* 1 when this entry comes from the tags file */
	int	file_idx;	/* tags: index into the picker's Tagdb; else -1 */
	char	pattern[160];	/* tags: search pattern, empty when line is set */
} Sym;

typedef struct sympick {
	Editor	*e;
	Sym	*ent;
	int	 n, cap;
	char	 line[256];	/* reused by label() for one row */
	int	 chosen;	/* selected row index on PICK_DONE, else -1 */
	Tagdb	 db;		/* parsed tags file, owns the tag entries' files */
	const char *want;	/* non-NULL: include only this exact name */
	const char *sub;	/* non-NULL: include only names containing this */
} Sympick;

static int
sym_ident(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static int
sym_ident_start(int c)
{
	return isalpha((unsigned char)c) || c == '_';
}

/* Control words that are followed by '(' but are not function definitions. */
static int
sym_is_kw(const char *s, size_t n)
{
	static const char *const kw[] = {
		"if", "for", "while", "switch", "return", "sizeof", "do",
		"else", "catch", "foreach", NULL,
	};
	int i;

	for (i = 0; kw[i]; i++)
		if (strlen(kw[i]) == n && memcmp(kw[i], s, n) == 0)
			return 1;
	return 0;
}

static char
sym_copy(char *name, size_t namesz, const char *s, size_t from, size_t to,
    char kind)
{
	size_t nl = to - from;

	if (nl >= namesz)
		nl = namesz - 1;
	memcpy(name, s + from, nl);
	name[nl] = '\0';
	return kind;
}

/* Classify one line as a definition and extract its name. Returns the kind
 * char (and fills name) or 0 for a line that defines nothing. The rules are
 * deliberately simple line patterns, not a parser: a #define, a struct / union
 * / enum / class tag with an opening brace, a "} Name;" typedef alias, or a
 * top-level function (an identifier at column 0 right before '(', not a
 * control keyword, on a line that does not end in ';'). */
static char
sym_classify(const char *s, size_t len, char *name, size_t namesz)
{
	size_t i, j, start, end, last = len;
	const char *paren;

	while (last > 0 && (s[last - 1] == ' ' || s[last - 1] == '\t' ||
	    s[last - 1] == '\n' || s[last - 1] == '\r'))
		last--;
	if (last == 0)
		return 0;

	i = 0;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;

	if (s[i] == '#') {			/* #define NAME */
		j = i + 1;
		while (j < len && (s[j] == ' ' || s[j] == '\t'))
			j++;
		if (len - j >= 6 && memcmp(s + j, "define", 6) == 0 &&
		    (j + 6 >= len || !sym_ident(s[j + 6]))) {
			j += 6;
			while (j < len && (s[j] == ' ' || s[j] == '\t'))
				j++;
			start = j;
			while (j < len && sym_ident(s[j]))
				j++;
			if (j > start && sym_ident_start(s[start]))
				return sym_copy(name, namesz, s, start, j, 'd');
		}
		return 0;
	}

	if (s[0] == '}') {			/* } Name;  (typedef alias) */
		j = 1;
		while (j < len && (s[j] == ' ' || s[j] == '\t'))
			j++;
		start = j;
		while (j < len && sym_ident(s[j]))
			j++;
		end = j;
		while (j < len && (s[j] == ' ' || s[j] == '\t'))
			j++;
		if (end > start && sym_ident_start(s[start]) && j < len &&
		    s[j] == ';')
			return sym_copy(name, namesz, s, start, end, 't');
		return 0;
	}

	/* struct / union / enum / class TAG, with a brace on the line */
	{
		static const struct { const char *w; char kind; } kws[] = {
			{ "struct", 's' }, { "union", 's' }, { "enum", 's' },
			{ "class", 'c' }, { NULL, 0 },
		};
		int k;

		j = i;
		if (len - j >= 7 && memcmp(s + j, "typedef", 7) == 0 &&
		    (j + 7 >= len || !sym_ident(s[j + 7]))) {
			j += 7;
			while (j < len && (s[j] == ' ' || s[j] == '\t'))
				j++;
		}
		for (k = 0; kws[k].w; k++) {
			size_t wl = strlen(kws[k].w);

			if (len - j >= wl && memcmp(s + j, kws[k].w, wl) == 0 &&
			    j + wl < len && !sym_ident(s[j + wl])) {
				size_t tpos = j + wl;

				while (tpos < len &&
				    (s[tpos] == ' ' || s[tpos] == '\t'))
					tpos++;
				start = tpos;
				while (tpos < len && sym_ident(s[tpos]))
					tpos++;
				if (tpos > start && sym_ident_start(s[start]) &&
				    memchr(s, '{', len))
					return sym_copy(name, namesz, s, start,
					    tpos, kws[k].kind);
				break;
			}
		}
	}

	/* top-level function: a name at column 0 just before '(' */
	if (s[0] == ' ' || s[0] == '\t')
		return 0;
	if (s[last - 1] == ';')
		return 0;		/* a declaration or prototype */
	paren = memchr(s, '(', len);
	if (!paren)
		return 0;
	end = (size_t)(paren - s);
	while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t'))
		end--;
	start = end;
	while (start > 0 && sym_ident(s[start - 1]))
		start--;
	if (end > start && sym_ident_start(s[start]) &&
	    !sym_is_kw(s + start, end - start))
		return sym_copy(name, namesz, s, start, end, 'f');
	return 0;
}

/* Whether name passes the picker's active filter. */
static int
sym_wanted(Sympick *sp, const char *name)
{
	if (sp->want && strcmp(name, sp->want) != 0)
		return 0;
	if (sp->sub && !strstr(name, sp->sub))
		return 0;
	return 1;
}

/* Append one entry; returns a pointer to it, or NULL on allocation failure. */
static Sym *
sym_add(Sympick *sp)
{
	if (sp->n == sp->cap) {
		int nc = sp->cap ? sp->cap * 2 : 64;
		Sym *ne = realloc(sp->ent, (size_t)nc * sizeof(*ne));

		if (!ne)
			return NULL;
		sp->ent = ne;
		sp->cap = nc;
	}
	memset(&sp->ent[sp->n], 0, sizeof(sp->ent[sp->n]));
	sp->ent[sp->n].file_idx = -1;
	return &sp->ent[sp->n++];
}

/* Scan the current buffer for definitions, then merge in the tags file (when
 * one is found), skipping tag entries that point back into the current buffer
 * since the live buffer scan already covers those. The want/sub filters, when
 * set, limit which names are listed. */
static void
symscan(Sympick *sp)
{
	size_t nlines = text_lines(sp->e->t), i;
	char tagspath[PATH_MAX];
	int ti;

	for (i = 0; i < nlines; i++) {
		size_t len = 0;
		const char *s = text_line(sp->e->t, i, &len);
		char name[80], kind;
		Sym *se;

		if (!s)
			continue;
		kind = sym_classify(s, len, name, sizeof(name));
		if (!kind || !sym_wanted(sp, name))
			continue;
		se = sym_add(sp);
		if (!se)
			return;
		se->line = i;
		se->kind = kind;
		snprintf(se->name, sizeof(se->name), "%s", name);
	}

	if (tags_locate(sp->e, tagspath, sizeof(tagspath)) != 0)
		return;
	if (tags_load(&sp->db, tagspath) != 0)
		return;
	for (ti = 0; ti < sp->db.n; ti++) {
		const Tagent *te = &sp->db.ent[ti];
		char target[PATH_MAX];
		Sym *se;

		if (!sym_wanted(sp, te->name))
			continue;
		tag_resolve(&sp->db, te->file_idx, target, sizeof(target));
		if (sp->e->has_name && same_path(target, sp->e->path))
			continue;		/* the buffer scan already has it */
		se = sym_add(sp);
		if (!se)
			return;
		se->tag = 1;
		se->kind = te->kind;
		se->line = te->line;
		se->file_idx = te->file_idx;
		snprintf(se->name, sizeof(se->name), "%s", te->name);
		snprintf(se->pattern, sizeof(se->pattern), "%s", te->pattern);
	}
}

static const char *
sym_kindword(char kind)
{
	switch (kind) {
	case 'f': return "func";
	case 's': case 'u': case 'g': case 't': return "type";
	case 'c': return "class";
	case 'd': return "macro";
	case 'v': return "var";
	case 'm': return "member";
	default:  return "";
	}
}

static const char *
sympick_title(void *ctx)
{
	(void)ctx;
	return "Symbols";
}

static int
sympick_count(void *ctx)
{
	return ((Sympick *)ctx)->n;
}

static const char *
sympick_label(void *ctx, int i)
{
	Sympick *sp = ctx;
	const Sym *se;
	char where[PATH_MAX];

	if (i < 0 || i >= sp->n)
		return "";
	se = &sp->ent[i];
	if (se->tag)
		snprintf(where, sizeof(where), "%s",
		    se->file_idx >= 0 && se->file_idx < sp->db.nfiles ?
		    sp->db.files[se->file_idx] : "?");
	else
		snprintf(where, sizeof(where), "L%zu", se->line + 1);
	snprintf(sp->line, sizeof(sp->line), "%-8s %-28.48s %-6s %.150s",
	    se->tag ? "[tags]" : "[buffer]", se->name,
	    sym_kindword(se->kind), where);
	return sp->line;
}

static int
sympick_choose(void *ctx, int i)
{
	Sympick *sp = ctx;

	if (i < 0 || i >= sp->n)
		return PICK_STAY;
	sp->chosen = i;
	return PICK_DONE;
}

/* Record the current location on the tag stack before a jump, so a later pop
 * returns here. Unnamed buffers are not recorded (a pop reopens by path). */
static void
tagstack_push(Editor *e)
{
	Tagloc *tl;

	if (!e->has_name)
		return;
	if (e->tag_sp == e->tag_cap) {
		int nc = e->tag_cap ? e->tag_cap * 2 : 16;
		Tagloc *ns = realloc(e->tagstack, (size_t)nc * sizeof(*ns));

		if (!ns)
			return;			/* out of memory: skip recording */
		e->tagstack = ns;
		e->tag_cap = nc;
	}
	tl = &e->tagstack[e->tag_sp++];
	snprintf(tl->path, sizeof(tl->path), "%s", e->path);
	tl->cy = e->cy;
	tl->cx = e->cx;
}

/* Pop the most recent tag-jump origin and return to it. */
static void
ed_tag_pop(Editor *e)
{
	Tagloc tl;

	if (e->tag_sp == 0) {
		set_status(e, "tag stack empty");
		return;
	}
	tl = e->tagstack[--e->tag_sp];
	if (buf_open(e, tl.path) < 0)
		return;				/* buf_open set the status */
	if (text_lines(e->t) == 0)
		return;
	e->cy = tl.cy < text_lines(e->t) ? tl.cy : text_lines(e->t) - 1;
	e->cx = tl.cx;
	e->sel_active = 0;
	clamp_col(e);
	set_status(e, "%.100s:%zu  (%d on the tag stack)",
	    tl.path, e->cy + 1, e->tag_sp);
}

/****************************************************************
 * Jump list: a history of the positions a "jump" command left, so Ctrl-O and
 * Ctrl-I can walk back and forth across it (and the `` / '' marks name the most
 * recent one). A jump command records where it started before it moves.
 ****************************************************************/
#define JUMPS_MAX 100

static void vi_mark_set(Editor *e, int slot, size_t y, size_t x);
static int mark_index(int ch);

/* Append a location to the jump list, dropping the oldest when full. */
static void
jump_append(Editor *e, const char *path, size_t cy, size_t cx)
{
	Tagloc *jl;

	if (e->jump_n == e->jump_cap) {
		int nc = e->jump_cap ? e->jump_cap * 2 : 16;
		Tagloc *ns;

		if (nc > JUMPS_MAX)
			nc = JUMPS_MAX;
		if (e->jump_n >= nc) {		/* full: drop the oldest entry */
			memmove(e->jumps, e->jumps + 1,
			    (size_t)(e->jump_n - 1) * sizeof(*e->jumps));
			e->jump_n--;
		} else {
			ns = realloc(e->jumps, (size_t)nc * sizeof(*ns));
			if (!ns)
				return;
			e->jumps = ns;
			e->jump_cap = nc;
		}
	}
	jl = &e->jumps[e->jump_n++];
	snprintf(jl->path, sizeof(jl->path), "%s", path);
	jl->cy = cy;
	jl->cx = cx;
}

/* Record the current position as the origin of a jump about to happen, and set
 * the `` / '' previous-position mark to it. Collapses a repeat of the same line
 * in the same file, and drops any forward history past the cursor. */
static void
jump_record(Editor *e)
{
	const char *path = e->has_name ? e->path : "";

	vi_mark_set(e, MARK_PREV, e->cy, e->cx);

	e->jump_n = e->jump_cur;	/* a new jump truncates the forward part */
	if (e->jump_n > 0) {
		Tagloc *top = &e->jumps[e->jump_n - 1];

		if (top->cy == e->cy && strcmp(top->path, path) == 0) {
			top->cx = e->cx;	/* same line: just refresh it */
			e->jump_cur = e->jump_n;
			return;
		}
	}
	jump_append(e, path, e->cy, e->cx);
	e->jump_cur = e->jump_n;
}

/* Move the cursor (opening another file if need be) to a jump-list entry. */
static void
jump_goto(Editor *e, const Tagloc *jl)
{
	if (jl->path[0] && !(e->has_name && strcmp(jl->path, e->path) == 0)) {
		if (buf_open(e, jl->path) < 0)
			return;			/* buf_open set the status */
	}
	if (text_lines(e->t) == 0)
		return;
	e->cy = jl->cy < text_lines(e->t) ? jl->cy : text_lines(e->t) - 1;
	e->cx = jl->cx;
	e->sel_active = 0;
	clamp_col(e);
}

/* Ctrl-O: go to an older position in the jump list. */
static void
jump_back(Editor *e)
{
	if (e->jump_n == 0 || e->jump_cur == 0) {
		set_status(e, "already at oldest jump");
		return;
	}
	if (e->jump_cur >= e->jump_n) {		/* leaving the live position */
		const char *path = e->has_name ? e->path : "";

		jump_append(e, path, e->cy, e->cx);	/* so Ctrl-I can return */
		e->jump_cur = e->jump_n - 2;
	} else {
		e->jump_cur--;
	}
	jump_goto(e, &e->jumps[e->jump_cur]);
}

/* Ctrl-I: go to a newer position in the jump list. */
static void
jump_forward(Editor *e)
{
	if (e->jump_cur + 1 >= e->jump_n) {
		set_status(e, "already at newest jump");
		return;
	}
	e->jump_cur++;
	jump_goto(e, &e->jumps[e->jump_cur]);
}

/* The mark character for a slot, as :marks prints it and ` / ' name it. */
static char
mark_char(int slot)
{
	if (slot >= 0 && slot < MARK_LETTERS)
		return (char)('a' + slot);
	switch (slot) {
	case MARK_PREV:		return '\'';
	case MARK_CHANGE:	return '.';
	case MARK_INSERT:	return '^';
	case MARK_VISLT:	return '<';
	case MARK_VISGT:	return '>';
	}
	return '?';
}

/* :marks -- a picker over the set marks; choosing one jumps to it. */
typedef struct {
	Editor	*e;
	int	 slot[MARK_SLOTS];	/* the set slots, in display order */
	int	 n;
	int	 chosen;		/* slot chosen, or -1 */
	char	 line[160];
} Markpick;

static const char *
markpick_title(void *ctx)
{
	(void)ctx;
	return "mark  line  col  text";
}

static int
markpick_count(void *ctx)
{
	return ((Markpick *)ctx)->n;
}

static const char *
markpick_label(void *ctx, int i)
{
	Markpick *mp = ctx;
	Editor *e = mp->e;
	int slot;
	size_t y, llen = 0;
	const char *s = "";

	if (i < 0 || i >= mp->n)
		return "";
	slot = mp->slot[i];
	y = e->vi_mark_y[slot];
	if (y < text_lines(e->t))
		s = text_line(e->t, y, &llen);
	snprintf(mp->line, sizeof(mp->line), " %c  %6zu %4zu  %.80s",
	    mark_char(slot), y + 1, e->vi_mark_x[slot], s ? s : "");
	return mp->line;
}

static int
markpick_choose(void *ctx, int i)
{
	Markpick *mp = ctx;

	if (i < 0 || i >= mp->n)
		return PICK_STAY;
	mp->chosen = mp->slot[i];
	return PICK_DONE;
}

static void
dlg_marks_pick(Editor *e)
{
	Picksrc s = {
		.title = markpick_title, .count = markpick_count,
		.label = markpick_label, .choose = markpick_choose,
	};
	Markpick mp;
	int slot;

	memset(&mp, 0, sizeof(mp));
	mp.e = e;
	mp.chosen = -1;
	for (slot = 0; slot < MARK_SLOTS; slot++)
		if (e->vi_marks_set & ((uint64_t)1 << slot))
			mp.slot[mp.n++] = slot;
	if (mp.n == 0) {
		set_status(e, "no marks set");
		return;
	}
	s.ctx = &mp;
	if (dlg_pick(e, &s) && mp.chosen >= 0) {
		size_t y = e->vi_mark_y[mp.chosen];

		jump_record(e);
		if (y >= text_lines(e->t))
			y = text_lines(e->t) ? text_lines(e->t) - 1 : 0;
		e->cy = y;
		e->cx = e->vi_mark_x[mp.chosen];
		e->sel_active = 0;
		clamp_col(e);
	}
}

/* :jumps -- a picker over the jump list; choosing an entry goes to it. */
typedef struct {
	Editor	*e;
	int	 chosen;
	char	 line[PATH_MAX];
} Jumppick;

static const char *
jumppick_title(void *ctx)
{
	(void)ctx;
	return "jump  line  col  file";
}

static int
jumppick_count(void *ctx)
{
	return ((Jumppick *)ctx)->e->jump_n;
}

static const char *
jumppick_label(void *ctx, int i)
{
	Jumppick *jp = ctx;
	const Tagloc *jl;
	const char *name;

	if (i < 0 || i >= jp->e->jump_n)
		return "";
	jl = &jp->e->jumps[i];
	name = jl->path[0] ? jl->path : "[No Name]";
	if (strrchr(name, '/'))
		name = strrchr(name, '/') + 1;
	snprintf(jp->line, sizeof(jp->line), " %4d  %6zu %4zu  %.80s",
	    i, jl->cy + 1, jl->cx, name);
	return jp->line;
}

static int
jumppick_choose(void *ctx, int i)
{
	Jumppick *jp = ctx;

	if (i < 0 || i >= jp->e->jump_n)
		return PICK_STAY;
	jp->chosen = i;
	return PICK_DONE;
}

static void
dlg_jumps_pick(Editor *e)
{
	Picksrc s = {
		.title = jumppick_title, .count = jumppick_count,
		.label = jumppick_label, .choose = jumppick_choose,
	};
	Jumppick jp;

	if (e->jump_n == 0) {
		set_status(e, "jump list empty");
		return;
	}
	memset(&jp, 0, sizeof(jp));
	jp.e = e;
	jp.chosen = -1;
	s.ctx = &jp;
	if (dlg_pick(e, &s) && jp.chosen >= 0)
		jump_goto(e, &e->jumps[jp.chosen]);
}

/* :delmarks -- clear named marks (a b c ...), or all a-z marks with a !. */
static void
ex_delmarks(Editor *e, const char *rest, int bang)
{
	if (bang) {
		int i;

		for (i = 0; i < MARK_LETTERS; i++)
			e->vi_marks_set &= ~((uint64_t)1 << i);
		set_status(e, "cleared marks a-z");
		return;
	}
	if (!*rest) {
		set_status(e, "E471: argument required");
		return;
	}
	for (; *rest; rest++) {
		int idx;

		if (*rest == ' ' || *rest == '\t')
			continue;
		idx = mark_index((unsigned char)*rest);
		if (idx >= 0)
			e->vi_marks_set &= ~((uint64_t)1 << idx);
	}
	set_status(e, "marks cleared");
}

/* Jump to the chosen entry: a buffer line, or a tags entry in another file. */
static void
sym_goto(Editor *e, Sympick *sp, int i)
{
	const Sym *se;

	if (i < 0 || i >= sp->n)
		return;
	se = &sp->ent[i];
	if (se->tag) {
		char target[PATH_MAX];

		tagstack_push(e);		/* record where we jumped from */
		tag_resolve(&sp->db, se->file_idx, target, sizeof(target));
		if (ed_goto_target(e, target, se->line, se->pattern) == 0)
			set_status(e, "%.40s  %.90s:%zu",
			    se->name, target, e->cy + 1);
		else
			set_status(e,
			    "could not open %.100s", target);
	} else if (se->line < text_lines(e->t)) {
		e->cy = se->line;
		e->cx = 0;
		e->sel_active = 0;
		set_status(e, "line %zu", se->line + 1);
	}
}

/* Build the merged symbol list (optionally filtered), list it, and jump to the
 * chosen entry. When auto_single is set and exactly one entry matches, jump to
 * it without showing the picker. The core of Ctrl-T, :tag, and Ctrl-]. */
static void
symbol_pick_filtered(Editor *e, const char *want, const char *sub,
    int auto_single)
{
	Picksrc s = {
		.title = sympick_title, .count = sympick_count,
		.label = sympick_label, .choose = sympick_choose,
	};
	Sympick sp;

	memset(&sp, 0, sizeof(sp));
	sp.e = e;
	sp.chosen = -1;
	sp.want = want;
	sp.sub = sub;
	symscan(&sp);
	if (sp.n == 0) {
		if (want)
			set_status(e,
			    "tag not found: %.60s", want);
		else if (sub)
			set_status(e,
			    "no symbols match: %.60s", sub);
		else
			set_status(e,
			    "no symbols found");
		goto done;
	}
	if (auto_single && sp.n == 1) {
		sym_goto(e, &sp, 0);
		goto done;
	}
	s.ctx = &sp;
	if (dlg_pick(e, &s))
		sym_goto(e, &sp, sp.chosen);
done:
	free(sp.ent);
	tagdb_free(&sp.db);
}

/* Scan the current buffer and the tags file for definitions, list them, and
 * jump to the one chosen (Ctrl-T). */
static void
dlg_symbol_pick(Editor *e)
{
	symbol_pick_filtered(e, NULL, NULL, 0);
}

/* The identifier under the cursor, written to out; returns its length. */
static size_t
cursor_word(Editor *e, char *out, size_t outsz)
{
	size_t len = 0, a, b;
	const char *s = text_line(e->t, e->cy, &len);

	out[0] = '\0';
	if (!s || e->cx > len)
		return 0;
	a = e->cx;
	while (a > 0 && sym_ident((unsigned char)s[a - 1]))
		a--;
	b = e->cx;
	while (b < len && sym_ident((unsigned char)s[b]))
		b++;
	if (b <= a)
		return 0;
	if (b - a >= outsz)
		b = a + outsz - 1;
	memcpy(out, s + a, b - a);
	out[b - a] = '\0';
	return b - a;
}

/* vi Ctrl-]: jump to the tag named by the identifier under the cursor. */
static void
ed_tag_under_cursor(Editor *e)
{
	char word[80];

	if (cursor_word(e, word, sizeof(word)) == 0) {
		set_status(e, "no identifier under cursor");
		return;
	}
	symbol_pick_filtered(e, word, NULL, 1);
}

static void
ed_new(Editor *e)
{
	Text *nt;

	if (!dlg_confirm_discard(e))
		return;
	nt = text_new();
	if (!nt) {
		set_status(e, "out of memory");
		return;
	}
	text_free(e->t);
	e->t = nt;
	e->has_name = 0;
	e->path[0] = '\0';
	e->syn = NULL;
	buffer_reset(e);
	buf_save(e, &e->bufs[e->cur]);
	set_status(e, "new buffer");
}

/****************************************************************
 * File browser (a picker client)
 ****************************************************************/

typedef struct fpent {
	char	*name;		/* entry name, with a trailing '/' on a dir */
	int	 isdir;
} Fpent;

typedef struct filepick {
	Fpent		*ent;
	int		 n, cap;
	const char	*verb;		/* "Open" or "Save As", for the title */
	char		 dir[PATH_MAX];	/* current directory, canonical */
	char		 title[PATH_MAX + 16];
	char		 chosen[PATH_MAX];	/* result on PICK_DONE */
} Filepick;

static void
filepick_clear(Filepick *fp)
{
	int i;

	for (i = 0; i < fp->n; i++)
		free(fp->ent[i].name);
	fp->n = 0;
}

static int
filepick_cmp(const void *a, const void *b)
{
	const Fpent *x = a, *y = b;

	if (x->isdir != y->isdir)
		return y->isdir - x->isdir;	/* directories first */
	return strcmp(x->name, y->name);
}

/* Read fp->dir into fp->ent (directories first, then files, each sorted). */
static void
filepick_load(Filepick *fp)
{
	DIR *dp;
	struct dirent *de;

	filepick_clear(fp);
	snprintf(fp->title, sizeof(fp->title), "%s  %s",
	    fp->verb ? fp->verb : "Open", fp->dir);
	dp = opendir(fp->dir);
	if (!dp)
		return;
	while ((de = readdir(dp)) != NULL) {
		char path[PATH_MAX];
		struct stat st;
		int isdir, len;
		char *nm;

		if (strcmp(de->d_name, ".") == 0)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", fp->dir,
		    de->d_name) >= (int)sizeof(path))
			continue;
		if (stat(path, &st) != 0)
			continue;
		isdir = S_ISDIR(st.st_mode) ? 1 : 0;
		if (fp->n == fp->cap) {
			int nc = fp->cap ? fp->cap * 2 : 32;
			Fpent *ne = realloc(fp->ent, (size_t)nc * sizeof(*ne));

			if (!ne)
				break;
			fp->ent = ne;
			fp->cap = nc;
		}
		len = (int)strlen(de->d_name);
		nm = malloc((size_t)len + 2);
		if (!nm)
			break;
		memcpy(nm, de->d_name, (size_t)len);
		if (isdir)
			nm[len++] = '/';
		nm[len] = '\0';
		fp->ent[fp->n].name = nm;
		fp->ent[fp->n].isdir = isdir;
		fp->n++;
	}
	closedir(dp);
	qsort(fp->ent, (size_t)fp->n, sizeof(*fp->ent), filepick_cmp);
}

/* Move into dir (an absolute or dir-relative path) and relist. */
static void
filepick_chdir(Filepick *fp, const char *path)
{
	char real[PATH_MAX];

	if (realpath(path, real)) {
		snprintf(fp->dir, sizeof(fp->dir), "%s", real);
		filepick_load(fp);
	}
}

static const char *
filepick_title(void *ctx)
{
	return ((Filepick *)ctx)->title;
}

static int
filepick_count(void *ctx)
{
	return ((Filepick *)ctx)->n;
}

static const char *
filepick_label(void *ctx, int i)
{
	Filepick *fp = ctx;

	return (i >= 0 && i < fp->n) ? fp->ent[i].name : "";
}

static int
filepick_choose(void *ctx, int i)
{
	Filepick *fp = ctx;
	char path[PATH_MAX];

	if (i < 0 || i >= fp->n)
		return PICK_STAY;
	if (snprintf(path, sizeof(path), "%s/%s", fp->dir,
	    fp->ent[i].name) >= (int)sizeof(path))
		return PICK_STAY;
	if (fp->ent[i].isdir) {
		filepick_chdir(fp, path);
		return PICK_STAY;
	}
	snprintf(fp->chosen, sizeof(fp->chosen), "%s", path);
	return PICK_DONE;
}

/* The entry line: a typed directory descends, anything else opens (or creates)
 * a file by that name. */
static int
filepick_submit(void *ctx, const char *text)
{
	Filepick *fp = ctx;
	char cand[PATH_MAX];
	struct stat st;

	if (!text[0])
		return PICK_STAY;
	if (text[0] == '/') {
		if (snprintf(cand, sizeof(cand), "%s", text)
		    >= (int)sizeof(cand))
			return PICK_STAY;
	} else if (snprintf(cand, sizeof(cand), "%s/%s", fp->dir, text)
	    >= (int)sizeof(cand)) {
		return PICK_STAY;
	}
	if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
		filepick_chdir(fp, cand);
		return PICK_STAY;
	}
	snprintf(fp->chosen, sizeof(fp->chosen), "%s", cand);
	return PICK_DONE;
}

/* Run the file browser. verb names the action in the title. When start names a
 * file, the browser opens in its directory; a non-empty init pre-fills the
 * entry line and (with focus_entry) starts focus there, for a save prompt.
 * Returns 1 with out filled, 0 on cancel. */
/* Resolve the directory the browser should open in, given a start hint.
 * A directory hint is used directly, a file-path hint resolves to its
 * directory, and anything else (NULL, empty, or no match) falls back to the
 * current directory, then "/". The result is written canonical to out. */
static void
filepick_start_dir(const char *start, char *out, size_t outsz)
{
	char dirpart[PATH_MAX];
	struct stat st;
	const char *d = NULL;

	if (start && start[0] && stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
		d = start;			/* start is itself a directory */
	} else if (start && start[0]) {
		const char *slash = strrchr(start, '/');

		if (slash) {			/* directory part of a file path */
			snprintf(dirpart, sizeof(dirpart), "%.*s",
			    slash == start ? 1 : (int)(slash - start), start);
			d = dirpart;
		}
	}
	if (!d)
		d = ".";
	if (realpath(d, out))
		return;
	if (!realpath(".", out))
		snprintf(out, outsz, "/");
}

static int
dlg_file(Editor *e, const char *verb, const char *start, const char *init,
    int focus_entry, char *out, size_t outsz)
{
	Picksrc s = {
		.title = filepick_title, .count = filepick_count,
		.label = filepick_label, .choose = filepick_choose,
		.entry_label = "File: ", .submit = filepick_submit,
	};
	Filepick fp;
	int ok;

	memset(&fp, 0, sizeof(fp));
	fp.verb = verb;
	filepick_start_dir(start, fp.dir, sizeof(fp.dir));
	filepick_load(&fp);
	s.ctx = &fp;
	s.entry_init = init;
	s.focus_entry = focus_entry;
	ok = dlg_pick(e, &s);
	if (ok)
		snprintf(out, outsz, "%s", fp.chosen);
	filepick_clear(&fp);
	free(fp.ent);
	return ok;
}

/* Browse for a file to open. Returns 1 with out filled, 0 on cancel. */
static int
dlg_open_file(Editor *e, char *out, size_t outsz)
{
	return dlg_file(e, "Open", NULL, NULL, 0, out, outsz);
}

/* Browse for a path to save to, pre-filled with the current file name and
 * opened in its directory. Returns 1 with out filled, 0 on cancel. */
static int
dlg_save_file(Editor *e, char *out, size_t outsz)
{
	const char *start = e->has_name ? e->path : NULL;
	const char *base = NULL;

	if (e->has_name) {
		const char *slash = strrchr(e->path, '/');

		base = slash ? slash + 1 : e->path;
	}
	return dlg_file(e, "Save As", start, base, 1, out, outsz);
}

static void
ed_open(Editor *e)
{
	char path[PATH_MAX];
	Text *nt;

	if (!dlg_confirm_discard(e))
		return;
	path[0] = '\0';
	if (!dlg_open_file(e, path, sizeof(path)))
		return;
	nt = text_new();
	if (!nt) {
		set_status(e, "out of memory");
		return;
	}
	if (text_load(nt, path) < 0 && errno != ENOENT) {
		set_status(e, "open failed: %s",
		    strerror(errno));
		text_free(nt);
		return;
	}
	{
		struct stat st;
		time_t mt = (stat(path, &st) == 0) ? st.st_mtime : 0;
		int action = swap_recover(e, path, nt, mt);

		if (action == SWAP_ABORT) {	/* keep the current buffer */
			text_free(nt);
			return;
		}
		swap_remove(e);			/* drop the replaced buffer's swap */
		text_free(e->t);
		e->t = nt;
		snprintf(e->path, sizeof(e->path), "%s", path);
		e->has_name = 1;
		e->syn = syn_for_ext(file_ext(e->path));
		buffer_reset(e);
		swap_adopt(e, mt, action);
		buf_save(e, &e->bufs[e->cur]);
		if (action != SWAP_RECOVERED)
			set_status(e, "opened %.100s",
			    path);
	}
}

static void
ed_save_as(Editor *e)
{
	char path[PATH_MAX];

	path[0] = '\0';
	if (!dlg_save_file(e, path, sizeof(path)))
		return;
	swap_remove(e);			/* the old name's swap no longer applies */
	e->swap_path[0] = '\0';		/* recompute for the new name on next edit */
	snprintf(e->path, sizeof(e->path), "%s", path);
	e->has_name = 1;
	e->syn = syn_for_ext(file_ext(e->path));
	e->hl_valid = 0;
	(void)save_editor(e);
}

/* Switch between the modeless and vi personalities (also on F2). */
static void
toggle_vi(Editor *e)
{
	e->vi_visual = 0;
	if (e->mode == MODE_MODELESS) {
		e->mode = MODE_NORMAL;
		e->sel_active = 0;
		vi_reset_pending(e);
		vi_clamp(e);
		set_status(e,
		    "-- NORMAL -- (F2 returns to modeless)");
	} else {
		e->mode = MODE_MODELESS;
		e->sel_active = 0;
		vi_reset_pending(e);
		set_status(e,
		    "modeless mode (F2 for vi keys)");
	}
}

/* The About box content, one centered line per row plus a button. */
typedef struct about_ctx {
	const char *const	*lines;
	int			nlines;
	const char		*btn;
} Aboutctx;

static void
dlg_about_draw(Editor *e, const Modal *m, void *ctx)
{
	Aboutctx *a = ctx;
	Screen *d = e->d;
	int i, brow;

	for (i = 0; i < a->nlines; i++) {
		int lw = (int)strlen(a->lines[i]);

		scr_text(d, m->y + 1 + i, m->x + (m->w - lw) / 2, a->lines[i],
		    m->fg, m->bg, m->base);
	}
	brow = m->y + m->h - 2;
	scr_text(d, brow, m->x + (m->w - (int)strlen(a->btn)) / 2, a->btn,
	    m->fg, m->bg, m->base ^ ATTR_REVERSE);
}

static int
dlg_about_key(Editor *e, const Modal *m,
    const Event *ev, void *ctx)
{
	(void)e;
	(void)m;
	(void)ctx;
	if (ev->key.type == TKBD_KEY)
		return 1;		/* any key dismisses */
	/* a fresh left click dismisses; ignore the release that trailed the
	 * click which opened this dialog */
	if (ev->key.type == TKBD_MOUSE && ev->key.key == TKBD_MOUSE_LEFT &&
	    !(ev->key.mod & TKBD_MOD_MOTION))
		return 1;
	return 0;
}

/* A centered modal About box, dismissed by any key or a click. */
static void
dlg_about(Editor *e)
{
	static const char *const lines[] = {
		"vedit",
		"a full-screen text editor",
		("version " VEDIT_VERSION),
	};
	Aboutctx a = {
		lines, (int)(sizeof(lines) / sizeof(lines[0])), "[ OK ]"
	};
	int inner, boxw, i;

	inner = (int)strlen(a.btn);
	for (i = 0; i < a.nlines; i++) {
		int lw = (int)strlen(lines[i]);

		if (lw > inner)
			inner = lw;
	}
	inner += 4;			/* two spaces of padding each side */
	boxw = inner + 2;		/* left and right border */
	dlg_run(e, boxw, a.nlines + 4, &a, dlg_about_draw, dlg_about_key);
}

/****************************************************************
 * External tool commands: a per-language compile / make / run
 * layer. The command strings come from the config ([command
 * "<lang>"]); the editor expands their $(...) variables and hands
 * the final shell line to the tool API (see vedit.h). Capture
 * commands feed an output pane with a gcc/clang quickfix list;
 * an interactive command runs on the real terminal. Compiled out
 * by defining VEDIT_NO_TOOLS.
 ****************************************************************/
#ifndef VEDIT_NO_TOOLS

/* The syntax language name that keys [command "<lang>"], or NULL. */
static const char *
tool_lang(Editor *e)
{
	if (e->syn && e->syn->name && e->syn->name[0])
		return e->syn->name;
	return NULL;
}

/* command.<lang>.<which> from the config, or NULL. */
static const char *
tool_template(Editor *e, const char *which)
{
	const char *lang = tool_lang(e);
	char key[128];

	if (!lang)
		return NULL;
	snprintf(key, sizeof(key), "command.%s.%s", lang, which);
	return cfg_proj_get(key);
}

/* Whether command.<lang>.<which>.interactive is set. */
static int
tool_interactive(Editor *e, const char *which)
{
	const char *lang = tool_lang(e);
	char key[160];

	if (!lang || !g_cfg)
		return 0;
	snprintf(key, sizeof(key), "command.%s.%s.interactive", lang, which);
	return cfg_bool(g_cfg, key, 0);
}

/* Append n bytes to a growable, NUL-terminated string. Returns -1 on OOM. */
static int
sb_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
	if (*len + n + 1 > *cap) {
		size_t c = *cap ? *cap : 64;
		char *p;

		while (c < *len + n + 1)
			c *= 2;
		p = realloc(*buf, c);
		if (!p)
			return -1;
		*buf = p;
		*cap = c;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
	return 0;
}

/* Split path into directory, basename, stem (no extension), and extension (no
 * dot). dir is "." when there is no slash; ext is "" when there is no dot. */
static void
tool_split_path(const char *path, char *dir, char *base, char *stem, char *ext)
{
	const char *slash = strrchr(path, '/');
	const char *dot;

	if (slash) {
		size_t n = (size_t)(slash - path);

		if (n >= PATH_MAX)
			n = PATH_MAX - 1;
		memcpy(dir, path, n);
		dir[n] = '\0';
		snprintf(base, PATH_MAX, "%s", slash + 1);
	} else {
		snprintf(dir, PATH_MAX, ".");
		snprintf(base, PATH_MAX, "%s", path);
	}
	dot = strrchr(base, '.');
	if (dot && dot != base) {
		size_t n = (size_t)(dot - base);

		if (n >= PATH_MAX)
			n = PATH_MAX - 1;
		memcpy(stem, base, n);
		stem[n] = '\0';
		snprintf(ext, PATH_MAX, "%s", dot + 1);
	} else {
		snprintf(stem, PATH_MAX, "%s", base);
		ext[0] = '\0';
	}
}

/* Expand $(file) $(filename) $(filenoext) $(fileext) $(dir) in tmpl against
 * path. An unknown $(name) is copied verbatim. Returns malloc'd, or NULL. */
static char *
tool_expand(const char *tmpl, const char *path)
{
	char dir[PATH_MAX], base[PATH_MAX], stem[PATH_MAX], ext[PATH_MAX];
	char full[PATH_MAX];
	char *out = NULL;
	size_t olen = 0, ocap = 0;
	const char *p = tmpl;

	tool_split_path(path, dir, base, stem, ext);
	if (snprintf(full, sizeof(full), "%s/%s", dir, base) >= (int)sizeof(full))
		snprintf(full, sizeof(full), "%s", path);	/* too long: use path */

	while (*p) {
		if (p[0] == '$' && p[1] == '(') {
			const char *end = strchr(p + 2, ')');

			if (end) {
				size_t n = (size_t)(end - (p + 2));
				const char *v = p + 2, *val = NULL;

				if (n == 4 && memcmp(v, "file", 4) == 0)
					val = full;
				else if (n == 8 && memcmp(v, "filename", 8) == 0)
					val = base;
				else if (n == 9 && memcmp(v, "filenoext", 9) == 0)
					val = stem;
				else if (n == 7 && memcmp(v, "fileext", 7) == 0)
					val = ext;
				else if (n == 3 && memcmp(v, "dir", 3) == 0)
					val = dir;
				if (val) {
					if (sb_append(&out, &olen, &ocap, val,
					    strlen(val)) < 0)
						goto oom;
					p = end + 1;
					continue;
				}
			}
		}
		if (sb_append(&out, &olen, &ocap, p, 1) < 0)
			goto oom;
		p++;
	}
	if (!out)
		out = calloc(1, 1);
	return out;
oom:
	free(out);
	return NULL;
}

/* ---- captured output and diagnostics ---- */

static void
tool_clear_output(Editor *e)
{
	int i;

	for (i = 0; i < e->tool_nlines; i++)
		free(e->tool_lines[i]);
	free(e->tool_lines);
	e->tool_lines = NULL;
	e->tool_nlines = e->tool_lines_cap = 0;
	free(e->tool_errs);
	e->tool_errs = NULL;
	e->tool_nerr = e->tool_errs_cap = 0;
	free(e->tool_raw);
	e->tool_raw = NULL;
	e->tool_rawlen = e->tool_rawcap = 0;
	e->tool_curerr = -1;
}

/* The emit sink for run_capture: accumulate the bytes. */
static void
tool_emit(void *sink, const char *buf, size_t n)
{
	Editor *e = sink;

	(void)sb_append(&e->tool_raw, &e->tool_rawlen, &e->tool_rawcap, buf, n);
}

static int
tool_add_line(Editor *e, const char *s, size_t n)
{
	char *copy = malloc(n + 1);

	if (!copy)
		return -1;
	memcpy(copy, s, n);
	copy[n] = '\0';
	if (e->tool_nlines == e->tool_lines_cap) {
		int c = e->tool_lines_cap ? e->tool_lines_cap * 2 : 32;
		char **p = realloc(e->tool_lines, (size_t)c * sizeof(*p));

		if (!p) {
			free(copy);
			return -1;
		}
		e->tool_lines = p;
		e->tool_lines_cap = c;
	}
	e->tool_lines[e->tool_nlines++] = copy;
	return 0;
}

static void
tool_add_err(Editor *e, const char *file, size_t line, size_t col, int outline,
    int sev)
{
	Toolerr *te;

	if (e->tool_nerr == e->tool_errs_cap) {
		int c = e->tool_errs_cap ? e->tool_errs_cap * 2 : 16;
		Toolerr *p = realloc(e->tool_errs, (size_t)c * sizeof(*p));

		if (!p)
			return;
		e->tool_errs = p;
		e->tool_errs_cap = c;
	}
	te = &e->tool_errs[e->tool_nerr++];
	snprintf(te->file, sizeof(te->file), "%s", file);
	te->line = line;
	te->col = col;
	te->outline = outline;
	te->sev = sev;
}

/* Case-insensitive test of whether s begins with the lowercase word pfx. */
static int
tool_ci_prefix(const char *s, const char *pfx)
{
	for (; *pfx; s++, pfx++)
		if (tolower((unsigned char)*s) != (unsigned char)*pfx)
			return 0;
	return 1;
}

/* Classify the severity word that follows a diagnostic's "file:line:col: ".
 * gcc, clang, and MSVC all name it; anything unrecognized counts as an error. */
static int
tool_sev(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	if (tool_ci_prefix(p, "warning"))
		return TSEV_WARN;
	if (tool_ci_prefix(p, "note") || tool_ci_prefix(p, "remark"))
		return TSEV_NOTE;
	return TSEV_ERROR;		/* error, fatal error, or unlabeled */
}

/* The display name for a severity. */
static const char *
tool_sev_name(int sev)
{
	return sev == TSEV_ERROR ? "error" :
	    sev == TSEV_WARN ? "warning" : "note";
}

/* The decimal value of capture group m within s. */
static size_t
tool_group_num(const char *s, const rx_match *m)
{
	char buf[24];
	size_t n = (size_t)(m->eo - m->so);

	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	memcpy(buf, s + m->so, n);
	buf[n] = '\0';
	return (size_t)strtoul(buf, NULL, 10);
}

/* How many user "error.pattern" regexes are honored, beyond the built-ins. */
#define TOOL_MAX_PAT 16

/* Record a diagnostic if line s matches one of the patterns. Patterns are
 * tried in order and the first match wins. Capture group 1 is the file, group 2
 * the line, and the optional group 3 the column. Severity is read from the text
 * after the match, as gcc, clang, and MSVC all name it there. */
static void
tool_parse_line(Editor *e, rx_t **pats, int npat, const char *s, int outline)
{
	rx_match m[4];
	char file[PATH_MAX];
	size_t n;
	int i;

	for (i = 0; i < npat; i++) {
		if (!pats[i] || rx_exec(pats[i], s, strlen(s), 0, m, 4) != 1)
			continue;
		if (m[1].so < 0)		/* no file capture: not a hit */
			return;
		n = (size_t)(m[1].eo - m[1].so);
		if (n >= sizeof(file))
			n = sizeof(file) - 1;
		memcpy(file, s + m[1].so, n);
		file[n] = '\0';
		tool_add_err(e, file,
		    m[2].so >= 0 ? tool_group_num(s, &m[2]) : 0,
		    m[3].so >= 0 ? tool_group_num(s, &m[3]) : 0,
		    outline, tool_sev(s + m[0].eo));
		return;
	}
}

/* Build the pattern list: any user "error.pattern" regexes first, so they can
 * override the built-in gcc/clang/MSVC shapes, then the two built-ins. Returns
 * the count and fills pats (which must hold TOOL_MAX_PAT + 2 entries). */
static int
tool_patterns(rx_t **pats)
{
	int npat = 0, i;

	for (i = 0; g_cfg && i < g_cfg->count && npat < TOOL_MAX_PAT; i++) {
		rx_t *re;

		if (strcmp(g_cfg->entries[i].key, "error.pattern") != 0)
			continue;
		re = rx_compile(g_cfg->entries[i].value, 0, NULL);
		if (re)
			pats[npat++] = re;	/* bad regexes are skipped */
	}
	/* The file group allows an optional leading drive letter, so a Windows
	 * path like C:\src\foo.c is not cut off at the drive colon. The
	 * alternation adds no capture group, so file/line/col stay 1/2/3. */
	pats[npat++] = rx_compile(
	    "^([A-Za-z]:[^:]+|[^:]+):([0-9]+):([0-9]+): ", 0, NULL);
	pats[npat++] = rx_compile("^([A-Za-z]:[^:]+|[^:]+):([0-9]+): ", 0, NULL);
	return npat;
}

/* Split the captured bytes into lines and build the diagnostic list. */
static void
tool_parse_output(Editor *e)
{
	rx_t *pats[TOOL_MAX_PAT + 2];
	int npat = tool_patterns(pats), i;
	size_t start = 0, k;

	for (k = 0; e->tool_raw && k < e->tool_rawlen; k++) {
		if (e->tool_raw[k] != '\n')
			continue;
		if (tool_add_line(e, e->tool_raw + start, k - start) == 0)
			tool_parse_line(e, pats, npat,
			    e->tool_lines[e->tool_nlines - 1],
			    e->tool_nlines - 1);
		start = k + 1;
	}
	if (e->tool_raw && start < e->tool_rawlen) {	/* final partial line */
		if (tool_add_line(e, e->tool_raw + start,
		    e->tool_rawlen - start) == 0)
			tool_parse_line(e, pats, npat,
			    e->tool_lines[e->tool_nlines - 1],
			    e->tool_nlines - 1);
	}
	for (i = 0; i < npat; i++)
		rx_free(pats[i]);
}

/* ---- jumping to a diagnostic ---- */

/* True when a and b resolve to the same file on disk. */
static int
tool_same_file(const char *a, const char *b)
{
	char ra[PATH_MAX], rb[PATH_MAX];

	if (!realpath(a, ra) || !realpath(b, rb))
		return 0;
	return strcmp(ra, rb) == 0;
}

/* Resolve a diagnostic's file against the directory the command ran in. */
static void
tool_resolve(Editor *e, const char *file, char *out, size_t outsz)
{
	if (file[0] == '/')
		snprintf(out, outsz, "%s", file);
	else if (snprintf(out, outsz, "%s/%s", e->tool_dir, file) >= (int)outsz)
		snprintf(out, outsz, "%s", file);	/* too long: use it as given */
}

/* Switch to (or open) diagnostic ei's file and position the cursor. */
static int
tool_goto_err(Editor *e, int ei)
{
	Toolerr *te;
	char target[PATH_MAX];

	if (ei < 0 || ei >= e->tool_nerr)
		return -1;
	te = &e->tool_errs[ei];
	tool_resolve(e, te->file, target, sizeof(target));
	if (!(e->has_name && tool_same_file(target, e->path))) {
		if (buf_open(e, target) < 0)
			return -1;		/* buf_open set the status */
	}
	if (text_lines(e->t) == 0)
		return -1;
	e->cy = te->line ? te->line - 1 : 0;
	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
	e->cx = te->col ? te->col - 1 : 0;
	e->sel_active = 0;
	clamp_col(e);
	e->tool_curerr = ei;
	set_status(e, "%s %d/%d: %.120s:%zu",
	    tool_sev_name(te->sev), ei + 1, e->tool_nerr, te->file, te->line);
	return 0;
}

/* Count the errors and warnings among the parsed diagnostics. */
static void
tool_counts(Editor *e, int *nerr, int *nwarn)
{
	int i;

	*nerr = *nwarn = 0;
	for (i = 0; i < e->tool_nerr; i++) {
		if (e->tool_errs[i].sev == TSEV_ERROR)
			(*nerr)++;
		else if (e->tool_errs[i].sev == TSEV_WARN)
			(*nwarn)++;
	}
}

/* The first error (not warning or note), or -1 when there is none. */
static int
tool_first_error(Editor *e)
{
	int i;

	for (i = 0; i < e->tool_nerr; i++)
		if (e->tool_errs[i].sev == TSEV_ERROR)
			return i;
	return -1;
}

/* The highest severity present. Stepping visits only that tier, so with any
 * error present F4 walks errors and skips warnings and notes; a build with only
 * warnings still steps through them. */
static int
tool_nav_floor(Editor *e)
{
	int i, floor = TSEV_NOTE;

	for (i = 0; i < e->tool_nerr; i++)
		if (e->tool_errs[i].sev > floor)
			floor = e->tool_errs[i].sev;
	return floor;
}

/* F4 / Shift+F4: step to the next or previous diagnostic of the top severity
 * present, wrapping around at the ends. */
static void
ed_err_step(Editor *e, int dir)
{
	int floor, start, idx, i, wrapped = 0;

	if (e->tool_nerr == 0) {
		set_status(e, "no diagnostics");
		return;
	}
	floor = tool_nav_floor(e);
	start = e->tool_curerr;
	idx = start;
	for (i = 0; i < e->tool_nerr; i++) {
		idx += dir;
		if (idx < 0) {
			idx = e->tool_nerr - 1;
			wrapped = 1;
		} else if (idx >= e->tool_nerr) {
			idx = 0;
			wrapped = 1;
		}
		if (e->tool_errs[idx].sev >= floor) {
			tool_goto_err(e, idx);
			if (wrapped && start >= 0) {
				size_t n = strlen(e->status);

				snprintf(e->status + n, sizeof(e->status) - n,
				    " (wrapped)");
			}
			return;
		}
	}
}

/* The diagnostic shown on output line `outline`, or -1. */
static int
tool_err_at(Editor *e, int outline)
{
	int i;

	for (i = 0; i < e->tool_nerr; i++)
		if (e->tool_errs[i].outline == outline)
			return i;
	return -1;
}

/* An indexed terminal color as a value (CIDX is a struct initializer only). */
static Color
tool_color(uint8_t idx)
{
	Color c;

	c.type = COLOR_INDEXED;
	c.index = idx;
	return c;
}

/* The output pane: Up/Down (and PgUp/PgDn, Home/End) move the cursor line,
 * n / N step between diagnostics, Enter jumps to the one on the cursor line,
 * Esc or q closes. Diagnostic lines are drawn in red. */
static void
dlg_tool_output(Editor *e)
{
	const Pal *p = ed_chrome(e);
	uint16_t barat = (p->reverse_bars ? ATTR_REVERSE : 0) | ATTR_BOLD;
	char hdr[160];
	int cur = 0, top = 0;

	if (e->tool_nlines == 0) {
		set_status(e, "no output to show");
		return;
	}
	if (e->tool_nerr > 0)
		cur = e->tool_errs[0].outline;
	{
		int ne, nw;

		tool_counts(e, &ne, &nw);
		snprintf(hdr, sizeof(hdr),
		    " Output: %s  (%d error%s, %d warning%s)", e->tool_title,
		    ne, ne == 1 ? "" : "s", nw, nw == 1 ? "" : "s");
	}

	for (;;) {
		Screen *d = e->d;
		int rows = e->rows > 0 ? e->rows : 24;
		int body = rows - 2;
		int row;
		Event ev;

		if (cur < 0)
			cur = 0;
		if (cur >= e->tool_nlines)
			cur = e->tool_nlines - 1;
		if (cur < top)
			top = cur;
		if (body > 0 && cur >= top + body)
			top = cur - body + 1;
		if (top < 0)
			top = 0;

		scr_clear(d);
		ui_field(d, 0, 0, e->cols, hdr, p->bar_fg, p->bar_bg, barat);
		for (row = 1; row < rows - 1; row++) {
			int idx = top + row - 1;
			const char *s = (idx < e->tool_nlines) ?
			    e->tool_lines[idx] : "";
			Color fg = p->content_fg;
			uint16_t at = 0;
			int ei = (idx < e->tool_nlines) ?
			    tool_err_at(e, idx) : -1;

			if (ei >= 0) {
				switch (e->tool_errs[ei].sev) {
				case TSEV_ERROR: fg = tool_color(9); break;
				case TSEV_WARN:  fg = tool_color(11); break;
				default:	 fg = tool_color(14); break;
				}
			}
			if (idx == cur)
				at = ATTR_REVERSE;
			ui_field(d, row, 0, e->cols, s, fg, p->content_bg, at);
		}
		ui_field(d, rows - 1, 0, e->cols,
		    " Up/Down n/N move   Enter jump   Esc close",
		    p->bar_fg, p->bar_bg, barat);
		scr_present(d);

		if (scr_wait(d, &ev) == EVENT_EOF)
			return;
		if (ev.type == EVENT_RESIZE || ev.type == EVENT_RESUME) {
			scr_size(d, &e->rows, &e->cols);
			continue;
		}
		if (ev.type != EVENT_KEY || ev.key.type != TKBD_KEY)
			continue;
		switch (ev.key.key) {
		case TKBD_KEY_UP:	cur -= 1; break;
		case TKBD_KEY_DOWN:	cur += 1; break;
		case TKBD_KEY_PGUP:	cur -= body; break;
		case TKBD_KEY_PGDN:	cur += body; break;
		case TKBD_KEY_HOME:	cur = 0; break;
		case TKBD_KEY_END:	cur = e->tool_nlines - 1; break;
		case TKBD_KEY_ENTER: {
			int ei = tool_err_at(e, cur);

			if (ei >= 0) {
				tool_goto_err(e, ei);
				return;
			}
			break;
		}
		case TKBD_KEY_ESC:
			return;
		default:
			if (ev.key.ch == 'q' || ev.key.ch == 'Q')
				return;
			if ((ev.key.ch == 'n' || ev.key.ch == 'N') &&
			    e->tool_nerr > 0) {
				int step = (ev.key.ch == 'N') ? -1 : 1;
				int ei = (e->tool_curerr < 0) ? 0 :
				    e->tool_curerr + step;

				if (ei < 0)
					ei = 0;
				if (ei >= e->tool_nerr)
					ei = e->tool_nerr - 1;
				e->tool_curerr = ei;
				cur = e->tool_errs[ei].outline;
			}
			break;
		}
	}
}

/* ---- running a command ---- */

/* The directory the command runs in: the file's directory, absolute. */
static void
tool_build_dir(Editor *e, char *out, size_t outsz)
{
	char dir[PATH_MAX], base[PATH_MAX], stem[PATH_MAX], ext[PATH_MAX];
	char real[PATH_MAX];

	tool_split_path(e->path, dir, base, stem, ext);
	if (realpath(dir, real))
		snprintf(out, outsz, "%s", real);
	else
		snprintf(out, outsz, "%s", dir);
}

/* Resolve and run the per-language command `which` (labelled `label`). Compile
 * and make capture to the pane; a command marked interactive runs on the tty. */
static void
ed_tool_run(Editor *e, const char *which, const char *label)
{
	const char *tmpl;
	char *cmd;
	char dir[PATH_MAX];
	int interactive, rc;

	if (!e->tools ||
	    (!e->tools->run_capture && !e->tools->run_foreground)) {
		set_status(e,
		    "building is not available");
		return;
	}
	if (!e->has_name) {			/* building needs a path */
		if (save_editor(e) != 0)
			return;
	}
	tmpl = tool_template(e, which);
	if (!tmpl || !tmpl[0]) {
		set_status(e, "no %s command for %s",
		    which, tool_lang(e) ? tool_lang(e) : "this file type");
		return;
	}
	if (text_dirty(e->t)) {
		int k = dlg_prompt_key(e, "Save before building? (y)es (n)o ");

		if (k == 'y' || k == '\n') {
			if (save_editor(e) != 0)
				return;
		} else if (k != 'n') {
			set_status(e, "cancelled");
			return;
		}
	}
	cmd = tool_expand(tmpl, e->path);
	if (!cmd) {
		set_status(e, "out of memory");
		return;
	}
	tool_build_dir(e, dir, sizeof(dir));
	snprintf(e->tool_dir, sizeof(e->tool_dir), "%s", dir);
	interactive = tool_interactive(e, which);

	if (interactive && e->tools->run_foreground) {
		scr_end(e->d);			/* leave the alt screen */
		rc = e->tools->run_foreground(e->tools->ctx, cmd, dir);
		scr_begin(e->d);		/* re-enter; forces a full repaint */
		if (rc < 0)
			set_status(e,
			    "could not run: %.80s", cmd);
		else
			set_status(e,
			    "%s finished (exit %d)", label, rc);
	} else if (e->tools->run_capture) {
		int fe, ne, nw;

		tool_clear_output(e);
		snprintf(e->tool_title, sizeof(e->tool_title), "%s", label);
		rc = e->tools->run_capture(e->tools->ctx, cmd, dir, tool_emit,
		    e);
		tool_parse_output(e);
		e->tool_curerr = -1;
		fe = tool_first_error(e);
		if (fe >= 0)			/* land on the first error */
			tool_goto_err(e, fe);
		dlg_tool_output(e);
		tool_counts(e, &ne, &nw);
		if (rc < 0)
			set_status(e,
			    "could not run: %.80s", cmd);
		else
			set_status(e,
			    "%s exited %d, %d error%s, %d warning%s", label, rc,
			    ne, ne == 1 ? "" : "s", nw, nw == 1 ? "" : "s");
	} else {
		set_status(e,
		    "this command is interactive but no tty runner is set");
	}
	free(cmd);
}

/* Run an arbitrary shell command (vi :!cmd), capturing its output into the
 * tool pane. Goes through the host's tool runner, so an embedding host that
 * installs no runner (or none with capture) simply has no shell access. */
static void
ed_shell_cmd(Editor *e, const char *cmd)
{
	char dir[PATH_MAX];
	int rc;

	if (!cmd || !cmd[0]) {
		set_status(e, "usage: :!command");
		return;
	}
	if (!e->tools || !e->tools->run_capture) {
		set_status(e, "shell commands are not available");
		return;
	}
	tool_build_dir(e, dir, sizeof(dir));
	snprintf(e->tool_dir, sizeof(e->tool_dir), "%s", dir);
	tool_clear_output(e);
	snprintf(e->tool_title, sizeof(e->tool_title), "! %.40s", cmd);
	rc = e->tools->run_capture(e->tools->ctx, cmd, dir, tool_emit, e);
	tool_parse_output(e);
	e->tool_curerr = -1;
	dlg_tool_output(e);
	if (rc < 0)
		set_status(e, "could not run: %.80s", cmd);
	else
		set_status(e, "! exited %d", rc);
}

/* Map a tool key to its command, or CMD_NONE. Active in both personalities. */
static Cmd
tool_key_to_cmd(const struct tkbd_seq *seq)
{
	int ctrl, alt, shift;

	if (seq->type != TKBD_KEY)
		return CMD_NONE;
	ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;
	alt = (seq->mod & TKBD_MOD_ALT) != 0;
	shift = (seq->mod & TKBD_MOD_SHIFT) != 0;

	if (seq->key == TKBD_KEY_F9 && alt && !ctrl)
		return CMD_COMPILE;
	if (seq->key == TKBD_KEY_F9 && ctrl && !alt)
		return CMD_RUN;
	if (seq->key == TKBD_KEY_F9 && !ctrl && !alt && !shift)
		return CMD_MAKE;
	if (seq->key == TKBD_KEY_F5 && alt)
		return CMD_VIEW_OUTPUT;
	if (seq->key == TKBD_KEY_F4 && !shift && !ctrl && !alt)
		return CMD_ERR_NEXT;
	if (seq->key == TKBD_KEY_F4 && shift)
		return CMD_ERR_PREV;
	return CMD_NONE;
}

/* Carry out a tool command chosen by key or menu. */
static void
run_tool_cmd(Editor *e, Cmd c)
{
	switch (c) {
	case CMD_COMPILE:	ed_tool_run(e, "compile", "Compile"); break;
	case CMD_MAKE:		ed_tool_run(e, "build", "Make"); break;
	case CMD_RUN:		ed_tool_run(e, "run", "Run"); break;
	case CMD_VIEW_OUTPUT:	dlg_tool_output(e); break;
	case CMD_ERR_NEXT:	ed_err_step(e, +1); break;
	case CMD_ERR_PREV:	ed_err_step(e, -1); break;
	default:		break;
	}
}

/* Release the captured output and diagnostics. */
static void
tool_free(Editor *e)
{
	tool_clear_output(e);
}

#endif /* VEDIT_NO_TOOLS */

/* Carry out a chosen menu action. Returns 1 when the editor should quit. */
static int
run_menu_act(Editor *e, Menuact act)
{
	switch (act) {
	case MA_NEW:
		ed_new(e);
		break;
	case MA_OPEN:
		ed_open(e);
		break;
	case MA_SAVE:
		(void)save_editor(e);
		break;
	case MA_SAVE_AS:
		ed_save_as(e);
		break;
	case MA_BUF_NEXT:
		buf_cycle(e, 1);
		break;
	case MA_BUF_PREV:
		buf_cycle(e, -1);
		break;
	case MA_BUF_LIST:
		dlg_buffer_pick(e);
		break;
	case MA_EXIT:
		if (dlg_confirm_save(e, "Save changes before exiting?"))
			return 1;
		break;
	case MA_UNDO:
		(void)ed_dispatch(e, CMD_UNDO, NULL);
		break;
	case MA_REDO:
		(void)ed_dispatch(e, CMD_REDO, NULL);
		break;
	case MA_CUT:
		(void)ed_dispatch(e, CMD_CUT, NULL);
		break;
	case MA_COPY:
		(void)ed_dispatch(e, CMD_COPY, NULL);
		break;
	case MA_PASTE:
		(void)ed_dispatch(e, CMD_PASTE, NULL);
		break;
	case MA_OSC_COPY:
		osc_copy_selection(e);
		break;
	case MA_OSC_COPY_FILE:
		osc_copy_file(e);
		break;
	case MA_FIND:
		find_prompt(e);
		break;
	case MA_FIND_NEXT:
		if (e->last_find[0])
			ed_find(e, e->last_find);
		else
			set_status(e,
			    "no previous search");
		break;
	case MA_REPLACE:
		replace_prompt(e);
		break;
	case MA_SYMBOL:
		dlg_symbol_pick(e);
		break;
	case MA_TAG_POP:
		ed_tag_pop(e);
		break;
	case MA_OPEN_HEADER:
		ed_open_header(e);
		break;
	case MA_GOTO:
		goto_prompt(e);
		break;
	case MA_SYNTAX:
		e->hl_on = !e->hl_on;
		e->hl_valid = 0;
		set_status(e,
		    "syntax highlight %s", e->hl_on ? "on" : "off");
		break;
	case MA_SCHEME: {
		static const char *const names[] = { "DOS", "black", "plain" };

		e->scheme = (e->scheme + 1) % SCHEME_COUNT;
		set_status(e, "%s colors",
		    names[e->scheme]);
		break;
	}
	case MA_LINENO:
		e->show_lineno = !e->show_lineno;
		set_status(e, "line numbers %s",
		    e->show_lineno ? "on" : "off");
		break;
	case MA_WRAP:
		e->wrap = !e->wrap;
		if (e->wrap)
			e->left = 0;
		set_status(e, "word wrap %s",
		    e->wrap ? "on" : "off");
		break;
	case MA_EOL: {
		/* cycle LF -> CRLF -> NUL */
		int next = (text_eol(e->t) + 1) % 3;

		text_set_eol(e->t, next);
		set_status(e, "line endings: %s",
		    eol_name(next));
		break;
	}
	case MA_SHOW_TABS:
		e->show_tabs = !e->show_tabs;
		set_status(e, "show tabs %s",
		    e->show_tabs ? "on" : "off");
		break;
	case MA_AUTO_INDENT:
		e->auto_indent = !e->auto_indent;
		set_status(e, "auto-indent %s",
		    e->auto_indent ? "on" : "off");
		break;
	case MA_EXPAND_TABS:
		e->expand_tabs = !e->expand_tabs;
		set_status(e, "indent with %s",
		    e->expand_tabs ? "spaces" : "tabs");
		break;
	case MA_TABS_TO_SPACES:
		ed_retab(e, 1);
		break;
	case MA_SPACES_TO_TABS:
		ed_retab(e, 0);
		break;
	case MA_HEX:
		e->hex_view = !e->hex_view;
		e->hex_top = 0;
		e->hex_ascii = 0;
		e->hex_pending = -1;
		e->hex_insert = 0;
		e->hex_sel = 0;
		set_status(e, "%s view",
		    e->hex_view ? "hex" : "text");
		break;
	case MA_DRAW:
		draw_toggle(e);
		break;
	case MA_VI_MODE:
		toggle_vi(e);
		break;
	case MA_RELOAD_CONFIG:
		ed_reload_config(e);
		break;
#ifndef VEDIT_NO_TOOLS
	case MA_COMPILE:
		run_tool_cmd(e, CMD_COMPILE);
		break;
	case MA_MAKE:
		run_tool_cmd(e, CMD_MAKE);
		break;
	case MA_RUN:
		run_tool_cmd(e, CMD_RUN);
		break;
	case MA_VIEW_OUTPUT:
		run_tool_cmd(e, CMD_VIEW_OUTPUT);
		break;
	case MA_ERR_NEXT:
		run_tool_cmd(e, CMD_ERR_NEXT);
		break;
	case MA_ERR_PREV:
		run_tool_cmd(e, CMD_ERR_PREV);
		break;
#endif
	case MA_HELP:
		dlg_help(e);
		break;
	case MA_TUTORIAL:
		dlg_tutorial(e);
		break;
	case MA_ABOUT:
		dlg_about(e);
		break;
	case MA_NONE:
	case MA_SEP:
		break;
	}
	return 0;
}


/* First selectable item in menu m (skips a leading separator). */
static int
menu_first(int m)
{
	int i;

	for (i = 0; i < MENUS[m].n; i++)
		if (MENUS[m].items[i].act != MA_SEP)
			return i;
	return 0;
}

/* Step the selection within menu m by dir, skipping separators and wrapping. */
static int
menu_step(int m, int sel, int dir)
{
	int n = MENUS[m].n;
	int i;

	for (i = 0; i < n; i++) {
		sel = (sel + dir + n) % n;
		if (MENUS[m].items[sel].act != MA_SEP)
			break;
	}
	return sel;
}

/* Run the menu bar with menu `start` open. Returns the chosen action, or
 * MA_NONE when the user backs out with Esc. */
static Menuact
menu_bar_run(Editor *e, int start, int open)
{
	int cur = start;
	int sel;
	int menu_open = open;	/* 1: a dropdown is shown; 0: the bar is armed
				 * (MS-EDIT style) and a letter opens a menu */

	if (cur < 0)
		cur = 0;
	if (cur >= MENU_COUNT)
		cur = MENU_COUNT - 1;
	sel = menu_first(cur);

	for (;;) {
		Event ev;
		uint32_t ch;

		render_body(e, e->d);
		ui_menubar(e, ed_chrome(e), cur);
		if (menu_open)
			ui_dropdown(e, cur, sel);
		scr_cursor_vis(e->d, 0);
		scr_present(e->d);

		switch (scr_wait(e->d, &ev)) {
		case EVENT_EOF:
			return MA_EXIT;
		case EVENT_RESIZE:
		case EVENT_RESUME:
			scr_size(e->d, &e->rows, &e->cols);
			continue;
		case EVENT_KEY:
			break;
		default:
			continue;
		}
		if (ev.key.type == TKBD_MOUSE) {
			int boxw, x0, y0, item;

			if (ev.key.key != TKBD_MOUSE_LEFT)
				continue;	/* ignore drag/release/wheel */
			if (ev.key.y == 0) {	/* click on the bar */
				int m = menu_hit(e, ev.key.x);

				if (m < 0)
					return MA_NONE;	/* off a title: close */
				cur = m;
				sel = menu_first(cur);
				continue;
			}
			/* click inside the open drop-down selects an item */
			boxw = dropdown_width(e, cur) + 2;
			x0 = dropdown_x(e, cur, boxw);
			y0 = 1;			/* top border row */
			item = ev.key.y - (y0 + 1);
			if (ev.key.x > x0 && ev.key.x < x0 + boxw - 1 &&
			    item >= 0 && item < MENUS[cur].n &&
			    MENUS[cur].items[item].act != MA_SEP)
				return MENUS[cur].items[item].act;
			return MA_NONE;		/* click elsewhere closes */
		}
		if (ev.key.type != TKBD_KEY)
			continue;

		/* Alt+letter jumps straight to another menu and opens it. */
		ch = ev.key.ch;
		if ((ev.key.mod & TKBD_MOD_ALT) && ch != TKBD_CH_NONE &&
		    ch < 128) {
			int m = menu_title_by_mnemonic(tolower((int)ch));

			if (m >= 0) {
				cur = m;
				sel = menu_first(cur);
				menu_open = 1;
			}
			continue;
		}

		switch (ev.key.key) {
		case TKBD_KEY_ESC:
			if (menu_open) {
				menu_open = 0;	/* close the drop-down, keep bar */
				break;
			}
			return MA_NONE;		/* armed bar: Esc leaves the menu */
		case TKBD_KEY_LEFT:
			cur = (cur - 1 + MENU_COUNT) % MENU_COUNT;
			sel = menu_first(cur);
			break;
		case TKBD_KEY_RIGHT:
			cur = (cur + 1) % MENU_COUNT;
			sel = menu_first(cur);
			break;
		case TKBD_KEY_UP:
			if (menu_open)
				sel = menu_step(cur, sel, -1);
			else
				menu_open = 1;	/* open the armed menu */
			break;
		case TKBD_KEY_DOWN:
			if (menu_open)
				sel = menu_step(cur, sel, 1);
			else {
				menu_open = 1;	/* open the armed menu */
				sel = menu_first(cur);
			}
			break;
		case TKBD_KEY_ENTER:
			if (!menu_open) {
				menu_open = 1;	/* open the armed menu */
				sel = menu_first(cur);
			} else if (MENUS[cur].items[sel].act != MA_SEP) {
				return MENUS[cur].items[sel].act;
			}
			break;
		default:
			/* A plain letter: when the bar is only armed (F10), it
			 * opens the top-level menu with that mnemonic (MS-EDIT
			 * style). Once a menu is open, it selects an item. */
			if (ch == TKBD_CH_NONE || ch >= 128 ||
			    (ev.key.mod & (TKBD_MOD_CTRL | TKBD_MOD_ALT)))
				break;
			if (!menu_open) {
				int m = menu_title_by_mnemonic(tolower((int)ch));

				if (m >= 0) {
					cur = m;
					sel = menu_first(cur);
					menu_open = 1;
				}
			} else {
				int it = menu_item_by_mnemonic(cur,
				    tolower((int)ch));

				if (it >= 0)
					return MENUS[cur].items[it].act;
			}
			break;
		}
	}
}

/* Which menu a keypress opens, or -1 for none. F10 opens the bar; Alt+letter
 * opens the matching menu. */
static int
menu_trigger(const struct tkbd_seq *seq)
{
	uint32_t ch;

	if (seq->type != TKBD_KEY)
		return -1;
	if (seq->key == TKBD_KEY_F10)
		return 0;
	ch = seq->ch;
	if ((seq->mod & TKBD_MOD_ALT) && ch != TKBD_CH_NONE && ch < 128)
		return menu_title_by_mnemonic(tolower((int)ch));
	return -1;
}

/****************************************************************
 * Terminal / lifecycle
 ****************************************************************/

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [--utf8|--dec|-a|--ascii] [--16color|--256color]"
	    " [--scroll] [--config FILE|--no-config] [file]\n"
	    "\n"
	    "A single-file visual text editor for primitive terminals.\n"
	    "\n"
	    "  --utf8        draw the frame with Unicode box-drawing\n"
	    "  --dec         draw the frame with DEC VT100 line-drawing\n"
	    "  -a, --ascii   draw the frame with plain ASCII (+ - |)\n"
	    "                Default: UTF-8 under a UTF-8 locale, else ASCII.\n"
	    "                Also set by VEDIT_BOX=utf8|dec|ascii or VEDIT_ASCII.\n"
	    "  --16color     map colors to the 16 ANSI colors for old clients\n"
	    "  --256color    use the full 256-color palette\n"
	    "                Default: 256 when TERM/COLORTERM says so, else 16.\n"
	    "                Also set by VEDIT_COLORS=256|16.\n"
	    "  --scroll      use the VT100 scroll region when scrolling (faster\n"
	    "                on a slow link; needs a client that supports it)\n"
	    "  --no-scroll   repaint instead (the default; also VEDIT_SCROLL=0|1)\n"
	    "  --config FILE read settings from FILE (gitconfig style)\n"
	    "  --no-config   skip the config file\n"
	    "                Default: $VEDIT_CONFIG, else $XDG_CONFIG_HOME/vedit/\n"
	    "                config, else ~/.veditrc.\n"
	    "\n"
	    "Modeless (MS-EDIT) keys:\n"
	    "  arrows        move the cursor\n"
	    "  Home / End    start / end of line\n"
	    "  Ctrl+Home/End start / end of the file\n"
	    "  PgUp / PgDn   scroll by a screen\n"
	    "  Enter         split the line\n"
	    "  Backspace     delete left; Delete removes right\n"
	    "  Shift-arrows  extend a selection\n"
	    "  Ctrl-C / Ctrl-X  copy / cut (Ctrl-C with no selection copies the line)\n"
	    "  Ctrl-V        paste the internal clipboard\n"
	    "  Ctrl-F        incremental find (Enter repeats the last)\n"
	    "  Ctrl-R        replace, confirming each match (y/n/a/q)\n"
	    "  Ctrl-T        go to a symbol (buffer, plus a tags file if any)\n"
	    "  Ctrl-]        jump to the tag under the cursor\n"
	    "  Ctrl-L        go to a line number\n"
	    "  Ctrl-Z / Ctrl-Y  undo / redo\n"
	    "  Ctrl-S        save (prompts for a name if the buffer has none)\n"
	    "  Ctrl-Q        quit (prompts if the buffer was modified)\n"
	    "  F8 / Shift+F8 next / previous open buffer\n"
	    "  F10 or Alt+letter  open the menu bar\n"
	    "  F1            show the key bindings\n"
	    "  F2            toggle vi keys (modal editing)\n"
	    "\n"
	    "With vi keys on: NORMAL mode has h j k l, 0 ^ $, w b e, gg G,"
	    " f F t T,\n"
	    "  ; , { } ( ) %% H M L | counts (3j), operators d c y with"
	    " motions (dw,\n"
	    "  d$, dt), cc, dd, yy, x, p, u, and i a A I o O to insert; Esc"
	    " returns to\n"
	    "  NORMAL. ZZ writes and quits, ZQ quits without writing. ':'"
	    " runs w q wq\n"
	    "  q! qa wqa cq and :N; '/' searches and n repeats.\n"
	    "\n"
	    "Pasted text from the terminal is inserted literally (bracketed"
	    " paste).\n",
	    progname);
}

/* Carry out a request produced by a key handler, for the views that share the
 * editor's request framework (the text and hex views). Prompts and edits run
 * on the one backing buffer, so save, quit, find, and goto behave identically
 * whichever view raised them. Returns 1 when the editor should exit. */
static int
run_req(Editor *e, Req req)
{
	switch (req) {
	case REQ_FIND:
		find_prompt(e);
		break;
	case REQ_REPLACE:
		replace_prompt(e);
		break;
	case REQ_GOTO:
		goto_prompt(e);
		break;
	case REQ_HELP:
		dlg_help(e);
		break;
	case REQ_SAVE:
		(void)save_editor(e);
		break;
	case REQ_QUIT:
		if (dlg_confirm_save(e, "Save changes before exiting?"))
			return 1;
		break;
	default:
		break;
	}
	return 0;
}

/****************************************************************
 * Draw mode: a 2D / block editor for ASCII art and maps
 *
 * Toggled with the Insert key (or Options > Draw mode, or vi ':draw'), draw
 * mode layers over either personality. The cursor moves freely over a virtual
 * grid -- past the end of a line and below the last line -- typing overwrites
 * the cell under it (insert off), and writing into virtual space pads the line
 * with blanks and adds blank lines as needed. Backspace and Delete erase a cell
 * to a space instead of joining lines. Shift+arrows mark a rectangle that copy,
 * cut (erase to spaces), paste (overlay), and box (ASCII border) act on.
 ****************************************************************/

/* Append blank lines until row y exists. */
static void
draw_ensure_row(Editor *e, size_t y)
{
	while (text_lines(e->t) <= y) {
		size_t last = text_lines(e->t) - 1;

		text_split(e->t, last, text_line_len(e->t, last));
	}
}

/* Pad row y with spaces so it is at least col bytes long. */
static void
draw_pad_col(Editor *e, size_t y, size_t col)
{
	size_t len = text_line_len(e->t, y);
	char sp[128];
	size_t off = len, need;

	if (col <= len)
		return;
	need = col - len;
	memset(sp, ' ', sizeof(sp));
	while (need) {
		size_t chunk = need < sizeof(sp) ? need : sizeof(sp);

		text_insert(e->t, y, off, sp, chunk);
		off += chunk;
		need -= chunk;
	}
}

/* Overwrite the cell at the cursor with one glyph, extending virtual space as
 * needed, then advance the cursor past it. */
static void
draw_overtype(Editor *e, const char *bytes, size_t n)
{
	size_t len = 0;

	text_undo_group_begin(e->t);
	draw_ensure_row(e, e->cy);
	draw_pad_col(e, e->cy, e->cx);
	len = text_line_len(e->t, e->cy);
	if (e->cx < len) {
		const char *s = text_line(e->t, e->cy, &len);

		text_delete(e->t, e->cy, e->cx, rune_len_at(s, len, e->cx));
	}
	text_insert(e->t, e->cy, e->cx, bytes, n);
	e->cx += n;
	hl_touch(e, e->cy);
	text_undo_group_end(e->t);
}

/* Erase the cell under the cursor to a space, without moving. */
static void
draw_erase(Editor *e)
{
	size_t len;
	const char *s;

	if (e->cy >= text_lines(e->t))
		return;
	len = text_line_len(e->t, e->cy);
	if (e->cx >= len)
		return;			/* virtual column: nothing to erase */
	s = text_line(e->t, e->cy, &len);
	text_undo_group_begin(e->t);
	text_delete(e->t, e->cy, e->cx, rune_len_at(s, len, e->cx));
	text_insert(e->t, e->cy, e->cx, " ", 1);
	hl_touch(e, e->cy);
	text_undo_group_end(e->t);
}

/* Move the free cursor by (dy, dx) cells, never wrapping at an edge. */
static void
draw_move(Editor *e, int dy, int dx)
{
	if (dy < 0)
		e->cy = e->cy >= (size_t)(-dy) ? e->cy + dy : 0;
	else
		e->cy += dy;

	if (dx < 0 && e->cx > 0) {
		if (e->cy < text_lines(e->t) &&
		    e->cx <= text_line_len(e->t, e->cy)) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->cx -= prev_rune_len(s, e->cx);
		} else {
			e->cx--;
		}
	} else if (dx > 0) {
		if (e->cy < text_lines(e->t) &&
		    e->cx < text_line_len(e->t, e->cy)) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->cx += rune_len_at(s, len, e->cx);
		} else {
			e->cx++;
		}
	}
}

/* Rectangle bounds (inclusive) of the current block selection. */
static void
draw_block_bounds(Editor *e, size_t *y1, size_t *y2, size_t *x1,
    size_t *x2)
{
	*y1 = e->ay < e->cy ? e->ay : e->cy;
	*y2 = e->ay > e->cy ? e->ay : e->cy;
	*x1 = e->ax < e->cx ? e->ax : e->cx;
	*x2 = e->ax > e->cx ? e->ax : e->cx;
}

/* Copy the selected rectangle into the clipboard as blank-padded rows. */
static void
draw_block_copy(Editor *e)
{
	size_t y1, y2, x1, x2, w, y, cap, off = 0;
	char *buf;

	if (!e->sel_active || !e->sel_block)
		return;
	draw_block_bounds(e, &y1, &y2, &x1, &x2);
	w = x2 - x1 + 1;
	cap = (y2 - y1 + 1) * (w + 1) + 1;
	buf = malloc(cap);
	if (!buf)
		return;
	for (y = y1; y <= y2; y++) {
		size_t len = 0, c;
		const char *s = y < text_lines(e->t) ?
		    text_line(e->t, y, &len) : NULL;

		for (c = 0; c < w; c++) {
			size_t col = x1 + c;

			buf[off++] = (s && col < len) ? s[col] : ' ';
		}
		if (y < y2)
			buf[off++] = '\n';
	}
	free(e->clip);
	e->clip = buf;
	e->clip_len = off;
	e->clip_block = 1;
	e->clip_linewise = 0;
	set_status(e, "copied %zux%zu block",
	    w, y2 - y1 + 1);
}

/* Blank the selected rectangle in place (overlay model). */
static void
draw_block_erase(Editor *e, size_t y1, size_t y2, size_t x1, size_t x2)
{
	size_t y;

	text_undo_group_begin(e->t);
	for (y = y1; y <= y2 && y < text_lines(e->t); y++) {
		size_t len = text_line_len(e->t, y);
		size_t a = x1, b = x2 + 1;
		char sp[128];
		size_t off, need;

		if (a >= len)
			continue;
		if (b > len)
			b = len;
		if (b <= a)
			continue;
		text_delete(e->t, y, a, b - a);
		memset(sp, ' ', sizeof(sp));
		off = a;
		need = b - a;
		while (need) {
			size_t chunk = need < sizeof(sp) ? need : sizeof(sp);

			text_insert(e->t, y, off, sp, chunk);
			off += chunk;
			need -= chunk;
		}
		hl_touch(e, y);
	}
	text_undo_group_end(e->t);
}

/* Cut = copy then blank the rectangle. */
static void
draw_block_cut(Editor *e)
{
	size_t y1, y2, x1, x2;

	if (!e->sel_active || !e->sel_block)
		return;
	draw_block_bounds(e, &y1, &y2, &x1, &x2);
	draw_block_copy(e);
	draw_block_erase(e, y1, y2, x1, x2);
	e->sel_active = 0;
}

/* Overlay the clipboard rows at the cursor, extending virtual space. */
static void
draw_block_paste(Editor *e)
{
	size_t y, i;

	if (!e->clip || e->clip_len == 0)
		return;
	text_undo_group_begin(e->t);
	y = e->cy;
	i = 0;
	for (;;) {
		size_t j = i, rlen;

		while (j < e->clip_len && e->clip[j] != '\n')
			j++;
		rlen = j - i;
		draw_ensure_row(e, y);
		draw_pad_col(e, y, e->cx + rlen);
		if (rlen) {
			text_delete(e->t, y, e->cx, rlen);
			text_insert(e->t, y, e->cx, e->clip + i, rlen);
		}
		hl_touch(e, y);
		y++;
		if (j >= e->clip_len)
			break;
		i = j + 1;
	}
	text_undo_group_end(e->t);
	e->sel_active = 0;
}

/* Overwrite the single cell at (y, x) with one ASCII byte, extending virtual
 * space as needed. The caller owns the undo group. */
static void
draw_put_cell(Editor *e, size_t y, size_t x, char c)
{
	size_t len;

	draw_ensure_row(e, y);
	draw_pad_col(e, y, x);
	len = text_line_len(e->t, y);
	if (x < len) {
		const char *s = text_line(e->t, y, &len);

		text_delete(e->t, y, x, rune_len_at(s, len, x));
	}
	text_insert(e->t, y, x, &c, 1);
	hl_touch(e, y);
}

/* Draw an ASCII border ('+' corners, '-' top/bottom, '|' sides) around the
 * current block selection. A one-cell-wide or one-cell-tall rectangle reduces
 * to a straight line. The whole box is one undo step. */
static void
draw_block_box(Editor *e)
{
	size_t y1, y2, x1, x2, x, y;

	if (!e->sel_active || !e->sel_block)
		return;
	draw_block_bounds(e, &y1, &y2, &x1, &x2);
	text_undo_group_begin(e->t);
	if (y1 == y2 && x1 == x2) {		/* single cell */
		draw_put_cell(e, y1, x1, '+');
	} else if (y1 == y2) {			/* horizontal line */
		for (x = x1; x <= x2; x++)
			draw_put_cell(e, y1, x, (x == x1 || x == x2) ? '+' : '-');
	} else if (x1 == x2) {			/* vertical line */
		for (y = y1; y <= y2; y++)
			draw_put_cell(e, y, x1, (y == y1 || y == y2) ? '+' : '|');
	} else {
		for (x = x1; x <= x2; x++) {	/* top and bottom edges */
			char c = (x == x1 || x == x2) ? '+' : '-';

			draw_put_cell(e, y1, x, c);
			draw_put_cell(e, y2, x, c);
		}
		for (y = y1 + 1; y < y2; y++) {	/* left and right sides */
			draw_put_cell(e, y, x1, '|');
			draw_put_cell(e, y, x2, '|');
		}
	}
	text_undo_group_end(e->t);
	e->sel_active = 0;
	set_status(e, "boxed %zux%zu",
	    x2 - x1 + 1, y2 - y1 + 1);
}

/* Toggle draw mode. Entering from vi insert drops back to normal so leaving
 * draw mode lands somewhere sane. */
static void
draw_toggle(Editor *e)
{
	e->draw_mode = !e->draw_mode;
	e->sel_active = 0;
	if (e->draw_mode) {
		if (e->mode == MODE_INSERT)
			e->mode = MODE_NORMAL;
		set_status(e,
		    "-- DRAW -- Insert exits; type overwrites, arrows roam free");
	} else {
		set_status(e, "draw mode off");
	}
}

/* Handle one key in draw mode. Returns a request for the main loop. */
static Req
draw_key(Editor *e, const struct tkbd_seq *seq)
{
	int shift = seq->mod & TKBD_MOD_SHIFT;
	int ctrl = seq->mod & TKBD_MOD_CTRL;
	uint16_t k = seq->key;

	if (seq->type != TKBD_KEY)
		return REQ_CONTINUE;

	if (ctrl) {
		switch (k) {
		case TKBD_KEY_S:
			return REQ_SAVE;
		case TKBD_KEY_Q:
			return REQ_QUIT;
		case TKBD_KEY_F:
			return REQ_FIND;
		case TKBD_KEY_L:
			return REQ_GOTO;
		case TKBD_KEY_Z:
			e->sel_active = 0;
			if (text_undo(e->t, &e->cy, &e->cx) == 0)
				hl_touch(e, 0);
			return REQ_CONTINUE;
		case TKBD_KEY_Y:
			e->sel_active = 0;
			if (text_redo(e->t, &e->cy, &e->cx) == 0)
				hl_touch(e, 0);
			return REQ_CONTINUE;
		case TKBD_KEY_C:
			draw_block_copy(e);
			return REQ_CONTINUE;
		case TKBD_KEY_X:
			draw_block_cut(e);
			return REQ_CONTINUE;
		case TKBD_KEY_V:
			draw_block_paste(e);
			return REQ_CONTINUE;
		case TKBD_KEY_B:
			draw_block_box(e);
			return REQ_CONTINUE;
		default:
			return REQ_CONTINUE;
		}
	}

	/* Shift+arrow extends a rectangle; an unshifted move drops it. */
	if (k == TKBD_KEY_LEFT || k == TKBD_KEY_RIGHT || k == TKBD_KEY_UP ||
	    k == TKBD_KEY_DOWN || k == TKBD_KEY_HOME || k == TKBD_KEY_END) {
		if (shift) {
			if (!e->sel_active) {
				e->sel_active = 1;
				e->sel_block = 1;
				e->ay = e->cy;
				e->ax = e->cx;
			}
		} else {
			e->sel_active = 0;
		}
	}

	switch (k) {
	case TKBD_KEY_LEFT:
		draw_move(e, 0, -1);
		return REQ_CONTINUE;
	case TKBD_KEY_RIGHT:
		draw_move(e, 0, 1);
		return REQ_CONTINUE;
	case TKBD_KEY_UP:
		draw_move(e, -1, 0);
		return REQ_CONTINUE;
	case TKBD_KEY_DOWN:
		draw_move(e, 1, 0);
		return REQ_CONTINUE;
	case TKBD_KEY_HOME:
		e->cx = 0;
		return REQ_CONTINUE;
	case TKBD_KEY_END:
		e->cx = e->cy < text_lines(e->t) ?
		    text_line_len(e->t, e->cy) : e->cx;
		return REQ_CONTINUE;
	case TKBD_KEY_PGUP: {
		int h = text_height(e) - 1;

		if (h < 1)
			h = 1;
		e->sel_active = 0;
		e->cy = e->cy > (size_t)h ? e->cy - (size_t)h : 0;
		return REQ_CONTINUE;
	}
	case TKBD_KEY_PGDN: {
		int h = text_height(e) - 1;

		if (h < 1)
			h = 1;
		e->sel_active = 0;
		e->cy += (size_t)h;
		return REQ_CONTINUE;
	}
	case TKBD_KEY_ENTER:			/* carriage return */
		e->sel_active = 0;
		e->cy++;
		e->cx = 0;
		return REQ_CONTINUE;
	case TKBD_KEY_BACKSPACE:
	case TKBD_KEY_BACKSPACE2:
		e->sel_active = 0;
		draw_move(e, 0, -1);
		draw_erase(e);
		return REQ_CONTINUE;
	case TKBD_KEY_DEL:
		e->sel_active = 0;
		draw_erase(e);
		return REQ_CONTINUE;
	case TKBD_KEY_ESC:
		e->sel_active = 0;
		return REQ_CONTINUE;
	default:
		break;
	}

	if (seq->ch != TKBD_CH_NONE && seq->ch >= 0x20 && seq->ch != 0x7f) {
		unsigned char buf[8];
		int n = utf8_encode(buf, seq->ch);

		if (n > 0) {
			e->sel_active = 0;
			draw_overtype(e, (char *)buf, (size_t)n);
		}
	}
	return REQ_CONTINUE;
}

/****************************************************************
 * The editor loop, shared by the command-line binding and an
 * embedding host. Mouse handling and the build keys are gone; a
 * MUD client sends neither.
 ****************************************************************/

/* Set an editor's defaults on a zeroed struct. */
static void
editor_init(Editor *e)
{
	memset(e, 0, sizeof(*e));
	e->hl_on = 1;		/* highlight when a file type is recognized */
	e->show_tabs = 1;	/* show hard tabs by default */
	e->auto_indent = 1;	/* copy the previous line's indent by default */
	e->swap_enabled = 1;	/* write crash-recovery swap files by default */
	e->backup_enabled = 0;	/* keep no previous-version backup by default */
	e->hex_pending = -1;
	e->hex_cols = 16;
	e->scheme = SCHEME_DOS;	/* MS-EDIT look by default; View cycles it */
}

/* Run the event loop until the editor exits. Returns a process-style code:
 * 0 on a normal quit, 1 on end-of-input or vi ':cq'. */
static int
editor_loop(Editor *e)
{
	for (;;) {
		Event ev;
		struct tkbd_seq seq;

		switch (scr_wait(e->d, &ev)) {
		case EVENT_EOF:
			return 1;
		case EVENT_RESIZE:
		case EVENT_RESUME:
			scr_size(e->d, &e->rows, &e->cols);
			ed_render(e, e->d);
			continue;
		case EVENT_KEY:
			seq = ev.key;
			break;
		case EVENT_IDLE:
			swap_maybe_write(e);	/* snapshot a dirty buffer */
			continue;
		default:
			continue;
		}

		e->status[0] = '\0';	/* clear any transient message */

		/* F10 or Alt+letter opens the menu bar. It takes over input
		 * until an item is chosen or Esc backs out. */
		{
			int mi = menu_trigger(&seq);

			if (mi >= 0) {
				/* F10 arms the bar (a letter then opens a menu);
				 * Alt+letter opens that menu straight away. */
				int open = (seq.key != TKBD_KEY_F10);
				Menuact act = menu_bar_run(e, mi, open);

				if (run_menu_act(e, act))
					return 0;
				ed_render(e, e->d);
				continue;
			}
		}

		/* In the hex view, keys drive the hex navigator. It shares the
		 * request handling, so Ctrl-S and Ctrl-Q behave as usual. */
		if (e->hex_view) {
			if (run_req(e, hex_key(e, &seq)))
				return 0;
			ed_render(e, e->d);
			continue;
		}

		/* The Insert key toggles the 2D/block draw mode in either
		 * personality. */
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_INS &&
		    !(seq.mod & TKBD_MOD_CTRL)) {
			draw_toggle(e);
			ed_render(e, e->d);
			continue;
		}

		/* Draw mode takes over input while it is on. */
		if (e->draw_mode) {
			if (run_req(e, draw_key(e, &seq)))
				return 0;
			ed_render(e, e->d);
			continue;
		}

		/* F2 toggles between the modeless and vi personalities, ignored
		 * while inserting so it does not interrupt typing. */
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_F2 &&
		    !(seq.mod & TKBD_MOD_CTRL) && e->mode != MODE_INSERT) {
			if (e->mode == MODE_MODELESS) {
				e->mode = MODE_NORMAL;
				e->sel_active = 0;
				vi_reset_pending(e);
				vi_clamp(e);
				set_status(e,
				    "-- NORMAL -- (F2 returns to modeless)");
			} else {
				e->mode = MODE_MODELESS;
				vi_reset_pending(e);
				set_status(e,
				    "modeless mode (F2 for vi keys)");
			}
			ed_render(e, e->d);
			continue;
		}

		/* F8 cycles to the next open buffer, Shift+F8 to the previous
		 * one; a no-op while only one file is open. */
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_F8 &&
		    !(seq.mod & TKBD_MOD_CTRL)) {
			buf_cycle(e, (seq.mod & TKBD_MOD_SHIFT) ? -1 : 1);
			ed_render(e, e->d);
			continue;
		}

#ifndef VEDIT_NO_TOOLS
		/* The IDE keys (Compile, Make, Run, error stepping) work in both
		 * personalities and insert mode, like the function-key bindings. */
		{
			Cmd tc = tool_key_to_cmd(&seq);

			if (tc != CMD_NONE) {
				run_tool_cmd(e, tc);
				ed_render(e, e->d);
				continue;
			}
		}
#endif

		/* Ctrl-] jumps to the tag under the cursor, in either personality
		 * (it arrives as ']' with the Ctrl modifier). */
		if (seq.type == TKBD_KEY && seq.key == ']' &&
		    (seq.mod & TKBD_MOD_CTRL) && e->mode != MODE_INSERT) {
			ed_tag_under_cursor(e);
			ed_render(e, e->d);
			continue;
		}

		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_PASTE_BEGIN) {
			if (e->mode == MODE_NORMAL)
				paste_discard(e);
			else
				paste_input(e);
			ed_render(e, e->d);
			continue;
		}

		if (e->mode != MODE_MODELESS) {
			switch (vi_dispatch(e, &seq)) {
			case REQ_VI_COLON:
				switch (vi_colon(e)) {
				case REQ_FORCE_QUIT:
					return 0;
				case REQ_QUIT_ERR:
					return 1;	/* :cq exits nonzero */
				default:
					break;
				}
				break;
			case REQ_VI_SEARCH:
				vi_search(e);
				break;
			case REQ_HELP:
				dlg_help(e);
				break;
			case REQ_FORCE_QUIT:
				return 0;
			default:
				break;
			}
			ed_render(e, e->d);
			continue;
		}

		if (run_req(e, ed_dispatch(e, key_to_cmd(&seq), &seq)))
			return 0;
		ed_render(e, e->d);
	}
}

/* Release everything an editor owns, including the draw surface (which frees
 * the terminal backend through it). */
static void
editor_teardown(Editor *e)
{
	int i;

	if (e->d) {
		scr_end(e->d);
		scr_free(e->d);
		e->d = NULL;
		e->term = NULL;
	}
	swap_remove(e);			/* a clean exit leaves no swap behind */
	if (e->nbuf > 0) {
		buf_save(e, &e->bufs[e->cur]);
		for (i = 0; i < e->nbuf; i++) {
			if (i != e->cur && e->bufs[i].swap_on &&
			    e->bufs[i].swap_path[0])
				unlink(e->bufs[i].swap_path);
			buf_free_fields(e->bufs[i].t, e->bufs[i].line_state);
		}
	} else {
		buf_free_fields(e->t, e->line_state);
	}
	free(e->bufs);
	free(e->clip);
	for (i = 0; i < 26; i++)
		free(e->vi_regs[i].bytes);
	free(e->vi_dot.ev);
	free(e->vi_rec.ev);
	free(e->hl_buf);
	free(e->tagstack);
	free(e->jumps);
	if (g_cfg == e->cfg_owned)	/* don't leave the global dangling */
		g_cfg = NULL;
	vedit_cfg_free(e->cfg_owned);
#ifndef VEDIT_NO_TOOLS
	tool_free(e);
#endif
}

/****************************************************************
 * Embed API
 *
 * A host (a MUD) builds a vedit over its own struct vedit_io, then
 * drives it to completion with vedit_run(). Raw mode and SIGWINCH
 * live in the host's io callbacks, not here; a resize is delivered
 * by calling vedit_set_size().
 ****************************************************************/

struct vedit {
	Editor	e;
	int		registered;	/* buffer 0 registered yet */
};

/* Create an editor bound to the host's io vtable. Returns NULL on failure. */
struct vedit *
vedit_new(const struct vedit_io *io)
{
	struct vedit *v = calloc(1, sizeof(*v));
	Scrbuf *term;

	if (!v)
		return NULL;
	editor_init(&v->e);
	rune_width_init();
	v->e.t = text_new();
	if (!v->e.t) {
		free(v);
		return NULL;
	}
	term = scr_new_io(io);
	if (!term) {
		text_free(v->e.t);
		free(v);
		return NULL;
	}
	v->e.term = term;
	v->e.d = scr_new(term);
	if (!v->e.d) {
		free(term->out);
		free(term->cur);
		free(term->shadow);
		free(term->rowdirty);
		free(term);
		text_free(v->e.t);
		free(v);
		return NULL;
	}
	scr_size(v->e.d, &v->e.rows, &v->e.cols);
	return v;
}

/* Load a file into the editor before vedit_run(). A missing file opens as an
 * empty, named buffer. Returns 0, or -1 on a read error other than ENOENT. */
int
vedit_open(struct vedit *v, const char *path)
{
	struct stat st;

	snprintf(v->e.path, sizeof(v->e.path), "%s", path);
	v->e.has_name = 1;
	if (text_load(v->e.t, v->e.path) < 0 && errno != ENOENT)
		return -1;
	v->e.load_mtime = (stat(v->e.path, &st) == 0) ? st.st_mtime : 0;
	v->e.syn = syn_for_ext(file_ext(v->e.path));
	v->e.expand_tabs = indent_expand_default(v->e.syn ? v->e.syn->name : NULL);
	return 0;
}

/* Choose the box-drawing mode for this instance. Call before vedit_run(). */
void
vedit_set_box_mode(struct vedit *v, enum vedit_box_mode mode)
{
	if (v->e.term)
		v->e.term->box_mode = mode;
}

/* Tell the editor how many colors the client supports: 256 for the full
 * palette, anything less for the 16 ANSI colors (256-palette indices are then
 * mapped to the nearest ANSI color and sent as classic SGR codes). A host that
 * negotiates this (telnet MTTS/TTYPE) should call it before vedit_run(). */
void
vedit_set_colors(struct vedit *v, int colors)
{
	if (v->e.term)
		v->e.term->colors = (colors >= 256) ? 256 : 16;
}

/* Enable or disable the VT100 scroll-region fast path (see scroll_default).
 * A host that knows the client supports a scroll region (most do; the oldest
 * line-at-a-time clients do not) turns it on for a large saving when the user
 * scrolls or inserts lines. Off by default. Call before vedit_run(). */
void
vedit_set_scroll(struct vedit *v, int on)
{
	if (v->e.term)
		v->e.term->scroll = on ? 1 : 0;
}

/* Apply the editor-level defaults from g_cfg: color scheme, word wrap, line
 * numbers, startup personality, and syntax highlighting. Each unset key keeps
 * the value editor_init() already set. */
static void
ed_apply_config(Editor *e)
{
	const char *s;

	if (!g_cfg)
		return;
	s = cfg_get(g_cfg, "ui.scheme");
	if (s) {
		int ti;

		if (strcmp(s, "dos") == 0)
			e->scheme = SCHEME_DOS;
		else if (strcmp(s, "black") == 0)
			e->scheme = SCHEME_BLACK;
		else if (strcmp(s, "plain") == 0)
			e->scheme = SCHEME_PLAIN;
		else if ((ti = theme_by_name(s)) >= 0)
			e->scheme = SCHEME_COUNT + ti;
	}
	e->wrap = cfg_bool(g_cfg, "ui.wrap", e->wrap);
	e->show_lineno = cfg_bool(g_cfg, "ui.number", e->show_lineno);
	e->show_tabs = cfg_bool(g_cfg, "ui.tabs", e->show_tabs);
	e->auto_indent = cfg_bool(g_cfg, "edit.autoindent", e->auto_indent);
	e->swap_enabled = cfg_bool(g_cfg, "edit.swap", e->swap_enabled);
	e->backup_enabled = cfg_bool(g_cfg, "edit.backup", e->backup_enabled);
	e->search_icase = cfg_bool(g_cfg, "edit.ignorecase", e->search_icase);
	s = cfg_get(g_cfg, "edit.shiftwidth");
	if (s) {
		int v = atoi(s);

		if (v >= 0 && v <= 32)
			e->shiftwidth = v;
	}
	e->expand_tabs = indent_expand_default(e->syn ? e->syn->name : NULL);
	e->hl_on = cfg_bool(g_cfg, "syntax.enable", e->hl_on);
	e->clip_osc52 = cfg_bool(g_cfg, "ui.clipboard", e->clip_osc52);
	s = cfg_get(g_cfg, "edit.mode");
	if (s) {
		if (strcmp(s, "vi") == 0)
			e->mode = MODE_NORMAL;
		else if (strcmp(s, "modeless") == 0)
			e->mode = MODE_MODELESS;
	}
}

/* Reloading the grammars rebuilds the Syntax registries, so every buffer's cached
 * syn pointer is recomputed from its path, and its highlight cache invalidated. */
static void
ed_refresh_syntax(Editor *e)
{
	int i;

	e->syn = e->has_name ? syn_for_ext(file_ext(e->path)) : NULL;
	e->hl_valid = 0;
	for (i = 0; i < e->nbuf; i++) {
		if (i == e->cur)
			continue;	/* the active buffer lives in the flat fields */
		e->bufs[i].syn = e->bufs[i].has_name
		    ? syn_for_ext(file_ext(e->bufs[i].path)) : NULL;
		e->bufs[i].hl_valid = 0;
	}
}

/* Re-read the config file named at startup and apply it to the running editor:
 * the scheme and themes, box mode and colors, syntax grammars, and the editor
 * toggles, with a repaint on return. Env variables still rank above the file, as
 * at startup. A no-op with a message when no config file backs this session (an
 * embedding host, --no-config, or built-in defaults only). */
static void
ed_reload_config(Editor *e)
{
	Cfg *nc;

	if (e->cfg_path[0] == '\0') {
		set_status(e, "no config file to reload");
		return;
	}
	nc = vedit_cfg_new();
	if (!nc) {
		set_status(e, "out of memory");
		return;
	}
	if (vedit_cfg_load(nc, e->cfg_path) != 0) {
		vedit_cfg_free(nc);
		set_status(e, "cannot read config: %.80s",
		    e->cfg_path);
		return;
	}
	g_cfg = nc;
	themes_load_cfg(nc);		/* before ed_apply_config resolves scheme */
	syntax_load_defaults();		/* built-in C and shell grammars */
	syntax_load_cfg(&g_user, nc);	/* user grammars override them */
	ed_refresh_syntax(e);		/* re-point syn before ed_apply_config reads it */
	if (e->term) {
		e->term->box_mode = box_default();
		e->term->colors = color_default();
		e->term->scroll = scroll_default();
	}
	ed_apply_config(e);
	vedit_cfg_free(e->cfg_owned);	/* free the previous reload, if any */
	e->cfg_owned = nc;
	set_status(e, "config reloaded");
}

/* Hand the editor a parsed configuration (see vedit_cfg_load). It is borrowed,
 * not copied, so it must outlive the editor; pass NULL to clear it. Call before
 * vedit_run(). The startup knobs (box mode, colors, scroll) are re-resolved so
 * the config takes effect even after vedit_new(), still ranked below any
 * environment variable or explicit setter. */
void
vedit_set_config(struct vedit *v, const struct cfg *c)
{
	g_cfg = c;
	themes_load_cfg(c);		/* before ed_apply_config resolves scheme */
	syntax_load_defaults();		/* built-in C and shell grammars */
	syntax_load_cfg(&g_user, c);	/* user grammars override them */
	if (v->e.term) {
		v->e.term->box_mode = box_default();
		v->e.term->colors = color_default();
		v->e.term->scroll = scroll_default();
	}
	ed_apply_config(&v->e);
}

/* Record the path the config was read from, so the editor can re-read it at the
 * user's request (the ":reload" ex command and Options > Reload Config). Without
 * it, those report that there is no config file to reload. Pass NULL to clear. */
void
vedit_set_config_path(struct vedit *v, const char *path)
{
	snprintf(v->e.cfg_path, sizeof(v->e.cfg_path), "%s", path ? path : "");
}

#ifndef VEDIT_NO_TOOLS
/* Install the tool runner (public API). NULL disables building. */
void
vedit_set_tools(struct vedit *v, const struct vedit_tool_api *api)
{
	v->e.tools = api;
}
#endif

/* Deliver a new terminal size. The host calls this from wherever it learns the
 * size (telnet NAWS, a SIGWINCH it caught, a resize message). The change is
 * picked up by the run loop, which repaints. */
void
vedit_set_size(struct vedit *v, int rows, int cols)
{
	if (rows < 1 || cols < 1)
		return;
	if (rows == v->e.rows && cols == v->e.cols)
		return;
	scr_resize(v->e.d, rows, cols);
	v->e.rows = rows;
	v->e.cols = cols;
	v->e.term->want_resize = 1;
}

/* Run the editor loop to completion. Returns 0 on a normal quit, 1 on
 * end-of-input or ':cq'. */
int
vedit_run(struct vedit *v)
{
	Editor *e = &v->e;
	int rc;

	if (!v->registered) {
		if (buf_slot(e) < 0)
			return 1;
		buf_save(e, &e->bufs[0]);
		v->registered = 1;
	}
	scr_begin(e->d);
	set_status(e, "Press F1 for help");
	/* The screen is up now, so the initial file can prompt for recovery
	 * if a swap from a previous crashed session sits beside it. */
	if (e->has_name) {
		int action = swap_recover(e, e->path, e->t, e->load_mtime);

		if (action == SWAP_ABORT) {
			scr_end(e->d);
			return 1;
		}
		swap_adopt(e, e->load_mtime, action);
		buf_save(e, &e->bufs[e->cur]);
	}
	ed_render(e, e->d);
	rc = editor_loop(e);
	return rc;
}

/* Free an editor and everything it owns. */
void
vedit_free(struct vedit *v)
{
	if (!v)
		return;
	editor_teardown(&v->e);
	free(v);
}

/****************************************************************
 * Command-line binding: a vedit_io over a real tty, with raw mode
 * and SIGWINCH. This is the only part that touches termios and
 * signals; an embedded host supplies its own io instead.
 ****************************************************************/

typedef struct tty_io {
	int		in_fd, out_fd;
	struct termios	saved;
	int		raw;
} Ttyio;

static Ttyio g_tty;	/* the CLI runs a single editor */

static void
tty_on_winch(int sig)
{
	(void)sig;
	g_winch = 1;
}

/* Restore the terminal when the process is killed (SIGTERM/SIGHUP), so a
 * closed window or a kill does not leave the shell in raw mode and the alt
 * screen. Uses only async-signal-safe calls, then re-raises with the default
 * handler so the exit status reflects the signal. (Ctrl-C and friends do not
 * reach here: raw mode clears ISIG, so they arrive as ordinary keys.) */
static void
tty_on_fatal(int sig)
{
	static const char restore[] =
	    "\033[0m\033[?2004l\033[?25h\033[?1049l";
	ssize_t wr;

	if (g_tty.raw) {
		tcsetattr(g_tty.in_fd, TCSANOW, &g_tty.saved);
		g_tty.raw = 0;
	}
	wr = write(g_tty.out_fd, restore, sizeof(restore) - 1);
	(void)wr;
	signal(sig, SIG_DFL);
	raise(sig);
}

static long
tty_read(void *ctx, void *buf, long n)
{
	Ttyio *t = ctx;
	ssize_t r = read(t->in_fd, buf, (size_t)n);

	if (r < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		return -1;
	}
	return (long)r;		/* 0 means end of input */
}

static long
tty_write(void *ctx, const void *buf, long n)
{
	Ttyio *t = ctx;
	const char *p = buf;
	long off = 0;

	while (off < n) {
		ssize_t w = write(t->out_fd, p + off, (size_t)(n - off));

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += w;
	}
	return off;
}

static int
tty_poll(void *ctx, int timeout_ms)
{
	Ttyio *t = ctx;
	fd_set rfds;
	struct timeval tv, *ptv = NULL;
	int r;

	FD_ZERO(&rfds);
	FD_SET(t->in_fd, &rfds);
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		ptv = &tv;
	}
	r = select(t->in_fd + 1, &rfds, NULL, NULL, ptv);
	if (r < 0)
		return (errno == EINTR) ? 0 : -1;
	return r > 0 ? 1 : 0;
}

static void
tty_begin(void *ctx)
{
	Ttyio *t = ctx;
	struct termios raw;
	struct sigaction sa;

	if (tcgetattr(t->in_fd, &t->saved) == 0) {
		raw = t->saved;
		raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP |
		    IXON);
		raw.c_oflag &= ~(tcflag_t)(OPOST);
		raw.c_cflag |= (tcflag_t)CS8;
		raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		if (tcsetattr(t->in_fd, TCSAFLUSH, &raw) == 0)
			t->raw = 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = tty_on_winch;
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = tty_on_fatal;		/* restore on a kill */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
}

static void
tty_end(void *ctx)
{
	Ttyio *t = ctx;
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigaction(SIGWINCH, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	if (t->raw) {
		tcsetattr(t->in_fd, TCSAFLUSH, &t->saved);
		t->raw = 0;
	}
}

static int
tty_getsize(void *ctx, int *rows, int *cols)
{
	Ttyio *t = ctx;
	struct winsize ws;

	if (ioctl(t->out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
		return 0;
	}
	return -1;
}

#ifndef VEDIT_NO_TOOLS
/* The standalone binary's default tool runner: it spawns "sh -c <cmd>" in the
 * file's directory. An embedding host installs its own vedit_tool_api instead
 * (for example to run the command inside a sandbox), or passes NULL to disable
 * building entirely. */

/* Run cmd in dir, piping its combined stdout and stderr to emit(). Returns the
 * child's exit status, or -1 if it could not be started. */
static int
cli_run_capture(void *ctx, const char *cmd, const char *dir,
    void (*emit)(void *sink, const char *buf, size_t n), void *sink)
{
	int pipefd[2];
	pid_t pid;
	int status;

	(void)ctx;
	if (pipe(pipefd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {			/* child */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO)
			close(pipefd[1]);
		if (dir && dir[0] && chdir(dir) != 0)
			_exit(127);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	for (;;) {
		char buf[4096];
		ssize_t n = read(pipefd[0], buf, sizeof(buf));

		if (n > 0)
			emit(sink, buf, (size_t)n);
		else if (n == 0)
			break;
		else if (errno != EINTR)
			break;
	}
	close(pipefd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

/* Run cmd in dir connected to the real terminal, for an interactive program.
 * The caller has already left the alt screen. Returns the exit status, or -1. */
static int
cli_run_foreground(void *ctx, const char *cmd, const char *dir)
{
	pid_t pid;
	int status;

	(void)ctx;
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {			/* child: inherits the tty */
		if (dir && dir[0] && chdir(dir) != 0)
			_exit(127);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

static const struct vedit_tool_api cli_tools = {
	NULL, cli_run_capture, cli_run_foreground,
};
#endif /* VEDIT_NO_TOOLS */

/* Pick the config file path. An explicit --config (opt) or $VEDIT_CONFIG is
 * used as given; otherwise the first of $XDG_CONFIG_HOME/vedit/config and
 * ~/.veditrc that exists. Writes buf and returns 1, or returns 0 for none. */
static int
cli_config_path(const char *opt, char *buf, size_t bufsz)
{
	const char *env;

	if (opt) {
		snprintf(buf, bufsz, "%s", opt);
		return 1;
	}
	env = getenv("VEDIT_CONFIG");
	if (env && env[0]) {
		snprintf(buf, bufsz, "%s", env);
		return 1;
	}
	env = getenv("XDG_CONFIG_HOME");
	if (env && env[0]) {
		snprintf(buf, bufsz, "%s/vedit/config", env);
		if (access(buf, R_OK) == 0)
			return 1;
	}
	env = getenv("HOME");
	if (env && env[0]) {
		snprintf(buf, bufsz, "%s/.veditrc", env);
		if (access(buf, R_OK) == 0)
			return 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct vedit_io io;
	struct vedit *v;
	struct cfg *cfg = NULL;
	const char *file = NULL;
	const char *cfg_opt = NULL;
	char cfg_path[PATH_MAX];
	int rc, i, no_config = 0;

	if (argv[0])
		progname = argv[0];
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		}
		if (strcmp(argv[i], "-a") == 0 ||
		    strcmp(argv[i], "--ascii") == 0) {
			g_box_force = VEDIT_BOX_ASCII;	/* dumbest clients */
			continue;
		}
		if (strcmp(argv[i], "--dec") == 0) {
			g_box_force = VEDIT_BOX_DEC;	/* VT100 line-drawing */
			continue;
		}
		if (strcmp(argv[i], "--utf8") == 0) {
			g_box_force = VEDIT_BOX_UTF8;	/* Unicode box-drawing */
			continue;
		}
		if (strcmp(argv[i], "--16color") == 0) {
			g_colors_force = 16;		/* primitive clients */
			continue;
		}
		if (strcmp(argv[i], "--256color") == 0) {
			g_colors_force = 256;
			continue;
		}
		if (strcmp(argv[i], "--scroll") == 0) {
			g_scroll_force = 1;		/* VT100 scroll region */
			continue;
		}
		if (strcmp(argv[i], "--no-scroll") == 0) {
			g_scroll_force = 0;
			continue;
		}
		if (strcmp(argv[i], "--no-config") == 0) {
			no_config = 1;
			continue;
		}
		if (strcmp(argv[i], "--config") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s: --config needs a path\n",
				    progname);
				return 1;
			}
			cfg_opt = argv[++i];
			continue;
		}
		if (!file) {
			file = argv[i];
			continue;
		}
		fprintf(stderr, "%s: too many arguments\n", progname);
		return 1;
	}
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "%s: not a terminal\n", progname);
		return 1;
	}

	g_tty.in_fd = STDIN_FILENO;
	g_tty.out_fd = STDOUT_FILENO;
	g_tty.raw = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &g_tty;
	io.read = tty_read;
	io.write = tty_write;
	io.poll = tty_poll;
	io.begin = tty_begin;
	io.end = tty_end;
	io.getsize = tty_getsize;

	v = vedit_new(&io);
	if (!v) {
		fprintf(stderr, "%s: out of memory\n", progname);
		return 1;
	}
#ifndef VEDIT_NO_TOOLS
	vedit_set_tools(v, &cli_tools);		/* the default shell spawner */
#endif
	if (!no_config && cli_config_path(cfg_opt, cfg_path, sizeof(cfg_path))) {
		cfg = vedit_cfg_new();
		if (cfg && vedit_cfg_load(cfg, cfg_path) == 0) {
			vedit_set_config(v, cfg);
			vedit_set_config_path(v, cfg_path);	/* enable :reload */
		} else {
			if (cfg_opt)		/* an explicit path should exist */
				fprintf(stderr, "%s: %s: cannot read config\n",
				    progname, cfg_path);
			vedit_cfg_free(cfg);
			cfg = NULL;
		}
	}
	if (file && vedit_open(v, file) < 0) {
		fprintf(stderr, "%s: %s: %s\n", progname, file,
		    strerror(errno));
		vedit_free(v);
		vedit_cfg_free(cfg);
		return 1;
	}
	rc = vedit_run(v);
	vedit_free(v);
	vedit_cfg_free(cfg);
	return rc;
}

/****************************************************************
 * vi personality
 ****************************************************************/
/* The vi personality.
 *
 * A self-contained modal layer over the modeless editor's buffer operations
 * in edit.c. It keeps its state on Editor (mode, a pending count, a
 * pending operator) so one key press advances a small state machine: digits
 * build a count, d/c/y arm an operator, and a motion either moves the cursor
 * or, when an operator is armed, defines the span it acts on. It reaches the
 * shared buffer, cursor, and prompt helpers through editor.h. */




/****************************************************************
 * vi personality -- modal editing (toggle with F2)
 *
 * This is a self-contained block layered on top of the modeless editor's
 * buffer operations. It keeps its state on Editor (mode, a pending
 * count, and a pending operator) so a single key press advances a small
 * state machine: digits build a count, d/c/y arm an operator, and a motion
 * either moves the cursor or, when an operator is armed, defines the span it
 * acts on. When this grows (visual mode, named registers, marks) it should
 * move to its own vi.c with a shared editor-core header.
 ****************************************************************/

/* Byte offset of the first non-blank on a line, or 0 if the line is blank. */
static size_t
first_nonblank(Editor *e, size_t y)
{
	size_t len = 0, x = 0;
	const char *s = text_line(e->t, y, &len);

	if (!s)
		return 0;
	while (x < len && (s[x] == ' ' || s[x] == '\t'))
		x++;
	if (x >= len)
		x = 0;
	return x;
}

/* Keep the cursor on a valid line and rune boundary. In normal mode the
 * cursor rests on a character, so it may not sit past the last rune. */
void
vi_clamp(Editor *e)
{
	size_t len = 0;
	const char *s;

	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
	s = text_line(e->t, e->cy, &len);
	if (e->mode == MODE_NORMAL && len > 0) {
		if (e->cx >= len)
			e->cx = len - prev_rune_len(s, len);
	} else if (e->cx > len) {
		e->cx = len;
	}
	while (e->cx > 0 && e->cx < len &&
	    ((unsigned char)s[e->cx] & 0xc0) == 0x80)
		e->cx--;			/* snap off a continuation byte */
}

/* Clear any pending count, operator, and 'g' prefix. */
void
vi_reset_pending(Editor *e)
{
	e->vi_count = 0;
	e->vi_op = 0;
	e->vi_op_count = 0;
	e->vi_gpending = 0;
	e->vi_charsearch = 0;
	e->vi_textobj = 0;
	e->vi_markcmd = 0;
	e->vi_rpending = 0;
	e->vi_regpending = 0;
	e->vi_zpending = 0;
}

/* Character class for word motions: 0 blank, 1 word, 2 punctuation. With big
 * set (W/B/E motions) every non-blank rune is a word rune. */
static int
vi_class(uint32_t r, int big)
{
	if (r == ' ' || r == '\t')
		return 0;
	if (big)
		return 1;
	if (r == '_' || (r >= '0' && r <= '9') || (r >= 'A' && r <= 'Z') ||
	    (r >= 'a' && r <= 'z') || r >= 0x80)
		return 1;
	return 2;
}

/* Decode the rune at (y,x); returns its byte length and fills *r, or 0 when
 * the position is at or past the end of the line. */
static int
vi_rune(Editor *e, size_t y, size_t x, uint32_t *r)
{
	size_t len = 0;
	const char *s = text_line(e->t, y, &len);
	int n;

	if (!s || x >= len)
		return 0;
	n = utf8_decode(r, (const unsigned char *)s + x, len - x);
	return n > 0 ? n : 1;
}

/* Step one rune left, crossing to the end of the previous line. Returns 0
 * when already at the start of the buffer. */
static int
vi_step_back(Editor *e, size_t *y, size_t *x)
{
	if (*x > 0) {
		size_t len = 0;
		const char *s = text_line(e->t, *y, &len);

		*x -= prev_rune_len(s, *x);
		return 1;
	}
	if (*y == 0)
		return 0;
	(*y)--;
	*x = text_line_len(e->t, *y);
	return 1;
}

/* Advance (*py,*px) to the start of the next word. */
static void
vi_pos_word_fwd(Editor *e, size_t *py, size_t *px, int big)
{
	size_t y = *py, x = *px, len;
	uint32_t r;
	int n, cls;

	n = vi_rune(e, y, x, &r);
	if (n > 0) {
		cls = vi_class(r, big);
		if (cls != 0)
			while ((n = vi_rune(e, y, x, &r)) > 0 &&
			    vi_class(r, big) == cls)
				x += (size_t)n;
	}
	for (;;) {
		len = text_line_len(e->t, y);
		if (x >= len) {
			if (y + 1 >= text_lines(e->t)) {
				x = len;
				break;
			}
			y++;
			x = 0;
			if (text_line_len(e->t, y) == 0)
				break;		/* an empty line is a word */
			continue;
		}
		n = vi_rune(e, y, x, &r);
		if (n <= 0)
			break;
		if (vi_class(r, big) != 0)
			break;
		x += (size_t)n;
	}
	*py = y;
	*px = x;
}

/* Move (*py,*px) back to the start of the previous word. */
static void
vi_pos_word_back(Editor *e, size_t *py, size_t *px, int big)
{
	size_t y = *py, x = *px;
	uint32_t r;
	int cls;

	if (!vi_step_back(e, &y, &x)) {
		*py = 0;
		*px = 0;
		return;
	}
	for (;;) {
		size_t len = text_line_len(e->t, y);

		if (len == 0)
			break;			/* an empty line is a word */
		if (x >= len) {
			if (!vi_step_back(e, &y, &x))
				goto done;
			continue;
		}
		if (vi_rune(e, y, x, &r) > 0 && vi_class(r, big) != 0)
			break;
		if (!vi_step_back(e, &y, &x))
			goto done;
	}
	if (vi_rune(e, y, x, &r) > 0) {
		cls = vi_class(r, big);
		for (;;) {
			size_t ny = y, nx = x;

			if (!vi_step_back(e, &ny, &nx))
				break;
			if (vi_rune(e, ny, nx, &r) <= 0 ||
			    vi_class(r, big) != cls)
				break;
			y = ny;
			x = nx;
		}
	}
done:
	*py = y;
	*px = x;
}

/* Advance (*py,*px) to the last rune of the next word (inclusive target). */
static void
vi_pos_word_end(Editor *e, size_t *py, size_t *px, int big)
{
	size_t y = *py, x = *px;
	uint32_t r;
	int n, cls;

	n = vi_rune(e, y, x, &r);
	if (n > 0)
		x += (size_t)n;			/* leave the current rune */
	for (;;) {
		size_t len = text_line_len(e->t, y);

		if (x >= len) {
			if (y + 1 >= text_lines(e->t)) {
				x = len;
				goto done;
			}
			y++;
			x = 0;
			continue;
		}
		n = vi_rune(e, y, x, &r);
		if (n <= 0)
			goto done;
		if (vi_class(r, big) != 0)
			break;
		x += (size_t)n;
	}
	cls = vi_class(r, big);
	for (;;) {
		size_t nx = x + (size_t)n;
		uint32_t r2;
		int n2 = vi_rune(e, y, nx, &r2);

		if (n2 <= 0 || vi_class(r2, big) != cls)
			break;
		x = nx;
		n = n2;
	}
done:
	*py = y;
	*px = x;
}

/* Move one rune forward, crossing to the start of the next line. Returns 0
 * at the end of the buffer. */
static int
vi_step_fwd(Editor *e, size_t *y, size_t *x)
{
	size_t len = 0;
	const char *s = text_line(e->t, *y, &len);

	if (s && *x < len) {
		*x += rune_len_at(s, len, *x);
		return 1;
	}
	if (*y + 1 >= text_lines(e->t))
		return 0;
	(*y)++;
	*x = 0;
	return 1;
}

/* Paragraphs are separated by empty lines. '}' moves to the next empty line
 * below (or the end of the buffer); '{' to the previous one (or the top). */
static void
vi_para_fwd(Editor *e, size_t *py, size_t *px, int count)
{
	size_t y = *py, nlines = text_lines(e->t);
	int i;

	for (i = 0; i < count; i++) {
		size_t z = y + 1;

		while (z < nlines && text_line_len(e->t, z) != 0)
			z++;
		if (z >= nlines) {
			*py = nlines - 1;
			*px = text_line_len(e->t, nlines - 1);
			return;
		}
		y = z;
	}
	*py = y;
	*px = 0;
}

static void
vi_para_back(Editor *e, size_t *py, size_t *px, int count)
{
	size_t y = *py;
	int i;

	for (i = 0; i < count && y > 0; i++) {
		size_t z = y - 1;

		while (z > 0 && text_line_len(e->t, z) != 0)
			z--;
		y = z;
	}
	*py = y;
	*px = 0;
}

static int
vi_is_closer(uint32_t r)
{
	return r == ')' || r == ']' || r == '"' || r == '\'';
}

/* True when a<b in reading order. */
static int
vi_pos_lt(size_t ay, size_t ax, size_t by, size_t bx)
{
	return ay < by || (ay == by && ax < bx);
}

/* Advance (*py,*px) to the start of the next sentence. A sentence ends at
 * '.', '!' or '?' followed by optional closers and then whitespace or the end
 * of a line; an empty line is also a boundary. */
static void
vi_sentence_step_fwd(Editor *e, size_t *py, size_t *px)
{
	size_t y = *py, x = *px, nlines = text_lines(e->t);
	uint32_t r;

	if (!vi_step_fwd(e, &y, &x))
		goto done;
	for (;;) {
		int n;

		if (text_line_len(e->t, y) == 0) {	/* paragraph boundary */
			x = 0;
			goto done;
		}
		n = vi_rune(e, y, x, &r);
		if (n == 0) {				/* end of line */
			if (y + 1 >= nlines) {
				x = text_line_len(e->t, y);
				goto done;
			}
			y++;
			x = 0;
			continue;
		}
		if (r == '.' || r == '!' || r == '?') {
			size_t yy = y, xx = x + (size_t)n;
			uint32_t r2;
			int n2;

			while ((n2 = vi_rune(e, yy, xx, &r2)) > 0 &&
			    vi_is_closer(r2))
				xx += (size_t)n2;
			n2 = vi_rune(e, yy, xx, &r2);
			if (n2 == 0 || r2 == ' ' || r2 == '\t') {
				y = yy;
				x = xx;
				for (;;) {	/* skip to the next non-blank */
					int n3;
					uint32_t r3;

					if (text_line_len(e->t, y) == 0) {
						x = 0;
						goto done;
					}
					n3 = vi_rune(e, y, x, &r3);
					if (n3 == 0) {
						if (y + 1 >= nlines) {
							x = text_line_len(
							    e->t, y);
							goto done;
						}
						y++;
						x = 0;
						continue;
					}
					if (r3 == ' ' || r3 == '\t') {
						x += (size_t)n3;
						continue;
					}
					goto done;
				}
			}
		}
		if (!vi_step_fwd(e, &y, &x))
			goto done;
	}
done:
	*py = y;
	*px = x;
}

/* First content line of the paragraph containing y, at its first non-blank.
 * A blank line is its own paragraph start. */
static void
vi_para_content_start(Editor *e, size_t y, size_t *sy, size_t *sx)
{
	if (text_line_len(e->t, y) == 0) {
		*sy = y;
		*sx = 0;
		return;
	}
	while (y > 0 && text_line_len(e->t, y - 1) != 0)
		y--;
	*sy = y;
	*sx = first_nonblank(e, y);
}

/* Move (*py,*px) back to the start of the current or previous sentence. It
 * walks forward from a point at most a paragraph earlier and keeps the last
 * sentence start before the cursor, which handles crossing a blank line. */
static void
vi_sentence_step_back(Editor *e, size_t *py, size_t *px)
{
	size_t cy = *py, cx = *px, scan_y, scan_x, best_y, best_x, cur_y, cur_x;

	if (cy == 0 && cx == 0)
		return;

	vi_para_content_start(e, cy, &scan_y, &scan_x);
	if (!vi_pos_lt(scan_y, scan_x, cy, cx)) {
		/* cursor is at the paragraph's first sentence: back up into
		 * the previous paragraph so its sentences are in range */
		size_t z = scan_y;

		if (z == 0) {
			*py = 0;
			*px = 0;
			return;
		}
		z--;
		while (z > 0 && text_line_len(e->t, z) == 0)
			z--;
		vi_para_content_start(e, z, &scan_y, &scan_x);
	}

	best_y = scan_y;
	best_x = scan_x;
	cur_y = scan_y;
	cur_x = scan_x;
	for (;;) {
		size_t ny = cur_y, nx = cur_x;

		vi_sentence_step_fwd(e, &ny, &nx);
		if (!vi_pos_lt(cur_y, cur_x, ny, nx))
			break;			/* no forward progress */
		if (!vi_pos_lt(ny, nx, cy, cx))
			break;			/* reached the cursor */
		best_y = ny;
		best_x = nx;
		cur_y = ny;
		cur_x = nx;
	}
	*py = best_y;
	*px = best_x;
}

/* A resolved motion: a target position plus how an operator treats it. */
typedef struct vi_mot {
	size_t	y, x;
	int	line;		/* operate on whole lines */
	int	incl;		/* charwise: include the target rune */
	int	valid;
} Motion;

/* Classify a bracket rune: fill *match with its partner and *forward with the
 * direction to search for it. Returns 0 for a non-bracket. */
static int
vi_bracket_info(uint32_t r, uint32_t *match, int *forward)
{
	switch (r) {
	case '(': *match = ')'; *forward = 1; return 1;
	case '[': *match = ']'; *forward = 1; return 1;
	case '{': *match = '}'; *forward = 1; return 1;
	case ')': *match = '('; *forward = 0; return 1;
	case ']': *match = '['; *forward = 0; return 1;
	case '}': *match = '{'; *forward = 0; return 1;
	default:  return 0;
	}
}

/* The vi '%' motion: from the bracket at or forward of the cursor on the
 * current line, jump to its match, counting nesting across lines. The result
 * is inclusive so d% covers through the match. Invalid when the line holds no
 * bracket at or after the cursor, or the match is unbalanced. */
static Motion
vi_match_pair(Editor *e)
{
	Motion r = { e->cy, e->cx, 0, 0, 0 };
	size_t len = 0, bx = e->cx;
	const char *s = text_line(e->t, e->cy, &len);
	uint32_t open = 0, want = 0;
	int forward = 0, depth = 0;
	size_t y, x;

	if (!s)
		return r;
	while (bx < len) {			/* first bracket at/after cursor */
		uint32_t rr;
		int n = utf8_decode(&rr, (const unsigned char *)s + bx,
		    len - bx);

		if (n <= 0)
			n = 1;
		if (vi_bracket_info(rr, &want, &forward)) {
			open = rr;
			break;
		}
		bx += (size_t)n;
	}
	if (!open)
		return r;

	y = e->cy;
	x = bx;
	for (;;) {
		uint32_t rr;
		int n = vi_rune(e, y, x, &rr);

		if (n > 0) {
			if (rr == open)
				depth++;
			else if (rr == want && --depth == 0) {
				r.y = y;
				r.x = x;
				r.incl = 1;
				r.valid = 1;
				return r;
			}
		}
		if (forward) {
			if (!vi_step_fwd(e, &y, &x))
				break;
		} else if (!vi_step_back(e, &y, &x)) {
			break;
		}
	}
	return r;				/* no match found */
}

/* Byte offset of the rune that occupies display column target_col (0-based)
 * on line y, expanding tabs and honoring rune widths. Returns the line length
 * when the column is past the end. This is the inverse of disp_cols(). */
size_t
vi_col_to_byte(Editor *e, size_t y, int target_col)
{
	size_t len = 0;
	const char *s = text_line(e->t, y, &len);
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;

	if (!s || target_col < 0)
		return 0;
	while (i < len) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, len - i);
		int w;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if (r < 0x20 || r == 0x7f)
			w = 1;
		else {
			w = rune_width(r);
			if (w < 0)
				w = 1;
		}
		if (target_col < col + w)
			break;			/* the column falls in this rune */
		col += w;
		i += (size_t)n;
	}
	return i;
}

/* Resolve a motion character to a target. have_count says whether the caller
 * actually typed a count (matters for G/gg, whose count is a line number). */
static Motion
vi_motion(Editor *e, uint32_t m, int count, int have_count)
{
	Motion r = { e->cy, e->cx, 0, 0, 1 };
	size_t len = text_line_len(e->t, e->cy);
	const char *line = text_line(e->t, e->cy, NULL);
	int i;

	if (count < 1)
		count = 1;

	switch (m) {
	case 'h':
		for (i = 0; i < count && r.x > 0; i++)
			r.x -= prev_rune_len(line, r.x);
		break;
	case 'l':
	case ' ':
		for (i = 0; i < count && r.x < len; i++)
			r.x += rune_len_at(line, len, r.x);
		break;
	case '0':
		r.x = 0;
		break;
	case '^':
		r.x = first_nonblank(e, e->cy);
		break;
	case '$':
		r.x = len;
		break;
	case 'w':
	case 'W': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_pos_word_fwd(e, &y, &x, m == 'W');
		r.y = y;
		r.x = x;
		break;
	}
	case 'b':
	case 'B': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_pos_word_back(e, &y, &x, m == 'B');
		r.y = y;
		r.x = x;
		break;
	}
	case 'e':
	case 'E': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_pos_word_end(e, &y, &x, m == 'E');
		r.y = y;
		r.x = x;
		r.incl = 1;
		break;
	}
	case 'j':
		r.line = 1;
		r.y = e->cy + (size_t)count;
		if (r.y >= text_lines(e->t))
			r.y = text_lines(e->t) - 1;
		break;
	case 'k':
		r.line = 1;
		r.y = e->cy > (size_t)count ? e->cy - (size_t)count : 0;
		break;
	case 'G':
		r.line = 1;
		r.y = have_count ? (size_t)(count - 1) : text_lines(e->t) - 1;
		if (r.y >= text_lines(e->t))
			r.y = text_lines(e->t) - 1;
		break;
	case 'g':				/* the second g of gg */
		r.line = 1;
		r.y = have_count ? (size_t)(count - 1) : 0;
		if (r.y >= text_lines(e->t))
			r.y = text_lines(e->t) - 1;
		break;
	case '}': {
		size_t y = e->cy, x = e->cx;

		vi_para_fwd(e, &y, &x, count);
		r.y = y;
		r.x = x;
		break;
	}
	case '{': {
		size_t y = e->cy, x = e->cx;

		vi_para_back(e, &y, &x, count);
		r.y = y;
		r.x = x;
		break;
	}
	case ')': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_sentence_step_fwd(e, &y, &x);
		r.y = y;
		r.x = x;
		break;
	}
	case '(': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_sentence_step_back(e, &y, &x);
		r.y = y;
		r.x = x;
		break;
	}
	case '%':
		if (have_count) {		/* N% -- go to N percent of file */
			size_t nlines = text_lines(e->t);
			size_t ln = (size_t)((long)count * (long)nlines + 99)
			    / 100;

			if (ln < 1)
				ln = 1;
			if (ln > nlines)
				ln = nlines;
			r.line = 1;
			r.y = ln - 1;
			break;
		}
		return vi_match_pair(e);	/* bare % -- jump to match */
	case '|':				/* go to display column count */
		r.x = vi_col_to_byte(e, e->cy, count - 1);
		break;
	case 'H':				/* top line of the window */
	case 'M':				/* middle line */
	case 'L': {				/* bottom line */
		int text_h = text_height(e);
		size_t top = e->top, last;

		last = top + (size_t)text_h - 1;
		if (last >= text_lines(e->t))
			last = text_lines(e->t) - 1;
		r.line = 1;
		if (m == 'H') {
			r.y = top + (size_t)(count - 1);
			if (r.y > last)
				r.y = last;
		} else if (m == 'L') {
			r.y = last >= (size_t)(count - 1) ?
			    last - (size_t)(count - 1) : 0;
			if (r.y < top)
				r.y = top;
		} else {
			r.y = top + (last - top) / 2;
		}
		break;
	}
	default:
		r.valid = 0;
	}
	return r;
}

/* Byte offset of the count-th occurrence of target at or after `from` on the
 * line s[0,len). Sets *found. */
static size_t
find_char_fwd(const char *s, size_t len, size_t from, uint32_t target,
    int count, int *found)
{
	size_t x = from;
	int hits = 0;

	while (x < len) {
		uint32_t r;
		int n = utf8_decode(&r, (const unsigned char *)s + x, len - x);

		if (n <= 0)
			n = 1;
		if (r == target && ++hits == count) {
			*found = 1;
			return x;
		}
		x += (size_t)n;
	}
	*found = 0;
	return 0;
}

/* Byte offset of the count-th occurrence of target strictly before `from`,
 * counting leftward from just before `from`. Sets *found. */
static size_t
find_char_back(const char *s, size_t from, uint32_t target, int count,
    int *found)
{
	size_t x = 0;
	int tot = 0, want, idx = 0;

	while (x < from) {			/* count matches before `from` */
		uint32_t r;
		int n = utf8_decode(&r, (const unsigned char *)s + x, from - x);

		if (n <= 0)
			n = 1;
		if (r == target)
			tot++;
		x += (size_t)n;
	}
	if (count > tot) {
		*found = 0;
		return 0;
	}
	want = tot - count;			/* nearest-left is the last match */
	x = 0;
	while (x < from) {
		uint32_t r;
		int n = utf8_decode(&r, (const unsigned char *)s + x, from - x);

		if (n <= 0)
			n = 1;
		if (r == target && idx++ == want) {
			*found = 1;
			return x;
		}
		x += (size_t)n;
	}
	*found = 0;
	return 0;
}

/* Resolve an f/F/t/T search for target on the current line. repeat is set for
 * ; and , so a t/T advances past an adjacent match instead of sticking. */
static Motion
vi_charsearch_motion(Editor *e, char cmd, uint32_t target, int count,
    int repeat)
{
	Motion r = { e->cy, e->cx, 0, 0, 0 };
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	int forward = (cmd == 'f' || cmd == 't');
	int till = (cmd == 't' || cmd == 'T');
	int found = 0;
	size_t pos;

	if (!s || count < 1)
		return r;

	if (forward) {
		size_t from = e->cx + rune_len_at(s, len, e->cx);

		if (till && repeat && from < len)
			from += rune_len_at(s, len, from);
		pos = find_char_fwd(s, len, from, target, count, &found);
		if (!found)
			return r;
		r.x = till ? pos - prev_rune_len(s, pos) : pos;
		r.incl = 1;		/* f/t include the target for operators */
	} else {
		size_t from = e->cx;

		if (till && repeat && from > 0)
			from -= prev_rune_len(s, from);
		pos = find_char_back(s, from, target, count, &found);
		if (!found)
			return r;
		r.x = till ? pos + rune_len_at(s, len, pos) : pos;
		r.incl = 0;		/* backward: region excludes the cursor */
	}
	r.valid = 1;
	return r;
}

/* Remove whole lines [y1,y2], keeping the buffer's one-line invariant. */
static void
vi_delete_lines(Editor *e, size_t y1, size_t y2)
{
	size_t nlines = text_lines(e->t);
	size_t count, i;

	if (y2 >= nlines)
		y2 = nlines - 1;
	if (y1 > y2) {
		size_t tmp = y1;

		y1 = y2;
		y2 = tmp;
	}
	count = y2 - y1 + 1;
	hl_touch(e, y1);

	if (count >= nlines) {			/* the whole buffer */
		size_t last = 0;

		text_line(e->t, nlines - 1, &last);
		delete_region(e, 0, 0, nlines - 1, last);
		e->cy = 0;
		e->cx = 0;
		return;
	}
	for (i = 0; i < count; i++) {
		size_t ll = text_line_len(e->t, y1);

		text_delete(e->t, y1, 0, ll);
		if (y1 + 1 < text_lines(e->t))
			text_join(e->t, y1);		/* pull the next line up */
		else
			text_join(e->t, y1 - 1);	/* drop the last line */
	}
	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
}

/* Store bytes (ownership transferred) into the current register: the unnamed
 * register when none is armed, else the register named by e->vi_reg, with an
 * uppercase name appending rather than replacing. The unnamed register always
 * mirrors the stored text, and the one-shot register selection is released. */
static void
vi_reg_store(Editor *e, char *bytes, size_t len, int linewise)
{
	char reg = e->vi_reg;

	if (reg >= 'a' && reg <= 'z') {
		Reg *r = &e->vi_regs[reg - 'a'];
		char *dup = malloc(len ? len : 1);

		if (dup) {
			memcpy(dup, bytes, len);
			free(r->bytes);
			r->bytes = dup;
			r->len = len;
			r->linewise = linewise;
		}
	} else if (reg >= 'A' && reg <= 'Z') {
		Reg *r = &e->vi_regs[reg - 'A'];
		size_t nl = r->len + len;
		char *cat = malloc(nl ? nl : 1);

		if (cat) {
			if (r->len)
				memcpy(cat, r->bytes, r->len);
			memcpy(cat + r->len, bytes, len);
			free(r->bytes);
			r->bytes = cat;
			r->len = nl;
			r->linewise = r->linewise || linewise;
			free(bytes);			/* mirror the whole reg */
			bytes = malloc(nl ? nl : 1);
			len = bytes ? nl : 0;
			if (bytes)
				memcpy(bytes, cat, nl);
			linewise = r->linewise;
		}
	}
	clip_set(e, bytes, len);		/* unnamed register */
	e->clip_linewise = linewise;
	e->vi_reg = 0;				/* consume the selection */
}

/* Resolve the register to read for a put: the named register selected by
 * e->vi_reg, or the unnamed clip. The returned bytes are owned by the editor
 * and must not be freed by the caller. */
static void
vi_reg_get(Editor *e, const char **bytes, size_t *len, int *linewise)
{
	char reg = e->vi_reg;

	if (reg >= 'A' && reg <= 'Z')
		reg += 'a' - 'A';
	if (reg >= 'a' && reg <= 'z') {
		Reg *r = &e->vi_regs[reg - 'a'];

		*bytes = r->bytes;
		*len = r->len;
		*linewise = r->linewise;
	} else {
		*bytes = e->clip;
		*len = e->clip_len;
		*linewise = e->clip_linewise;
	}
}

/* Yank a charwise span into the register. */
static void
vi_yank_region(Editor *e, size_t sy, size_t sx, size_t ey, size_t ex)
{
	size_t rl = 0;
	char *r = region_text(e, sy, sx, ey, ex, &rl);

	if (r)
		vi_reg_store(e, r, rl, 0);
}

/* Yank whole lines [y1,y2] into the register, with a trailing newline so a
 * later put reproduces them as lines. */
static void
vi_yank_lines(Editor *e, size_t y1, size_t y2)
{
	size_t rl = 0, lastlen = text_line_len(e->t, y2);
	char *r = region_text(e, y1, 0, y2, lastlen, &rl);
	char *r2;

	if (!r)
		return;
	r2 = realloc(r, rl + 1);
	if (r2) {
		r2[rl] = '\n';
		vi_reg_store(e, r2, rl + 1, 1);
	} else {
		vi_reg_store(e, r, rl, 1);
	}
}

static void
enter_insert(Editor *e)
{
	e->mode = MODE_INSERT;
}

static void vi_shift_lines(Editor *e, size_t y1, size_t y2, int dir);

/* Apply operator op linewise over lines [lo,hi]. The caller has already opened
 * the undo group; this closes it (except for a change, which stays open until
 * the insert Esc). */
static Req
vi_op_lines(Editor *e, char op, size_t lo, size_t hi)
{
	vi_yank_lines(e, lo, hi);
	if (op == 'y') {
		e->cy = lo;
		e->cx = first_nonblank(e, lo);
		vi_clamp(e);
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	vi_delete_lines(e, lo, hi);
	if (op == 'c') {
		if (lo >= text_lines(e->t)) {
			size_t last = text_lines(e->t) - 1;

			e->cy = last;
			e->cx = text_line_len(e->t, last);
			ed_newline(e);
		} else {
			text_split(e->t, lo, 0);
			e->cy = lo;
			e->cx = 0;
		}
		enter_insert(e);
		return REQ_CONTINUE;		/* group stays open until Esc */
	}
	e->cy = lo;
	e->cx = first_nonblank(e, lo);
	vi_clamp(e);
	text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/* Apply an armed operator (d/c/y, or the > / < shifts) over a resolved
 * motion. */
static Req
vi_apply_operator(Editor *e, char op, Motion m)
{
	size_t sy, sx, ey, ex;

	/* Shift operators act on whole lines and neither yank nor delete. */
	if (op == '>' || op == '<') {
		size_t lo = e->cy < m.y ? e->cy : m.y;
		size_t hi = e->cy < m.y ? m.y : e->cy;

		vi_shift_lines(e, lo, hi, op == '>' ? 1 : -1);
		return REQ_CONTINUE;
	}

	/* One undo step per operator. A change (c) keeps the group open so
	 * the text typed afterward undoes together with the deletion; the
	 * insert-mode Esc closes it. */
	text_undo_group_begin(e->t);

	if (m.line) {
		size_t lo = e->cy < m.y ? e->cy : m.y;
		size_t hi = e->cy < m.y ? m.y : e->cy;

		return vi_op_lines(e, op, lo, hi);
	}

	if (e->cy < m.y || (e->cy == m.y && e->cx <= m.x)) {
		sy = e->cy;
		sx = e->cx;
		ey = m.y;
		ex = m.x;
	} else {
		sy = m.y;
		sx = m.x;
		ey = e->cy;
		ex = e->cx;
	}
	if (m.incl) {
		size_t elen = 0;
		const char *es = text_line(e->t, ey, &elen);

		if (ex < elen)
			ex += rune_len_at(es, elen, ex);
	} else if (ey > sy && ex == 0) {
		/* Exclusive-motion special case (d}, d{): an end in column 0 of
		 * a lower line pulls back to the close of the previous line, and
		 * becomes linewise when the start is at or before its first
		 * non-blank. */
		ey--;
		ex = text_line_len(e->t, ey);
		if (sx <= first_nonblank(e, sy))
			return vi_op_lines(e, op, sy, ey);
	}
	if (sy == ey && sx == ex) {		/* empty span */
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	vi_yank_region(e, sy, sx, ey, ex);
	if (op == 'y') {
		e->cy = sy;
		e->cx = sx;
		vi_clamp(e);
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	delete_region(e, sy, sx, ey, ex);
	if (op == 'c') {
		enter_insert(e);
		return REQ_CONTINUE;		/* group stays open until Esc */
	}
	vi_clamp(e);
	text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/* Put the register after (or before) the cursor: linewise as new lines,
 * charwise inline. */
/* Insert n spaces at the end of line y so its length reaches col. */
static void
vi_pad_to_col(Editor *e, size_t y, size_t col)
{
	size_t len = text_line_len(e->t, y), off = len, need;
	char sp[128];

	if (len >= col)
		return;
	need = col - len;
	memset(sp, ' ', sizeof(sp));
	while (need) {
		size_t chunk = need < sizeof(sp) ? need : sizeof(sp);

		text_insert(e->t, y, off, sp, chunk);
		off += chunk;
		need -= chunk;
	}
}

/* Visual-block delete: copy the rectangle to the register (blockwise), then
 * remove those columns from each row so the text closes up. One undo step. */
static void
vi_block_delete(Editor *e)
{
	size_t y1, y2, x1, x2, y;

	if (!e->sel_active)
		return;
	draw_block_bounds(e, &y1, &y2, &x1, &x2);
	draw_block_copy(e);			/* fills the register, clip_block=1 */
	text_undo_group_begin(e->t);
	for (y = y1; y <= y2 && y < text_lines(e->t); y++) {
		size_t len = text_line_len(e->t, y);
		size_t a = x1 < len ? x1 : len;
		size_t b = (x2 + 1) < len ? x2 + 1 : len;

		if (b > a)
			text_delete(e->t, y, a, b - a);
		hl_touch(e, y);
	}
	text_undo_group_end(e->t);
	e->cy = y1;
	e->cx = x1;
	vi_clamp(e);
	set_status(e, "deleted %zux%zu block",
	    x2 - x1 + 1, y2 - y1 + 1);
}

/* Visual-block yank: copy the rectangle blockwise, cursor to its top-left. */
static void
vi_block_yank(Editor *e)
{
	size_t y1, y2, x1, x2;

	if (!e->sel_active)
		return;
	draw_block_bounds(e, &y1, &y2, &x1, &x2);
	draw_block_copy(e);
	e->cy = y1;
	e->cx = x1;
	vi_clamp(e);
}

/* Enter insert mode for a block I (left edge) or A (right edge). The typed text
 * is replicated down the other rows by vi_block_insert_finish on Esc. The whole
 * session is one undo step, opened here and closed by the insert-mode Esc. */
static void
vi_block_insert_enter(Editor *e, int append)
{
	size_t y1, y2, x1, x2, col, len;

	draw_block_bounds(e, &y1, &y2, &x1, &x2);
	col = append ? x2 + 1 : x1;
	e->vi_visual = 0;
	e->sel_active = 0;
	e->sel_block = 0;
	vi_reset_pending(e);
	text_undo_group_begin(e->t);
	e->cy = y1;
	if (append)
		vi_pad_to_col(e, y1, col);	/* A reaches col even on a short top row */
	len = text_line_len(e->t, y1);
	e->cx = col < len ? col : len;
	e->vi_block_insert = 1;
	e->vi_bi_append = append;
	e->vi_bi_col = col;
	e->vi_bi_start = e->cx;
	e->vi_bi_y1 = y1;
	e->vi_bi_y2 = y2;
	enter_insert(e);
}

/* On leaving a block insert, copy what was typed on the top row and apply it to
 * the lower rows at the block column. Called while the undo group is still open.
 * I skips rows too short to reach the column; A pads them first. */
static void
vi_block_insert_finish(Editor *e)
{
	size_t y, col = e->vi_bi_col, start = e->vi_bi_start, len, tlen;
	const char *s;
	char *text;

	e->vi_block_insert = 0;
	if (e->cy != e->vi_bi_y1 || e->cx <= start)
		return;				/* nothing usable was typed */
	s = text_line(e->t, e->vi_bi_y1, &len);
	tlen = e->cx - start;
	text = malloc(tlen);
	if (!text)
		return;
	memcpy(text, s + start, tlen);
	for (y = e->vi_bi_y1 + 1; y <= e->vi_bi_y2 && y < text_lines(e->t); y++) {
		len = text_line_len(e->t, y);
		if (e->vi_bi_append)
			vi_pad_to_col(e, y, col);
		else if (len < col)
			continue;		/* I leaves short lines alone */
		text_insert(e->t, y, col, text, tlen);
		hl_touch(e, y);
	}
	free(text);
}

/* Paste a block-yanked register: insert each row's columns at the paste column
 * on consecutive lines, padding short lines and creating lines past the end so
 * the rectangle lands intact. One undo step. */
static void
vi_block_put(Editor *e, int after)
{
	const char *clip = e->clip;
	size_t clip_len = e->clip_len, i = 0, col, y, len = 0;
	const char *s;

	col = e->cx;
	if (after) {
		s = text_line(e->t, e->cy, &len);
		col = (e->cx < len) ? e->cx + rune_len_at(s, len, e->cx) : len;
	}
	text_undo_group_begin(e->t);
	y = e->cy;
	for (;;) {
		size_t j = i, rlen;

		while (j < clip_len && clip[j] != '\n')
			j++;
		rlen = j - i;
		while (y >= text_lines(e->t)) {		/* grow the buffer downward */
			size_t last = text_lines(e->t) - 1;

			text_split(e->t, last, text_line_len(e->t, last));
		}
		vi_pad_to_col(e, y, col);
		if (rlen)
			text_insert(e->t, y, col, clip + i, rlen);
		hl_touch(e, y);
		y++;
		if (j >= clip_len)
			break;
		i = j + 1;
	}
	text_undo_group_end(e->t);
	e->cx = col;
	vi_clamp(e);
	set_status(e, "pasted block");
}

static void
vi_put(Editor *e, int after)
{
	const char *clip;
	size_t clip_len;
	int linewise;

	vi_reg_get(e, &clip, &clip_len, &linewise);
	if (!clip || clip_len == 0) {
		set_status(e, "clipboard is empty");
		e->vi_reg = 0;
		return;
	}
	if (e->clip_block && clip == e->clip) {	/* a block-yanked rectangle */
		vi_block_put(e, after);
		e->vi_reg = 0;
		return;
	}
	text_undo_group_begin(e->t);		/* the whole put is one undo */

	if (linewise) {
		size_t l = clip_len, i = 0, first;
		int top_before = (!after && e->cy == 0);

		if (l && clip[l - 1] == '\n')
			l--;
		if (after) {
			e->cx = text_line_len(e->t, e->cy);
			first = e->cy + 1;
		} else if (top_before) {
			e->cy = 0;
			e->cx = 0;
			first = 0;
		} else {
			e->cy -= 1;
			e->cx = text_line_len(e->t, e->cy);
			first = e->cy + 1;
		}
		for (;;) {
			size_t j = i;

			while (j < l && clip[j] != '\n')
				j++;
			if (top_before) {
				if (j > i)
					ed_insert(e, clip + i, j - i);
				ed_newline(e);
			} else {
				ed_newline(e);
				if (j > i)
					ed_insert(e, clip + i, j - i);
			}
			if (j >= l)
				break;
			i = j + 1;
		}
		e->cy = first;
		e->cx = first_nonblank(e, first);
		vi_clamp(e);
	} else {
		if (after) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			if (e->cx < len)
				e->cx += rune_len_at(s, len, e->cx);
		}
		insert_bytes(e, clip, clip_len);
		if (e->cx > 0) {		/* rest on the last pasted rune */
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->cx -= prev_rune_len(s, e->cx);
		}
		vi_clamp(e);
	}
	text_undo_group_end(e->t);
	e->vi_reg = 0;				/* consume the selection */
}

/* Delete count runes at the cursor (the vi 'x' command). */
static void
vi_delete_char(Editor *e, int count)
{
	size_t len = 0, start = e->cx, x = e->cx, rl = 0;
	const char *s = text_line(e->t, e->cy, &len);
	char *r;
	int i;

	if (count < 1)
		count = 1;
	if (start >= len)
		return;
	for (i = 0; i < count && x < len; i++)
		x += rune_len_at(s, len, x);
	r = region_text(e, e->cy, start, e->cy, x, &rl);
	if (r)
		vi_reg_store(e, r, rl, 0);
	text_undo_boundary(e->t);
	hl_touch(e, e->cy);
	text_delete(e->t, e->cy, start, x - start);
	vi_clamp(e);
	text_undo_boundary(e->t);
}

/* Enter insert mode at the point implied by an insert-entry command. */
static void
vi_enter_insert_cmd(Editor *e, uint32_t c)
{
	size_t len = 0;
	const char *s;

	/* One undo step for the whole insert session; Esc closes the group. */
	text_undo_group_begin(e->t);
	switch (c) {
	case 'i':
		break;
	case 'a':
		s = text_line(e->t, e->cy, &len);
		if (e->cx < len)
			e->cx += rune_len_at(s, len, e->cx);
		break;
	case 'A':
		e->cx = text_line_len(e->t, e->cy);
		break;
	case 'I':
		e->cx = first_nonblank(e, e->cy);
		break;
	case 'o':
		e->cx = text_line_len(e->t, e->cy);
		ed_newline_indent(e);		/* indent like the line above */
		break;
	case 'O': {
		char indent[256];
		size_t ind = e->auto_indent ?
		    line_indent(e, e->cy, indent, sizeof(indent)) : 0;

		e->cx = 0;
		hl_touch(e, e->cy);
		text_split(e->t, e->cy, 0);	/* empty line; content moves down */
		if (ind)
			ed_insert(e, indent, ind);	/* match the line below */
		break;
	}
	}
	enter_insert(e);
}

/* Move the cursor by a signed number of lines, for the scroll keys. */
static void
vi_move_lines(Editor *e, int delta)
{
	if (delta < 0) {
		size_t d = (size_t)(-delta);

		e->cy = e->cy > d ? e->cy - d : 0;
	} else {
		e->cy += (size_t)delta;
		if (e->cy >= text_lines(e->t))
			e->cy = text_lines(e->t) - 1;
	}
	vi_clamp(e);
}

/* Shift lines [y1,y2] one indent level: dir > 0 prepends a tab, dir < 0 drops
 * a leading tab or up to TAB_WIDTH leading spaces. Blank lines are left alone.
 * The cursor rests on the first non-blank of the first shifted line. */
static void
vi_shift_lines(Editor *e, size_t y1, size_t y2, int dir)
{
	size_t y;

	if (y2 < y1) {
		size_t tmp = y1;

		y1 = y2;
		y2 = tmp;
	}
	if (y2 >= text_lines(e->t))
		y2 = text_lines(e->t) - 1;

	/* Shift by shiftwidth columns, or one tab stop when it is unset. */
	int sw = e->shiftwidth > 0 ? e->shiftwidth : TAB_WIDTH;

	text_undo_group_begin(e->t);
	for (y = y1; y <= y2; y++) {
		size_t len = 0;
		const char *s = text_line(e->t, y, &len);

		if (len == 0)			/* leave blank lines unindented */
			continue;
		if (dir > 0) {
			char ind[TAB_WIDTH * 8 + 8];	/* a bounded indent run */
			int ni = 0, cols = sw;

			if (e->expand_tabs) {
				while (cols-- > 0 && ni < (int)sizeof(ind))
					ind[ni++] = ' ';
			} else {
				while (cols >= TAB_WIDTH && ni < (int)sizeof(ind)) {
					ind[ni++] = '\t';
					cols -= TAB_WIDTH;
				}
				while (cols-- > 0 && ni < (int)sizeof(ind))
					ind[ni++] = ' ';
			}
			if (ni)
				text_insert(e->t, y, 0, ind, (size_t)ni);
		} else {
			size_t i = 0;
			int cols = 0;

			/* drop leading whitespace up to sw columns (a tab is a
			 * full stop, so the last one may slightly overshoot) */
			while (i < len && cols < sw) {
				if (s[i] == '\t')
					cols += TAB_WIDTH;
				else if (s[i] == ' ')
					cols += 1;
				else
					break;
				i++;
			}
			if (i)
				text_delete(e->t, y, 0, i);
		}
	}
	e->cy = y1;
	e->cx = first_nonblank(e, y1);
	hl_touch(e, y1);
	vi_clamp(e);
	text_undo_group_end(e->t);
}

/* Carry out a motion character: move the cursor, or, when an operator is
 * armed, apply it over the motion's span. */
static Req
vi_do_motion(Editor *e, uint32_t motchar)
{
	int mot_have = e->vi_count > 0;
	int mot_count = mot_have ? e->vi_count : 1;
	int have, count;
	Motion m;

	if (e->vi_op) {
		int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;

		count = oc * mot_count;
		have = e->vi_op_count > 0 || mot_have;
	} else {
		count = mot_count;
		have = mot_have;
	}

	m = vi_motion(e, motchar, count, have);
	if (!m.valid) {
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}

	/* The long-range motions are jumps: remember where we came from. */
	if (!e->vi_op && (motchar == 'G' || motchar == 'g' || motchar == 'H' ||
	    motchar == 'M' || motchar == 'L' || motchar == '%' ||
	    motchar == '{' || motchar == '}' || motchar == '(' ||
	    motchar == ')'))
		jump_record(e);

	if (e->vi_op) {
		char op = e->vi_op;

		/* cw/cW change to the end of the word, like ce/cE */
		if (op == 'c' && (motchar == 'w' || motchar == 'W'))
			m = vi_motion(e, motchar == 'w' ? 'e' : 'E', count,
			    have);
		/* dw/yw stop at the end of the line rather than joining */
		else if ((motchar == 'w' || motchar == 'W') && m.y != e->cy) {
			m.y = e->cy;
			m.x = text_line_len(e->t, e->cy);
		}
		vi_reset_pending(e);
		return vi_apply_operator(e, op, m);
	}

	if (motchar == 'j' || motchar == 'k') {
		/* j/k aim for the display column of the run's first line, so
		 * passing through short lines does not lose the column. */
		if (!e->vi_vert_prev) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->vi_want_col = s ? disp_cols(s, e->cx) : 0;
		}
		e->cy = m.y;
		e->cx = vi_col_to_byte(e, e->cy, e->vi_want_col);
		vi_clamp(e);
		e->vi_vert_run = 1;
	} else if (m.line) {
		e->cy = m.y;
		if (motchar == 'G' || motchar == 'g' || motchar == 'H' ||
		    motchar == 'M' || motchar == 'L' || motchar == '%')
			e->cx = first_nonblank(e, e->cy);
		vi_clamp(e);
	} else {
		e->cy = m.y;
		e->cx = m.x;
		vi_clamp(e);
		if (motchar == '$') {		/* stick to end of line under j/k */
			e->vi_want_col = INT_MAX;
			e->vi_vert_run = 1;
		}
	}
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* Carry out an f/F/t/T (or a ; / , repeat) search, moving the cursor or, when
 * an operator is armed, applying it over the span. repeat is set for ; and ,
 * so the remembered search is not overwritten. */
static Req
vi_do_charsearch(Editor *e, char cmd, uint32_t target, int repeat)
{
	int mot_count = e->vi_count > 0 ? e->vi_count : 1;
	int count;
	Motion m;

	if (!repeat) {
		e->vi_last_fT = cmd;
		e->vi_last_fT_ch = target;
	}
	if (e->vi_op) {
		int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;

		count = oc * mot_count;
	} else {
		count = mot_count;
	}

	m = vi_charsearch_motion(e, cmd, target, count, repeat);
	if (!m.valid) {
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}
	if (e->vi_op) {
		char op = e->vi_op;

		vi_reset_pending(e);
		return vi_apply_operator(e, op, m);
	}
	e->cy = m.y;
	e->cx = m.x;
	vi_clamp(e);
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* Text objects: iw/aw, i(/a( and friends, i"/a" and friends. Each resolves to
 * a charwise span [sy,sx)..(ey,ex) with an exclusive end, which an operator
 * (diw, ci() or visual mode (viw) then acts on. */

/* Word object on the current line. The run of same-class runes under the
 * cursor for 'i'; 'a' extends by trailing whitespace, or leading whitespace
 * when there is none, or (on whitespace) the following word. big picks WORD
 * class (whitespace-delimited) over word class. */
static int
vi_word_object(Editor *e, char kind, int big, size_t *sx, size_t *ex)
{
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t cx, start, end;
	uint32_t r;
	int cls;

	if (!s || len == 0)
		return 0;
	cx = e->cx;
	if (cx >= len)
		cx = len - prev_rune_len(s, len);	/* last rune */
	vi_rune(e, e->cy, cx, &r);
	cls = vi_class(r, big);

	start = cx;
	while (start > 0) {				/* back over same class */
		size_t pl = prev_rune_len(s, start);
		uint32_t pr;

		vi_rune(e, e->cy, start - pl, &pr);
		if (vi_class(pr, big) != cls)
			break;
		start -= pl;
	}
	end = cx;
	while (end < len) {				/* forward over same class */
		uint32_t nr;
		int nl = vi_rune(e, e->cy, end, &nr);

		if (vi_class(nr, big) != cls)
			break;
		end += (size_t)nl;
	}

	if (kind == 'a' && cls != 0) {			/* word + trailing ws */
		size_t e2 = end;

		while (e2 < len) {
			uint32_t nr;
			int nl = vi_rune(e, e->cy, e2, &nr);

			if (vi_class(nr, big) != 0)
				break;
			e2 += (size_t)nl;
		}
		if (e2 > end) {
			end = e2;
		} else {				/* none: leading ws */
			while (start > 0) {
				size_t pl = prev_rune_len(s, start);
				uint32_t pr;

				vi_rune(e, e->cy, start - pl, &pr);
				if (vi_class(pr, big) != 0)
					break;
				start -= pl;
			}
		}
	} else if (kind == 'a') {			/* ws + following word */
		while (end < len) {
			uint32_t nr;
			int nl = vi_rune(e, e->cy, end, &nr);

			if (vi_class(nr, big) == 0)
				break;
			end += (size_t)nl;
		}
	}

	*sx = start;
	*ex = end;
	return 1;
}

/* Bracket object: find the pair of open/close brackets enclosing the cursor,
 * counting nesting across lines. 'i' spans inside them, 'a' includes them. */
static int
vi_bracket_object(Editor *e, char kind, uint32_t open, uint32_t close,
    size_t *sy, size_t *sx, size_t *ey, size_t *ex)
{
	size_t oy = e->cy, ox = e->cx, ny, nx;
	uint32_t r;
	int depth, ol, cl;

	depth = 0;					/* enclosing open, back */
	for (;;) {
		if (vi_rune(e, oy, ox, &r) > 0) {
			if (r == close && !(oy == e->cy && ox == e->cx))
				depth++;
			else if (r == open) {
				if (depth == 0)
					break;
				depth--;
			}
		}
		if (!vi_step_back(e, &oy, &ox))
			return 0;
	}

	ny = oy;					/* matching close, fwd */
	nx = ox;
	depth = 0;
	for (;;) {
		if (vi_rune(e, ny, nx, &r) > 0) {
			if (r == open)
				depth++;
			else if (r == close && --depth == 0)
				break;
		}
		if (!vi_step_fwd(e, &ny, &nx))
			return 0;
	}

	{ uint32_t rr; ol = vi_rune(e, oy, ox, &rr); cl = vi_rune(e, ny, nx, &rr); }
	if (ol <= 0)
		ol = 1;
	if (cl <= 0)
		cl = 1;
	if (kind == 'a') {
		*sy = oy;
		*sx = ox;
		*ey = ny;
		*ex = nx + (size_t)cl;
	} else {
		*sy = oy;
		*sx = ox + (size_t)ol;
		*ey = ny;
		*ex = nx;
	}
	return 1;
}

/* Quote object on the current line. Quotes pair left to right; the chosen pair
 * is the first whose closing quote is at or after the cursor. 'i' spans
 * between the quotes, 'a' includes them. */
static int
vi_quote_object(Editor *e, char kind, uint32_t q, size_t *sx,
    size_t *ex)
{
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t x = 0, open_pos = 0;
	int have_open = 0;

	if (!s)
		return 0;
	while (x < len) {
		uint32_t r;
		int n = vi_rune(e, e->cy, x, &r);

		if (n <= 0)
			n = 1;
		if (r == q) {
			if (!have_open) {
				open_pos = x;
				have_open = 1;
			} else {
				if (e->cx <= x) {
					if (kind == 'a') {
						*sx = open_pos;
						*ex = x + (size_t)n;
					} else {
						*sx = open_pos + 1;
						*ex = x;
					}
					return 1;
				}
				have_open = 0;
			}
		}
		x += (size_t)n;
	}
	return 0;
}

/* Resolve a text object named by obj under the cursor into a charwise span.
 * Returns 1 on success. Word and quote objects are line-local; bracket objects
 * may span lines. */
static int
vi_text_object(Editor *e, char kind, uint32_t obj, size_t *sy,
    size_t *sx, size_t *ey, size_t *ex)
{
	*sy = *ey = e->cy;
	switch (obj) {
	case 'w':
		return vi_word_object(e, kind, 0, sx, ex);
	case 'W':
		return vi_word_object(e, kind, 1, sx, ex);
	case '(':
	case ')':
	case 'b':
		return vi_bracket_object(e, kind, '(', ')', sy, sx, ey, ex);
	case '{':
	case '}':
	case 'B':
		return vi_bracket_object(e, kind, '{', '}', sy, sx, ey, ex);
	case '[':
	case ']':
		return vi_bracket_object(e, kind, '[', ']', sy, sx, ey, ex);
	case '<':
	case '>':
		return vi_bracket_object(e, kind, '<', '>', sy, sx, ey, ex);
	case '"':
		return vi_quote_object(e, kind, '"', sx, ex);
	case '\'':
		return vi_quote_object(e, kind, '\'', sx, ex);
	case '`':
		return vi_quote_object(e, kind, '`', sx, ex);
	default:
		return 0;
	}
}

/* Apply an armed operator (d/c/y) over a resolved text-object span. Mirrors
 * the charwise branch of vi_apply_operator, but the span is explicit rather
 * than cursor-to-motion. */
static Req
vi_apply_textobject_op(Editor *e, char op, size_t sy, size_t sx,
    size_t ey, size_t ex)
{
	if (op == '>' || op == '<') {		/* shift the object's lines */
		vi_shift_lines(e, sy, ey, op == '>' ? 1 : -1);
		return REQ_CONTINUE;
	}
	text_undo_group_begin(e->t);
	if (sy == ey && sx == ex) {		/* empty object: nothing to do */
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	vi_yank_region(e, sy, sx, ey, ex);
	if (op == 'y') {
		e->cy = sy;
		e->cx = sx;
		vi_clamp(e);
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	delete_region(e, sy, sx, ey, ex);
	if (op == 'c') {
		enter_insert(e);
		return REQ_CONTINUE;		/* group stays open until Esc */
	}
	vi_clamp(e);
	text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/* Map a mark character to a slot index, or -1 if it names no mark. The special
 * marks (. ^ < > and the ` / ' previous-position pair) are set by commands, not
 * by the user, so m rejects them (see the markcmd handler). */
static int
mark_index(int ch)
{
	if (ch >= 'a' && ch <= 'z')
		return ch - 'a';
	switch (ch) {
	case '`': case '\'':	return MARK_PREV;
	case '.':		return MARK_CHANGE;
	case '^':		return MARK_INSERT;
	case '<':		return MARK_VISLT;
	case '>':		return MARK_VISGT;
	}
	return -1;
}

/* Record a position in a mark slot. */
static void
vi_mark_set(Editor *e, int slot, size_t y, size_t x)
{
	if (slot < 0 || slot >= MARK_SLOTS)
		return;
	e->vi_mark_y[slot] = y;
	e->vi_mark_x[slot] = x;
	e->vi_marks_set |= (uint64_t)1 << slot;
}

/* Jump to a stored mark position (my,mx). cmd is '`' (go to the exact column)
 * or '\'' (go to the line's first non-blank). With an operator armed it applies
 * the operator over the span instead of moving: charwise and exclusive for '`',
 * linewise for '\''. Taking (my,mx) by value lets the caller read the mark
 * before anything (like recording the jump) overwrites it. */
static Req
vi_goto_pos(Editor *e, char cmd, size_t my, size_t mx)
{
	size_t y = my, x;
	char op = e->vi_op;

	if (y >= text_lines(e->t))
		y = text_lines(e->t) - 1;
	x = (cmd == '`') ? mx : first_nonblank(e, y);

	if (op) {
		Motion m = { y, x, 0, 0, 1 };

		if (cmd == '\'')
			m.line = 1;		/* '<mark> is linewise */
		vi_reset_pending(e);
		return vi_apply_operator(e, op, m);
	}
	e->cy = y;
	e->cx = x;
	vi_clamp(e);
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* Toggle the case of count runes from the cursor, advancing past each. Only
 * ASCII letters flip; other runes are stepped over unchanged (the vi '~'). */
static void
vi_toggle_case(Editor *e, int count)
{
	int i;

	if (count < 1)
		count = 1;
	text_undo_group_begin(e->t);
	for (i = 0; i < count; i++) {
		size_t len = 0;
		const char *s = text_line(e->t, e->cy, &len);
		uint32_t r;
		int n;

		if (!s || e->cx >= len)
			break;
		n = vi_rune(e, e->cy, e->cx, &r);
		if (n == 1 && ((r >= 'a' && r <= 'z') ||
		    (r >= 'A' && r <= 'Z'))) {
			char t = (char)(r ^ 0x20);

			text_delete(e->t, e->cy, e->cx, 1);
			text_insert(e->t, e->cy, e->cx, &t, 1);
		}
		e->cx += (size_t)(n > 0 ? n : 1);
	}
	hl_touch(e, e->cy);
	vi_clamp(e);
	text_undo_group_end(e->t);
}

/* Join the current line with the ones below (the vi 'J'). count is the number
 * of lines involved, so it performs count-1 joins (a bare J joins one pair).
 * The newline goes away and a single space replaces the next line's leading
 * blanks, unless the current line is empty or already ends in whitespace. The
 * cursor rests at the join. */
static void
vi_join_lines(Editor *e, int count)
{
	int joins = count > 1 ? count - 1 : 1;
	int i;

	text_undo_group_begin(e->t);
	for (i = 0; i < joins; i++) {
		size_t curlen, nlen = 0, lead = 0, joinpos;
		const char *ns;

		if (e->cy + 1 >= text_lines(e->t))
			break;
		ns = text_line(e->t, e->cy + 1, &nlen);
		while (lead < nlen && (ns[lead] == ' ' || ns[lead] == '\t'))
			lead++;
		if (lead)
			text_delete(e->t, e->cy + 1, 0, lead);
		curlen = text_line_len(e->t, e->cy);
		joinpos = curlen;
		if (curlen > 0) {
			size_t cl = 0;
			const char *cs = text_line(e->t, e->cy, &cl);

			if (cs[cl - 1] != ' ' && cs[cl - 1] != '\t')
				text_insert(e->t, e->cy, curlen, " ", 1);
		}
		text_join(e->t, e->cy);		/* pull the next line up */
		e->cx = joinpos;
	}
	hl_touch(e, e->cy);
	vi_clamp(e);
	text_undo_group_end(e->t);
}

/* Change ('c') or delete ('d') the charwise span from the cursor to (ey,ex),
 * end exclusive. A change enters insert even when the span is empty, so C and
 * s at the end of a line still open for typing. */
static Req
vi_edit_span(Editor *e, char op, size_t ey, size_t ex)
{
	Motion m = { ey, ex, 0, 0, 1 };

	if (op == 'c' && e->cy == ey && e->cx == ex) {
		text_undo_group_begin(e->t);	/* closed by the insert Esc */
		enter_insert(e);
		return REQ_CONTINUE;
	}
	return vi_apply_operator(e, op, m);
}

/* Replace count runes at the cursor with the character ch (the vi 'r'). A
 * newline replaces them with a line break. Nothing happens when the line does
 * not hold count runes from the cursor, matching vi. The cursor rests on the
 * last replaced rune. */
static Req
vi_do_replace(Editor *e, uint32_t ch)
{
	int cnt = e->vi_count > 0 ? e->vi_count : 1;
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t x = e->cx, probe = e->cx;
	int avail = 0, i;

	while (probe < len) {			/* runes from cursor to EOL */
		probe += rune_len_at(s, len, probe);
		avail++;
	}
	if (cnt > avail) {			/* not enough on the line */
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}

	text_undo_group_begin(e->t);
	if (ch == '\n') {			/* r<CR>: drop the runes, break */
		size_t end = x;

		for (i = 0; i < cnt; i++)
			end += rune_len_at(s, len, end);
		text_delete(e->t, e->cy, x, end - x);
		text_split(e->t, e->cy, x);
		e->cy++;
		e->cx = 0;
	} else {
		unsigned char buf[8];
		int bn = utf8_encode(buf, ch);

		if (bn <= 0) {
			buf[0] = (unsigned char)ch;
			bn = 1;
		}
		for (i = 0; i < cnt; i++) {
			size_t rl;

			s = text_line(e->t, e->cy, &len);
			rl = rune_len_at(s, len, x);
			text_delete(e->t, e->cy, x, rl);
			text_insert(e->t, e->cy, x, (char *)buf, (size_t)bn);
			x += (size_t)bn;
		}
		e->cx = x - (size_t)bn;		/* rest on the last one */
	}
	hl_touch(e, e->cy);
	vi_clamp(e);
	text_undo_group_end(e->t);
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* Search for the word under (or next on the line after) the cursor, in
 * direction dir (the vi '*' and '#'). The word is matched as a plain
 * substring; there are no word boundaries, so it also matches inside longer
 * words. */
static void
vi_search_word(Editor *e, int dir)
{
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t x = e->cx, start, end, wl;
	char word[250];		/* +"\<" "\>" +NUL still fits e->last_find[256] */

	if (!s || len == 0) {
		set_status(e, "no word under cursor");
		return;
	}
	while (x < len) {			/* find a word char on the line */
		uint32_t r;
		int n = vi_rune(e, e->cy, x, &r);

		if (vi_class(r, 0) == 1)
			break;
		x += (size_t)(n > 0 ? n : 1);
	}
	if (x >= len) {
		set_status(e, "no word under cursor");
		return;
	}
	start = x;
	while (start > 0) {			/* back to the word start */
		size_t pl = prev_rune_len(s, start);
		uint32_t pr;

		vi_rune(e, e->cy, start - pl, &pr);
		if (vi_class(pr, 0) != 1)
			break;
		start -= pl;
	}
	end = x;
	while (end < len) {			/* out to the word end */
		uint32_t r;
		int n = vi_rune(e, e->cy, end, &r);

		if (vi_class(r, 0) != 1)
			break;
		end += (size_t)(n > 0 ? n : 1);
	}
	wl = end - start;
	if (wl == 0 || wl >= sizeof(word))
		return;
	memcpy(word, s + start, wl);
	word[wl] = '\0';
	/* match the whole word only, like Vim's * and # (the word is made of
	 * identifier characters, so it needs no regex escaping). */
	snprintf(e->last_find, sizeof(e->last_find), "\\<%s\\>", word);
	e->vi_search_dir = dir;
	e->cx = start;				/* search from the word start */
	ed_find_dir(e, e->last_find, dir);
}

static void vi_dot_replay(Editor *e);
static void vi_play_register(Editor *e, char reg, int count);

/* Start recording typed keys into a register (vi q). A lowercase name replaces
 * the register, an uppercase name appends to it. */
static void
vi_record_start(Editor *e, char reg)
{
	e->vi_rec_append = (reg >= 'A' && reg <= 'Z');
	e->vi_recording = e->vi_rec_append ? (char)(reg - 'A' + 'a') : reg;
	scr_record_start(e->d);
	set_status(e, "recording @%c", e->vi_recording);
}

/* Stop recording and store the captured keys in the register, dropping the
 * trailing q that ended it. The bytes are raw input, so @ replays them. */
static void
vi_record_stop(Editor *e)
{
	char reg = e->vi_recording;
	Reg *r = &e->vi_regs[reg - 'a'];
	size_t len;
	const unsigned char *bytes = scr_record_stop(e->d, &len);

	if (len > 0 && bytes[len - 1] == 'q')	/* the stop key is not recorded */
		len--;
	if (e->vi_rec_append && r->len) {
		size_t nl = r->len + len;
		char *cat = malloc(nl ? nl : 1);

		if (cat) {
			memcpy(cat, r->bytes, r->len);
			memcpy(cat + r->len, bytes, len);
			free(r->bytes);
			r->bytes = cat;
			r->len = nl;
		}
	} else {
		char *dup = malloc(len ? len : 1);

		if (dup) {
			memcpy(dup, bytes, len);
			free(r->bytes);
			r->bytes = dup;
			r->len = len;
		}
	}
	r->linewise = 0;
	e->vi_recording = 0;
	e->vi_rec_append = 0;
	set_status(e, "recorded %zu bytes into @%c", r->len, reg);
}

/* Handle one key in vi normal mode. */
static Req
vi_normal_key(Editor *e, const struct tkbd_seq *seq)
{
	uint32_t c = seq->ch;
	int ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;
	int page = text_height(e) - 1;
	int reg_fresh = e->vi_reg_fresh;

	if (page < 1)
		page = 1;

	/* Track whether the previous command was a vertical j/k/$ move, so a
	 * run of them keeps aiming at the same display column. Any other
	 * command leaves vi_vert_run clear and ends the run. */
	e->vi_vert_prev = e->vi_vert_run;
	e->vi_vert_run = 0;

	/* Release a register armed with " once the command that might use it
	 * has come and gone. It survives the arming key and any pending state
	 * that keeps a command in flight (a count, an operator, and so on). */
	e->vi_reg_fresh = 0;
	if (!reg_fresh && e->vi_reg && !e->vi_op && e->vi_count == 0 &&
	    !e->vi_gpending && !e->vi_charsearch && !e->vi_textobj &&
	    !e->vi_markcmd && !e->vi_regpending && !e->vi_zpending)
		e->vi_reg = 0;

	/* A pending f/F/t/T takes the next key as its literal target char. */
	if (e->vi_charsearch) {
		char cmd = e->vi_charsearch;

		e->vi_charsearch = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC)) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		return vi_do_charsearch(e, cmd, seq->ch, 0);
	}

	/* A pending i/a (after an operator) takes the next key as the object
	 * name: diw, ci(, ya" and so on. */
	if (e->vi_textobj) {
		char kind = e->vi_textobj;
		char op = e->vi_op;
		size_t sy, sx, ey, ex;

		e->vi_textobj = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC) ||
		    !op || !vi_text_object(e, kind, c, &sy, &sx, &ey, &ex)) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		vi_reset_pending(e);
		return vi_apply_textobject_op(e, op, sy, sx, ey, ex);
	}

	/* A pending m/`/' takes the next key as the mark name. m sets a-z only;
	 * the jumps ` and ' also reach the special marks (. ^ < > and ` / '). A
	 * plain jump records the origin in the jump list; the g`/g' variants do
	 * not (vi_mark_norec). */
	if (e->vi_markcmd) {
		char cmd = e->vi_markcmd;
		int norec = e->vi_mark_norec;
		int idx = ctrl ? -1 : mark_index((int)c);
		size_t my, mx;

		e->vi_markcmd = 0;
		e->vi_mark_norec = 0;
		if (idx < 0 || (cmd == 'm' && idx >= MARK_LETTERS)) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		if (cmd == 'm') {			/* set the mark */
			vi_mark_set(e, idx, e->cy, e->cx);
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		if (!(e->vi_marks_set & ((uint64_t)1 << idx))) {
			set_status(e, "E20: mark not set");
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		/* Read the target before jump_record rewrites the ` / ' mark. */
		my = e->vi_mark_y[idx];
		mx = e->vi_mark_x[idx];
		if (!norec)
			jump_record(e);
		return vi_goto_pos(e, cmd, my, mx);
	}

	/* A pending @ takes the next key as the macro register to play (a-z,
	 * @ for the last one, " for the unnamed register). */
	if (e->vi_atpending) {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;
		char reg = 0;

		e->vi_atpending = 0;
		if (ctrl)
			reg = 0;
		else if (c == '@')
			reg = e->vi_last_macro;
		else if (c >= 'a' && c <= 'z')
			reg = (char)c;
		else if (c >= 'A' && c <= 'Z')
			reg = (char)(c - 'A' + 'a');
		else if (c == '"')
			reg = '"';
		vi_reset_pending(e);
		if (reg)
			vi_play_register(e, reg, cnt);
		return REQ_CONTINUE;
	}

	/* A pending q takes the next key as the register to record into: a-z to
	 * replace it, A-Z to append. Anything else cancels. */
	if (e->vi_qpending) {
		e->vi_qpending = 0;
		if (!ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
			vi_record_start(e, (char)c);
		else
			set_status(e, "bad register for q");
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}

	/* A pending " takes the next key as the register name (a-z/A-Z). */
	if (e->vi_regpending) {
		e->vi_regpending = 0;
		if (ctrl || !((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z'))) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		e->vi_reg = (char)c;		/* armed for the next command */
		e->vi_reg_fresh = 1;
		return REQ_CONTINUE;
	}

	/* A pending r takes the next key as the replacement character. */
	if (e->vi_rpending) {
		e->vi_rpending = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC)) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		if (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ENTER)
			return vi_do_replace(e, '\n');
		return vi_do_replace(e, seq->ch);
	}

	/* A leading 'Z' expects a second key: ZZ writes and quits, ZQ quits
	 * without writing. */
	if (e->vi_zpending) {
		e->vi_zpending = 0;
		vi_reset_pending(e);
		if (c == 'Z') {			/* write if modified, then quit */
			if (text_dirty(e->t)) {
				if (!e->has_name) {
					set_status(e,
					    "E32: no file name");
					return REQ_CONTINUE;
				}
				if (ed_save_file(e) < 0) {
					set_status(e,
					    "save failed: %s",
					    strerror(errno));
					return REQ_CONTINUE;
				}
			}
			return REQ_FORCE_QUIT;
		}
		if (c == 'Q')			/* quit, discarding changes */
			return REQ_FORCE_QUIT;
		return REQ_CONTINUE;		/* any other key cancels */
	}

	if (ctrl && seq->type == TKBD_KEY) {
		vi_reset_pending(e);
		switch (seq->key) {
		case TKBD_KEY_D:
			vi_move_lines(e, page / 2);
			break;
		case TKBD_KEY_U:
			vi_move_lines(e, -(page / 2));
			break;
		case TKBD_KEY_F:
			vi_move_lines(e, page);
			break;
		case TKBD_KEY_B:
			vi_move_lines(e, -page);
			break;
		case TKBD_KEY_HOME:		/* Ctrl+Home: start of file (gg) */
			jump_record(e);
			e->cy = 0;
			e->cx = first_nonblank(e, 0);
			vi_clamp(e);
			break;
		case TKBD_KEY_END: {		/* Ctrl+End: end of file (G) */
			size_t last = text_lines(e->t);

			jump_record(e);
			e->cy = last ? last - 1 : 0;
			e->cx = first_nonblank(e, e->cy);
			vi_clamp(e);
			break;
		}
		case TKBD_KEY_R:
			e->sel_active = 0;
			e->vi_suppress_dot = 1;		/* redo is not a '.' */
			if (text_redo(e->t, &e->cy, &e->cx) != 0)
				set_status(e,
				    "nothing to redo");
			else {
				hl_touch(e, 0);
				vi_clamp(e);
			}
			break;
		case TKBD_KEY_T:			/* pop the tag stack (vim) */
			ed_tag_pop(e);
			vi_clamp(e);
			break;
		case TKBD_KEY_O:			/* Ctrl-O: older jump-list entry */
			jump_back(e);
			break;
		case TKBD_KEY_V:			/* Ctrl-V: visual block select */
			e->vi_visual = VI_VBLOCK;
			e->sel_active = 1;
			e->sel_block = 1;
			e->ay = e->cy;
			e->ax = e->cx;
			break;
		default:
			break;
		}
		return REQ_CONTINUE;
	}

	if (seq->type == TKBD_KEY) {
		switch (seq->key) {
		case TKBD_KEY_ESC:
			vi_reset_pending(e);
			return REQ_CONTINUE;
		case TKBD_KEY_TAB:		/* Ctrl-I: newer jump-list entry */
			vi_reset_pending(e);
			jump_forward(e);
			return REQ_CONTINUE;
		case TKBD_KEY_F1:
			vi_reset_pending(e);
			return REQ_HELP;
		case TKBD_KEY_LEFT:
			c = 'h';
			break;
		case TKBD_KEY_RIGHT:
			c = 'l';
			break;
		case TKBD_KEY_UP:
			c = 'k';
			break;
		case TKBD_KEY_DOWN:
		case TKBD_KEY_ENTER:
			c = 'j';
			break;
		case TKBD_KEY_HOME:
			c = '0';
			break;
		case TKBD_KEY_END:
			c = '$';
			break;
		case TKBD_KEY_BACKSPACE:
		case TKBD_KEY_BACKSPACE2:
			c = 'h';
			break;
		case TKBD_KEY_DEL:
			c = 'x';
			break;
		case TKBD_KEY_PGUP:
			vi_reset_pending(e);
			vi_move_lines(e, -page);
			return REQ_CONTINUE;
		case TKBD_KEY_PGDN:
			vi_reset_pending(e);
			vi_move_lines(e, page);
			return REQ_CONTINUE;
		default:
			break;
		}
	}

	if (e->vi_gpending) {
		e->vi_gpending = 0;
		if (c == 'g')
			return vi_do_motion(e, 'g');
		if (c == '`' || c == '\'') {	/* g` / g': jump without a jumplist push */
			e->vi_markcmd = (char)c;
			e->vi_mark_norec = 1;
			return REQ_CONTINUE;
		}
		if (c == 'f') {
			ed_open_header(e);
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		if (c == 'v' && e->vi_last_vis) {	/* reselect the last range */
			size_t nl = text_lines(e->t);

			vi_reset_pending(e);
			e->vi_visual = e->vi_last_vis;
			e->sel_block = (e->vi_last_vis == VI_VBLOCK);
			e->ay = e->vi_lv_ay < nl ? e->vi_lv_ay : nl - 1;
			e->cy = e->vi_lv_cy < nl ? e->vi_lv_cy : nl - 1;
			e->ax = e->vi_lv_ax;
			e->cx = e->vi_lv_cx;
			if (e->ax > text_line_len(e->t, e->ay))
				e->ax = text_line_len(e->t, e->ay);
			e->sel_active = 1;
			vi_clamp(e);		/* fix the cursor end if it moved */
			return REQ_CONTINUE;
		}
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}

	if (c >= '1' && c <= '9') {
		e->vi_count = e->vi_count * 10 + (int)(c - '0');
		if (e->vi_count > 1000000)
			e->vi_count = 1000000;
		return REQ_CONTINUE;
	}
	if (c == '0' && e->vi_count > 0) {
		e->vi_count *= 10;
		if (e->vi_count > 1000000)
			e->vi_count = 1000000;
		return REQ_CONTINUE;
	}

	switch (c) {
	case 'h':
	case 'l':
	case 'k':
	case 'j':
	case '0':
	case '^':
	case '$':
	case 'w':
	case 'W':
	case 'b':
	case 'B':
	case 'e':
	case 'E':
	case 'G':
	case '{':
	case '}':
	case '(':
	case ')':
	case '%':
	case 'H':
	case 'M':
	case 'L':
	case '|':
	case ' ':
		return vi_do_motion(e, c);
	case 'f':
	case 'F':
	case 't':
	case 'T':
		e->vi_charsearch = (char)c;	/* wait for the target char */
		return REQ_CONTINUE;
	case ';':
		if (!e->vi_last_fT) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		return vi_do_charsearch(e, e->vi_last_fT, e->vi_last_fT_ch, 1);
	case ',': {
		char rev;

		if (!e->vi_last_fT) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		switch (e->vi_last_fT) {		/* the opposite direction */
		case 'f': rev = 'F'; break;
		case 'F': rev = 'f'; break;
		case 't': rev = 'T'; break;
		default:  rev = 't'; break;	/* was 'T' */
		}
		return vi_do_charsearch(e, rev, e->vi_last_fT_ch, 1);
	}
	case 'g':
		e->vi_gpending = 1;
		return REQ_CONTINUE;
	case 'Z':
		e->vi_zpending = 1;
		return REQ_CONTINUE;
	case 'm':
	case '`':
	case '\'':
		e->vi_markcmd = (char)c;		/* wait for the letter */
		return REQ_CONTINUE;
	case '"':
		e->vi_regpending = 1;			/* wait for the register */
		return REQ_CONTINUE;
	case 'd':
	case 'c':
	case 'y':
		if (e->vi_op == (char)c) {		/* dd / cc / yy */
			int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;
			int mc = e->vi_count > 0 ? e->vi_count : 1;
			char op = e->vi_op;
			Motion m = { 0, 0, 1, 0, 1 };

			m.y = e->cy + (size_t)(oc * mc - 1);
			if (m.y >= text_lines(e->t))
				m.y = text_lines(e->t) - 1;
			vi_reset_pending(e);
			return vi_apply_operator(e, op, m);
		}
		e->vi_op = (char)c;
		e->vi_op_count = e->vi_count;
		e->vi_count = 0;
		return REQ_CONTINUE;
	case '>':
	case '<':
		if (e->vi_op == (char)c) {		/* >> / << */
			int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;
			int mc = e->vi_count > 0 ? e->vi_count : 1;
			int dir = (c == '>') ? 1 : -1;
			size_t y2 = e->cy + (size_t)(oc * mc - 1);

			vi_reset_pending(e);
			vi_shift_lines(e, e->cy, y2, dir);
			return REQ_CONTINUE;
		}
		e->vi_op = (char)c;
		e->vi_op_count = e->vi_count;
		e->vi_count = 0;
		return REQ_CONTINUE;
	case 'i':
	case 'a':
		if (e->vi_op) {			/* diw, ci(, ... : await object */
			e->vi_textobj = (char)c;
			return REQ_CONTINUE;
		}
		vi_reset_pending(e);
		vi_enter_insert_cmd(e, c);
		return REQ_CONTINUE;
	case 'A':
	case 'I':
	case 'o':
	case 'O':
		vi_reset_pending(e);
		vi_enter_insert_cmd(e, c);
		return REQ_CONTINUE;
	case 'v':
	case 'V':
		vi_reset_pending(e);
		e->vi_visual = (char)c;
		e->sel_active = 1;
		e->ay = e->cy;			/* anchor the selection here */
		e->ax = e->cx;
		return REQ_CONTINUE;
	case 'x': {
		int count = e->vi_count > 0 ? e->vi_count : 1;

		vi_reset_pending(e);
		vi_delete_char(e, count);
		return REQ_CONTINUE;
	}
	case 'p':
		vi_reset_pending(e);
		vi_put(e, 1);
		return REQ_CONTINUE;
	case 'P':
		vi_reset_pending(e);
		vi_put(e, 0);
		return REQ_CONTINUE;
	case '.':
		vi_reset_pending(e);
		vi_dot_replay(e);
		return REQ_CONTINUE;
	case '@':			/* play a register as keystrokes */
		e->vi_atpending = 1;
		return REQ_CONTINUE;
	case 'q':			/* record keystrokes into a register */
		vi_reset_pending(e);
		if (e->vi_recording)
			vi_record_stop(e);
		else
			e->vi_qpending = 1;
		return REQ_CONTINUE;
	case 'J': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;

		vi_reset_pending(e);
		vi_join_lines(e, cnt);
		return REQ_CONTINUE;
	}
	case '~': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;

		vi_reset_pending(e);
		vi_toggle_case(e, cnt);
		return REQ_CONTINUE;
	}
	case 'r':
		e->vi_rpending = 1;		/* wait for the new character */
		return REQ_CONTINUE;
	case 'R':
		vi_reset_pending(e);
		e->vi_overtype = 1;		/* Replace mode: typing overwrites */
		vi_enter_insert_cmd(e, 'R');
		return REQ_CONTINUE;
	case 'D':
	case 'C': {
		char op = (c == 'D') ? 'd' : 'c';
		size_t len = text_line_len(e->t, e->cy);

		vi_reset_pending(e);
		return vi_edit_span(e, op, e->cy, len);
	}
	case 's': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;
		size_t len = 0;
		const char *s = text_line(e->t, e->cy, &len);
		size_t x = e->cx;
		int i;

		for (i = 0; i < cnt && x < len; i++)
			x += rune_len_at(s, len, x);
		vi_reset_pending(e);
		return vi_edit_span(e, 'c', e->cy, x);
	}
	case 'S': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;
		Motion m = { 0, 0, 1, 0, 1 };

		m.y = e->cy + (size_t)(cnt - 1);
		if (m.y >= text_lines(e->t))
			m.y = text_lines(e->t) - 1;
		vi_reset_pending(e);
		return vi_apply_operator(e, 'c', m);
	}
	case 'u':
		vi_reset_pending(e);
		e->sel_active = 0;
		e->vi_suppress_dot = 1;			/* undo is not a '.' */
		if (text_undo(e->t, &e->cy, &e->cx) != 0)
			set_status(e,
			    "nothing to undo");
		else {
			hl_touch(e, 0);
			vi_clamp(e);
		}
		return REQ_CONTINUE;
	case 'n':
	case 'N': {
		int dir = e->vi_search_dir < 0 ? -1 : 1;

		if (c == 'N')				/* repeat the other way */
			dir = -dir;
		vi_reset_pending(e);
		if (e->last_find[0]) {
			jump_record(e);
			ed_find_dir(e, e->last_find, dir);
		} else
			set_status(e,
			    "no previous search");
		return REQ_CONTINUE;
	}
	case '*':
		vi_reset_pending(e);
		vi_search_word(e, 1);
		return REQ_CONTINUE;
	case '#':
		vi_reset_pending(e);
		vi_search_word(e, -1);
		return REQ_CONTINUE;
	case ':':
		vi_reset_pending(e);
		return REQ_VI_COLON;
	case '/':
		vi_reset_pending(e);
		e->vi_search_dir = 1;
		return REQ_VI_SEARCH;
	case '?':
		vi_reset_pending(e);
		e->vi_search_dir = -1;
		return REQ_VI_SEARCH;
	default:
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}
}

/* Handle one key in vi insert mode. Esc returns to normal mode, backing the
 * cursor up one rune the way vi does. */
static Req
vi_insert_key(Editor *e, const struct tkbd_seq *seq)
{
	unsigned char buf[8];
	int ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;
	int n;

	if (seq->type != TKBD_KEY)
		return REQ_CONTINUE;

	switch (seq->key) {
	case TKBD_KEY_ESC:
		e->mode = MODE_NORMAL;
		e->vi_overtype = 0;
		if (e->vi_block_insert)		/* replicate within the open group */
			vi_block_insert_finish(e);
		text_undo_group_end(e->t);	/* close the insert session */
		if (e->cx > 0) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->cx -= prev_rune_len(s, e->cx);
		}
		vi_clamp(e);
		vi_mark_set(e, MARK_INSERT, e->cy, e->cx);	/* '^ */
		return REQ_CONTINUE;
	case TKBD_KEY_ENTER:
		ed_newline_indent(e);
		return REQ_CONTINUE;
	case TKBD_KEY_TAB:
		ed_indent_tab(e);
		return REQ_CONTINUE;
	case TKBD_KEY_BACKSPACE:
	case TKBD_KEY_BACKSPACE2:
		ed_backspace(e);
		return REQ_CONTINUE;
	case TKBD_KEY_DEL:
		ed_delete(e);
		return REQ_CONTINUE;
	case TKBD_KEY_LEFT:
		move_left(e);
		return REQ_CONTINUE;
	case TKBD_KEY_RIGHT:
		move_right(e);
		return REQ_CONTINUE;
	case TKBD_KEY_UP:
		if (e->cy > 0) {
			e->cy--;
			clamp_col(e);
		}
		return REQ_CONTINUE;
	case TKBD_KEY_DOWN:
		if (e->cy + 1 < text_lines(e->t)) {
			e->cy++;
			clamp_col(e);
		}
		return REQ_CONTINUE;
	case TKBD_KEY_HOME:
		if (ctrl)			/* Ctrl+Home: start of file */
			e->cy = 0;
		e->cx = 0;
		return REQ_CONTINUE;
	case TKBD_KEY_END:
		if (ctrl)			/* Ctrl+End: end of file */
			e->cy = text_lines(e->t) ? text_lines(e->t) - 1 : 0;
		e->cx = text_line_len(e->t, e->cy);
		return REQ_CONTINUE;
	default:
		break;
	}

	if (!ctrl && seq->ch != TKBD_CH_NONE && seq->ch >= 0x20 &&
	    seq->ch != 0x7f) {
		n = utf8_encode(buf, seq->ch);
		if (n > 0) {
			if (e->vi_overtype) {	/* R mode: overwrite the rune */
				size_t len = 0;
				const char *s = text_line(e->t, e->cy, &len);

				if (e->cx < len)
					text_delete(e->t, e->cy, e->cx,
					    rune_len_at(s, len, e->cx));
			}
			ed_insert(e, (char *)buf, (size_t)n);
		}
	}
	return REQ_CONTINUE;
}

/* Leave visual mode, dropping the selection. */
static void
vi_leave_visual(Editor *e)
{
	e->vi_visual = 0;
	e->sel_active = 0;
	e->sel_block = 0;
	vi_reset_pending(e);
}

/* The motion an operator sees for the current visual selection: the anchor is
 * the far end, the cursor the near end. Charwise is inclusive of both cells;
 * linewise spans whole lines. vi_apply_operator orders the two ends. */
static Motion
vi_visual_span(Editor *e)
{
	Motion m = { e->ay, e->ax, 0, 0, 1 };

	if (e->vi_visual == 'V')
		m.line = 1;
	else
		m.incl = 1;
	return m;
}

/* Handle one key in visual mode. Operators act on the selection and return to
 * normal mode; the visual keys and Esc leave it; everything else (motions,
 * counts, searches) runs through the normal handler and, with no operator
 * pending, just moves the cursor, so the selection tracks it. */
static Req
vi_visual_key(Editor *e, const struct tkbd_seq *seq)
{
	uint32_t c = seq->ch;
	int ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;

	/* Remember the selection before this key acts on it, so gv can reselect
	 * the same extent even after an operator consumes it. */
	e->vi_last_vis = e->vi_visual;
	e->vi_lv_ay = e->ay;
	e->vi_lv_ax = e->ax;
	e->vi_lv_cy = e->cy;
	e->vi_lv_cx = e->cx;

	/* Keep '< and '> on the current selection, so they still point at the
	 * last range after it is left (by Esc, a motion to normal, or an op). */
	if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
		vi_mark_set(e, MARK_VISLT, e->ay, e->ax);
		vi_mark_set(e, MARK_VISGT, e->cy, e->cx);
	} else {
		vi_mark_set(e, MARK_VISLT, e->cy, e->cx);
		vi_mark_set(e, MARK_VISGT, e->ay, e->ax);
	}

	/* A pending i/a takes the next key as the object name (viw, va(): the
	 * object becomes the selection, cursor on its last rune. */
	if (e->vi_textobj) {
		char kind = e->vi_textobj;
		size_t sy, sx, ey, ex, y, x;

		e->vi_textobj = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC) ||
		    !vi_text_object(e, kind, c, &sy, &sx, &ey, &ex) ||
		    (sy == ey && sx == ex))
			return REQ_CONTINUE;
		e->ay = sy;
		e->ax = sx;
		y = ey;
		x = ex;
		if (vi_step_back(e, &y, &x)) {	/* end is exclusive; step in */
			e->cy = y;
			e->cx = x;
		}
		vi_clamp(e);
		return REQ_CONTINUE;
	}

	if (e->vi_charsearch)		/* resolve an f/F/t/T target as a motion */
		return vi_normal_key(e, seq);

	if (seq->type == TKBD_KEY) {
		if (seq->key == TKBD_KEY_ESC) {
			vi_leave_visual(e);
			return REQ_CONTINUE;
		}
		if (seq->key == TKBD_KEY_DEL)
			c = 'x';		/* Delete removes the selection */
	}

	if (ctrl && seq->type == TKBD_KEY) {
		if (seq->key == TKBD_KEY_R)	/* no redo while selecting */
			return REQ_CONTINUE;
		if (seq->key == TKBD_KEY_V) {	/* Ctrl-V toggles / switches to block */
			if (e->vi_visual == VI_VBLOCK) {
				vi_leave_visual(e);
			} else {
				e->vi_visual = VI_VBLOCK;
				e->sel_block = 1;
			}
			return REQ_CONTINUE;
		}
		return vi_normal_key(e, seq);	/* Ctrl-D/U/F/B scroll */
	}

	/* Block mode has its own operators; motions and o/O/>/< fall through. */
	if (e->vi_visual == VI_VBLOCK) {
		switch (c) {
		case 'd':
		case 'x':
			vi_block_delete(e);
			vi_leave_visual(e);
			return REQ_CONTINUE;
		case 'y':
			vi_block_yank(e);
			vi_leave_visual(e);
			return REQ_CONTINUE;
		case 'I':
			vi_block_insert_enter(e, 0);
			return REQ_CONTINUE;
		case 'A':
			vi_block_insert_enter(e, 1);
			return REQ_CONTINUE;
		case 'v':
		case 'V':
			e->vi_visual = (char)c;	/* switch to charwise / linewise */
			e->sel_block = 0;
			return REQ_CONTINUE;
		/* no block form yet: swallow so they don't act charwise */
		case 'c':
		case 's':
		case 'C':
		case 'S':
		case 'r':
		case 'p':
		case 'P':
		case 'i':
		case 'a':
		case '~':
		case 'J':
			return REQ_CONTINUE;
		default:
			break;			/* o/O, >/<, and motions below */
		}
	}

	switch (c) {
	case 'v':
	case 'V':
		if (e->vi_visual == (char)c)
			vi_leave_visual(e);	/* same key toggles off */
		else
			e->vi_visual = (char)c;	/* switch charwise <-> linewise */
		return REQ_CONTINUE;
	case 'o':
	case 'O': {			/* jump to the other end of the selection */
		size_t ty = e->cy, tx = e->cx;

		e->cy = e->ay;
		e->cx = e->ax;
		e->ay = ty;
		e->ax = tx;
		vi_clamp(e);
		return REQ_CONTINUE;
	}
	case 'd':
	case 'x':
	case 'y': {
		Motion m = vi_visual_span(e);
		char op = (c == 'y') ? 'y' : 'd';

		vi_reset_pending(e);
		vi_apply_operator(e, op, m);
		vi_leave_visual(e);
		return REQ_CONTINUE;
	}
	case 'c':
	case 's': {
		Motion m = vi_visual_span(e);

		vi_reset_pending(e);
		vi_apply_operator(e, 'c', m);	/* deletes, then enters INSERT */
		e->vi_visual = 0;
		e->sel_active = 0;
		return REQ_CONTINUE;
	}
	case 'p':
	case 'P': {			/* replace the selection with the register */
		Motion m = vi_visual_span(e);
		char *reg = NULL;
		size_t reglen = e->clip_len;
		int reglw = e->clip_linewise;

		if (e->clip && reglen > 0) {
			reg = malloc(reglen);
			if (reg)
				memcpy(reg, e->clip, reglen);
		}
		vi_reset_pending(e);
		/* One undo step for the delete and the put together. Deleting
		 * fills the register with the removed text, so put back the
		 * saved register before pasting it in. */
		text_undo_group_begin(e->t);
		vi_apply_operator(e, 'd', m);
		if (reg) {
			clip_set(e, reg, reglen);	/* takes ownership */
			e->clip_linewise = reglw;
			vi_put(e, 0);
		}
		text_undo_group_end(e->t);
		vi_leave_visual(e);
		return REQ_CONTINUE;
	}
	case 'i':
	case 'a':			/* select the text object under cursor */
		e->vi_textobj = (char)c;
		return REQ_CONTINUE;
	case '>':
	case '<': {			/* shift the selected lines one level */
		size_t lo = e->cy < e->ay ? e->cy : e->ay;
		size_t hi = e->cy < e->ay ? e->ay : e->cy;

		vi_reset_pending(e);
		vi_shift_lines(e, lo, hi, c == '>' ? 1 : -1);
		vi_leave_visual(e);
		return REQ_CONTINUE;
	}
	/* Editing commands that have no selection form yet must not leak to
	 * the normal handler mid-selection; swallow them. */
	case 'A':
	case 'I':
	case 'u':
	case 'r':
	case 'Z':
	case '~':
	case 'J':
	case 'D':
	case 'C':
	case 'S':
		return REQ_CONTINUE;
	default:
		break;
	}

	/* Anything else is a motion (or count, or search): move the cursor and
	 * let the selection follow. */
	return vi_normal_key(e, seq);
}

/* Route a key to the handler for the current mode. */
static Req
vi_dispatch_key(Editor *e, const struct tkbd_seq *seq)
{
	if (e->mode == MODE_INSERT)
		return vi_insert_key(e, seq);
	if (e->vi_visual)
		return vi_visual_key(e, seq);
	return vi_normal_key(e, seq);
}

/* True when no command is in flight: normal mode with nothing pending and no
 * register armed. A command begins and ends at these rest points, which is
 * where the '.' recorder starts a recording and commits it. */
static int
vi_at_rest(const Editor *e)
{
	return e->mode == MODE_NORMAL && !e->vi_visual && !e->vi_op &&
	    e->vi_count == 0 && !e->vi_gpending && !e->vi_charsearch &&
	    !e->vi_textobj && !e->vi_markcmd && !e->vi_regpending &&
	    !e->vi_zpending && !e->vi_reg_fresh;
}

/* Append one key to a recording log, growing it as needed. */
static void
vi_keylog_push(Keylog *log, const struct tkbd_seq *seq)
{
	if (log->len >= log->cap) {
		int ncap = log->cap ? log->cap * 2 : 16;
		struct tkbd_seq *nev = realloc(log->ev,
		    (size_t)ncap * sizeof(*nev));

		if (!nev)
			return;			/* drop: the repeat may truncate */
		log->ev = nev;
		log->cap = ncap;
	}
	log->ev[log->len++] = *seq;
}

/* Copy the just-recorded command into the '.' log. */
static void
vi_dot_commit(Editor *e)
{
	Keylog *d = &e->vi_dot, *s = &e->vi_rec;

	if (s->len == 0)
		return;
	if (d->cap < s->len) {
		struct tkbd_seq *nev = realloc(d->ev,
		    (size_t)s->len * sizeof(*nev));

		if (!nev)
			return;
		d->ev = nev;
		d->cap = s->len;
	}
	memcpy(d->ev, s->ev, (size_t)s->len * sizeof(*s->ev));
	d->len = s->len;
}

/* Replay the last change recorded for '.'. */
static void
vi_dot_replay(Editor *e)
{
	int i, n = e->vi_dot.len;

	if (n == 0) {
		set_status(e, "nothing to repeat");
		return;
	}
	e->vi_cmd_open = 0;		/* keep this repeat out of the recording */
	e->vi_replaying = 1;
	for (i = 0; i < n; i++) {
		struct tkbd_seq seq = e->vi_dot.ev[i];

		vi_dispatch(e, &seq);
	}
	e->vi_replaying = 0;
}

/* Play a register's bytes back as keystrokes (vi @), count times. The bytes
 * are decoded the same way typed input is, except a bare ESC byte is always
 * the Esc key (never folded into an escape sequence) so a macro can leave
 * insert mode. A depth guard stops a register that calls itself. */
static void
vi_play_register(Editor *e, char reg, int count)
{
	const char *bytes;
	size_t len;
	int rep;

	if (reg == '"' || reg == 0) {
		bytes = e->clip;
		len = e->clip_len;
	} else if (reg >= 'a' && reg <= 'z') {
		bytes = e->vi_regs[reg - 'a'].bytes;
		len = e->vi_regs[reg - 'a'].len;
	} else {
		set_status(e, "no such register");
		return;
	}
	if (!bytes || len == 0) {
		set_status(e, "register empty");
		return;
	}
	if (e->vi_macro_depth > 50) {
		set_status(e, "macro nesting too deep");
		return;
	}
	if (count < 1)
		count = 1;
	e->vi_last_macro = reg ? reg : '"';
	e->vi_macro_depth++;
	for (rep = 0; rep < count; rep++) {
		size_t i = 0;
		int save = e->vi_replaying;

		e->vi_replaying = 1;		/* a macro records nothing itself */
		while (i < len) {
			struct tkbd_seq seq;
			int n;

			if ((unsigned char)bytes[i] == 0x1b) {
				seq_simple(&seq, TKBD_KEY_ESC, 0);
				n = 1;
			} else {
				n = tkbd_decode(&seq,
				    (const unsigned char *)bytes + i,
				    (int)(len - i));
				if (n <= 0) {	/* incomplete/unusable: skip a byte */
					i++;
					continue;
				}
			}
			vi_dispatch(e, &seq);
			i += (size_t)n;
		}
		e->vi_replaying = save;
	}
	e->vi_macro_depth--;
}

Req
vi_dispatch(Editor *e, const struct tkbd_seq *seq)
{
	int at_rest_before;
	Req r;

	if (e->vi_replaying)		/* a '.' replay records nothing */
		return vi_dispatch_key(e, seq);

	at_rest_before = vi_at_rest(e);
	if (at_rest_before) {		/* a fresh command starts here */
		e->vi_rec.len = 0;
		e->vi_cmd_open = 1;
		e->vi_suppress_dot = 0;
		e->vi_cmd_rev = text_revision(e->t);
	}
	if (e->vi_cmd_open)
		vi_keylog_push(&e->vi_rec, seq);

	r = vi_dispatch_key(e, seq);

	/* Back at rest: if the buffer changed, record the change position for '.
	 * and, when the command is repeatable, make it the new dot command. */
	if (e->vi_cmd_open && vi_at_rest(e)) {
		e->vi_cmd_open = 0;
		if (text_revision(e->t) != e->vi_cmd_rev) {
			vi_mark_set(e, MARK_CHANGE, e->cy, e->cx);	/* '. */
			if (!e->vi_suppress_dot)
				vi_dot_commit(e);
		}
	}
	return r;
}

/* A delimiter is a printable non-space, non-alphanumeric character, so that a
 * word command like ':syntax' is not mistaken for ':s/.../'. */
static int
is_ex_delim(char d)
{
	if (d == '\0' || d == ' ')
		return 0;
	return !((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
	    (d >= '0' && d <= '9'));
}

/* Count the matches of re on line s[0, len): all of them, or just the first
 * when global is 0. Empty matches advance by one so the count stays finite. */
static int
re_count_line(rx_t *re, const char *s, size_t len, int global)
{
	size_t from = 0;
	int n = 0;

	while (from <= len) {
		rx_match m[1];
		size_t ms, me;

		if (rx_exec(re, s, len, from, m, 1) != 1)
			break;
		ms = (size_t)m[0].so;
		me = (size_t)m[0].eo;
		n++;
		if (!global)
			break;
		from = (me > ms) ? me : ms + 1;
	}
	return n;
}

/* Replace matches of the compiled regex re with the template rep on line y: the
 * first only, or every one when global. Returns the number of substitutions
 * made. The template honours rx's &, \1..\9, and case conversions. */
static int
vi_ex_subst_line(Editor *e, size_t y, rx_t *re, const char *rep, int global)
{
	size_t len = 0;
	const char *s = text_line(e->t, y, &len);
	int matches;
	char *out;
	size_t outlen;

	if (!s)
		return 0;
	matches = re_count_line(re, s, len, global);
	if (matches == 0)
		return 0;
	out = rx_replace(re, s, len, rep, global ? RX_GLOBAL : 0);
	if (!out)
		return 0;
	outlen = strlen(out);
	text_delete(e->t, y, 0, len);
	if (outlen)
		text_insert(e->t, y, 0, out, outlen);
	free(out);
	hl_touch(e, y);
	return matches;
}

/* Run a :[range]s/pat/rep/[g] over lines [lo,hi]. `args` begins at the
 * delimiter (so args[0] is the '/' or other separator); an empty pattern
 * reuses the last search string. */
static Req
vi_ex_substitute(Editor *e, size_t lo, size_t hi, const char *args)
{
	char delim = args[0];
	char pat[256], rep[256];
	const char *p = args + 1, *use, *err;
	size_t n = 0, y;
	int global = 0, subs = 0, lines = 0;
	rx_t *re;

	if (!is_ex_delim(delim)) {
		set_status(e, "E146: missing separator");
		return REQ_CONTINUE;
	}
	while (*p && *p != delim) {			/* pattern */
		if (n < sizeof(pat) - 1)
			pat[n++] = *p;
		p++;
	}
	pat[n] = '\0';
	if (*p == delim)
		p++;
	n = 0;
	while (*p && *p != delim) {			/* replacement */
		if (n < sizeof(rep) - 1)
			rep[n++] = *p;
		p++;
	}
	rep[n] = '\0';
	if (*p == delim)
		p++;
	for (; *p; p++)					/* flags */
		if (*p == 'g')
			global = 1;

	use = pat[0] ? pat : e->last_find;
	if (!use || !use[0]) {
		set_status(e,
		    "E35: no previous regular expression");
		return REQ_CONTINUE;
	}
	re = rx_compile(use, search_flags(e), &err);
	if (!re) {
		set_status(e, "bad pattern: %.80s", err);
		return REQ_CONTINUE;
	}
	if (pat[0])
		snprintf(e->last_find, sizeof(e->last_find), "%s", pat);

	if (hi >= text_lines(e->t))
		hi = text_lines(e->t) - 1;
	text_undo_group_begin(e->t);
	for (y = lo; y <= hi; y++) {
		int k = vi_ex_subst_line(e, y, re, rep, global);

		if (k > 0) {
			subs += k;
			lines++;
			e->cy = y;
		}
	}
	text_undo_group_end(e->t);
	rx_free(re);

	if (subs == 0)
		set_status(e,
		    "pattern not found: %.60s", use);
	else {
		e->cx = first_nonblank(e, e->cy);
		vi_clamp(e);
		set_status(e,
		    "%d substitution%s on %d line%s", subs,
		    subs == 1 ? "" : "s", lines, lines == 1 ? "" : "s");
	}
	return REQ_CONTINUE;
}

/* Run :[range]g/pat/cmd -- apply cmd to each line matching pat (or, for :v and
 * :g!, each line not matching). The range defaults to the whole file. The
 * supported commands are d (delete) and s (substitute). Matching lines are
 * collected first so the command can shift line numbers safely. */
static Req
vi_ex_global(Editor *e, size_t lo, size_t hi, int had_range,
    const char *args, int invert)
{
	const char *p = args, *sub, *use, *err;
	char delim, pat[256];
	size_t n = 0, y, *rows, nrows = 0, i;
	rx_t *re;

	delim = *p;
	if (!is_ex_delim(delim)) {
		set_status(e, "E146: missing pattern");
		return REQ_CONTINUE;
	}
	p++;
	while (*p && *p != delim) {
		if (n < sizeof(pat) - 1)
			pat[n++] = *p;
		p++;
	}
	pat[n] = '\0';
	if (*p == delim)
		p++;
	while (*p == ' ')
		p++;
	sub = p;				/* command to run on each match */

	use = pat[0] ? pat : e->last_find;
	if (!use || !use[0]) {
		set_status(e,
		    "E35: no previous regular expression");
		return REQ_CONTINUE;
	}
	re = rx_compile(use, search_flags(e), &err);
	if (!re) {
		set_status(e, "bad pattern: %.80s", err);
		return REQ_CONTINUE;
	}
	if (pat[0])
		snprintf(e->last_find, sizeof(e->last_find), "%s", pat);

	if (!had_range) {			/* :g defaults to the whole file */
		lo = 0;
		hi = text_lines(e->t) - 1;
	}
	if (hi >= text_lines(e->t))
		hi = text_lines(e->t) - 1;

	rows = malloc((hi - lo + 1) * sizeof(*rows));
	if (!rows) {
		rx_free(re);
		return REQ_CONTINUE;
	}
	for (y = lo; y <= hi; y++) {
		size_t llen = 0;
		const char *s = text_line(e->t, y, &llen);
		rx_match m[1];
		int match = s && rx_exec(re, s, llen, 0, m, 1) == 1;

		if (match ^ invert)		/* keep matches, or non-matches */
			rows[nrows++] = y;
	}
	rx_free(re);

	text_undo_group_begin(e->t);
	if (sub[0] == 'd' && (sub[1] == '\0' || sub[1] == ' ')) {
		for (i = nrows; i > 0; i--)	/* delete bottom-up */
			vi_delete_lines(e, rows[i - 1], rows[i - 1]);
	} else if (sub[0] == 's' && is_ex_delim(sub[1])) {
		for (i = 0; i < nrows; i++)	/* substitute keeps line count */
			vi_ex_substitute(e, rows[i], rows[i], sub + 1);
	} else if (sub[0] == 'y' && (sub[1] == '\0' || sub[1] == ' ')) {
		size_t total = 0, off = 0;	/* yank the matches, linewise */
		char *buf;

		for (i = 0; i < nrows; i++)
			total += text_line_len(e->t, rows[i]) + 1;
		buf = malloc(total ? total : 1);
		if (buf) {
			for (i = 0; i < nrows; i++) {
				size_t ll = 0;
				const char *s = text_line(e->t, rows[i], &ll);

				if (ll)
					memcpy(buf + off, s, ll);
				off += ll;
				buf[off++] = '\n';
			}
			vi_reg_store(e, buf, off, 1);	/* takes the buffer */
		}
	} else if ((sub[0] == '>' || sub[0] == '<') &&
	    (sub[1] == '\0' || sub[1] == ' ')) {
		for (i = 0; i < nrows; i++)	/* shift keeps line count */
			vi_shift_lines(e, rows[i], rows[i], sub[0] == '>' ? 1 : -1);
	} else {
		text_undo_group_end(e->t);
		free(rows);
		set_status(e,
		    "unsupported :g command (d, s, y, >, <): %.30s", sub);
		return REQ_CONTINUE;
	}
	text_undo_group_end(e->t);

	set_status(e, "%zu line%s matched", nrows,
	    nrows == 1 ? "" : "s");
	free(rows);
	e->cx = first_nonblank(e, e->cy);
	vi_clamp(e);
	return REQ_CONTINUE;
}

/* Parse one ex line address at *pp into *out (a 1-based line number), starting
 * from the current line cur. Handles '.', '$', a number, a mark ('x), and any
 * run of +N/-N offsets. Returns 1 when an address was read, 0 when there was
 * none, or -1 on an error (an undefined mark). *pp is advanced past it. */
static int
vi_ex_addr(Editor *e, char **pp, long cur, long *out)
{
	char *p = *pp;
	long base = cur;
	int have = 0;

	while (*p == ' ')
		p++;
	if (*p == '.') {
		base = cur;
		p++;
		have = 1;
	} else if (*p == '$') {
		base = (long)text_lines(e->t);
		p++;
		have = 1;
	} else if (*p == '\'') {			/* 'x -- a mark */
		int idx = p[1] - 'a';

		if (p[1] < 'a' || p[1] > 'z' ||
		    !(e->vi_marks_set & ((uint32_t)1 << idx)))
			return -1;
		base = (long)e->vi_mark_y[idx] + 1;
		p += 2;
		have = 1;
	} else if (*p >= '0' && *p <= '9') {
		base = strtol(p, &p, 10);
		have = 1;
	}
	while (*p == '+' || *p == '-') {		/* offsets */
		int sign = (*p == '+') ? 1 : -1;
		long n = 1;

		p++;
		if (*p >= '0' && *p <= '9')
			n = strtol(p, &p, 10);
		base += sign * n;
		have = 1;
	}
	*out = base;
	*pp = p;
	return have;
}

/* Parse an optional leading line range at *pp into 0-based inclusive [*lo,*hi],
 * clamped to the buffer and ordered. Returns 1 when a range (or '%') was
 * present, 0 when none was, or -1 on an error. *pp is advanced past it. */
static int
vi_ex_parse_range(Editor *e, char **pp, size_t *lo, size_t *hi)
{
	char *p = *pp;
	long cur = (long)e->cy + 1;
	long last = (long)text_lines(e->t);
	long a1, a2;
	int r;

	while (*p == ' ')
		p++;
	if (*p == '%') {				/* the whole file */
		p++;
		*lo = 0;
		*hi = (size_t)(last - 1);
		*pp = p;
		return 1;
	}
	r = vi_ex_addr(e, &p, cur, &a1);
	if (r < 0)
		return -1;
	if (r == 0) {
		*pp = p;
		return 0;
	}
	a2 = a1;
	if (*p == ',' || *p == ';') {
		int semi = (*p == ';');
		long c;

		p++;
		if (semi)				/* ; moves . to the first */
			cur = a1;
		r = vi_ex_addr(e, &p, cur, &c);
		if (r < 0)
			return -1;
		if (r > 0)
			a2 = c;
		else
			a2 = cur;
	}
	if (a1 < 1)
		a1 = 1;
	if (a2 < 1)
		a2 = 1;
	if (a1 > last)
		a1 = last;
	if (a2 > last)
		a2 = last;
	if (a1 > a2) {
		long t = a1;

		a1 = a2;
		a2 = t;
	}
	*lo = (size_t)(a1 - 1);
	*hi = (size_t)(a2 - 1);
	*pp = p;
	return 1;
}

/* Read the file named by fn into the buffer, inserting its contents on the line
 * below line "at". The cursor lands on the first inserted line, matching vi's
 * :r. fn is the argument already past the command word. */
static Req
vi_ex_read_file(Editor *e, size_t at, const char *fn)
{
	Text *nt;
	char *bytes;
	size_t nlines, i, total, off;

	if (*fn == '\0') {
		set_status(e, "E32: no file name");
		return REQ_CONTINUE;
	}
	nt = text_new();
	if (!nt) {
		set_status(e, "out of memory");
		return REQ_CONTINUE;
	}
	if (text_load(nt, fn) < 0) {
		set_status(e,
		    "E484: cannot open %.80s", fn);
		text_free(nt);
		return REQ_CONTINUE;
	}

	/* Join the file's lines with newlines, led by one newline so the
	 * text opens on a fresh line below "at". */
	nlines = text_lines(nt);
	total = 1;
	for (i = 0; i < nlines; i++)
		total += text_line_len(nt, i);
	if (nlines > 1)
		total += nlines - 1;		/* separators between lines */
	bytes = malloc(total);
	if (!bytes) {
		set_status(e, "out of memory");
		text_free(nt);
		return REQ_CONTINUE;
	}
	bytes[0] = '\n';
	off = 1;
	for (i = 0; i < nlines; i++) {
		size_t ll = 0;
		const char *lp = text_line(nt, i, &ll);

		if (lp && ll) {
			memcpy(bytes + off, lp, ll);
			off += ll;
		}
		if (i + 1 < nlines)
			bytes[off++] = '\n';
	}

	if (at >= text_lines(e->t))
		at = text_lines(e->t) - 1;
	e->cy = at;
	e->cx = text_line_len(e->t, at);
	text_undo_group_begin(e->t);
	insert_bytes(e, bytes, off);
	text_undo_group_end(e->t);
	e->cy = at + 1 < text_lines(e->t) ? at + 1 : at;
	e->cx = first_nonblank(e, e->cy);
	e->hl_valid = 0;
	vi_clamp(e);

	set_status(e, "\"%.80s\" %zu line%s", fn,
	    nlines, nlines == 1 ? "" : "s");
	free(bytes);
	text_free(nt);
	return REQ_CONTINUE;
}

/* Apply a :set option (arg is past the "set" word and any spaces). vedit
 * accepts the vim abbreviations and the no/inv prefixes and ! suffix. */
static Req
ex_set(Editor *e, const char *arg)
{
	if (strcmp(arg, "number") == 0 || strcmp(arg, "nu") == 0)
		e->show_lineno = 1;
	else if (strcmp(arg, "nonumber") == 0 || strcmp(arg, "nonu") == 0)
		e->show_lineno = 0;
	else if (strcmp(arg, "number!") == 0 || strcmp(arg, "nu!") == 0 ||
	    strcmp(arg, "invnumber") == 0)
		e->show_lineno = !e->show_lineno;
	else if (strcmp(arg, "wrap") == 0)
		e->wrap = 1;
	else if (strcmp(arg, "nowrap") == 0)
		e->wrap = 0;
	else if (strcmp(arg, "wrap!") == 0 || strcmp(arg, "invwrap") == 0)
		e->wrap = !e->wrap;
	else if (strcmp(arg, "list") == 0 || strcmp(arg, "nolist") == 0 ||
	    strcmp(arg, "list!") == 0 || strcmp(arg, "invlist") == 0) {
		if (arg[0] == 'n')
			e->show_tabs = 0;
		else if (strchr(arg, '!') || arg[0] == 'i')
			e->show_tabs = !e->show_tabs;
		else
			e->show_tabs = 1;
		set_status(e, "show tabs %s",
		    e->show_tabs ? "on" : "off");
		return REQ_CONTINUE;
	} else if (strcmp(arg, "autoindent") == 0 || strcmp(arg, "ai") == 0 ||
	    strcmp(arg, "noautoindent") == 0 || strcmp(arg, "noai") == 0 ||
	    strcmp(arg, "autoindent!") == 0 || strcmp(arg, "ai!") == 0 ||
	    strcmp(arg, "invai") == 0) {
		if (arg[0] == 'n')
			e->auto_indent = 0;
		else if (strchr(arg, '!') || arg[0] == 'i')
			e->auto_indent = !e->auto_indent;
		else
			e->auto_indent = 1;
		set_status(e, "auto-indent %s",
		    e->auto_indent ? "on" : "off");
		return REQ_CONTINUE;
	} else if (strcmp(arg, "expandtab") == 0 || strcmp(arg, "et") == 0 ||
	    strcmp(arg, "noexpandtab") == 0 || strcmp(arg, "noet") == 0 ||
	    strcmp(arg, "expandtab!") == 0 || strcmp(arg, "et!") == 0 ||
	    strcmp(arg, "invet") == 0) {
		if (arg[0] == 'n')
			e->expand_tabs = 0;
		else if (strchr(arg, '!') || arg[0] == 'i')
			e->expand_tabs = !e->expand_tabs;
		else
			e->expand_tabs = 1;
		set_status(e, "indent with %s",
		    e->expand_tabs ? "spaces" : "tabs");
		return REQ_CONTINUE;
	} else if (strcmp(arg, "swapfile") == 0 || strcmp(arg, "swf") == 0 ||
	    strcmp(arg, "noswapfile") == 0 || strcmp(arg, "noswf") == 0 ||
	    strcmp(arg, "swapfile!") == 0 || strcmp(arg, "invswapfile") == 0) {
		if (arg[0] == 'n') {
			swap_remove(e);		/* turning it off drops the file */
			e->swap_enabled = 0;
		} else if (strchr(arg, '!') || arg[0] == 'i') {
			e->swap_enabled = !e->swap_enabled;
			if (!e->swap_enabled)
				swap_remove(e);
		} else
			e->swap_enabled = 1;
		set_status(e, "swap file %s",
		    e->swap_enabled ? "on" : "off");
		return REQ_CONTINUE;
	} else if (strcmp(arg, "backup") == 0 || strcmp(arg, "bk") == 0 ||
	    strcmp(arg, "nobackup") == 0 || strcmp(arg, "nobk") == 0 ||
	    strcmp(arg, "backup!") == 0 || strcmp(arg, "invbackup") == 0) {
		if (arg[0] == 'n')
			e->backup_enabled = 0;
		else if (strchr(arg, '!') || arg[0] == 'i')
			e->backup_enabled = !e->backup_enabled;
		else
			e->backup_enabled = 1;
		set_status(e, "backup %s",
		    e->backup_enabled ? "on" : "off");
		return REQ_CONTINUE;
	} else if (strcmp(arg, "ignorecase") == 0 || strcmp(arg, "ic") == 0 ||
	    strcmp(arg, "noignorecase") == 0 || strcmp(arg, "noic") == 0 ||
	    strcmp(arg, "ignorecase!") == 0 || strcmp(arg, "invignorecase") == 0) {
		/* "ignorecase" and "ic" both begin with 'i', so the usual
		 * arg[0] heuristic for inv/no does not apply; match exactly. */
		if (strcmp(arg, "noignorecase") == 0 || strcmp(arg, "noic") == 0)
			e->search_icase = 0;
		else if (strcmp(arg, "ignorecase!") == 0 ||
		    strcmp(arg, "invignorecase") == 0)
			e->search_icase = !e->search_icase;
		else
			e->search_icase = 1;
		set_status(e, "ignorecase %s",
		    e->search_icase ? "on" : "off");
		return REQ_CONTINUE;
	} else if (strncmp(arg, "shiftwidth=", 11) == 0 ||
	    strncmp(arg, "sw=", 3) == 0) {
		int v = atoi(strchr(arg, '=') + 1);

		if (v < 0 || v > 32) {
			set_status(e, "shiftwidth out of range (0-32)");
			return REQ_CONTINUE;
		}
		e->shiftwidth = v;
		set_status(e, "shiftwidth %d%s", v,
		    v == 0 ? " (one tab stop)" : "");
		return REQ_CONTINUE;
	} else if (strncmp(arg, "ff=", 3) == 0 ||
	    strncmp(arg, "fileformat=", 11) == 0) {
		const char *val = strchr(arg, '=') + 1;
		int eol;

		if (strcmp(val, "unix") == 0 || strcmp(val, "lf") == 0)
			eol = EOL_LF;
		else if (strcmp(val, "dos") == 0 || strcmp(val, "crlf") == 0)
			eol = EOL_CRLF;
		else if (strcmp(val, "nul") == 0)
			eol = EOL_NUL;
		else {
			set_status(e,
			    "E474: invalid fileformat: %.20s", val);
			return REQ_CONTINUE;
		}
		text_set_eol(e->t, eol);
		set_status(e, "fileformat=%s",
		    eol_name(eol));
		return REQ_CONTINUE;
	} else {
		set_status(e,
		    "E518: unknown option: %.40s", arg);
		return REQ_CONTINUE;
	}
	if (e->wrap)
		e->left = 0;
	if (strstr(arg, "wrap"))
		set_status(e, "word wrap %s",
		    e->wrap ? "on" : "off");
	else
		set_status(e, "line numbers %s",
		    e->show_lineno ? "on" : "off");
	return REQ_CONTINUE;
}

/* Apply :syntax on|off|<name> (arg is past the "syntax" word and any spaces). */
static Req
ex_syntax(Editor *e, const char *arg)
{
	if (strcmp(arg, "off") == 0) {
		e->hl_on = 0;
	} else if (*arg == '\0' || strcmp(arg, "on") == 0) {
		e->hl_on = 1;
		e->hl_valid = 0;		/* recolor from the top */
	} else {
		const Syntax *sy = syn_for_ext(arg);

		if (!sy) {
			set_status(e,
			    "no syntax for '%.40s'", arg);
			return REQ_CONTINUE;
		}
		e->syn = sy;
		e->hl_on = 1;
		e->hl_valid = 0;
	}
	return REQ_CONTINUE;
}

/* The ex command set. Each ex command may be abbreviated to any prefix of its
 * full name that is at least `min` characters long, the de-facto vi/ex rule
 * (so :s, :su .. :substitute all work, and :e .. :edit, :w .. :write). The
 * table is scanned in order and the first match wins; the `min` values are
 * chosen so the standard abbreviations never collide. */
enum excmd {
	EX_NONE, EX_SUBST, EX_GLOBAL, EX_VGLOBAL, EX_DELETE, EX_YANK, EX_READ,
	EX_EDIT, EX_ENEW, EX_WRITE, EX_WQ, EX_XIT, EX_QUIT, EX_QALL, EX_WQALL,
	EX_CQUIT, EX_SET, EX_SYNTAX, EX_LS, EX_BUFFER, EX_BNEXT, EX_BPREV,
	EX_BDELETE, EX_TAG, EX_POP, EX_RETAB, EX_DRAW, EX_RELOAD,
	EX_MARKS, EX_DELMARKS, EX_JUMPS,
};

static const struct excmd_name {
	const char	*name;
	int		 min;
	int		 id;
} ex_cmds[] = {
	{ "substitute",	1, EX_SUBST },
	{ "global",	1, EX_GLOBAL },
	{ "vglobal",	1, EX_VGLOBAL },
	{ "delete",	1, EX_DELETE },
	{ "yank",	1, EX_YANK },
	{ "read",	1, EX_READ },
	{ "edit",	1, EX_EDIT },
	{ "enew",	3, EX_ENEW },
	{ "write",	1, EX_WRITE },
	{ "wq",		2, EX_WQ },
	{ "wqall",	3, EX_WQALL },
	{ "xall",	2, EX_WQALL },
	{ "xit",	1, EX_XIT },
	{ "exit",	2, EX_XIT },
	{ "quit",	1, EX_QUIT },
	{ "qall",	2, EX_QALL },
	{ "quitall",	5, EX_QALL },
	{ "cquit",	2, EX_CQUIT },
	{ "set",	2, EX_SET },
	{ "syntax",	2, EX_SYNTAX },
	{ "buffers",	7, EX_LS },
	{ "files",	3, EX_LS },
	{ "ls",		2, EX_LS },
	{ "buffer",	1, EX_BUFFER },
	{ "bnext",	2, EX_BNEXT },
	{ "bprevious",	2, EX_BPREV },
	{ "bNext",	2, EX_BPREV },
	{ "bdelete",	2, EX_BDELETE },
	{ "tag",	2, EX_TAG },
	{ "pop",	2, EX_POP },
	{ "retab",	3, EX_RETAB },
	{ "draw",	2, EX_DRAW },
	{ "reload",	3, EX_RELOAD },
	{ "marks",	3, EX_MARKS },
	{ "delmarks",	4, EX_DELMARKS },
	{ "jumps",	2, EX_JUMPS },
};

/* Copy the leading run of ASCII letters of *pp into word[], advancing *pp past
 * them. Returns the stored length. */
static size_t
ex_word(const char **pp, char *word, size_t wsz)
{
	const char *p = *pp;
	size_t n = 0;

	while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
		if (n < wsz - 1)
			word[n++] = *p;
		p++;
	}
	word[n] = '\0';
	*pp = p;
	return n;
}

/* Resolve a command word to its id by the prefix-abbreviation rule. */
static int
ex_lookup(const char *word)
{
	size_t wl = strlen(word), i;

	if (wl == 0)
		return EX_NONE;
	for (i = 0; i < sizeof(ex_cmds) / sizeof(ex_cmds[0]); i++) {
		size_t nl = strlen(ex_cmds[i].name);

		if (wl >= (size_t)ex_cmds[i].min && wl <= nl &&
		    strncmp(word, ex_cmds[i].name, wl) == 0)
			return ex_cmds[i].id;
	}
	return EX_NONE;
}

/* Save the current buffer to e->path. Returns -1 and sets the status on error,
 * or 0 on success. */
static int
ex_write_current(Editor *e)
{
	if (!e->has_name) {
		set_status(e, "E32: no file name");
		return -1;
	}
	if (ed_save_file(e) < 0) {
		set_status(e, "save failed: %s",
		    strerror(errno));
		return -1;
	}
	return 0;
}

/* Run an already-entered ex command line. Returns REQ_FORCE_QUIT when the
 * command asks to leave, otherwise REQ_CONTINUE. Command names follow the
 * vi/ex abbreviation rule (see ex_cmds); a trailing '!' forces. Split from
 * vi_colon so it can run without the interactive prompt. */
static Req
vi_ex_exec(Editor *e, char *buf)
{
	char *p = buf, *after;
	size_t lo = 0, hi = 0;
	int rr, had_range, id, bang = 0, invert = 0;
	char word[32];
	const char *rest, *wp;

	while (*p == ' ')
		p++;

	if (*p == '!') {		/* :!cmd -- run a shell command */
		p++;
		while (*p == ' ')
			p++;
#ifndef VEDIT_NO_TOOLS
		ed_shell_cmd(e, p);
#else
		set_status(e, "shell commands are not available");
#endif
		return REQ_CONTINUE;
	}

	/* An optional leading line range. */
	after = p;
	rr = vi_ex_parse_range(e, &after, &lo, &hi);
	if (rr < 0) {
		set_status(e, "E16: invalid range");
		return REQ_CONTINUE;
	}
	while (*after == ' ')
		after++;
	had_range = rr > 0;

	if (had_range && *after == '\0') {		/* :N -- go to line */
		e->cy = hi;
		e->cx = first_nonblank(e, e->cy);
		vi_clamp(e);
		return REQ_CONTINUE;
	}
	if (!had_range)					/* default: current line */
		lo = hi = e->cy;

	if (*after == '\0')				/* a bare ':' does nothing */
		return REQ_CONTINUE;

	/* The non-word line-shift commands. */
	if (*after == '>' || *after == '<') {
		vi_shift_lines(e, lo, hi, *after == '>' ? 1 : -1);
		return REQ_CONTINUE;
	}

	/* The command word, then look it up by the abbreviation rule. */
	wp = after;
	ex_word(&wp, word, sizeof(word));
	rest = wp;
	id = ex_lookup(word);

	/* Substitute and global read their own delimiter, so the leading '!' of
	 * :g! is theirs, not the generic force flag. */
	if (id == EX_SUBST) {
		if (!is_ex_delim(*rest))
			goto unknown;
		return vi_ex_substitute(e, lo, hi, rest);
	}
	if (id == EX_GLOBAL || id == EX_VGLOBAL) {
		invert = (id == EX_VGLOBAL);
		if (id == EX_GLOBAL && *rest == '!') {
			invert = 1;
			rest++;
		}
		if (!is_ex_delim(*rest)) {
			set_status(e,
			    "E146: missing pattern");
			return REQ_CONTINUE;
		}
		return vi_ex_global(e, lo, hi, had_range, rest, invert);
	}

	/* A trailing '!' forces; then skip to the argument. */
	if (*rest == '!') {
		bang = 1;
		rest++;
	}
	while (*rest == ' ')
		rest++;

	switch (id) {
	case EX_DELETE:
		vi_yank_lines(e, lo, hi);
		vi_delete_lines(e, lo, hi);
		e->cy = lo < text_lines(e->t) ? lo : text_lines(e->t) - 1;
		e->cx = first_nonblank(e, e->cy);
		vi_clamp(e);
		return REQ_CONTINUE;
	case EX_YANK:
		vi_yank_lines(e, lo, hi);
		e->cy = lo;
		vi_clamp(e);
		return REQ_CONTINUE;
	case EX_READ:
		return vi_ex_read_file(e, hi, rest);
	case EX_EDIT:
		if (*rest) {			/* :e file -- open or switch */
			buf_open(e, rest);
			return REQ_CONTINUE;
		}
		if (!e->has_name) {		/* :e -- reload the current file */
			set_status(e,
			    "E32: no file name");
			return REQ_CONTINUE;
		}
		{
			Text *nt = text_new();

			if (!nt) {
				set_status(e,
				    "out of memory");
				return REQ_CONTINUE;
			}
			if (text_load(nt, e->path) < 0) {
				set_status(e,
				    "reload failed: %s", strerror(errno));
				text_free(nt);
				return REQ_CONTINUE;
			}
			text_free(e->t);
			e->t = nt;
			e->cy = e->cx = e->top = e->left = 0;
			e->sel_active = 0;
			e->hl_valid = 0;
			set_status(e, "reloaded %.100s",
			    e->path);
		}
		return REQ_CONTINUE;
	case EX_ENEW:
		buf_open(e, NULL);
		return REQ_CONTINUE;
	case EX_WRITE:
		if (*rest) {			/* :w file -- set the name */
			snprintf(e->path, sizeof(e->path), "%s", rest);
			e->has_name = 1;
		}
		if (ex_write_current(e) == 0)
			set_status(e, "wrote %.120s",
			    e->path);
		return REQ_CONTINUE;
	case EX_WQ:
	case EX_XIT:
		if (ex_write_current(e) != 0)
			return REQ_CONTINUE;
		return REQ_FORCE_QUIT;
	case EX_QUIT:
		if (!bang && text_dirty(e->t)) {
			set_status(e,
			    "E37: no write since last change (:q! overrides)");
			return REQ_CONTINUE;
		}
		return REQ_FORCE_QUIT;
	case EX_QALL:
		if (!bang && text_dirty(e->t)) {
			set_status(e,
			    "E37: no write since last change (add ! to override)");
			return REQ_CONTINUE;
		}
		return REQ_FORCE_QUIT;
	case EX_WQALL:
		if (ex_write_current(e) != 0)
			return REQ_CONTINUE;
		return REQ_FORCE_QUIT;
	case EX_CQUIT:
		return REQ_QUIT_ERR;		/* exit with a nonzero code */
	case EX_SET:
		return ex_set(e, rest);
	case EX_SYNTAX:
		return ex_syntax(e, rest);
	case EX_LS:
		buf_list(e);
		return REQ_CONTINUE;
	case EX_MARKS:
		dlg_marks_pick(e);
		return REQ_CONTINUE;
	case EX_DELMARKS:
		ex_delmarks(e, rest, bang);
		return REQ_CONTINUE;
	case EX_JUMPS:
		dlg_jumps_pick(e);
		return REQ_CONTINUE;
	case EX_BUFFER: {
		long n;

		if (*rest < '0' || *rest > '9') {
			set_status(e,
			    "E471: argument required");
			return REQ_CONTINUE;
		}
		n = strtol(rest, NULL, 10);
		if (n < 1 || n > e->nbuf)
			set_status(e,
			    "E86: no buffer %ld", n);
		else
			buf_switch(e, (int)(n - 1));
		return REQ_CONTINUE;
	}
	case EX_BNEXT:
		buf_cycle(e, 1);
		return REQ_CONTINUE;
	case EX_BPREV:
		buf_cycle(e, -1);
		return REQ_CONTINUE;
	case EX_BDELETE:
		if (!bang && text_dirty(e->t)) {
			set_status(e,
			    "E89: no write since last change (add ! to override)");
			return REQ_CONTINUE;
		}
		if (buf_close(e, e->cur) < 0)
			set_status(e,
			    "cannot close the last buffer");
		return REQ_CONTINUE;
	case EX_TAG:
		if (*rest == '/')
			symbol_pick_filtered(e, NULL, rest + 1, 0);
		else if (*rest)
			symbol_pick_filtered(e, rest, NULL, 1);
		else
			set_status(e,
			    "E471: argument required");
		return REQ_CONTINUE;
	case EX_POP:
		ed_tag_pop(e);
		return REQ_CONTINUE;
	case EX_RETAB:
		ed_retab(e, e->expand_tabs);
		return REQ_CONTINUE;
	case EX_DRAW:
		draw_toggle(e);
		return REQ_CONTINUE;
	case EX_RELOAD:
		ed_reload_config(e);
		return REQ_CONTINUE;
	default:
		break;
	}

unknown:
	set_status(e, "E492: not an editor command: "
	    "%.80s", after);
	return REQ_CONTINUE;
}

/* Prompt for a ':' ex command and run it. */
Req
vi_colon(Editor *e)
{
	char buf[PATH_MAX];

	buf[0] = '\0';
	if (!dlg_prompt_line(e, ":", buf, sizeof(buf)))
		return REQ_CONTINUE;
	return vi_ex_exec(e, buf);
}

/* Vi '/' (forward) and '?' (backward) incremental search. The direction was
 * stashed in vi_search_dir by the key that requested it. */
void
vi_search(Editor *e)
{
	int dir = e->vi_search_dir < 0 ? -1 : 1;

	incsearch(e, dir, dir < 0 ? "?" : "/");
}

