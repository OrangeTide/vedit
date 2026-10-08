/* guterm.h : graphical micro terminal, a single-header cell grid window */
/* vendored from guterm 44ed555 */

/*
 * guterm gives a text-based program a window: a grid of character cells
 * drawn with a bitmap font through SDL3 and OpenGL ES 2.0 class shaders.
 *
 * The primary interface is the cell buffer, struct gut_buf. A program
 * fills it with gut_buf_put() and friends, or lets the optional VT layer
 * fill it from a byte stream, then hands it to gut_present() to draw.
 * Input comes back as struct gut_event records from gut_poll(), and
 * gut_encode_event() turns those into xterm style key bytes for programs
 * that speak the terminal protocol.
 *
 * Usage, in exactly one C file:
 *
 *     #define GUTERM_IMPLEMENTATION
 *     #include "guterm.h"
 *
 * Every other file includes the header plainly. Configuration macros,
 * set before the implementation include:
 *
 *     GUTERM_NO_WINDOW   leave out SDL and the renderer; only the cell
 *                        buffer, font tables, key encoder and VT layer
 *                        remain, with no dependency beyond libc.
 *     GUTERM_NO_VT       leave out the VT escape sequence layer.
 *     GUT_API            linkage for the public functions (default extern).
 *
 * Linking needs SDL3 unless GUTERM_NO_WINDOW is set. OpenGL entry points
 * are resolved at run time through SDL, so no GL library or header is
 * needed at build time.
 */

#ifndef GUTERM_H
#define GUTERM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GUT_API
#define GUT_API extern
#endif

#define GUT_VERSION "0.1.0"

/****************************************************************
 * Cells
 ****************************************************************/

enum gut_attr {
    GUT_ATTR_BOLD      = 1 << 0,
    GUT_ATTR_UNDERLINE = 1 << 1,
    GUT_ATTR_REVERSE   = 1 << 2,
    GUT_ATTR_ITALIC    = 1 << 3,
    GUT_ATTR_BLINK     = 1 << 4,
    GUT_ATTR_DIM       = 1 << 5,
    GUT_ATTR_HIDDEN    = 1 << 6,
    GUT_ATTR_STRIKE    = 1 << 7,
};

enum gut_color_type {
    GUT_COLOR_DEFAULT,  /* the window's default foreground or background */
    GUT_COLOR_INDEXED,  /* 0-255 palette: 16 ANSI, 216 cube, 24 grays */
    GUT_COLOR_RGB,      /* 24-bit true color */
};

struct gut_color {
    uint8_t type;       /* enum gut_color_type */
    uint8_t index;      /* GUT_COLOR_INDEXED */
    uint8_t r, g, b;    /* GUT_COLOR_RGB */
};

/* A cell that continues a wide character to its left has width 0 and its
 * codepoint set to GUT_CELL_CONT. The renderer skips such cells. */
#define GUT_CELL_CONT 0xFFFEu

struct gut_cell {
    uint32_t cp;
    struct gut_color fg;
    struct gut_color bg;
    uint16_t attrs;     /* enum gut_attr bits */
    uint8_t width;      /* 1, 2 for the left half of a wide char, 0 cont */
};

GUT_API struct gut_color gut_color_default(void);
GUT_API struct gut_color gut_color_indexed(int index);
GUT_API struct gut_color gut_color_rgb(int r, int g, int b);
GUT_API int gut_color_equal(struct gut_color a, struct gut_color b);

/** Reset a cell to a blank space with the given background. Foreground
 * and attributes return to their defaults. */
GUT_API void gut_cell_erase(struct gut_cell *c, struct gut_color bg);

/****************************************************************
 * Cell buffer
 ****************************************************************/

enum gut_cursor_shape {
    GUT_CURSOR_BLOCK,
    GUT_CURSOR_UNDERLINE,
    GUT_CURSOR_BAR,
};

struct gut_buf {
    int rows, cols;
    struct gut_cell *cells;     /* rows * cols, row major */
    uint8_t *dirty;             /* one flag per row, set by every writer */
    int cursor_row, cursor_col;
    int cursor_visible;
    int cursor_shape;           /* enum gut_cursor_shape */
};

/** Allocate a rows by cols grid of blank cells. Returns 0 or -1. */
GUT_API int gut_buf_init(struct gut_buf *b, int rows, int cols);
GUT_API void gut_buf_free(struct gut_buf *b);

/** Change the grid size, keeping the overlapping top-left content. */
GUT_API int gut_buf_resize(struct gut_buf *b, int rows, int cols);

/** Cell at row, col or NULL when out of range. Callers that write through
 * this pointer must set b->dirty[row] themselves. */
GUT_API struct gut_cell *gut_buf_cell(struct gut_buf *b, int row, int col);

/** Erase every cell to a blank space with background bg. */
GUT_API void gut_buf_clear(struct gut_buf *b, struct gut_color bg);

/** Erase rows from (inclusive) to to (exclusive). */
GUT_API void gut_buf_clear_rows(struct gut_buf *b, int from, int to,
                                struct gut_color bg);

/** Write one codepoint at row, col. Wide characters take two cells and
 * are dropped when they would not fit. Returns the cells used, 0 when the
 * position is out of range. */
GUT_API int gut_buf_put(struct gut_buf *b, int row, int col, uint32_t cp,
                        struct gut_color fg, struct gut_color bg,
                        uint16_t attrs);

/** Write a UTF-8 string left to right from row, col, clipping at the right
 * edge. Control characters are skipped. Returns the cells used. */
GUT_API int gut_buf_text(struct gut_buf *b, int row, int col,
                         const char *utf8, struct gut_color fg,
                         struct gut_color bg, uint16_t attrs);

/** Fill a w by h rectangle with one codepoint. */
GUT_API void gut_buf_fill(struct gut_buf *b, int row, int col, int h, int w,
                          uint32_t cp, struct gut_color fg,
                          struct gut_color bg, uint16_t attrs);

/** Scroll rows top (inclusive) to bot (exclusive) by count lines. Positive
 * moves content up and exposes blank lines at the bottom, negative the
 * other way. Exposed lines take background bg. */
GUT_API void gut_buf_scroll(struct gut_buf *b, int top, int bot, int count,
                            struct gut_color bg);

GUT_API void gut_buf_dirty_all(struct gut_buf *b);

enum gut_copy_mode {
    GUT_COPY_STREAM,    /* reading order from start to end, inclusive */
    GUT_COPY_RECT,      /* the rectangle with those corners */
};

/** Turn a region of cells into UTF-8 text for the clipboard. Rows are
 * joined with newlines, trailing blanks are trimmed from each row, and
 * continuation cells are skipped. The corners may be given in either
 * order. Like snprintf, returns the length the full text needs and
 * writes at most n - 1 bytes plus a NUL; n may be 0 with out NULL. */
GUT_API size_t gut_buf_copy_text(const struct gut_buf *b, int row0, int col0,
                                 int row1, int col1, int mode, char *out,
                                 size_t n);

/****************************************************************
 * UTF-8 and character width
 ****************************************************************/

/** Decode one codepoint from up to len bytes. Returns the bytes consumed,
 * at least 1 for len > 0. Malformed input yields U+FFFD. */
GUT_API int gut_utf8_decode(uint32_t *cp, const unsigned char *s, size_t len);

/** Encode cp into buf (at least 4 bytes). Returns bytes written, 0 when
 * cp is not encodable. */
GUT_API int gut_utf8_encode(unsigned char *buf, uint32_t cp);

/** Display width of cp: 0 for combining marks, 2 for East Asian wide and
 * common emoji, -1 for control characters, else 1. */
GUT_API int gut_rune_width(uint32_t cp);

/****************************************************************
 * Fonts
 ****************************************************************/

/* A fixed-cell bitmap font. Glyph g occupies glyph_h rows of
 * (glyph_w + 7) / 8 bytes each starting at bits[g * glyph_h * stride];
 * bit 0 of the first byte is the leftmost column. cmap lists the
 * codepoint of each glyph in ascending order. */
struct gut_font {
    int glyph_w, glyph_h;
    int nglyphs;
    const uint8_t *bits;
    const uint32_t *cmap;
};

/** The built-in 8x16 face (unscii-16, public domain, 491 glyphs). */
GUT_API const struct gut_font *gut_font_default(void);

/** Glyph index of cp in font, or -1 when the font has no glyph for it. */
GUT_API int gut_font_lookup(const struct gut_font *font, uint32_t cp);

/****************************************************************
 * Events
 ****************************************************************/

enum gut_event_type {
    GUT_EVENT_NONE,
    GUT_EVENT_KEY,          /* key, mods, repeat */
    GUT_EVENT_TEXT,         /* text: committed UTF-8 */
    GUT_EVENT_RESIZE,       /* cols, rows: the grid now fits this many */
    GUT_EVENT_QUIT,         /* window close requested */
    GUT_EVENT_MOUSE_DOWN,   /* col, row, x, y, button, mods */
    GUT_EVENT_MOUSE_UP,
    GUT_EVENT_MOUSE_MOVE,
    GUT_EVENT_MOUSE_WHEEL,  /* dx, dy in notches */
    GUT_EVENT_FOCUS_IN,
    GUT_EVENT_FOCUS_OUT,
    GUT_EVENT_COMPOSE,      /* data, len, cursor: IME text in progress */
    GUT_EVENT_PASTE,        /* data, len, primary; col, row for primary */
    GUT_EVENT_PAD_ADDED,    /* pad: a game controller was connected */
    GUT_EVENT_PAD_REMOVED,  /* pad */
    GUT_EVENT_PAD_DOWN,     /* pad, button: enum gut_pad_button */
    GUT_EVENT_PAD_UP,
    GUT_EVENT_PAD_AXIS,     /* pad, axis: enum gut_pad_axis, value */
};

enum gut_mod {
    GUT_MOD_SHIFT = 1 << 0,
    GUT_MOD_CTRL  = 1 << 1,
    GUT_MOD_ALT   = 1 << 2,
    GUT_MOD_SUPER = 1 << 3,
};

/* Key codes. Values below GUT_KEY_SPECIAL are Unicode codepoints for the
 * key's unshifted printable symbol. Letters arrive lowercase. */
enum gut_key {
    GUT_KEY_NONE = 0,
    GUT_KEY_SPECIAL = 0x110000,
    GUT_KEY_UP,
    GUT_KEY_DOWN,
    GUT_KEY_LEFT,
    GUT_KEY_RIGHT,
    GUT_KEY_HOME,
    GUT_KEY_END,
    GUT_KEY_PAGEUP,
    GUT_KEY_PAGEDOWN,
    GUT_KEY_INSERT,
    GUT_KEY_DELETE,
    GUT_KEY_BACKSPACE,
    GUT_KEY_TAB,
    GUT_KEY_ENTER,
    GUT_KEY_ESCAPE,
    GUT_KEY_F1,
    GUT_KEY_F2,
    GUT_KEY_F3,
    GUT_KEY_F4,
    GUT_KEY_F5,
    GUT_KEY_F6,
    GUT_KEY_F7,
    GUT_KEY_F8,
    GUT_KEY_F9,
    GUT_KEY_F10,
    GUT_KEY_F11,
    GUT_KEY_F12,
};

enum gut_button {
    GUT_BUTTON_LEFT = 1,
    GUT_BUTTON_MIDDLE = 2,
    GUT_BUTTON_RIGHT = 3,
};

/* Game controller buttons, in the Xbox layout SDL maps every pad to. */
enum gut_pad_button {
    GUT_PAD_A,              /* south */
    GUT_PAD_B,              /* east */
    GUT_PAD_X,              /* west */
    GUT_PAD_Y,              /* north */
    GUT_PAD_BACK,
    GUT_PAD_GUIDE,
    GUT_PAD_START,
    GUT_PAD_LSTICK,
    GUT_PAD_RSTICK,
    GUT_PAD_LSHOULDER,
    GUT_PAD_RSHOULDER,
    GUT_PAD_UP,
    GUT_PAD_DOWN,
    GUT_PAD_LEFT,
    GUT_PAD_RIGHT,
    GUT_PAD_BUTTON_COUNT,
};

enum gut_pad_axis {
    GUT_PAD_AXIS_LX,        /* sticks: -32768 left or up to 32767 */
    GUT_PAD_AXIS_LY,
    GUT_PAD_AXIS_RX,
    GUT_PAD_AXIS_RY,
    GUT_PAD_AXIS_LT,        /* triggers: 0 released to 32767 */
    GUT_PAD_AXIS_RT,
    GUT_PAD_AXIS_COUNT,
};

#define GUT_MAX_PADS 4

struct gut_event {
    int type;           /* enum gut_event_type */
    int key;            /* enum gut_key or codepoint */
    int mods;           /* enum gut_mod bits */
    int repeat;         /* key auto-repeat */
    char text[32];      /* GUT_EVENT_TEXT, NUL terminated, may truncate */
    const char *data;   /* TEXT, COMPOSE, PASTE: full text, NUL terminated,
                           owned by the window until the next gut_poll() */
    size_t len;         /* bytes in data */
    int cursor;         /* COMPOSE: caret position in codepoints, or -1 */
    int primary;        /* PASTE: 1 from the primary selection */
    int col, row;       /* mouse position in cells */
    int x, y;           /* mouse position in pixels */
    int button;         /* enum gut_button, or enum gut_pad_button */
    int clicks;         /* MOUSE_DOWN: 1 single, 2 double, 3 triple */
    int dx, dy;         /* wheel notches */
    int cols, rows;     /* GUT_EVENT_RESIZE */
    int pad;            /* PAD events: controller slot 0 to 3 */
    int axis;           /* PAD_AXIS: enum gut_pad_axis */
    int value;          /* PAD_AXIS: -32768 to 32767 */
};

/* gut_encode_event flags */
#define GUT_ENC_APP_CURSOR    (1 << 0)  /* DECCKM: arrows send SS3 */
#define GUT_ENC_BS_DEL        (1 << 1)  /* backspace sends DEL (default) */
#define GUT_ENC_BS_BS         (1 << 2)  /* backspace sends BS */
#define GUT_ENC_BRACKET_PASTE (1 << 3)  /* wrap pastes in CSI 200~ 201~ */
#define GUT_ENC_MOUSE_BTN     (1 << 4)  /* mode 1000: presses and releases */
#define GUT_ENC_MOUSE_DRAG    (1 << 5)  /* mode 1002: also motion with a
                                           button held */
#define GUT_ENC_MOUSE_ANY     (1 << 6)  /* mode 1003: all motion */
#define GUT_ENC_MOUSE_SGR     (1 << 7)  /* mode 1006: CSI < b;x;y M form */
#define GUT_ENC_FOCUS         (1 << 8)  /* mode 1004: CSI I and CSI O */

/** Translate an event into the bytes an xterm would send a program.
 * GUT_EVENT_TEXT copies the text. GUT_EVENT_KEY encodes special keys and
 * Ctrl or Alt combinations; a plain printable key yields nothing because
 * the matching GUT_EVENT_TEXT carries it. GUT_EVENT_PASTE copies the
 * pasted text with newlines turned into carriage returns, bracketed when
 * the flag asks for it. Mouse events become xterm mouse reports under
 * the GUT_ENC_MOUSE_* flags and focus events become CSI I and CSI O
 * under GUT_ENC_FOCUS. Like snprintf, returns the length the full
 * encoding needs and writes at most n - 1 bytes plus a NUL. 32 bytes
 * cover every key and mouse report; a paste needs ev->len + 16. */
GUT_API size_t gut_encode_event(const struct gut_event *ev, char *out,
                                size_t n, int flags);

/****************************************************************
 * Selection
 *
 * Tracks a mouse driven selection over a buffer. gut_sel_mouse()
 * consumes mouse events: left press starts a selection, a double click
 * selects words and a triple click lines, dragging extends, Shift+click
 * extends the existing selection, and Alt makes it a rectangle. The
 * selection is in buffer cells and goes stale when the buffer scrolls;
 * clear it then. The window highlights it through gut_set_selection().
 ****************************************************************/

enum gut_sel_unit {
    GUT_SEL_CELL,
    GUT_SEL_WORD,
    GUT_SEL_LINE,
};

struct gut_sel {
    int active;             /* a selection exists */
    int dragging;           /* the button is still held */
    int mode;               /* enum gut_copy_mode */
    int unit;               /* enum gut_sel_unit */
    int anchor_row, anchor_col;
    int row0, col0;         /* normalised: start, inclusive */
    int row1, col1;         /* end, inclusive */
};

GUT_API void gut_sel_clear(struct gut_sel *s);

/** Start a selection at a cell. */
GUT_API void gut_sel_begin(struct gut_sel *s, const struct gut_buf *b,
                           int row, int col, int mode, int unit);

/** Move the far end of the selection to a cell; the unit snaps both
 * ends outward. */
GUT_API void gut_sel_extend(struct gut_sel *s, const struct gut_buf *b,
                            int row, int col);

/** Interpret a mouse event. Returns 1 when the selection changed, so the
 * caller knows to present and, on release, to publish the text. */
GUT_API int gut_sel_mouse(struct gut_sel *s, const struct gut_buf *b,
                          const struct gut_event *ev);

GUT_API int gut_sel_contains(const struct gut_sel *s, int row, int col);

/** The selected text, with gut_buf_copy_text() sizing. 0 when there is
 * no selection. */
GUT_API size_t gut_sel_text(const struct gut_sel *s, const struct gut_buf *b,
                            char *out, size_t n);

/****************************************************************
 * Window
 ****************************************************************/

#ifndef GUTERM_NO_WINDOW

struct gut_desc {
    const char *title;
    int cols, rows;                 /* initial grid, default 80 x 25 */
    int scale;                      /* integer pixel zoom; 0 picks 1, or
                                       the display content scale on a
                                       high density display */
    const struct gut_font *font;    /* NULL for the built-in 8x16 */
    uint32_t fg, bg;                /* 0xRRGGBB defaults; 0 means unset */
    const uint32_t *palette;        /* 16 ANSI colors 0xRRGGBB or NULL */
    int fixed_size;                 /* 1 disables window resizing */
    int no_paste_keys;              /* 1 delivers paste chords as keys */
    int no_compose_overlay;         /* 1 leaves IME preedit drawing to
                                       the program */
    int no_gamepad;                 /* 1 skips game controller support */
};

typedef struct gut_window gut_window;

/** Open a window sized for the grid. Returns NULL on failure, with the
 * reason available from gut_error(). */
GUT_API gut_window *gut_open(const struct gut_desc *desc);
GUT_API void gut_close(gut_window *w);

/** Draw the buffer and swap. Clears every dirty flag in b. */
GUT_API void gut_present(gut_window *w, struct gut_buf *b);

/** Wait up to timeout_ms (negative waits forever, 0 does not wait) for
 * an event. Returns 1 with ev filled, or 0 when the timeout passed. */
GUT_API int gut_poll(gut_window *w, struct gut_event *ev, int timeout_ms);

/** Grid cells that fit in the window at its current size. */
GUT_API void gut_grid_size(const gut_window *w, int *cols, int *rows);

/** Resize the window so the grid is exactly cols by rows. */
GUT_API void gut_set_grid_size(gut_window *w, int cols, int rows);

GUT_API void gut_set_title(gut_window *w, const char *title);
GUT_API void gut_set_defaults(gut_window *w, uint32_t fg, uint32_t bg);
GUT_API void gut_set_palette(gut_window *w, const uint32_t *palette16);

/** Clipboard text, owned by the window until the next call, or NULL. */
GUT_API const char *gut_clipboard_get(gut_window *w);
GUT_API void gut_clipboard_set(gut_window *w, const char *utf8);

/** The primary selection (middle click paste on X11 and Wayland). On
 * other platforms get returns NULL and set does nothing. */
GUT_API const char *gut_primary_get(gut_window *w);
GUT_API void gut_primary_set(gut_window *w, const char *utf8);

/* Paste chords recognised by gut_poll() unless gut_desc.no_paste_keys is
 * set: Shift+Insert everywhere, Ctrl+Shift+V on Linux and Windows,
 * Cmd+V on macOS, and middle click for the primary selection. Ctrl+V
 * alone stays a key, since terminal programs use it. */

/** Turn system text input on or off. On by default; off makes the
 * platform stop composing text and hides any on-screen keyboard, so only
 * GUT_EVENT_KEY events arrive. */
GUT_API void gut_set_text_input(gut_window *w, int on);

/** Draw the in-progress IME text over the cursor cell on the next
 * gut_present(). On by default; a program that renders the preedit
 * itself from GUT_EVENT_COMPOSE turns it off. */
GUT_API void gut_set_compose_overlay(gut_window *w, int on);

/** Highlight a selection on the next gut_present(), drawn with the
 * cell colors swapped. The struct is copied; NULL clears it. */
GUT_API void gut_set_selection(gut_window *w, const struct gut_sel *sel);

/* Game controllers. Up to GUT_MAX_PADS are tracked in slots 0 to 3,
 * assigned in connection order; a slot is reused after its pad leaves.
 * Changes arrive as GUT_EVENT_PAD_* events and the whole state can be
 * polled at any time. */
struct gut_pad {
    int connected;
    char name[64];
    uint32_t buttons;       /* bit (1 << gut_pad_button) per held button */
    int16_t axes[GUT_PAD_AXIS_COUNT];
};

/** Copy the state of a slot. Returns 1 when a pad is connected there. */
GUT_API int gut_pad_get(const gut_window *w, int slot, struct gut_pad *out);

/** Vibrate for ms milliseconds, intensities 0 to 65535. Returns 0, or
 * -1 when the slot is empty or the pad cannot rumble. */
GUT_API int gut_pad_rumble(gut_window *w, int slot, uint16_t low,
                           uint16_t high, uint32_t ms);

/** Milliseconds since gut_open(). */
GUT_API uint64_t gut_ticks(const gut_window *w);

/** Last failure message from gut_open(), or an empty string. */
GUT_API const char *gut_error(void);

#endif /* GUTERM_NO_WINDOW */

/****************************************************************
 * VT layer
 ****************************************************************/

#ifndef GUTERM_NO_VT

/* Terminal modes, readable through gut_vt_modes(). */
#define GUT_VT_MODE_AUTOWRAP     (1u << 0)
#define GUT_VT_MODE_ORIGIN       (1u << 1)
#define GUT_VT_MODE_INSERT       (1u << 2)
#define GUT_VT_MODE_ALTSCREEN    (1u << 3)
#define GUT_VT_MODE_BRACKETPASTE (1u << 4)
#define GUT_VT_MODE_APP_CURSOR   (1u << 5)
#define GUT_VT_MODE_APP_KEYPAD   (1u << 6)
#define GUT_VT_MODE_MOUSE        (1u << 7)
#define GUT_VT_MODE_MOUSE_SGR    (1u << 8)
#define GUT_VT_MODE_FOCUS        (1u << 9)

#define GUT_VT_MAX_PARAMS 16
#define GUT_VT_OSC_MAX    (1 << 20)   /* longest OSC string kept */

/* OSC 52 selections */
#define GUT_CLIP_CLIPBOARD 0
#define GUT_CLIP_PRIMARY   1
#define GUT_VT_SCROLLBACK_DEFAULT 1000

struct gut_vt_saved {
    int row, col;
    uint16_t attrs;
    struct gut_color fg, bg;
};

/* One scrollback line: the cells up to the last non-blank one. */
struct gut_vt_line {
    struct gut_cell *cells;
    int n;
};

struct gut_vt {
    struct gut_buf *buf;        /* where the emulator writes */
    struct gut_buf *out;        /* the caller's buffer */

    /* emulation state */
    int row, col;               /* cursor */
    int wrap_pending;
    struct gut_vt_saved saved;
    int scroll_top, scroll_bot; /* inclusive top, exclusive bottom */
    unsigned modes;
    int mouse_mode;             /* 0, 1000, 1002 or 1003 */
    int mouse_row, mouse_col;   /* last cell reported for motion */
    uint16_t attrs;
    struct gut_color fg, bg;
    int charset;                /* 0 = G0, 1 = G1 */
    int g_set[2];               /* 0 = ASCII, 1 = DEC line drawing */
    uint8_t *tabstops;          /* one per column */

    /* alternate screen: the primary content parked while it is active */
    struct gut_cell *alt_saved;
    int alt_rows, alt_cols;
    struct gut_vt_saved alt_cursor;

    /* scrollback: a ring of lines that left the top of the primary
     * screen, oldest at sb_head */
    struct gut_vt_line *sb;
    int sb_cap;                 /* ring capacity, 0 disables */
    int sb_head;
    int sb_len;
    int view;                   /* lines scrolled back into view */
    struct gut_buf live;        /* the screen while view > 0 */

    /* parser */
    int state;
    int params[GUT_VT_MAX_PARAMS];
    int nparam;
    int cur_param;
    int has_digit;
    int intermed;
    char *osc;                  /* grows up to GUT_VT_OSC_MAX */
    size_t osc_len, osc_cap;
    int osc_overflow;           /* string too long: dropped at the end */
    unsigned char utf8_buf[4];
    int utf8_len, utf8_need;

    /* callbacks */
    void (*reply)(void *ctx, const char *data, size_t len);
    void *reply_ctx;
    void (*title)(void *ctx, const char *title);
    void *title_ctx;
    void (*bell)(void *ctx);
    void *bell_ctx;
    void (*clip_set)(void *ctx, int which, const char *text, size_t len);
    const char *(*clip_get)(void *ctx, int which);
    void *clip_ctx;
};

/** Attach the emulator to buf and reset it. Returns 0 or -1. */
GUT_API int gut_vt_init(struct gut_vt *vt, struct gut_buf *buf);
GUT_API void gut_vt_free(struct gut_vt *vt);

/** Full reset (RIS): modes, cursor, tabs, attributes, and a cleared
 * screen. */
GUT_API void gut_vt_reset(struct gut_vt *vt);

/** Interpret a chunk of program output into the buffer. */
GUT_API void gut_vt_feed(struct gut_vt *vt, const char *data, size_t len);

/** Resize the buffer and keep the emulation state consistent. */
GUT_API int gut_vt_resize(struct gut_vt *vt, int rows, int cols);

GUT_API unsigned gut_vt_modes(const struct gut_vt *vt);

/** gut_encode_event() flags matching the current modes. */
GUT_API int gut_vt_encode_flags(const struct gut_vt *vt);

/** Encode a mouse or focus event for the program when it asked for
 * reports, with the xterm conventions a host would otherwise have to
 * know: Shift bypasses tracking so the host can select, motion is
 * reported only when the cell changes, and the wheel on the alternate
 * screen with no tracking becomes three cursor key presses. Returns
 * the length with gut_encode_event() sizing, or 0 when the event is the
 * host's to use. */
GUT_API size_t gut_vt_mouse(struct gut_vt *vt, const struct gut_event *ev,
                            char *out, size_t n);

/** Set how many lines the scrollback keeps, 0 to disable. The newest
 * lines survive a shrink. Returns 0 or -1. */
GUT_API int gut_vt_set_scrollback(struct gut_vt *vt, int lines);

/** Lines currently held in the scrollback. */
GUT_API int gut_vt_scrollback_lines(const struct gut_vt *vt);

/** Discard the scrollback, as ED 3 does. */
GUT_API void gut_vt_clear_scrollback(struct gut_vt *vt);

/** Show the screen scrolled back by offset lines, 0 for the live screen.
 * The offset is clamped to what the scrollback holds and to 0 on the
 * alternate screen. Returns the offset in effect. While scrolled back
 * the caller's buffer shows scrollback lines above the top of the live
 * screen with the cursor hidden, and output keeps arriving behind the
 * view, which stays on the same lines until it is moved. */
GUT_API int gut_vt_set_view(struct gut_vt *vt, int offset);

/** Move the view by delta lines, positive is further back. Returns the
 * offset in effect. */
GUT_API int gut_vt_scroll_view(struct gut_vt *vt, int delta);

GUT_API int gut_vt_view_offset(const struct gut_vt *vt);

/** Receiver for answers the program expects, such as DSR and DA. */
GUT_API void gut_vt_set_reply(struct gut_vt *vt,
                              void (*fn)(void *ctx, const char *data,
                                         size_t len),
                              void *ctx);
GUT_API void gut_vt_set_title_cb(struct gut_vt *vt,
                                 void (*fn)(void *ctx, const char *title),
                                 void *ctx);
GUT_API void gut_vt_set_bell_cb(struct gut_vt *vt, void (*fn)(void *ctx),
                                void *ctx);

/** OSC 52, the program's access to the clipboard. set receives the
 * decoded text, NUL terminated, for GUT_CLIP_CLIPBOARD or
 * GUT_CLIP_PRIMARY; an empty text means clear. get answers a query
 * with the current text, or NULL for none, valid until it returns; the
 * reply goes through the reply callback. Either may be NULL: with no
 * get, queries are ignored, which is the safe default since a query
 * lets the program read what the user copied elsewhere. */
GUT_API void gut_vt_set_clipboard_cb(struct gut_vt *vt,
                                     void (*set)(void *ctx, int which,
                                                 const char *text,
                                                 size_t len),
                                     const char *(*get)(void *ctx,
                                                        int which),
                                     void *ctx);

#endif /* GUTERM_NO_VT */

#ifdef __cplusplus
}
#endif

#endif /* GUTERM_H */

/****************************************************************
 * Implementation
 ****************************************************************/

#ifdef GUTERM_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void gut_buf_repair_row(struct gut_buf *b, int row);

/****************************************************************
 * Colors and cells
 ****************************************************************/

struct gut_color
gut_color_default(void)
{
    struct gut_color c = { GUT_COLOR_DEFAULT, 0, 0, 0, 0 };

    return c;
}

struct gut_color
gut_color_indexed(int index)
{
    struct gut_color c = { GUT_COLOR_INDEXED, 0, 0, 0, 0 };

    c.index = (uint8_t)(index & 0xFF);
    return c;
}

struct gut_color
gut_color_rgb(int r, int g, int b)
{
    struct gut_color c = { GUT_COLOR_RGB, 0, 0, 0, 0 };

    c.r = (uint8_t)r;
    c.g = (uint8_t)g;
    c.b = (uint8_t)b;
    return c;
}

int
gut_color_equal(struct gut_color a, struct gut_color b)
{
    if (a.type != b.type)
        return 0;
    switch (a.type) {
    case GUT_COLOR_INDEXED:
        return a.index == b.index;
    case GUT_COLOR_RGB:
        return a.r == b.r && a.g == b.g && a.b == b.b;
    default:
        return 1;
    }
}

void
gut_cell_erase(struct gut_cell *c, struct gut_color bg)
{
    c->cp = ' ';
    c->fg = gut_color_default();
    c->bg = bg;
    c->attrs = 0;
    c->width = 1;
}

/****************************************************************
 * UTF-8
 ****************************************************************/

#define GUT_RUNE_ERROR 0xFFFDu
#define GUT_RUNE_MAX   0x10FFFFu

int
gut_utf8_decode(uint32_t *cp, const unsigned char *s, size_t len)
{
    uint32_t v;
    int need;

    if (len == 0) {
        *cp = GUT_RUNE_ERROR;
        return 0;
    }
    if (s[0] < 0x80) {
        *cp = s[0];
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
        *cp = GUT_RUNE_ERROR;
        return 1;
    }
    if ((size_t)need > len) {
        *cp = GUT_RUNE_ERROR;
        return 1;
    }
    for (int i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = GUT_RUNE_ERROR;
            return 1;
        }
        v = (v << 6) | (s[i] & 0x3F);
    }
    if ((need == 2 && v < 0x80) || (need == 3 && v < 0x800) ||
        (need == 4 && v < 0x10000) || (v >= 0xD800 && v <= 0xDFFF) ||
        v > GUT_RUNE_MAX) {
        *cp = GUT_RUNE_ERROR;
        return 1;
    }
    *cp = v;
    return need;
}

int
gut_utf8_encode(unsigned char *buf, uint32_t cp)
{
    if (cp <= 0x7F) {
        buf[0] = (unsigned char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        buf[0] = (unsigned char)(0xC0 | (cp >> 6));
        buf[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;
        buf[0] = (unsigned char)(0xE0 | (cp >> 12));
        buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= GUT_RUNE_MAX) {
        buf[0] = (unsigned char)(0xF0 | (cp >> 18));
        buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (unsigned char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/****************************************************************
 * Character width. A compact wcwidth: combining marks are zero,
 * the common CJK and emoji ranges are two, everything else one.
 ****************************************************************/

struct gut_wrange {
    uint32_t lo, hi;
};

static int
gut_in_ranges(uint32_t cp, const struct gut_wrange *r, size_t n)
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

static const struct gut_wrange gut_zero_width[] = {
    { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD },
    { 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 },
    { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 }, { 0x0901, 0x0903 },
    { 0x093C, 0x093C }, { 0x0941, 0x0948 }, { 0x094D, 0x094D },
    { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0EB1, 0x0EB1 },
    { 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x200B, 0x200F },
    { 0x20D0, 0x20FF }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
};

static const struct gut_wrange gut_wide[] = {
    { 0x1100, 0x115F }, { 0x2329, 0x232A }, { 0x2E80, 0x303E },
    { 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF },
    { 0xA000, 0xA4CF }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF },
    { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 },
    { 0xFFE0, 0xFFE6 }, { 0x1F300, 0x1F64F }, { 0x1F900, 0x1F9FF },
    { 0x20000, 0x3FFFD },
};

int
gut_rune_width(uint32_t cp)
{
    if (cp == 0)
        return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return -1;
    if (gut_in_ranges(cp, gut_zero_width,
                      sizeof(gut_zero_width) / sizeof(gut_zero_width[0])))
        return 0;
    if (gut_in_ranges(cp, gut_wide, sizeof(gut_wide) / sizeof(gut_wide[0])))
        return 2;
    return 1;
}

/****************************************************************
 * Cell buffer
 ****************************************************************/

int
gut_buf_init(struct gut_buf *b, int rows, int cols)
{
    memset(b, 0, sizeof(*b));
    if (rows < 1 || cols < 1)
        return -1;
    b->cells = calloc((size_t)rows * (size_t)cols, sizeof(*b->cells));
    b->dirty = calloc((size_t)rows, 1);
    if (!b->cells || !b->dirty) {
        gut_buf_free(b);
        return -1;
    }
    b->rows = rows;
    b->cols = cols;
    b->cursor_visible = 1;
    gut_buf_clear(b, gut_color_default());
    return 0;
}

void
gut_buf_free(struct gut_buf *b)
{
    free(b->cells);
    free(b->dirty);
    b->cells = NULL;
    b->dirty = NULL;
    b->rows = 0;
    b->cols = 0;
}

int
gut_buf_resize(struct gut_buf *b, int rows, int cols)
{
    struct gut_cell *cells;
    uint8_t *dirty;
    int keep_rows, keep_cols;

    if (rows < 1 || cols < 1)
        return -1;
    if (rows == b->rows && cols == b->cols)
        return 0;
    cells = calloc((size_t)rows * (size_t)cols, sizeof(*cells));
    dirty = malloc((size_t)rows);
    if (!cells || !dirty) {
        free(cells);
        free(dirty);
        return -1;
    }
    for (int i = 0; i < rows * cols; i++)
        gut_cell_erase(&cells[i], gut_color_default());
    keep_rows = rows < b->rows ? rows : b->rows;
    keep_cols = cols < b->cols ? cols : b->cols;
    for (int r = 0; r < keep_rows; r++)
        memcpy(&cells[r * cols], &b->cells[r * b->cols],
               (size_t)keep_cols * sizeof(*cells));
    memset(dirty, 1, (size_t)rows);
    free(b->cells);
    free(b->dirty);
    b->cells = cells;
    b->dirty = dirty;
    b->rows = rows;
    b->cols = cols;
    /* a wide character cut in half at the new right edge */
    for (int r = 0; r < keep_rows; r++)
        gut_buf_repair_row(b, r);
    if (b->cursor_row >= rows)
        b->cursor_row = rows - 1;
    if (b->cursor_col >= cols)
        b->cursor_col = cols - 1;
    return 0;
}

struct gut_cell *
gut_buf_cell(struct gut_buf *b, int row, int col)
{
    if (row < 0 || row >= b->rows || col < 0 || col >= b->cols)
        return NULL;
    return &b->cells[row * b->cols + col];
}

void
gut_buf_clear(struct gut_buf *b, struct gut_color bg)
{
    gut_buf_clear_rows(b, 0, b->rows, bg);
}

void
gut_buf_clear_rows(struct gut_buf *b, int from, int to, struct gut_color bg)
{
    if (from < 0)
        from = 0;
    if (to > b->rows)
        to = b->rows;
    for (int r = from; r < to; r++) {
        for (int c = 0; c < b->cols; c++)
            gut_cell_erase(&b->cells[r * b->cols + c], bg);
        b->dirty[r] = 1;
    }
}

/* Clear the partner cell when writing over half of a wide character so no
 * orphaned halves remain. */
static void
gut_buf_unwide(struct gut_buf *b, int row, int col)
{
    struct gut_cell *c = &b->cells[row * b->cols + col];

    if (c->width == 0 && col > 0) {
        struct gut_cell *left = c - 1;

        if (left->width == 2)
            gut_cell_erase(left, left->bg);
    } else if (c->width == 2 && col + 1 < b->cols) {
        struct gut_cell *right = c + 1;

        if (right->width == 0)
            gut_cell_erase(right, right->bg);
    }
}

/* Restore the wide character pairing of a row after cells were shifted:
 * a wide cell must be followed by its continuation and a continuation
 * must follow a wide cell, else the leftover half becomes a blank. */
static void
gut_buf_repair_row(struct gut_buf *b, int row)
{
    struct gut_cell *r = &b->cells[row * b->cols];

    for (int c = 0; c < b->cols; c++) {
        if (r[c].width == 2 &&
            (c + 1 >= b->cols || r[c + 1].width != 0))
            gut_cell_erase(&r[c], r[c].bg);
        else if (r[c].width == 0 && (c == 0 || r[c - 1].width != 2))
            gut_cell_erase(&r[c], r[c].bg);
    }
}

int
gut_buf_put(struct gut_buf *b, int row, int col, uint32_t cp,
            struct gut_color fg, struct gut_color bg, uint16_t attrs)
{
    struct gut_cell *c;
    int w;

    if (row < 0 || row >= b->rows || col < 0 || col >= b->cols)
        return 0;
    w = gut_rune_width(cp);
    if (w <= 0)
        w = 1;
    if (w == 2 && col + 1 >= b->cols)
        return 0;
    gut_buf_unwide(b, row, col);
    c = &b->cells[row * b->cols + col];
    c->cp = cp;
    c->fg = fg;
    c->bg = bg;
    c->attrs = attrs;
    c->width = (uint8_t)w;
    if (w == 2) {
        gut_buf_unwide(b, row, col + 1);
        c[1].cp = GUT_CELL_CONT;
        c[1].fg = fg;
        c[1].bg = bg;
        c[1].attrs = attrs;
        c[1].width = 0;
    }
    b->dirty[row] = 1;
    return w;
}

int
gut_buf_text(struct gut_buf *b, int row, int col, const char *utf8,
             struct gut_color fg, struct gut_color bg, uint16_t attrs)
{
    const unsigned char *s = (const unsigned char *)utf8;
    size_t len = strlen(utf8);
    int used = 0;

    while (len > 0 && col < b->cols) {
        uint32_t cp;
        int n = gut_utf8_decode(&cp, s, len);
        int w;

        s += n;
        len -= (size_t)n;
        if (gut_rune_width(cp) <= 0)
            continue;
        w = gut_buf_put(b, row, col, cp, fg, bg, attrs);
        if (w == 0)
            break;
        col += w;
        used += w;
    }
    return used;
}

void
gut_buf_fill(struct gut_buf *b, int row, int col, int h, int w, uint32_t cp,
             struct gut_color fg, struct gut_color bg, uint16_t attrs)
{
    for (int r = row; r < row + h; r++)
        for (int c = col; c < col + w; c++)
            gut_buf_put(b, r, c, cp, fg, bg, attrs);
}

void
gut_buf_scroll(struct gut_buf *b, int top, int bot, int count,
               struct gut_color bg)
{
    int n;

    if (top < 0)
        top = 0;
    if (bot > b->rows)
        bot = b->rows;
    n = bot - top;
    if (n <= 0 || count == 0)
        return;
    if (count >= n || -count >= n) {
        gut_buf_clear_rows(b, top, bot, bg);
        return;
    }
    if (count > 0) {
        memmove(&b->cells[top * b->cols], &b->cells[(top + count) * b->cols],
                (size_t)(n - count) * (size_t)b->cols * sizeof(*b->cells));
        gut_buf_clear_rows(b, bot - count, bot, bg);
    } else {
        count = -count;
        memmove(&b->cells[(top + count) * b->cols], &b->cells[top * b->cols],
                (size_t)(n - count) * (size_t)b->cols * sizeof(*b->cells));
        gut_buf_clear_rows(b, top, top + count, bg);
    }
    memset(&b->dirty[top], 1, (size_t)n);
}

void
gut_buf_dirty_all(struct gut_buf *b)
{
    memset(b->dirty, 1, (size_t)b->rows);
}

/* snprintf style sink: counts everything, stores what fits */
struct gut_sink {
    char *out;
    size_t cap;
    size_t len;
};

static void
gut_sink_put(struct gut_sink *s, const char *data, size_t n)
{
    if (s->cap > 0 && s->len < s->cap - 1) {
        size_t room = s->cap - 1 - s->len;

        memcpy(s->out + s->len, data, n < room ? n : room);
    }
    s->len += n;
}

static void
gut_sink_end(struct gut_sink *s)
{
    if (s->cap > 0)
        s->out[s->len < s->cap - 1 ? s->len : s->cap - 1] = '\0';
}

static int
gut_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

size_t
gut_buf_copy_text(const struct gut_buf *b, int row0, int col0, int row1,
                  int col1, int mode, char *out, size_t n)
{
    struct gut_sink s = { out, n, 0 };

    if (row1 < row0 || (row1 == row0 && col1 < col0)) {
        int t;

        t = row0; row0 = row1; row1 = t;
        t = col0; col0 = col1; col1 = t;
    }
    row0 = gut_clamp(row0, 0, b->rows - 1);
    row1 = gut_clamp(row1, 0, b->rows - 1);
    col0 = gut_clamp(col0, 0, b->cols - 1);
    col1 = gut_clamp(col1, 0, b->cols - 1);
    if (mode == GUT_COPY_RECT && col1 < col0) {
        int t = col0;

        col0 = col1;
        col1 = t;
    }
    for (int r = row0; r <= row1; r++) {
        const struct gut_cell *row = &b->cells[r * b->cols];
        int from = col0, to = col1;
        int last;

        if (mode == GUT_COPY_STREAM) {
            if (r > row0)
                from = 0;
            if (r < row1)
                to = b->cols - 1;
        }
        /* trim trailing blanks */
        for (last = to; last >= from; last--) {
            const struct gut_cell *c = &row[last];

            if (c->width != 0 && c->cp != ' ' && c->cp != 0)
                break;
        }
        for (int c = from; c <= last; c++) {
            unsigned char u[4];
            int ulen;

            if (row[c].width == 0)
                continue;
            ulen = gut_utf8_encode(u, row[c].cp ? row[c].cp : ' ');
            if (ulen == 0)
                ulen = gut_utf8_encode(u, GUT_RUNE_ERROR);
            gut_sink_put(&s, (const char *)u, (size_t)ulen);
        }
        if (r < row1)
            gut_sink_put(&s, "\n", 1);
    }
    gut_sink_end(&s);
    return s.len;
}

/****************************************************************
 * Font lookup
 ****************************************************************/

int
gut_font_lookup(const struct gut_font *font, uint32_t cp)
{
    int lo = 0, hi = font->nglyphs;

    while (lo < hi) {
        int mid = (lo + hi) / 2;

        if (cp < font->cmap[mid])
            hi = mid;
        else if (cp > font->cmap[mid])
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

/****************************************************************
 * Key encoder: events to xterm byte sequences
 ****************************************************************/

static int
gut_enc_mods(int mods)
{
    int m = 1;

    if (mods & GUT_MOD_SHIFT)
        m += 1;
    if (mods & GUT_MOD_ALT)
        m += 2;
    if (mods & GUT_MOD_CTRL)
        m += 4;
    return m;
}

/* xterm mouse report. Returns the length, 0 when the flags do not ask
 * for this event or the position does not fit the legacy encoding. */
static size_t
gut_encode_mouse(const struct gut_event *ev, char *out, size_t n, int flags)
{
    int code, release = 0;
    int col = ev->col + 1, row = ev->row + 1;
    size_t len;

    if (!(flags & (GUT_ENC_MOUSE_BTN | GUT_ENC_MOUSE_DRAG |
                   GUT_ENC_MOUSE_ANY)))
        return 0;
    switch (ev->type) {
    case GUT_EVENT_MOUSE_DOWN:
    case GUT_EVENT_MOUSE_UP:
        if (ev->button < GUT_BUTTON_LEFT || ev->button > GUT_BUTTON_RIGHT)
            return 0;
        code = ev->button - 1;
        release = ev->type == GUT_EVENT_MOUSE_UP;
        break;
    case GUT_EVENT_MOUSE_MOVE:
        if (ev->button) {
            if (!(flags & (GUT_ENC_MOUSE_DRAG | GUT_ENC_MOUSE_ANY)))
                return 0;
            code = ev->button - 1;
        } else {
            if (!(flags & GUT_ENC_MOUSE_ANY))
                return 0;
            code = 3;
        }
        code += 32;
        break;
    case GUT_EVENT_MOUSE_WHEEL:
        if (ev->dy > 0)
            code = 64;
        else if (ev->dy < 0)
            code = 65;
        else if (ev->dx < 0)
            code = 66;
        else if (ev->dx > 0)
            code = 67;
        else
            return 0;
        break;
    default:
        return 0;
    }
    if (ev->mods & GUT_MOD_SHIFT)
        code += 4;
    if (ev->mods & GUT_MOD_ALT)
        code += 8;
    if (ev->mods & GUT_MOD_CTRL)
        code += 16;
    if (flags & GUT_ENC_MOUSE_SGR) {
        len = (size_t)snprintf(out, n, "\033[<%d;%d;%d%c", code, col, row,
                               release ? 'm' : 'M');
    } else {
        char tmp[8];

        if (col > 223 || row > 223 || col < 1 || row < 1)
            return 0;
        if (release)
            code = (code & ~3) | 3;
        len = (size_t)snprintf(tmp, sizeof(tmp), "\033[M%c%c%c",
                               32 + code, 32 + col, 32 + row);
        snprintf(out, n, "%s", tmp);
    }
    return len;
}

size_t
gut_encode_event(const struct gut_event *ev, char *out, size_t n, int flags)
{
    char tmp[32];
    size_t len = 0;
    int mods, m;
    const char *csi = NULL;     /* CSI final for arrows and home/end */
    int tilde = 0;              /* CSI n ~ keys */
    const char *ss3 = NULL;     /* SS3 letter for F1-F4 */

    if (n)
        out[0] = '\0';
    if (ev->type == GUT_EVENT_TEXT || ev->type == GUT_EVENT_PASTE) {
        struct gut_sink s = { out, n, 0 };
        const char *text = ev->data ? ev->data : ev->text;
        size_t tlen = ev->data ? ev->len : strlen(ev->text);

        if (ev->type == GUT_EVENT_PASTE && (flags & GUT_ENC_BRACKET_PASTE))
            gut_sink_put(&s, "\033[200~", 6);
        if (ev->type == GUT_EVENT_PASTE) {
            /* a terminal sends Enter as CR; \r\n becomes one CR */
            for (size_t i = 0; i < tlen; i++) {
                if (text[i] == '\n') {
                    if (i == 0 || text[i - 1] != '\r')
                        gut_sink_put(&s, "\r", 1);
                } else {
                    gut_sink_put(&s, &text[i], 1);
                }
            }
        } else {
            gut_sink_put(&s, text, tlen);
        }
        if (ev->type == GUT_EVENT_PASTE && (flags & GUT_ENC_BRACKET_PASTE))
            gut_sink_put(&s, "\033[201~", 6);
        gut_sink_end(&s);
        return s.len;
    }
    if (ev->type == GUT_EVENT_FOCUS_IN || ev->type == GUT_EVENT_FOCUS_OUT) {
        if (!(flags & GUT_ENC_FOCUS))
            return 0;
        return (size_t)snprintf(out, n, "\033[%c",
                                ev->type == GUT_EVENT_FOCUS_IN ? 'I' : 'O');
    }
    if (ev->type >= GUT_EVENT_MOUSE_DOWN && ev->type <= GUT_EVENT_MOUSE_WHEEL)
        return gut_encode_mouse(ev, out, n, flags);
    if (ev->type != GUT_EVENT_KEY)
        return 0;

    mods = ev->mods;
    m = gut_enc_mods(mods);

    switch (ev->key) {
    case GUT_KEY_ENTER:
        len = (size_t)snprintf(tmp, sizeof(tmp), "%s\r",
                               (mods & GUT_MOD_ALT) ? "\033" : "");
        break;
    case GUT_KEY_TAB:
        if (mods & GUT_MOD_SHIFT)
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033[Z");
        else
            len = (size_t)snprintf(tmp, sizeof(tmp), "%s\t",
                                   (mods & GUT_MOD_ALT) ? "\033" : "");
        break;
    case GUT_KEY_BACKSPACE:
        len = (size_t)snprintf(tmp, sizeof(tmp), "%s%c",
                               (mods & GUT_MOD_ALT) ? "\033" : "",
                               (flags & GUT_ENC_BS_BS) ? 0x08 : 0x7F);
        break;
    case GUT_KEY_ESCAPE:
        len = (size_t)snprintf(tmp, sizeof(tmp), "%s\033",
                               (mods & GUT_MOD_ALT) ? "\033" : "");
        break;
    case GUT_KEY_UP:    csi = "A"; break;
    case GUT_KEY_DOWN:  csi = "B"; break;
    case GUT_KEY_RIGHT: csi = "C"; break;
    case GUT_KEY_LEFT:  csi = "D"; break;
    case GUT_KEY_HOME:  csi = "H"; break;
    case GUT_KEY_END:   csi = "F"; break;
    case GUT_KEY_INSERT:   tilde = 2; break;
    case GUT_KEY_DELETE:   tilde = 3; break;
    case GUT_KEY_PAGEUP:   tilde = 5; break;
    case GUT_KEY_PAGEDOWN: tilde = 6; break;
    case GUT_KEY_F1: ss3 = "P"; break;
    case GUT_KEY_F2: ss3 = "Q"; break;
    case GUT_KEY_F3: ss3 = "R"; break;
    case GUT_KEY_F4: ss3 = "S"; break;
    case GUT_KEY_F5:  tilde = 15; break;
    case GUT_KEY_F6:  tilde = 17; break;
    case GUT_KEY_F7:  tilde = 18; break;
    case GUT_KEY_F8:  tilde = 19; break;
    case GUT_KEY_F9:  tilde = 20; break;
    case GUT_KEY_F10: tilde = 21; break;
    case GUT_KEY_F11: tilde = 23; break;
    case GUT_KEY_F12: tilde = 24; break;
    default:
        if (ev->key <= 0 || ev->key >= GUT_KEY_SPECIAL)
            return 0;
        if (!(mods & (GUT_MOD_CTRL | GUT_MOD_ALT)))
            return 0;   /* the GUT_EVENT_TEXT carries plain keys */
        {
            uint32_t cp = (uint32_t)ev->key;
            unsigned char u[4];
            int ulen;

            if (mods & GUT_MOD_CTRL) {
                if (cp >= 'a' && cp <= 'z')
                    cp = cp - 'a' + 1;
                else if (cp >= '@' && cp <= '_')
                    cp = cp - '@';
                else if (cp == ' ' || cp == '2')
                    cp = 0;
                else if (cp == '/' || cp == '7')
                    cp = 0x1F;
                else if (cp == '8')
                    cp = 0x7F;
                else if (cp == '[' || cp == '3')
                    cp = 0x1B;
                else if (cp == '\\' || cp == '4')
                    cp = 0x1C;
                else if (cp == ']' || cp == '5')
                    cp = 0x1D;
                else if (cp == '6')
                    cp = 0x1E;
                else
                    return 0;
            } else if (mods & GUT_MOD_SHIFT) {
                if (cp >= 'a' && cp <= 'z')
                    cp = cp - 'a' + 'A';
            }
            ulen = gut_utf8_encode(u, cp);
            if (ulen == 0)
                return 0;
            if (mods & GUT_MOD_ALT)
                tmp[len++] = 0x1B;
            memcpy(tmp + len, u, (size_t)ulen);
            len += (size_t)ulen;
        }
        break;
    }

    if (csi) {
        if (m > 1)
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033[1;%d%s", m, csi);
        else if (flags & GUT_ENC_APP_CURSOR)
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033O%s", csi);
        else
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033[%s", csi);
    } else if (tilde) {
        if (m > 1)
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033[%d;%d~", tilde, m);
        else
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033[%d~", tilde);
    } else if (ss3) {
        if (m > 1)
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033[1;%d%s", m, ss3);
        else
            len = (size_t)snprintf(tmp, sizeof(tmp), "\033O%s", ss3);
    }

    {
        struct gut_sink s = { out, n, 0 };

        gut_sink_put(&s, tmp, len);
        gut_sink_end(&s);
    }
    return len;
}

/****************************************************************
 * Built-in font: unscii-16 subset (Viznut, public domain)
 ****************************************************************/

#define GUT_FONT16_NGLYPHS 491

static const uint8_t gut_font16_bits[GUT_FONT16_NGLYPHS][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+0020 */
    {0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, /* U+0021 */
    {0x00,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+0022 */
    {0x00,0x00,0x36,0x36,0x36,0x7F,0x36,0x36,0x36,0x7F,0x36,0x36,0x36,0x00,0x00,0x00}, /* U+0023 */
    {0x00,0x18,0x18,0x3C,0x66,0x06,0x0C,0x18,0x30,0x60,0x66,0x3C,0x18,0x18,0x00,0x00}, /* U+0024 */
    {0x00,0x00,0x60,0x63,0x33,0x33,0x18,0x18,0x0C,0x0C,0x66,0x66,0x63,0x03,0x00,0x00}, /* U+0025 */
    {0x00,0x00,0x1C,0x36,0x36,0x1C,0x0C,0x5E,0x7B,0x33,0x33,0x33,0x6E,0x00,0x00,0x00}, /* U+0026 */
    {0x00,0x18,0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+0027 */
    {0x00,0x30,0x18,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x18,0x30,0x00,0x00}, /* U+0028 */
    {0x00,0x0C,0x18,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x18,0x0C,0x00,0x00}, /* U+0029 */
    {0x00,0x00,0x00,0x00,0x66,0x66,0x3C,0xFF,0x3C,0x66,0x66,0x00,0x00,0x00,0x00,0x00}, /* U+002A */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x7E,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* U+002B */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1C,0x18,0x18,0x0C,0x06,0x00}, /* U+002C */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+002D */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+002E */
    {0xC0,0xC0,0x60,0x60,0x30,0x30,0x18,0x18,0x0C,0x0C,0x06,0x06,0x03,0x03,0x00,0x00}, /* U+002F */
    {0x00,0x00,0x1C,0x36,0x63,0x63,0x73,0x6B,0x67,0x63,0x63,0x36,0x1C,0x00,0x00,0x00}, /* U+0030 */
    {0x00,0x00,0x18,0x1C,0x1E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00}, /* U+0031 */
    {0x00,0x00,0x3C,0x66,0x66,0x60,0x60,0x30,0x18,0x0C,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+0032 */
    {0x00,0x00,0x3C,0x66,0x66,0x60,0x60,0x38,0x60,0x60,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+0033 */
    {0x00,0x00,0x30,0x38,0x3C,0x36,0x33,0x33,0x7F,0x30,0x30,0x30,0x30,0x00,0x00,0x00}, /* U+0034 */
    {0x00,0x00,0x7E,0x06,0x06,0x06,0x3E,0x60,0x60,0x60,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+0035 */
    {0x00,0x00,0x38,0x0C,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+0036 */
    {0x00,0x00,0x7E,0x60,0x60,0x60,0x30,0x30,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+0037 */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x6E,0x3C,0x76,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+0038 */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x30,0x1C,0x00,0x00,0x00}, /* U+0039 */
    {0x00,0x00,0x00,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+003A */
    {0x00,0x00,0x00,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x1C,0x18,0x18,0x0C,0x06,0x00}, /* U+003B */
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00}, /* U+003C */
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+003D */
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00}, /* U+003E */
    {0x00,0x3C,0x66,0x66,0x60,0x30,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, /* U+003F */
    {0x00,0x00,0x3E,0x63,0x63,0x63,0x7B,0x7B,0x7B,0x3B,0x03,0x03,0x3E,0x00,0x00,0x00}, /* U+0040 */
    {0x00,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+0041 */
    {0x00,0x00,0x3E,0x66,0x66,0x66,0x36,0x1E,0x36,0x66,0x66,0x66,0x3E,0x00,0x00,0x00}, /* U+0042 */
    {0x00,0x00,0x3C,0x66,0x66,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+0043 */
    {0x00,0x00,0x1E,0x36,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x36,0x1E,0x00,0x00,0x00}, /* U+0044 */
    {0x00,0x00,0x7E,0x06,0x06,0x06,0x06,0x3E,0x06,0x06,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+0045 */
    {0x00,0x00,0x7E,0x06,0x06,0x06,0x3E,0x06,0x06,0x06,0x06,0x06,0x06,0x00,0x00,0x00}, /* U+0046 */
    {0x00,0x00,0x3C,0x66,0x66,0x06,0x06,0x76,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+0047 */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+0048 */
    {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00}, /* U+0049 */
    {0x00,0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+004A */
    {0x00,0x00,0x63,0x63,0x33,0x33,0x1B,0x0F,0x1B,0x33,0x33,0x63,0x63,0x00,0x00,0x00}, /* U+004B */
    {0x00,0x00,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+004C */
    {0x00,0x00,0x63,0x77,0x77,0x7F,0x6B,0x6B,0x63,0x63,0x63,0x63,0x63,0x00,0x00,0x00}, /* U+004D */
    {0x00,0x00,0x63,0x63,0x67,0x67,0x6F,0x7F,0x7B,0x73,0x73,0x63,0x63,0x00,0x00,0x00}, /* U+004E */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+004F */
    {0x00,0x00,0x3E,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x06,0x06,0x00,0x00,0x00}, /* U+0050 */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x30,0x60,0x00}, /* U+0051 */
    {0x00,0x00,0x3E,0x66,0x66,0x66,0x66,0x3E,0x36,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+0052 */
    {0x00,0x00,0x3C,0x66,0x66,0x06,0x0C,0x18,0x30,0x60,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+0053 */
    {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+0054 */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+0055 */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x3C,0x18,0x18,0x00,0x00,0x00}, /* U+0056 */
    {0x00,0x00,0x63,0x63,0x63,0x63,0x63,0x6B,0x6B,0x7F,0x77,0x77,0x63,0x00,0x00,0x00}, /* U+0057 */
    {0x00,0x00,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x3C,0x66,0xC3,0xC3,0x00,0x00,0x00}, /* U+0058 */
    {0x00,0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+0059 */
    {0x00,0x00,0x7E,0x60,0x60,0x30,0x30,0x18,0x0C,0x0C,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+005A */
    {0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00}, /* U+005B */
    {0x03,0x03,0x06,0x06,0x0C,0x0C,0x18,0x18,0x30,0x30,0x60,0x60,0xC0,0xC0,0x00,0x00}, /* U+005C */
    {0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00}, /* U+005D */
    {0x00,0x08,0x1C,0x36,0x36,0x63,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+005E */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* U+005F */
    {0x00,0x18,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+0060 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+0061 */
    {0x00,0x00,0x06,0x06,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x66,0x3E,0x00,0x00,0x00}, /* U+0062 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00}, /* U+0063 */
    {0x00,0x00,0x60,0x60,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+0064 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x7E,0x06,0x06,0x3C,0x00,0x00,0x00}, /* U+0065 */
    {0x00,0x00,0x78,0x0C,0x0C,0x0C,0x7E,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x00,0x00,0x00}, /* U+0066 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0x3E}, /* U+0067 */
    {0x00,0x00,0x06,0x06,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+0068 */
    {0x00,0x00,0x18,0x18,0x00,0x00,0x1E,0x18,0x18,0x18,0x18,0x18,0x78,0x00,0x00,0x00}, /* U+0069 */
    {0x00,0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x1E}, /* U+006A */
    {0x00,0x00,0x06,0x06,0x06,0x06,0x66,0x66,0x36,0x1E,0x36,0x66,0x66,0x00,0x00,0x00}, /* U+006B */
    {0x00,0x00,0x1E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x78,0x00,0x00,0x00}, /* U+006C */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x33,0x7F,0x6B,0x6B,0x6B,0x6B,0x63,0x00,0x00,0x00}, /* U+006D */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+006E */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+006F */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x06}, /* U+0070 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0x60}, /* U+0071 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x06,0x06,0x06,0x06,0x00,0x00,0x00}, /* U+0072 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x06,0x06,0x3C,0x60,0x60,0x3E,0x00,0x00,0x00}, /* U+0073 */
    {0x00,0x00,0x00,0x0C,0x0C,0x0C,0x7E,0x0C,0x0C,0x0C,0x0C,0x0C,0x78,0x00,0x00,0x00}, /* U+0074 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+0075 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00}, /* U+0076 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x63,0x63,0x6B,0x6B,0x6B,0x3E,0x36,0x00,0x00,0x00}, /* U+0077 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x63,0x63,0x36,0x1C,0x36,0x63,0x63,0x00,0x00,0x00}, /* U+0078 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0x3C}, /* U+0079 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x60,0x30,0x18,0x0C,0x06,0x7E,0x00,0x00,0x00}, /* U+007A */
    {0x00,0x70,0x18,0x18,0x18,0x18,0x18,0x0F,0x18,0x18,0x18,0x18,0x18,0x70,0x00,0x00}, /* U+007B */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00}, /* U+007C */
    {0x00,0x07,0x0C,0x0C,0x0C,0x0C,0x0C,0x78,0x0C,0x0C,0x0C,0x0C,0x0C,0x07,0x00,0x00}, /* U+007D */
    {0x00,0x4E,0x6B,0x39,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+007E */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00A0 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+00A1 */
    {0x00,0x18,0x18,0x18,0x3C,0x66,0x06,0x06,0x66,0x3C,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+00A2 */
    {0x00,0x00,0x1C,0x36,0x36,0x06,0x06,0x0F,0x06,0x06,0x66,0x66,0x3F,0x00,0x00,0x00}, /* U+00A3 */
    {0x00,0x00,0x66,0x3C,0x66,0x66,0x66,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00A4 */
    {0x00,0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x7E,0x18,0x7E,0x18,0x18,0x00,0x00,0x00}, /* U+00A5 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00}, /* U+00A6 */
    {0x00,0x3C,0x66,0x06,0x0C,0x1C,0x36,0x66,0x6C,0x38,0x30,0x60,0x66,0x3C,0x00,0x00}, /* U+00A7 */
    {0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00A8 */
    {0x00,0x00,0x3C,0x42,0x81,0xB9,0x8D,0x8D,0x8D,0xB9,0x81,0x42,0x3C,0x00,0x00,0x00}, /* U+00A9 */
    {0x00,0x3C,0x60,0x60,0x7C,0x66,0x66,0x7C,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00AA */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0x66,0x33,0x66,0xCC,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00AB */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00}, /* U+00AC */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00AD */
    {0x00,0x00,0x3C,0x42,0x81,0x9D,0xA5,0x9D,0xA5,0xA5,0x81,0x42,0x3C,0x00,0x00,0x00}, /* U+00AE */
    {0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00AF */
    {0x00,0x3C,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00B0 */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x7E,0x18,0x18,0x18,0x00,0x7E,0x00,0x00,0x00}, /* U+00B1 */
    {0x00,0x1C,0x36,0x30,0x18,0x0C,0x06,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00B2 */
    {0x00,0x1C,0x36,0x30,0x1C,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00B3 */
    {0x30,0x30,0x18,0x18,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00B4 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x03}, /* U+00B5 */
    {0x00,0x00,0x7C,0x5E,0x5E,0x5E,0x5E,0x5C,0x58,0x58,0x58,0x58,0x58,0x00,0x00,0x00}, /* U+00B6 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00B7 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x60,0x38}, /* U+00B8 */
    {0x00,0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00B9 */
    {0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00BA */
    {0x00,0x00,0x00,0x00,0x00,0x33,0x66,0xCC,0x66,0x33,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+00BB */
    {0x00,0x02,0x63,0x62,0x32,0x32,0x18,0x18,0x0C,0x0C,0x46,0x66,0x53,0xF3,0x40,0x00}, /* U+00BC */
    {0x00,0x02,0x63,0x62,0x32,0x32,0x18,0x18,0x0C,0x0C,0x36,0x46,0x23,0x13,0x70,0x00}, /* U+00BD */
    {0x00,0x03,0x64,0x66,0x34,0x33,0x18,0x18,0x0C,0x0C,0x46,0x66,0x53,0xF3,0x40,0x00}, /* U+00BE */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x0C,0x0C,0x06,0x06,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00BF */
    {0x0C,0x18,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+00C0 */
    {0x30,0x18,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+00C1 */
    {0x18,0x66,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+00C2 */
    {0x6E,0x3B,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+00C3 */
    {0x66,0x66,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+00C4 */
    {0x3C,0x66,0x3C,0x00,0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+00C5 */
    {0x00,0x00,0xFC,0x3E,0x3F,0x33,0x33,0x7F,0x33,0x33,0x33,0x33,0xF3,0x00,0x00,0x00}, /* U+00C6 */
    {0x00,0x00,0x3C,0x66,0x66,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x30,0x60,0x38}, /* U+00C7 */
    {0x0C,0x18,0x00,0x7E,0x06,0x06,0x06,0x3E,0x06,0x06,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+00C8 */
    {0x30,0x18,0x00,0x7E,0x06,0x06,0x06,0x3E,0x06,0x06,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+00C9 */
    {0x18,0x66,0x00,0x7E,0x06,0x06,0x06,0x3E,0x06,0x06,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+00CA */
    {0x66,0x66,0x00,0x7E,0x06,0x06,0x06,0x3E,0x06,0x06,0x06,0x06,0x7E,0x00,0x00,0x00}, /* U+00CB */
    {0x0C,0x18,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00}, /* U+00CC */
    {0x30,0x18,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00}, /* U+00CD */
    {0x18,0x66,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00}, /* U+00CE */
    {0x66,0x66,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00}, /* U+00CF */
    {0x00,0x00,0x1E,0x36,0x66,0x66,0x66,0x6F,0x66,0x66,0x66,0x36,0x1E,0x00,0x00,0x00}, /* U+00D0 */
    {0x6E,0x3B,0x00,0x63,0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x63,0x63,0x00,0x00,0x00}, /* U+00D1 */
    {0x0C,0x18,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00D2 */
    {0x30,0x18,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00D3 */
    {0x18,0x66,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00D4 */
    {0x6E,0x3B,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00D5 */
    {0x66,0x66,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00D6 */
    {0x00,0x00,0x00,0x00,0x41,0x63,0x36,0x1C,0x36,0x63,0x41,0x00,0x00,0x00,0x00,0x00}, /* U+00D7 */
    {0x00,0x00,0x7C,0x66,0x76,0x76,0x76,0x7E,0x6E,0x6E,0x6E,0x66,0x3E,0x00,0x00,0x00}, /* U+00D8 */
    {0x0C,0x18,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00D9 */
    {0x30,0x18,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00DA */
    {0x18,0x66,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00DB */
    {0x66,0x66,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00DC */
    {0x30,0x18,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+00DD */
    {0x00,0x00,0x03,0x03,0x3F,0x63,0x63,0x63,0x63,0x63,0x3F,0x03,0x03,0x00,0x00,0x00}, /* U+00DE */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1B,0x33,0x63,0x63,0x63,0x63,0x33,0x00,0x00,0x00}, /* U+00DF */
    {0x00,0x00,0x0C,0x18,0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00E0 */
    {0x00,0x00,0x30,0x18,0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00E1 */
    {0x00,0x18,0x3C,0x66,0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00E2 */
    {0x00,0x00,0x6E,0x3B,0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00E3 */
    {0x00,0x00,0x66,0x66,0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00E4 */
    {0x00,0x00,0x3C,0x66,0x3C,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00E5 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0xD8,0xD8,0xFE,0x1B,0x1B,0xEE,0x00,0x00,0x00}, /* U+00E6 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x06,0x06,0x06,0x66,0x3C,0x30,0x60,0x38}, /* U+00E7 */
    {0x00,0x00,0x0C,0x18,0x00,0x00,0x3C,0x66,0x66,0x7E,0x06,0x06,0x3C,0x00,0x00,0x00}, /* U+00E8 */
    {0x00,0x00,0x30,0x18,0x00,0x00,0x3C,0x66,0x66,0x7E,0x06,0x06,0x3C,0x00,0x00,0x00}, /* U+00E9 */
    {0x00,0x18,0x3C,0x66,0x00,0x00,0x3C,0x66,0x66,0x7E,0x06,0x06,0x3C,0x00,0x00,0x00}, /* U+00EA */
    {0x00,0x00,0x66,0x66,0x00,0x00,0x3C,0x66,0x66,0x7E,0x06,0x06,0x3C,0x00,0x00,0x00}, /* U+00EB */
    {0x00,0x00,0x0C,0x18,0x00,0x00,0x1C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* U+00EC */
    {0x00,0x00,0x30,0x18,0x00,0x00,0x1C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* U+00ED */
    {0x00,0x18,0x3C,0x66,0x00,0x00,0x1C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* U+00EE */
    {0x00,0x00,0x66,0x66,0x00,0x00,0x1C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* U+00EF */
    {0x00,0x6C,0x38,0x38,0x6C,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00F0 */
    {0x00,0x00,0x6E,0x3B,0x00,0x00,0x3E,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* U+00F1 */
    {0x00,0x00,0x0C,0x18,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00F2 */
    {0x00,0x00,0x30,0x18,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00F3 */
    {0x00,0x18,0x3C,0x66,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00F4 */
    {0x00,0x00,0x6E,0x3B,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00F5 */
    {0x00,0x00,0x66,0x66,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00}, /* U+00F6 */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x7E,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* U+00F7 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xBC,0x66,0x76,0x7E,0x6E,0x66,0x3D,0x00,0x00,0x00}, /* U+00F8 */
    {0x00,0x00,0x0C,0x18,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00F9 */
    {0x00,0x00,0x30,0x18,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00FA */
    {0x00,0x18,0x3C,0x66,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00FB */
    {0x00,0x00,0x66,0x66,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00}, /* U+00FC */
    {0x00,0x00,0x30,0x18,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0x3C}, /* U+00FD */
    {0x00,0x00,0x00,0x06,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x06}, /* U+00FE */
    {0x00,0x00,0x66,0x66,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0x3C}, /* U+00FF */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2010 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2011 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2012 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2013 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2014 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2015 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x00,0x00}, /* U+2016 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xFF}, /* U+2017 */
    {0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2018 */
    {0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2019 */
    {0x33,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+201C */
    {0x66,0x33,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+201D */
    {0x00,0x00,0x18,0x18,0x18,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+2020 */
    {0x00,0x00,0x18,0x18,0x18,0x7E,0x18,0x18,0x18,0x7E,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+2021 */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x3C,0x3C,0x3C,0x3C,0x18,0x18,0x00,0x00,0x00,0x00}, /* U+2022 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x00,0x00,0x00}, /* U+2024 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x00,0x00}, /* U+2025 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x49,0x49,0x00,0x00,0x00}, /* U+2026 */
    {0x00,0x00,0x63,0x63,0x33,0x33,0x18,0x18,0x06,0x06,0xDB,0xDB,0xDB,0xDB,0x00,0x00}, /* U+2030 */
    {0x00,0x00,0x63,0x63,0x33,0x33,0x18,0x18,0x06,0x06,0xAB,0xAB,0xAB,0xAB,0x00,0x00}, /* U+2031 */
    {0x30,0x30,0x18,0x18,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2032 */
    {0x00,0x00,0x6C,0x36,0x1B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2033 */
    {0x00,0x00,0x54,0x2A,0x15,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2034 */
    {0x00,0x1C,0x36,0x36,0x36,0x36,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2070 */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2071 */
    {0x00,0x38,0x3C,0x36,0x7E,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2074 */
    {0x00,0x3E,0x06,0x1E,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2075 */
    {0x00,0x1C,0x06,0x06,0x1E,0x36,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2076 */
    {0x00,0x3E,0x30,0x30,0x18,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2077 */
    {0x00,0x1C,0x36,0x36,0x1C,0x36,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2078 */
    {0x00,0x1C,0x36,0x36,0x3C,0x30,0x30,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2079 */
    {0x00,0x18,0x18,0x7E,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+207A */
    {0x00,0x00,0x7E,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+207B */
    {0x00,0x00,0x7E,0x7E,0x00,0x7E,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+207C */
    {0x00,0x00,0x00,0x1E,0x36,0x36,0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+207F */
    {0x00,0x57,0x72,0x72,0x52,0x52,0x52,0x52,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2122 */
    {0x00,0x00,0x00,0x00,0x00,0x08,0x0C,0xFE,0xFE,0x0C,0x08,0x00,0x00,0x00,0x00,0x00}, /* U+2190 */
    {0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2191 */
    {0x00,0x00,0x00,0x00,0x00,0x10,0x30,0x7F,0x7F,0x30,0x10,0x00,0x00,0x00,0x00,0x00}, /* U+2192 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x3C,0x18}, /* U+2193 */
    {0x00,0x00,0x00,0x00,0x00,0x24,0x66,0xFF,0xFF,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* U+2194 */
    {0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x3C,0x18}, /* U+2195 */
    {0x0F,0x0F,0x07,0x07,0x0F,0x0F,0x1D,0x1D,0x38,0x38,0x70,0x70,0x20,0x20,0x00,0x00}, /* U+2196 */
    {0xF0,0xF0,0xE0,0xE0,0xF0,0xF0,0xB8,0xB8,0x1C,0x1C,0x0E,0x0E,0x04,0x04,0x00,0x00}, /* U+2197 */
    {0x00,0x00,0x04,0x04,0x0E,0x0E,0x1C,0x1C,0xB8,0xB8,0xF0,0xF0,0xE0,0xE0,0xF0,0xF0}, /* U+2198 */
    {0x00,0x00,0x20,0x20,0x70,0x70,0x38,0x38,0x1D,0x1D,0x0F,0x0F,0x07,0x07,0x0F,0x0F}, /* U+2199 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2500 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2501 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2502 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2503 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xBB,0xBB,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2504 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xBB,0xBB,0xBB,0xBB,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2505 */
    {0x18,0x18,0x00,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00,0x18}, /* U+2506 */
    {0x3C,0x3C,0x00,0x3C,0x3C,0x3C,0x00,0x3C,0x3C,0x3C,0x00,0x3C,0x3C,0x3C,0x00,0x3C}, /* U+2507 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x55,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2508 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x55,0x55,0x55,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2509 */
    {0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18}, /* U+250A */
    {0x3C,0x00,0x00,0x3C,0x3C,0x00,0x00,0x3C,0x3C,0x00,0x00,0x3C,0x3C,0x00,0x00,0x3C}, /* U+250B */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+250C */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0xF8,0xF8,0xF8,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+250D */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+250E */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0xFC,0xFC,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+250F */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2510 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x1F,0x1F,0x1F,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2511 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2512 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x3F,0x3F,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2513 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2514 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xF8,0xF8,0xF8,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2515 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2516 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFC,0xFC,0xFC,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2517 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2518 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x1F,0x1F,0x1F,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2519 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+251A */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0x3F,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+251B */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+251C */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xF8,0xF8,0xF8,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+251D */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+251E */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xF8,0xFC,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+251F */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2520 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFC,0xF8,0xF8,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2521 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2522 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFC,0xFC,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2523 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2524 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x1F,0x1F,0x1F,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2525 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2526 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x1F,0x3F,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2527 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2528 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0x3F,0x1F,0x1F,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2529 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+252A */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0x3F,0x3F,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+252B */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+252C */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0xFF,0xFF,0x1F,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+252D */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0xFF,0xFF,0xF8,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+252E */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+252F */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2530 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0xFF,0xFF,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2531 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0xFF,0xFF,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2532 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2533 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2534 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0xFF,0xFF,0x0F,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2535 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xFF,0xFF,0xF0,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2536 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2537 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2538 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0xFF,0xFF,0x0F,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2539 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFF,0xFF,0xF0,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+253A */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+253B */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+253C */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0xFF,0xFF,0x1F,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+253D */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xFF,0xFF,0xF8,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+253E */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0xFF,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+253F */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2540 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0xFF,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2541 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFF,0xFF,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2542 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0xFF,0xFF,0x1F,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2543 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFF,0xFF,0xF8,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2544 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0xFF,0xFF,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2545 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0xFF,0xFF,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2546 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFF,0xFF,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2547 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0xFF,0xFF,0xFF,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2548 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3F,0xFF,0xFF,0x3F,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+2549 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFC,0xFF,0xFF,0xFC,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+254A */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0xFF,0xFF,0xFF,0xFF,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+254B */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x99,0x99,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+254C */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x99,0x99,0x99,0x99,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+254D */
    {0x18,0x18,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x18,0x18}, /* U+254E */
    {0x3C,0x3C,0x00,0x00,0x00,0x00,0x3C,0x3C,0x3C,0x3C,0x00,0x00,0x00,0x00,0x3C,0x3C}, /* U+254F */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2550 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2551 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x18,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2552 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0xFE,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2553 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x06,0xF6,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2554 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x18,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2555 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x3F,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2556 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x30,0x37,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2557 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0x18,0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2558 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0xFE,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2559 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0xF6,0x06,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+255A */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x18,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+255B */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+255C */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x37,0x30,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+255D */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0x18,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+255E */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0xF6,0xF6,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+255F */
    {0x36,0x36,0x36,0x36,0x36,0x36,0xF6,0x06,0xF6,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2560 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x18,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2561 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x37,0x37,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2562 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x37,0x30,0x37,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2563 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2564 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2565 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xF7,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+2566 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2567 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2568 */
    {0x36,0x36,0x36,0x36,0x36,0x36,0xF7,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2569 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x18,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+256A */
    {0x36,0x36,0x36,0x36,0x36,0x36,0x36,0xFF,0xFF,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+256B */
    {0x36,0x36,0x36,0x36,0x36,0x36,0xF7,0x00,0xF7,0x36,0x36,0x36,0x36,0x36,0x36,0x36}, /* U+256C */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xE0,0xF0,0x38,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+256D */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x0F,0x1C,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+256E */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x1C,0x0F,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+256F */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x38,0xF0,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2570 */
    {0xC0,0xC0,0x60,0x60,0x30,0x30,0x18,0x18,0x0C,0x0C,0x06,0x06,0x03,0x03,0x01,0x01}, /* U+2571 */
    {0x03,0x03,0x06,0x06,0x0C,0x0C,0x18,0x18,0x30,0x30,0x60,0x60,0xC0,0xC0,0x80,0x80}, /* U+2572 */
    {0xC3,0xC3,0x66,0x66,0x3C,0x3C,0x18,0x18,0x3C,0x3C,0x66,0x66,0xC3,0xC3,0x81,0x81}, /* U+2573 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2574 */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2575 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2576 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+2577 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2578 */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2579 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0xF0,0xF0,0xF0,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+257A */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+257B */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0xFF,0xFF,0xF0,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+257C */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C}, /* U+257D */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0xFF,0xFF,0x0F,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+257E */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* U+257F */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2580 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF}, /* U+2581 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF}, /* U+2582 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+2583 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+2584 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+2585 */
    {0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+2586 */
    {0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+2587 */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+2588 */
    {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F}, /* U+2589 */
    {0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F}, /* U+258A */
    {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}, /* U+258B */
    {0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F}, /* U+258C */
    {0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07}, /* U+258D */
    {0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03}, /* U+258E */
    {0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01}, /* U+258F */
    {0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0}, /* U+2590 */
    {0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11}, /* U+2591 */
    {0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA}, /* U+2592 */
    {0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE}, /* U+2593 */
    {0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2594 */
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80}, /* U+2595 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F}, /* U+2596 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0}, /* U+2597 */
    {0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+2598 */
    {0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+2599 */
    {0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0}, /* U+259A */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F}, /* U+259B */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0}, /* U+259C */
    {0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+259D */
    {0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F}, /* U+259E */
    {0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+259F */
    {0x00,0x00,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x00,0x00}, /* U+25A0 */
    {0x00,0x00,0x7E,0x7E,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x7E,0x7E,0x00,0x00}, /* U+25A1 */
    {0x3E,0x3E,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x3E,0x3E,0x00,0x00}, /* U+25A2 */
    {0x7F,0x7F,0x41,0x41,0x5D,0x5D,0x5D,0x5D,0x5D,0x5D,0x41,0x41,0x7F,0x7F,0x00,0x00}, /* U+25A3 */
    {0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00}, /* U+25A4 */
    {0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55}, /* U+25A5 */
    {0x22,0xFF,0x22,0x22,0x22,0xFF,0x22,0x22,0x22,0xFF,0x22,0x22,0x22,0xFF,0x22,0x22}, /* U+25A6 */
    {0x11,0x11,0x22,0x22,0x44,0x44,0x88,0x88,0x11,0x11,0x22,0x22,0x44,0x44,0x88,0x88}, /* U+25A7 */
    {0x88,0x88,0x44,0x44,0x22,0x22,0x11,0x11,0x88,0x88,0x44,0x44,0x22,0x22,0x11,0x11}, /* U+25A8 */
    {0x55,0x55,0x22,0x22,0x55,0x55,0x88,0x88,0x55,0x55,0x22,0x22,0x55,0x55,0x88,0x88}, /* U+25A9 */
    {0x00,0x00,0x00,0x00,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+25AA */
    {0x00,0x00,0x00,0x00,0x1C,0x1C,0x14,0x14,0x1C,0x1C,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+25AB */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x00,0x00}, /* U+25AC */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x7E,0x66,0x66,0x7E,0x7E,0x00,0x00}, /* U+25AD */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x00,0x00}, /* U+25AE */
    {0x3C,0x3C,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x3C,0x3C,0x00,0x00}, /* U+25AF */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0xFC,0x7E,0x7E,0x3F,0x3F,0x00,0x00}, /* U+25B0 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0xFC,0x66,0x66,0x3F,0x3F,0x00,0x00}, /* U+25B1 */
    {0x18,0x18,0x18,0x18,0x3C,0x3C,0x3C,0x3C,0x7E,0x7E,0x7E,0x7E,0xFF,0xFF,0xFF,0xFF}, /* U+25B2 */
    {0x18,0x18,0x18,0x18,0x3C,0x3C,0x66,0x66,0x66,0x66,0xC3,0xC3,0xC3,0xC3,0xFF,0xFF}, /* U+25B3 */
    {0x00,0x00,0x08,0x08,0x08,0x08,0x1C,0x1C,0x1C,0x1C,0x3E,0x3E,0x3E,0x3E,0x00,0x00}, /* U+25B4 */
    {0x00,0x00,0x08,0x08,0x1C,0x14,0x14,0x36,0x22,0x22,0x63,0x41,0x41,0x7F,0x00,0x00}, /* U+25B5 */
    {0x01,0x03,0x07,0x0F,0x1F,0x3F,0x7F,0xFF,0xFF,0x7F,0x3F,0x1F,0x0F,0x07,0x03,0x01}, /* U+25B6 */
    {0x01,0x03,0x07,0x0F,0x1B,0x33,0x63,0xC3,0x63,0x33,0x1B,0x0F,0x07,0x03,0x01,0x00}, /* U+25B7 */
    {0x00,0x00,0x00,0x02,0x06,0x0E,0x1E,0x3E,0x1E,0x0E,0x06,0x02,0x00,0x00,0x00,0x00}, /* U+25B8 */
    {0x00,0x00,0x00,0x02,0x06,0x0A,0x12,0x22,0x12,0x0A,0x06,0x02,0x00,0x00,0x00,0x00}, /* U+25B9 */
    {0x00,0x00,0x00,0x00,0x03,0x0F,0x3F,0xFF,0x3F,0x0F,0x03,0x00,0x00,0x00,0x00,0x00}, /* U+25BA */
    {0x00,0x00,0x00,0x00,0x03,0x0F,0x3B,0xE3,0x3B,0x0F,0x03,0x00,0x00,0x00,0x00,0x00}, /* U+25BB */
    {0xFF,0xFF,0xFF,0xFF,0x7E,0x7E,0x7E,0x7E,0x3C,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x18}, /* U+25BC */
    {0xFF,0xFF,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x66,0x66,0x3C,0x3C,0x18,0x18,0x18,0x18}, /* U+25BD */
    {0x00,0x00,0x3E,0x3E,0x3E,0x3E,0x1C,0x1C,0x1C,0x1C,0x08,0x08,0x08,0x08,0x00,0x00}, /* U+25BE */
    {0x00,0x00,0x7F,0x41,0x41,0x63,0x22,0x22,0x36,0x14,0x14,0x1C,0x08,0x08,0x00,0x00}, /* U+25BF */
    {0x80,0xC0,0xE0,0xF0,0xF8,0xFC,0xFE,0xFF,0xFF,0xFE,0xFC,0xF8,0xF0,0xE0,0xC0,0x80}, /* U+25C0 */
    {0x80,0xC0,0xE0,0xF0,0xD8,0xCC,0xC6,0xC3,0xC6,0xCC,0xD8,0xF0,0xE0,0xC0,0x80,0x00}, /* U+25C1 */
    {0x00,0x00,0x00,0x20,0x30,0x38,0x3C,0x3E,0x3C,0x38,0x30,0x20,0x00,0x00,0x00,0x00}, /* U+25C2 */
    {0x00,0x00,0x00,0x20,0x30,0x28,0x24,0x22,0x24,0x28,0x30,0x20,0x00,0x00,0x00,0x00}, /* U+25C3 */
    {0x00,0x00,0x00,0x00,0xC0,0xF0,0xFC,0xFF,0xFC,0xF0,0xC0,0x00,0x00,0x00,0x00,0x00}, /* U+25C4 */
    {0x00,0x00,0x00,0x00,0xC0,0xF0,0xDC,0xC7,0xDC,0xF0,0xC0,0x00,0x00,0x00,0x00,0x00}, /* U+25C5 */
    {0x18,0x18,0x3C,0x3C,0x7E,0x7E,0xFF,0xFF,0xFF,0xFF,0x7E,0x7E,0x3C,0x3C,0x18,0x18}, /* U+25C6 */
    {0x18,0x18,0x3C,0x3C,0x66,0x66,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x3C,0x18,0x18}, /* U+25C7 */
    {0x18,0x18,0x24,0x24,0x5A,0x5A,0xBD,0xBD,0xBD,0xBD,0x5A,0x5A,0x24,0x24,0x18,0x18}, /* U+25C8 */
    {0x3C,0x3C,0x66,0x66,0xC3,0xC3,0xDB,0xDB,0xDB,0xDB,0xC3,0xC3,0x66,0x66,0x3C,0x3C}, /* U+25C9 */
    {0x08,0x08,0x1C,0x1C,0x36,0x36,0x63,0x63,0x63,0x63,0x36,0x36,0x1C,0x1C,0x08,0x08}, /* U+25CA */
    {0x00,0x00,0x3C,0x3C,0x66,0x66,0x42,0x42,0x42,0x42,0x66,0x66,0x3C,0x3C,0x00,0x00}, /* U+25CB */
    {0x08,0x08,0x22,0x22,0x00,0x00,0x41,0x41,0x00,0x00,0x22,0x22,0x08,0x08,0x00,0x00}, /* U+25CC */
    {0x00,0x00,0x1C,0x1C,0x36,0x36,0x55,0x55,0x55,0x55,0x36,0x36,0x1C,0x1C,0x00,0x00}, /* U+25CD */
    {0x1C,0x1C,0x22,0x22,0x49,0x49,0x55,0x55,0x49,0x49,0x22,0x22,0x1C,0x1C,0x00,0x00}, /* U+25CE */
    {0x00,0x00,0x3C,0x3C,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x3C,0x3C,0x00,0x00}, /* U+25CF */
    {0x3C,0x3C,0x6E,0x6E,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0x6E,0x6E,0x3C,0x3C}, /* U+25D0 */
    {0x3C,0x3C,0x76,0x76,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0x76,0x76,0x3C,0x3C}, /* U+25D1 */
    {0x3C,0x3C,0x66,0x66,0xC3,0xC3,0xC3,0xC3,0xFF,0xFF,0xFF,0xFF,0x7E,0x7E,0x3C,0x3C}, /* U+25D2 */
    {0x3C,0x3C,0x7E,0x7E,0xFF,0xFF,0xFF,0xFF,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x3C}, /* U+25D3 */
    {0x3C,0x3C,0x76,0x76,0xF3,0xF3,0xF3,0xF3,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x3C}, /* U+25D4 */
    {0x3C,0x3C,0x76,0x76,0xF3,0xF3,0xF3,0xF3,0xFF,0xFF,0xFF,0xFF,0x7E,0x7E,0x3C,0x3C}, /* U+25D5 */
    {0x00,0xC0,0xF0,0xF8,0xF8,0xFC,0xFC,0xFC,0xFC,0xFC,0xFC,0xF8,0xF8,0xF0,0xC0,0x00}, /* U+25D6 */
    {0x00,0x03,0x0F,0x1F,0x1F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x1F,0x1F,0x0F,0x03,0x00}, /* U+25D7 */
    {0xFF,0xFF,0xFF,0xFF,0xE7,0xE7,0xC3,0xC3,0xC3,0xC3,0xE7,0xE7,0xFF,0xFF,0xFF,0xFF}, /* U+25D8 */
    {0xFF,0xFF,0xC3,0xC3,0x99,0x99,0xBD,0xBD,0xBD,0xBD,0x99,0x99,0xC3,0xC3,0xFF,0xFF}, /* U+25D9 */
    {0xFF,0xFF,0xC3,0xC3,0x99,0x99,0xBD,0xBD,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+25DA */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xBD,0xBD,0x99,0x99,0xC3,0xC3,0xFF,0xFF}, /* U+25DB */
    {0x1C,0x1C,0x06,0x06,0x03,0x03,0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+25DC */
    {0x38,0x38,0x60,0x60,0xC0,0xC0,0xC0,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+25DD */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0xC0,0xC0,0xC0,0x60,0x60,0x38,0x38}, /* U+25DE */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x03,0x03,0x03,0x06,0x06,0x1C,0x1C}, /* U+25DF */
    {0x3C,0x3C,0x66,0x66,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+25E0 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x3C}, /* U+25E1 */
    {0x80,0x80,0xC0,0xC0,0xE0,0xE0,0xF0,0xF0,0xF8,0xF8,0xFC,0xFC,0xFE,0xFE,0xFF,0xFF}, /* U+25E2 */
    {0x01,0x01,0x03,0x03,0x07,0x07,0x0F,0x0F,0x1F,0x1F,0x3F,0x3F,0x7F,0x7F,0xFF,0xFF}, /* U+25E3 */
    {0xFF,0xFF,0x7F,0x7F,0x3F,0x3F,0x1F,0x1F,0x0F,0x0F,0x07,0x07,0x03,0x03,0x01,0x01}, /* U+25E4 */
    {0xFF,0xFF,0xFE,0xFE,0xFC,0xFC,0xF8,0xF8,0xF0,0xF0,0xE0,0xE0,0xC0,0xC0,0x80,0x80}, /* U+25E5 */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x24,0x24,0x24,0x24,0x18,0x18,0x00,0x00,0x00,0x00}, /* U+25E6 */
    {0xFF,0xFF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xCF,0xFF,0xFF}, /* U+25E7 */
    {0xFF,0xFF,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xFF,0xFF}, /* U+25E8 */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xDF,0xDF,0xCF,0xCF,0xC7,0xC7,0xC3,0xC3,0xFF,0xFF}, /* U+25E9 */
    {0xFF,0xFF,0xC3,0xC3,0xE3,0xE3,0xF3,0xF3,0xFB,0xFB,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* U+25EA */
    {0xFF,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xFF,0xFF}, /* U+25EB */
    {0x18,0x18,0x18,0x18,0x24,0x24,0x42,0x42,0x5A,0x5A,0x99,0x99,0x81,0x81,0xFF,0xFF}, /* U+25EC */
    {0x18,0x18,0x18,0x18,0x3C,0x3C,0x6E,0x6E,0x6E,0x6E,0xCF,0xCF,0xCF,0xCF,0xFF,0xFF}, /* U+25ED */
    {0x18,0x18,0x18,0x18,0x3C,0x3C,0x76,0x76,0x76,0x76,0xF3,0xF3,0xF3,0xF3,0xFF,0xFF}, /* U+25EE */
    {0x3C,0x3C,0x66,0x66,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x3C}, /* U+25EF */
    {0xFF,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDF,0xDF,0xC3,0xC3,0xC3,0xC3,0xFF,0xFF}, /* U+25F0 */
    {0xFF,0xFF,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xDF,0xDF,0xDB,0xDB,0xDB,0xDB,0xFF,0xFF}, /* U+25F1 */
    {0xFF,0xFF,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xFB,0xFB,0xDB,0xDB,0xDB,0xDB,0xFF,0xFF}, /* U+25F2 */
    {0xFF,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xFB,0xFB,0xC3,0xC3,0xC3,0xC3,0xFF,0xFF}, /* U+25F3 */
    {0x3C,0x3C,0x7E,0x7E,0xDB,0xDB,0xDB,0xDB,0xDF,0xDF,0xC3,0xC3,0x66,0x66,0x3C,0x3C}, /* U+25F4 */
    {0x3C,0x3C,0x66,0x66,0xC3,0xC3,0xDF,0xDF,0xDB,0xDB,0xDB,0xDB,0x7E,0x7E,0x3C,0x3C}, /* U+25F5 */
    {0x3C,0x3C,0x66,0x66,0xC3,0xC3,0xFB,0xFB,0xDB,0xDB,0xDB,0xDB,0x7E,0x7E,0x3C,0x3C}, /* U+25F6 */
    {0x3C,0x3C,0x7E,0x7E,0xDB,0xDB,0xDB,0xDB,0xFB,0xFB,0xC3,0xC3,0x66,0x66,0x3C,0x3C}, /* U+25F7 */
    {0xFF,0xFF,0x63,0x63,0x33,0x33,0x1B,0x1B,0x0F,0x0F,0x07,0x07,0x03,0x03,0x01,0x01}, /* U+25F8 */
    {0xFF,0xFF,0xC6,0xC6,0xCC,0xCC,0xD8,0xD8,0xF0,0xF0,0xE0,0xE0,0xC0,0xC0,0x80,0x80}, /* U+25F9 */
    {0x01,0x01,0x03,0x03,0x07,0x07,0x0F,0x0F,0x1B,0x1B,0x33,0x33,0x63,0x63,0xFF,0xFF}, /* U+25FA */
    {0x00,0x00,0x00,0x00,0x3C,0x3C,0x24,0x24,0x24,0x24,0x3C,0x3C,0x00,0x00,0x00,0x00}, /* U+25FB */
    {0x00,0x00,0x00,0x00,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x00,0x00,0x00,0x00}, /* U+25FC */
    {0x80,0x80,0xC0,0xC0,0xE0,0xE0,0xF0,0xF0,0xD8,0xD8,0xCC,0xCC,0xC6,0xC6,0xFF,0xFF}, /* U+25FF */
};

static const uint32_t gut_font16_cmap[GUT_FONT16_NGLYPHS] = {
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x00A0,
    0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8,
    0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, 0x00B0,
    0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8,
    0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00C0,
    0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8,
    0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x00D0,
    0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8,
    0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF, 0x00E0,
    0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8,
    0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F0,
    0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8,
    0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF, 0x2010,
    0x2011, 0x2012, 0x2013, 0x2014, 0x2015, 0x2016, 0x2017, 0x2018,
    0x2019, 0x201C, 0x201D, 0x2020, 0x2021, 0x2022, 0x2024, 0x2025,
    0x2026, 0x2030, 0x2031, 0x2032, 0x2033, 0x2034, 0x2070, 0x2071,
    0x2074, 0x2075, 0x2076, 0x2077, 0x2078, 0x2079, 0x207A, 0x207B,
    0x207C, 0x207F, 0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2194,
    0x2195, 0x2196, 0x2197, 0x2198, 0x2199, 0x2500, 0x2501, 0x2502,
    0x2503, 0x2504, 0x2505, 0x2506, 0x2507, 0x2508, 0x2509, 0x250A,
    0x250B, 0x250C, 0x250D, 0x250E, 0x250F, 0x2510, 0x2511, 0x2512,
    0x2513, 0x2514, 0x2515, 0x2516, 0x2517, 0x2518, 0x2519, 0x251A,
    0x251B, 0x251C, 0x251D, 0x251E, 0x251F, 0x2520, 0x2521, 0x2522,
    0x2523, 0x2524, 0x2525, 0x2526, 0x2527, 0x2528, 0x2529, 0x252A,
    0x252B, 0x252C, 0x252D, 0x252E, 0x252F, 0x2530, 0x2531, 0x2532,
    0x2533, 0x2534, 0x2535, 0x2536, 0x2537, 0x2538, 0x2539, 0x253A,
    0x253B, 0x253C, 0x253D, 0x253E, 0x253F, 0x2540, 0x2541, 0x2542,
    0x2543, 0x2544, 0x2545, 0x2546, 0x2547, 0x2548, 0x2549, 0x254A,
    0x254B, 0x254C, 0x254D, 0x254E, 0x254F, 0x2550, 0x2551, 0x2552,
    0x2553, 0x2554, 0x2555, 0x2556, 0x2557, 0x2558, 0x2559, 0x255A,
    0x255B, 0x255C, 0x255D, 0x255E, 0x255F, 0x2560, 0x2561, 0x2562,
    0x2563, 0x2564, 0x2565, 0x2566, 0x2567, 0x2568, 0x2569, 0x256A,
    0x256B, 0x256C, 0x256D, 0x256E, 0x256F, 0x2570, 0x2571, 0x2572,
    0x2573, 0x2574, 0x2575, 0x2576, 0x2577, 0x2578, 0x2579, 0x257A,
    0x257B, 0x257C, 0x257D, 0x257E, 0x257F, 0x2580, 0x2581, 0x2582,
    0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588, 0x2589, 0x258A,
    0x258B, 0x258C, 0x258D, 0x258E, 0x258F, 0x2590, 0x2591, 0x2592,
    0x2593, 0x2594, 0x2595, 0x2596, 0x2597, 0x2598, 0x2599, 0x259A,
    0x259B, 0x259C, 0x259D, 0x259E, 0x259F, 0x25A0, 0x25A1, 0x25A2,
    0x25A3, 0x25A4, 0x25A5, 0x25A6, 0x25A7, 0x25A8, 0x25A9, 0x25AA,
    0x25AB, 0x25AC, 0x25AD, 0x25AE, 0x25AF, 0x25B0, 0x25B1, 0x25B2,
    0x25B3, 0x25B4, 0x25B5, 0x25B6, 0x25B7, 0x25B8, 0x25B9, 0x25BA,
    0x25BB, 0x25BC, 0x25BD, 0x25BE, 0x25BF, 0x25C0, 0x25C1, 0x25C2,
    0x25C3, 0x25C4, 0x25C5, 0x25C6, 0x25C7, 0x25C8, 0x25C9, 0x25CA,
    0x25CB, 0x25CC, 0x25CD, 0x25CE, 0x25CF, 0x25D0, 0x25D1, 0x25D2,
    0x25D3, 0x25D4, 0x25D5, 0x25D6, 0x25D7, 0x25D8, 0x25D9, 0x25DA,
    0x25DB, 0x25DC, 0x25DD, 0x25DE, 0x25DF, 0x25E0, 0x25E1, 0x25E2,
    0x25E3, 0x25E4, 0x25E5, 0x25E6, 0x25E7, 0x25E8, 0x25E9, 0x25EA,
    0x25EB, 0x25EC, 0x25ED, 0x25EE, 0x25EF, 0x25F0, 0x25F1, 0x25F2,
    0x25F3, 0x25F4, 0x25F5, 0x25F6, 0x25F7, 0x25F8, 0x25F9, 0x25FA,
    0x25FB, 0x25FC, 0x25FF, 
};

const struct gut_font *
gut_font_default(void)
{
    static const struct gut_font f = {
        8, 16, GUT_FONT16_NGLYPHS, &gut_font16_bits[0][0], gut_font16_cmap,
    };

    return &f;
}

/****************************************************************
 * Selection
 ****************************************************************/

void
gut_sel_clear(struct gut_sel *s)
{
    memset(s, 0, sizeof(*s));
}

static int
gut_sel_is_word(const struct gut_buf *b, int row, int col)
{
    const struct gut_cell *c = &b->cells[row * b->cols + col];

    return c->cp != ' ' && c->cp != 0 && c->cp != 0xA0;
}

/* Snap a point outward to the start (dir < 0) or end of its unit. */
static void
gut_sel_snap(const struct gut_sel *s, const struct gut_buf *b, int *row,
             int *col, int dir)
{
    if (s->unit == GUT_SEL_LINE) {
        *col = dir < 0 ? 0 : b->cols - 1;
    } else if (s->unit == GUT_SEL_WORD && gut_sel_is_word(b, *row, *col)) {
        while (*col + dir >= 0 && *col + dir < b->cols &&
               gut_sel_is_word(b, *row, *col + dir))
            *col += dir;
    }
}

static void
gut_sel_set(struct gut_sel *s, const struct gut_buf *b, int row, int col)
{
    int ar = s->anchor_row, ac = s->anchor_col;
    int before;

    row = gut_clamp(row, 0, b->rows - 1);
    col = gut_clamp(col, 0, b->cols - 1);
    if (s->mode == GUT_COPY_RECT) {
        s->row0 = ar < row ? ar : row;
        s->row1 = ar < row ? row : ar;
        s->col0 = ac < col ? ac : col;
        s->col1 = ac < col ? col : ac;
        if (s->unit != GUT_SEL_CELL) {
            s->col0 = 0;
            s->col1 = b->cols - 1;
        }
        return;
    }
    before = row < ar || (row == ar && col < ac);
    if (before) {
        s->row0 = row;
        s->col0 = col;
        s->row1 = ar;
        s->col1 = ac;
    } else {
        s->row0 = ar;
        s->col0 = ac;
        s->row1 = row;
        s->col1 = col;
    }
    gut_sel_snap(s, b, &s->row0, &s->col0, -1);
    gut_sel_snap(s, b, &s->row1, &s->col1, 1);
    /* a continuation cell belongs with its wide character */
    if (s->col0 > 0 && b->cells[s->row0 * b->cols + s->col0].width == 0)
        s->col0--;
    if (s->col1 + 1 < b->cols &&
        b->cells[s->row1 * b->cols + s->col1 + 1].width == 0)
        s->col1++;
}

void
gut_sel_begin(struct gut_sel *s, const struct gut_buf *b, int row, int col,
              int mode, int unit)
{
    memset(s, 0, sizeof(*s));
    s->active = 1;
    s->mode = mode == GUT_COPY_RECT ? GUT_COPY_RECT : GUT_COPY_STREAM;
    s->unit = gut_clamp(unit, GUT_SEL_CELL, GUT_SEL_LINE);
    s->anchor_row = gut_clamp(row, 0, b->rows - 1);
    s->anchor_col = gut_clamp(col, 0, b->cols - 1);
    gut_sel_set(s, b, row, col);
}

void
gut_sel_extend(struct gut_sel *s, const struct gut_buf *b, int row, int col)
{
    if (!s->active)
        return;
    s->anchor_row = gut_clamp(s->anchor_row, 0, b->rows - 1);
    s->anchor_col = gut_clamp(s->anchor_col, 0, b->cols - 1);
    gut_sel_set(s, b, row, col);
}

int
gut_sel_mouse(struct gut_sel *s, const struct gut_buf *b,
              const struct gut_event *ev)
{
    switch (ev->type) {
    case GUT_EVENT_MOUSE_DOWN:
        if (ev->button != GUT_BUTTON_LEFT)
            return 0;
        if ((ev->mods & GUT_MOD_SHIFT) && s->active) {
            /* the end nearer the click moves; the other end anchors */
            int far_row = s->row1, far_col = s->col1;
            int d0 = (ev->row - s->row0) * b->cols + ev->col - s->col0;
            int d1 = (ev->row - s->row1) * b->cols + ev->col - s->col1;

            if (d0 < 0)
                d0 = -d0;
            if (d1 < 0)
                d1 = -d1;
            if (d1 < d0) {
                far_row = s->row0;
                far_col = s->col0;
            }
            s->anchor_row = far_row;
            s->anchor_col = far_col;
            s->dragging = 1;
            gut_sel_extend(s, b, ev->row, ev->col);
            return 1;
        }
        {
            int unit = ev->clicks >= 3 ? GUT_SEL_LINE
                     : ev->clicks == 2 ? GUT_SEL_WORD : GUT_SEL_CELL;
            int was_active = s->active;

            gut_sel_begin(s, b, ev->row, ev->col,
                          (ev->mods & GUT_MOD_ALT) ? GUT_COPY_RECT
                                                   : GUT_COPY_STREAM, unit);
            s->dragging = 1;
            if (unit == GUT_SEL_CELL) {
                /* a single click only clears until it is dragged */
                s->active = 0;
                return was_active;
            }
            return 1;
        }
    case GUT_EVENT_MOUSE_MOVE:
        if (!s->dragging || ev->button != GUT_BUTTON_LEFT)
            return 0;
        if (!s->active) {
            if (ev->row == s->anchor_row && ev->col == s->anchor_col)
                return 0;
            s->active = 1;
        }
        gut_sel_extend(s, b, ev->row, ev->col);
        return 1;
    case GUT_EVENT_MOUSE_UP:
        if (!s->dragging || ev->button != GUT_BUTTON_LEFT)
            return 0;
        s->dragging = 0;
        if (s->active)
            gut_sel_extend(s, b, ev->row, ev->col);
        return s->active;
    default:
        return 0;
    }
}

int
gut_sel_contains(const struct gut_sel *s, int row, int col)
{
    if (!s->active)
        return 0;
    if (s->mode == GUT_COPY_RECT)
        return row >= s->row0 && row <= s->row1 &&
               col >= s->col0 && col <= s->col1;
    if (row < s->row0 || row > s->row1)
        return 0;
    if (row == s->row0 && col < s->col0)
        return 0;
    if (row == s->row1 && col > s->col1)
        return 0;
    return 1;
}

size_t
gut_sel_text(const struct gut_sel *s, const struct gut_buf *b, char *out,
             size_t n)
{
    if (!s->active) {
        if (n)
            out[0] = '\0';
        return 0;
    }
    return gut_buf_copy_text(b, s->row0, s->col0, s->row1, s->col1, s->mode,
                             out, n);
}

#ifndef GUTERM_NO_WINDOW

#include <SDL3/SDL.h>

/****************************************************************
 * OpenGL loader. Only the ES 2.0 subset is used, so the same entry
 * points resolve on ES 2, ES 3 and desktop compatibility contexts.
 * No GL header is needed: types and constants are declared here.
 ****************************************************************/

#if defined(_WIN32) && !defined(__CYGWIN__)
#define GUT_GLAPI __stdcall
#else
#define GUT_GLAPI
#endif

typedef unsigned int gut_GLenum;
typedef unsigned int gut_GLuint;
typedef int gut_GLint;
typedef int gut_GLsizei;
typedef float gut_GLfloat;
typedef unsigned char gut_GLboolean;
typedef unsigned char gut_GLubyte;
typedef unsigned int gut_GLbitfield;
typedef char gut_GLchar;
typedef ptrdiff_t gut_GLsizeiptr;

#define GUT_GL_FALSE                0
#define GUT_GL_TRUE                 1
#define GUT_GL_TRIANGLES            0x0004
#define GUT_GL_DEPTH_TEST           0x0B71
#define GUT_GL_CULL_FACE            0x0B44
#define GUT_GL_BLEND                0x0BE2
#define GUT_GL_SCISSOR_TEST         0x0C11
#define GUT_GL_UNPACK_ALIGNMENT     0x0CF5
#define GUT_GL_TEXTURE_2D           0x0DE1
#define GUT_GL_UNSIGNED_BYTE        0x1401
#define GUT_GL_FLOAT                0x1406
#define GUT_GL_RGBA                 0x1908
#define GUT_GL_VERSION              0x1F02
#define GUT_GL_NEAREST              0x2600
#define GUT_GL_TEXTURE_MAG_FILTER   0x2800
#define GUT_GL_TEXTURE_MIN_FILTER   0x2801
#define GUT_GL_TEXTURE_WRAP_S       0x2802
#define GUT_GL_TEXTURE_WRAP_T       0x2803
#define GUT_GL_SRC_ALPHA            0x0302
#define GUT_GL_ONE_MINUS_SRC_ALPHA  0x0303
#define GUT_GL_COLOR_BUFFER_BIT     0x4000
#define GUT_GL_CLAMP_TO_EDGE        0x812F
#define GUT_GL_TEXTURE0             0x84C0
#define GUT_GL_ARRAY_BUFFER         0x8892
#define GUT_GL_STREAM_DRAW          0x88E0
#define GUT_GL_FRAGMENT_SHADER      0x8B30
#define GUT_GL_VERTEX_SHADER        0x8B31
#define GUT_GL_COMPILE_STATUS       0x8B81
#define GUT_GL_LINK_STATUS          0x8B82

#define GUT_GL_FUNCS(X) \
    X(const gut_GLubyte *, glGetString, (gut_GLenum)) \
    X(void, glEnable, (gut_GLenum)) \
    X(void, glDisable, (gut_GLenum)) \
    X(void, glBlendFunc, (gut_GLenum, gut_GLenum)) \
    X(void, glViewport, (gut_GLint, gut_GLint, gut_GLsizei, gut_GLsizei)) \
    X(void, glClearColor, (gut_GLfloat, gut_GLfloat, gut_GLfloat, \
                           gut_GLfloat)) \
    X(void, glClear, (gut_GLbitfield)) \
    X(void, glPixelStorei, (gut_GLenum, gut_GLint)) \
    X(void, glGenTextures, (gut_GLsizei, gut_GLuint *)) \
    X(void, glDeleteTextures, (gut_GLsizei, const gut_GLuint *)) \
    X(void, glBindTexture, (gut_GLenum, gut_GLuint)) \
    X(void, glActiveTexture, (gut_GLenum)) \
    X(void, glTexImage2D, (gut_GLenum, gut_GLint, gut_GLint, gut_GLsizei, \
                           gut_GLsizei, gut_GLint, gut_GLenum, gut_GLenum, \
                           const void *)) \
    X(void, glTexParameteri, (gut_GLenum, gut_GLenum, gut_GLint)) \
    X(gut_GLuint, glCreateShader, (gut_GLenum)) \
    X(void, glDeleteShader, (gut_GLuint)) \
    X(void, glShaderSource, (gut_GLuint, gut_GLsizei, \
                             const gut_GLchar *const *, const gut_GLint *)) \
    X(void, glCompileShader, (gut_GLuint)) \
    X(void, glGetShaderiv, (gut_GLuint, gut_GLenum, gut_GLint *)) \
    X(void, glGetShaderInfoLog, (gut_GLuint, gut_GLsizei, gut_GLsizei *, \
                                 gut_GLchar *)) \
    X(gut_GLuint, glCreateProgram, (void)) \
    X(void, glDeleteProgram, (gut_GLuint)) \
    X(void, glAttachShader, (gut_GLuint, gut_GLuint)) \
    X(void, glBindAttribLocation, (gut_GLuint, gut_GLuint, \
                                   const gut_GLchar *)) \
    X(void, glLinkProgram, (gut_GLuint)) \
    X(void, glGetProgramiv, (gut_GLuint, gut_GLenum, gut_GLint *)) \
    X(void, glGetProgramInfoLog, (gut_GLuint, gut_GLsizei, gut_GLsizei *, \
                                  gut_GLchar *)) \
    X(void, glUseProgram, (gut_GLuint)) \
    X(gut_GLint, glGetUniformLocation, (gut_GLuint, const gut_GLchar *)) \
    X(void, glUniform1i, (gut_GLint, gut_GLint)) \
    X(void, glUniform2f, (gut_GLint, gut_GLfloat, gut_GLfloat)) \
    X(void, glGenBuffers, (gut_GLsizei, gut_GLuint *)) \
    X(void, glDeleteBuffers, (gut_GLsizei, const gut_GLuint *)) \
    X(void, glBindBuffer, (gut_GLenum, gut_GLuint)) \
    X(void, glBufferData, (gut_GLenum, gut_GLsizeiptr, const void *, \
                           gut_GLenum)) \
    X(void, glEnableVertexAttribArray, (gut_GLuint)) \
    X(void, glVertexAttribPointer, (gut_GLuint, gut_GLint, gut_GLenum, \
                                    gut_GLboolean, gut_GLsizei, \
                                    const void *)) \
    X(void, glDrawArrays, (gut_GLenum, gut_GLint, gut_GLsizei))

#define GUT_GL_DECL(ret, name, args) ret (GUT_GLAPI *name) args;
static struct {
    GUT_GL_FUNCS(GUT_GL_DECL)
} gut_gl;
#undef GUT_GL_DECL

#define GUT_GL(fn) (gut_gl.fn)

static char *
gut_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);

    if (p)
        memcpy(p, s, n);
    return p;
}

static char gut_errbuf[1024];

const char *
gut_error(void)
{
    return gut_errbuf;
}

static void
gut_set_error(const char *what, const char *detail)
{
    snprintf(gut_errbuf, sizeof(gut_errbuf), "%s%s%s", what,
             detail && detail[0] ? ": " : "", detail ? detail : "");
}

/* Returns the name of the first missing entry point, or NULL. */
static const char *
gut_gl_load(void)
{
#define GUT_GL_LOAD(ret, name, args) \
    gut_gl.name = (ret (GUT_GLAPI *) args)SDL_GL_GetProcAddress(#name); \
    if (!gut_gl.name) \
        return #name;
    GUT_GL_FUNCS(GUT_GL_LOAD)
#undef GUT_GL_LOAD
    return NULL;
}

/****************************************************************
 * Window
 ****************************************************************/

struct gut_vertex {
    float x, y;
    float u, v;
    uint8_t r, g, b, a;
};

struct gut_window {
    SDL_Window *win;
    SDL_GLContext ctx;
    int is_es;

    const struct gut_font *font;
    int scale;
    int cell_w, cell_h;         /* pixels, after scaling */
    int px_w, px_h;             /* framebuffer size */
    int cols, rows;             /* grid that fits the framebuffer */

    uint32_t palette[256];
    uint32_t def_fg, def_bg;

    gut_GLuint prog, tex, vbo;
    gut_GLint u_screen, u_tex;
    int atlas_w, atlas_h, atlas_per_row;
    int glyph_fallback;         /* glyph for codepoints the font lacks */

    struct gut_vertex *verts;
    size_t nverts, cap;

    char *clip;                 /* last clipboard text handed out */
    char *primary;              /* last primary selection handed out */
    char *event_text;           /* data of the last TEXT or PASTE event */
    char *preedit;              /* IME composition in progress, or NULL */
    int preedit_cursor;
    int paste_keys;
    int overlay;
    int owns_video;             /* gut_open initialised SDL video */
    int owns_gamepad;
    uint64_t t0;
    int focused;

    struct gut_sel sel;         /* highlighted selection, if sel.active */

    SDL_Gamepad *pads[GUT_MAX_PADS];
    SDL_JoystickID pad_ids[GUT_MAX_PADS];
    struct gut_pad pad_state[GUT_MAX_PADS];
};

static const uint32_t gut_default_palette[16] = {
    0x000000, 0xCD0000, 0x00CD00, 0xCDCD00,
    0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
    0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

static const char gut_vert_src[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_col;\n"
    "uniform vec2 u_screen;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    vec2 ndc = vec2(a_pos.x / u_screen.x * 2.0 - 1.0,\n"
    "                    1.0 - a_pos.y / u_screen.y * 2.0);\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "    v_col = a_col;\n"
    "}\n";

static const char gut_frag_src[] =
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    gl_FragColor = v_col * texture2D(u_tex, v_uv);\n"
    "}\n";

void
gut_set_palette(gut_window *w, const uint32_t *palette16)
{
    static const uint8_t cube[6] = { 0, 95, 135, 175, 215, 255 };

    for (int i = 0; i < 16; i++)
        w->palette[i] = palette16 ? palette16[i] : gut_default_palette[i];
    for (int i = 0; i < 216; i++)
        w->palette[16 + i] = ((uint32_t)cube[i / 36] << 16) |
                             ((uint32_t)cube[(i / 6) % 6] << 8) |
                             cube[i % 6];
    for (int i = 0; i < 24; i++) {
        uint32_t v = (uint32_t)(8 + 10 * i);

        w->palette[232 + i] = (v << 16) | (v << 8) | v;
    }
}

void
gut_set_defaults(gut_window *w, uint32_t fg, uint32_t bg)
{
    w->def_fg = fg;
    w->def_bg = bg;
}

/* Compile the shaders with the version prefix this context needs. The
 * sources are GLSL ES 1.00, which is also GLSL 1.10 as far as desktop
 * compatibility contexts are concerned. */
static gut_GLuint
gut_compile(gut_window *w, gut_GLenum type, const char *src)
{
    const char *pieces[3];
    gut_GLuint sh;
    gut_GLint ok = 0;
    int n = 0;

    if (w->is_es) {
        pieces[n++] = "#version 100\n";
        if (type == GUT_GL_FRAGMENT_SHADER)
            pieces[n++] = "precision mediump float;\n";
    }
    pieces[n++] = src;
    sh = GUT_GL(glCreateShader)(type);
    GUT_GL(glShaderSource)(sh, n, pieces, NULL);
    GUT_GL(glCompileShader)(sh);
    GUT_GL(glGetShaderiv)(sh, GUT_GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        GUT_GL(glGetShaderInfoLog)(sh, sizeof(log), NULL, log);
        gut_set_error("shader compile failed", log);
        GUT_GL(glDeleteShader)(sh);
        return 0;
    }
    return sh;
}

static int
gut_make_program(gut_window *w)
{
    gut_GLuint vs, fs, prog;
    gut_GLint ok = 0;

    vs = gut_compile(w, GUT_GL_VERTEX_SHADER, gut_vert_src);
    if (!vs)
        return -1;
    fs = gut_compile(w, GUT_GL_FRAGMENT_SHADER, gut_frag_src);
    if (!fs) {
        GUT_GL(glDeleteShader)(vs);
        return -1;
    }
    prog = GUT_GL(glCreateProgram)();
    GUT_GL(glAttachShader)(prog, vs);
    GUT_GL(glAttachShader)(prog, fs);
    GUT_GL(glBindAttribLocation)(prog, 0, "a_pos");
    GUT_GL(glBindAttribLocation)(prog, 1, "a_uv");
    GUT_GL(glBindAttribLocation)(prog, 2, "a_col");
    GUT_GL(glLinkProgram)(prog);
    GUT_GL(glDeleteShader)(vs);
    GUT_GL(glDeleteShader)(fs);
    GUT_GL(glGetProgramiv)(prog, GUT_GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];

        GUT_GL(glGetProgramInfoLog)(prog, sizeof(log), NULL, log);
        gut_set_error("shader link failed", log);
        GUT_GL(glDeleteProgram)(prog);
        return -1;
    }
    w->prog = prog;
    w->u_screen = GUT_GL(glGetUniformLocation)(prog, "u_screen");
    w->u_tex = GUT_GL(glGetUniformLocation)(prog, "u_tex");
    return 0;
}

/* Bake the font into an RGBA atlas. Slot 0 is a solid block used for
 * backgrounds and rules; glyph g lives in slot g + 1. */
static int
gut_make_atlas(gut_window *w)
{
    const struct gut_font *f = w->font;
    int gw = f->glyph_w, gh = f->glyph_h;
    int stride = (gw + 7) / 8;
    int slots = f->nglyphs + 1;
    uint8_t *rgba;

    w->atlas_per_row = 256 / gw;
    if (w->atlas_per_row < 1)
        w->atlas_per_row = 1;
    w->atlas_w = w->atlas_per_row * gw;
    w->atlas_h = ((slots + w->atlas_per_row - 1) / w->atlas_per_row) * gh;
    rgba = calloc((size_t)w->atlas_w * (size_t)w->atlas_h, 4);
    if (!rgba) {
        gut_set_error("atlas allocation failed", NULL);
        return -1;
    }
    for (int s = 0; s < slots; s++) {
        int sx = (s % w->atlas_per_row) * gw;
        int sy = (s / w->atlas_per_row) * gh;
        const uint8_t *bits = s ? f->bits + (size_t)(s - 1) * gh * stride
                                : NULL;

        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                int on = bits ? (bits[y * stride + x / 8] >> (x % 8)) & 1
                              : 1;
                uint8_t *p = &rgba[((size_t)(sy + y) * w->atlas_w +
                                    (size_t)(sx + x)) * 4];

                p[0] = p[1] = p[2] = 255;
                p[3] = on ? 255 : 0;
            }
        }
    }
    GUT_GL(glGenTextures)(1, &w->tex);
    GUT_GL(glBindTexture)(GUT_GL_TEXTURE_2D, w->tex);
    GUT_GL(glPixelStorei)(GUT_GL_UNPACK_ALIGNMENT, 1);
    GUT_GL(glTexImage2D)(GUT_GL_TEXTURE_2D, 0, GUT_GL_RGBA, w->atlas_w,
                         w->atlas_h, 0, GUT_GL_RGBA, GUT_GL_UNSIGNED_BYTE,
                         rgba);
    GUT_GL(glTexParameteri)(GUT_GL_TEXTURE_2D, GUT_GL_TEXTURE_MIN_FILTER,
                            GUT_GL_NEAREST);
    GUT_GL(glTexParameteri)(GUT_GL_TEXTURE_2D, GUT_GL_TEXTURE_MAG_FILTER,
                            GUT_GL_NEAREST);
    GUT_GL(glTexParameteri)(GUT_GL_TEXTURE_2D, GUT_GL_TEXTURE_WRAP_S,
                            GUT_GL_CLAMP_TO_EDGE);
    GUT_GL(glTexParameteri)(GUT_GL_TEXTURE_2D, GUT_GL_TEXTURE_WRAP_T,
                            GUT_GL_CLAMP_TO_EDGE);
    free(rgba);

    w->glyph_fallback = gut_font_lookup(f, 0xFFFD);
    if (w->glyph_fallback < 0)
        w->glyph_fallback = gut_font_lookup(f, '?');
    return 0;
}

static void
gut_update_grid(gut_window *w)
{
    SDL_GetWindowSizeInPixels(w->win, &w->px_w, &w->px_h);
    w->cols = w->px_w / w->cell_w;
    w->rows = w->px_h / w->cell_h;
    if (w->cols < 1)
        w->cols = 1;
    if (w->rows < 1)
        w->rows = 1;
}

/* Try for an ES 2.0 context first, which is the native path on Linux
 * and small boards, then whatever the platform offers by default, which
 * is a desktop compatibility or legacy context on Windows and macOS. */
static int
gut_create_context(gut_window *w, const char *title, int pw, int ph,
                   int fixed)
{
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const char *missing;
    const char *ver;

    if (!fixed)
        flags |= SDL_WINDOW_RESIZABLE;

    for (int attempt = 0; attempt < 2 && !w->ctx; attempt++) {
        SDL_GL_ResetAttributes();
        if (attempt == 0) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        w->win = SDL_CreateWindow(title, pw, ph, flags);
        if (!w->win) {
            gut_set_error("SDL_CreateWindow", SDL_GetError());
            continue;
        }
        w->ctx = SDL_GL_CreateContext(w->win);
        if (!w->ctx) {
            gut_set_error("SDL_GL_CreateContext", SDL_GetError());
            SDL_DestroyWindow(w->win);
            w->win = NULL;
        }
    }
    if (!w->ctx)
        return -1;
    SDL_GL_MakeCurrent(w->win, w->ctx);
    missing = gut_gl_load();
    if (missing) {
        gut_set_error("missing GL entry point", missing);
        return -1;
    }
    ver = (const char *)GUT_GL(glGetString)(GUT_GL_VERSION);
    w->is_es = ver && strncmp(ver, "OpenGL ES", 9) == 0;
    return 0;
}

gut_window *
gut_open(const struct gut_desc *desc)
{
    struct gut_desc d = { 0 };
    gut_window *w;
    int cols, rows;

    if (desc)
        d = *desc;
    gut_errbuf[0] = '\0';
    w = calloc(1, sizeof(*w));
    if (!w) {
        gut_set_error("out of memory", NULL);
        return NULL;
    }
    w->font = d.font ? d.font : gut_font_default();
    cols = d.cols > 0 ? d.cols : 80;
    rows = d.rows > 0 ? d.rows : 25;
    gut_set_palette(w, d.palette);
    w->def_fg = d.fg ? d.fg : 0xD0D0D0;
    w->def_bg = d.bg ? d.bg : 0x000000;
    w->focused = 1;
    w->paste_keys = !d.no_paste_keys;
    w->overlay = !d.no_compose_overlay;

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            gut_set_error("SDL_Init", SDL_GetError());
            free(w);
            return NULL;
        }
        w->owns_video = 1;
    }
    if (!d.no_gamepad && !SDL_WasInit(SDL_INIT_GAMEPAD) &&
        SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        w->owns_gamepad = 1;    /* failure just means no pads */
    /* Zoom: as asked, else 1 on an ordinary display and the rounded
     * content scale on a high density one, so text is about the same
     * physical size everywhere. */
    if (d.scale > 0) {
        w->scale = d.scale;
    } else {
        float cs = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

        w->scale = cs >= 1.5f ? (int)(cs + 0.5f) : 1;
    }
    w->cell_w = w->font->glyph_w * w->scale;
    w->cell_h = w->font->glyph_h * w->scale;
    if (gut_create_context(w, d.title ? d.title : "guterm",
                           cols * w->cell_w, rows * w->cell_h,
                           d.fixed_size) != 0 ||
        gut_make_program(w) != 0 || gut_make_atlas(w) != 0) {
        gut_close(w);
        return NULL;
    }
    GUT_GL(glGenBuffers)(1, &w->vbo);
    GUT_GL(glDisable)(GUT_GL_DEPTH_TEST);
    GUT_GL(glDisable)(GUT_GL_CULL_FACE);
    GUT_GL(glDisable)(GUT_GL_SCISSOR_TEST);
    GUT_GL(glEnable)(GUT_GL_BLEND);
    GUT_GL(glBlendFunc)(GUT_GL_SRC_ALPHA, GUT_GL_ONE_MINUS_SRC_ALPHA);

    /* Size in points was requested; correct for pixel density so the
     * grid is exactly what was asked for, then show a cleared frame so
     * no stale video memory is ever presented. */
    gut_set_grid_size(w, cols, rows);
    gut_update_grid(w);
    SDL_GL_SetSwapInterval(0);
    for (int i = 0; i < 2; i++) {
        GUT_GL(glViewport)(0, 0, w->px_w, w->px_h);
        GUT_GL(glClearColor)(((w->def_bg >> 16) & 0xFF) / 255.0f,
                             ((w->def_bg >> 8) & 0xFF) / 255.0f,
                             (w->def_bg & 0xFF) / 255.0f, 1.0f);
        GUT_GL(glClear)(GUT_GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(w->win);
    }
    SDL_ShowWindow(w->win);
    SDL_GL_SetSwapInterval(1);
    SDL_StartTextInput(w->win);
    w->t0 = SDL_GetTicks();
    return w;
}

void
gut_close(gut_window *w)
{
    if (!w)
        return;
    if (w->ctx) {
        SDL_GL_MakeCurrent(w->win, w->ctx);
        if (w->vbo)
            GUT_GL(glDeleteBuffers)(1, &w->vbo);
        if (w->tex)
            GUT_GL(glDeleteTextures)(1, &w->tex);
        if (w->prog)
            GUT_GL(glDeleteProgram)(w->prog);
        SDL_GL_DestroyContext(w->ctx);
    }
    if (w->win)
        SDL_DestroyWindow(w->win);
    if (w->clip)
        SDL_free(w->clip);
    if (w->primary)
        SDL_free(w->primary);
    free(w->event_text);
    free(w->preedit);
    free(w->verts);
    for (int i = 0; i < GUT_MAX_PADS; i++)
        if (w->pads[i])
            SDL_CloseGamepad(w->pads[i]);
    if (w->owns_gamepad)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (w->owns_video)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(w);
}

void
gut_set_selection(gut_window *w, const struct gut_sel *sel)
{
    if (sel)
        w->sel = *sel;
    else
        gut_sel_clear(&w->sel);
}

int
gut_pad_get(const gut_window *w, int slot, struct gut_pad *out)
{
    if (slot < 0 || slot >= GUT_MAX_PADS) {
        if (out)
            memset(out, 0, sizeof(*out));
        return 0;
    }
    if (out)
        *out = w->pad_state[slot];
    return w->pad_state[slot].connected;
}

int
gut_pad_rumble(gut_window *w, int slot, uint16_t low, uint16_t high,
               uint32_t ms)
{
    if (slot < 0 || slot >= GUT_MAX_PADS || !w->pads[slot])
        return -1;
    return SDL_RumbleGamepad(w->pads[slot], low, high, ms) ? 0 : -1;
}

/* ---- game controllers ---- */

static int
gut_pad_slot(const gut_window *w, SDL_JoystickID id)
{
    for (int i = 0; i < GUT_MAX_PADS; i++)
        if (w->pads[i] && w->pad_ids[i] == id)
            return i;
    return -1;
}

static int
gut_pad_button_from_sdl(int b)
{
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH:          return GUT_PAD_A;
    case SDL_GAMEPAD_BUTTON_EAST:           return GUT_PAD_B;
    case SDL_GAMEPAD_BUTTON_WEST:           return GUT_PAD_X;
    case SDL_GAMEPAD_BUTTON_NORTH:          return GUT_PAD_Y;
    case SDL_GAMEPAD_BUTTON_BACK:           return GUT_PAD_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE:          return GUT_PAD_GUIDE;
    case SDL_GAMEPAD_BUTTON_START:          return GUT_PAD_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return GUT_PAD_LSTICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return GUT_PAD_RSTICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return GUT_PAD_LSHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return GUT_PAD_RSHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return GUT_PAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return GUT_PAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return GUT_PAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return GUT_PAD_RIGHT;
    default:                                return -1;
    }
}

static int
gut_pad_axis_from_sdl(int a)
{
    switch (a) {
    case SDL_GAMEPAD_AXIS_LEFTX:         return GUT_PAD_AXIS_LX;
    case SDL_GAMEPAD_AXIS_LEFTY:         return GUT_PAD_AXIS_LY;
    case SDL_GAMEPAD_AXIS_RIGHTX:        return GUT_PAD_AXIS_RX;
    case SDL_GAMEPAD_AXIS_RIGHTY:        return GUT_PAD_AXIS_RY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return GUT_PAD_AXIS_LT;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return GUT_PAD_AXIS_RT;
    default:                             return -1;
    }
}

/* Translate a gamepad event; returns 1 when ev was filled. */
static int
gut_translate_pad(gut_window *w, const SDL_Event *e, struct gut_event *ev)
{
    int slot;

    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *pad;
        const char *name;

        if (gut_pad_slot(w, e->gdevice.which) >= 0)
            return 0;
        for (slot = 0; slot < GUT_MAX_PADS && w->pads[slot]; slot++)
            ;
        if (slot == GUT_MAX_PADS)
            return 0;
        pad = SDL_OpenGamepad(e->gdevice.which);
        if (!pad)
            return 0;
        w->pads[slot] = pad;
        w->pad_ids[slot] = e->gdevice.which;
        memset(&w->pad_state[slot], 0, sizeof(w->pad_state[slot]));
        w->pad_state[slot].connected = 1;
        name = SDL_GetGamepadName(pad);
        snprintf(w->pad_state[slot].name, sizeof(w->pad_state[slot].name),
                 "%s", name ? name : "");
        ev->type = GUT_EVENT_PAD_ADDED;
        ev->pad = slot;
        return 1;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        slot = gut_pad_slot(w, e->gdevice.which);
        if (slot < 0)
            return 0;
        SDL_CloseGamepad(w->pads[slot]);
        w->pads[slot] = NULL;
        memset(&w->pad_state[slot], 0, sizeof(w->pad_state[slot]));
        ev->type = GUT_EVENT_PAD_REMOVED;
        ev->pad = slot;
        return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int button = gut_pad_button_from_sdl(e->gbutton.button);

        slot = gut_pad_slot(w, e->gbutton.which);
        if (slot < 0 || button < 0)
            return 0;
        if (e->gbutton.down)
            w->pad_state[slot].buttons |= 1u << button;
        else
            w->pad_state[slot].buttons &= ~(1u << button);
        ev->type = e->gbutton.down ? GUT_EVENT_PAD_DOWN : GUT_EVENT_PAD_UP;
        ev->pad = slot;
        ev->button = button;
        return 1;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int axis = gut_pad_axis_from_sdl(e->gaxis.axis);

        slot = gut_pad_slot(w, e->gaxis.which);
        if (slot < 0 || axis < 0)
            return 0;
        w->pad_state[slot].axes[axis] = e->gaxis.value;
        ev->type = GUT_EVENT_PAD_AXIS;
        ev->pad = slot;
        ev->axis = axis;
        ev->value = e->gaxis.value;
        return 1;
    }
    default:
        return 0;
    }
}

void
gut_grid_size(const gut_window *w, int *cols, int *rows)
{
    if (cols)
        *cols = w->cols;
    if (rows)
        *rows = w->rows;
}

void
gut_set_grid_size(gut_window *w, int cols, int rows)
{
    float density = SDL_GetWindowPixelDensity(w->win);
    int pw = cols * w->cell_w, ph = rows * w->cell_h;

    if (density <= 0.0f)
        density = 1.0f;
    SDL_SetWindowSize(w->win, (int)(pw / density + 0.5f),
                      (int)(ph / density + 0.5f));
    gut_update_grid(w);
}

void
gut_set_title(gut_window *w, const char *title)
{
    SDL_SetWindowTitle(w->win, title ? title : "");
}

const char *
gut_clipboard_get(gut_window *w)
{
    if (w->clip)
        SDL_free(w->clip);
    w->clip = SDL_GetClipboardText();
    return (w->clip && w->clip[0]) ? w->clip : NULL;
}

void
gut_clipboard_set(gut_window *w, const char *utf8)
{
    (void)w;
    SDL_SetClipboardText(utf8 ? utf8 : "");
}

const char *
gut_primary_get(gut_window *w)
{
    if (w->primary)
        SDL_free(w->primary);
    w->primary = SDL_HasPrimarySelectionText() ? SDL_GetPrimarySelectionText()
                                               : NULL;
    return (w->primary && w->primary[0]) ? w->primary : NULL;
}

void
gut_primary_set(gut_window *w, const char *utf8)
{
    (void)w;
    SDL_SetPrimarySelectionText(utf8 ? utf8 : "");
}

void
gut_set_text_input(gut_window *w, int on)
{
    if (on)
        SDL_StartTextInput(w->win);
    else
        SDL_StopTextInput(w->win);
    free(w->preedit);
    w->preedit = NULL;
}

void
gut_set_compose_overlay(gut_window *w, int on)
{
    w->overlay = on;
}

uint64_t
gut_ticks(const gut_window *w)
{
    return SDL_GetTicks() - w->t0;
}

/****************************************************************
 * Rendering
 ****************************************************************/

static uint32_t
gut_resolve(const gut_window *w, struct gut_color c, int is_fg, int bold)
{
    switch (c.type) {
    case GUT_COLOR_INDEXED:
        if (bold && c.index < 8)
            return w->palette[c.index + 8];
        return w->palette[c.index];
    case GUT_COLOR_RGB:
        return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    default:
        return is_fg ? w->def_fg : w->def_bg;
    }
}

static uint32_t
gut_dim(uint32_t rgb)
{
    uint32_t r = ((rgb >> 16) & 0xFF) * 2 / 3;
    uint32_t g = ((rgb >> 8) & 0xFF) * 2 / 3;
    uint32_t b = (rgb & 0xFF) * 2 / 3;

    return (r << 16) | (g << 8) | b;
}

/* Effective foreground and background of a cell after attributes. */
static void
gut_cell_colors(const gut_window *w, const struct gut_cell *c,
                uint32_t *fg, uint32_t *bg)
{
    int bold = (c->attrs & GUT_ATTR_BOLD) != 0;
    uint32_t f = gut_resolve(w, c->fg, 1, bold);
    uint32_t b = gut_resolve(w, c->bg, 0, 0);

    if (c->attrs & GUT_ATTR_DIM)
        f = gut_dim(f);
    if (c->attrs & GUT_ATTR_REVERSE) {
        uint32_t t = f;

        f = b;
        b = t;
    }
    if (c->attrs & GUT_ATTR_HIDDEN)
        f = b;
    *fg = f;
    *bg = b;
}

static struct gut_vertex *
gut_reserve(gut_window *w, size_t count)
{
    if (w->nverts + count > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 4096;
        struct gut_vertex *v;

        while (cap < w->nverts + count)
            cap *= 2;
        v = realloc(w->verts, cap * sizeof(*v));
        if (!v)
            return NULL;
        w->verts = v;
        w->cap = cap;
    }
    return &w->verts[w->nverts];
}

static void
gut_quad(gut_window *w, float x, float y, float qw, float qh,
         float u0, float v0, float u1, float v1, uint32_t rgb)
{
    struct gut_vertex *v = gut_reserve(w, 6);
    uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8);
    uint8_t b = (uint8_t)rgb;

    if (!v)
        return;
    v[0].x = x;      v[0].y = y;      v[0].u = u0; v[0].v = v0;
    v[1].x = x + qw; v[1].y = y;      v[1].u = u1; v[1].v = v0;
    v[2].x = x + qw; v[2].y = y + qh; v[2].u = u1; v[2].v = v1;
    v[3].x = x;      v[3].y = y;      v[3].u = u0; v[3].v = v0;
    v[4].x = x + qw; v[4].y = y + qh; v[4].u = u1; v[4].v = v1;
    v[5].x = x;      v[5].y = y + qh; v[5].u = u0; v[5].v = v1;
    for (int i = 0; i < 6; i++) {
        v[i].r = r;
        v[i].g = g;
        v[i].b = b;
        v[i].a = 255;
    }
    w->nverts += 6;
}

/* Solid rectangle from the white slot of the atlas. */
static void
gut_rect(gut_window *w, float x, float y, float qw, float qh, uint32_t rgb)
{
    float u = (w->font->glyph_w * 0.5f) / (float)w->atlas_w;
    float v = (w->font->glyph_h * 0.5f) / (float)w->atlas_h;

    gut_quad(w, x, y, qw, qh, u, v, u, v, rgb);
}

static void
gut_glyph(gut_window *w, int slot, float x, float y, uint32_t rgb)
{
    int gw = w->font->glyph_w, gh = w->font->glyph_h;
    float u0 = (float)((slot % w->atlas_per_row) * gw) / (float)w->atlas_w;
    float v0 = (float)((slot / w->atlas_per_row) * gh) / (float)w->atlas_h;
    float u1 = u0 + (float)gw / (float)w->atlas_w;
    float v1 = v0 + (float)gh / (float)w->atlas_h;

    gut_quad(w, x, y, (float)w->cell_w, (float)w->cell_h, u0, v0, u1, v1,
             rgb);
}

static void
gut_draw_cell(gut_window *w, const struct gut_cell *c, int row, int col,
              int cursor)
{
    float x = (float)(col * w->cell_w), y = (float)(row * w->cell_h);
    float s = (float)w->scale;
    uint32_t fg, bg;
    int slot;

    gut_cell_colors(w, c, &fg, &bg);
    if (gut_sel_contains(&w->sel, row, col)) {
        uint32_t t = fg;

        fg = bg;
        bg = t;
    }
    if (cursor && w->focused && c->width != 0) {
        /* a block cursor inverts the cell; the other shapes overlay */
        if (cursor == GUT_CURSOR_BLOCK + 1) {
            uint32_t t = fg;

            fg = bg;
            bg = t;
            gut_rect(w, x, y, (float)w->cell_w, (float)w->cell_h, bg);
        }
    }
    if (c->width == 0 || c->cp == ' ' || c->cp == 0 ||
        (c->attrs & GUT_ATTR_HIDDEN)) {
        slot = -1;
    } else {
        slot = gut_font_lookup(w->font, c->cp);
        if (slot < 0)
            slot = w->glyph_fallback;
    }
    if (slot >= 0) {
        gut_glyph(w, slot + 1, x, y, fg);
        if (c->attrs & GUT_ATTR_BOLD)
            gut_glyph(w, slot + 1, x + s, y, fg);
    }
    if (c->attrs & GUT_ATTR_UNDERLINE)
        gut_rect(w, x, y + (float)w->cell_h - 2.0f * s, (float)w->cell_w, s,
                 fg);
    if (c->attrs & GUT_ATTR_STRIKE)
        gut_rect(w, x, y + (float)(w->cell_h / 2), (float)w->cell_w, s, fg);
    if (cursor && w->focused) {
        if (cursor == GUT_CURSOR_UNDERLINE + 1)
            gut_rect(w, x, y + (float)w->cell_h - 2.0f * s,
                     (float)w->cell_w, 2.0f * s, fg);
        else if (cursor == GUT_CURSOR_BAR + 1)
            gut_rect(w, x, y, s, (float)w->cell_h, fg);
    } else if (cursor) {
        /* unfocused: hollow box */
        gut_rect(w, x, y, (float)w->cell_w, s, fg);
        gut_rect(w, x, y + (float)w->cell_h - s, (float)w->cell_w, s, fg);
        gut_rect(w, x, y, s, (float)w->cell_h, fg);
        gut_rect(w, x + (float)w->cell_w - s, y, s, (float)w->cell_h, fg);
    }
}

/* Draw the IME composition from the cursor cell rightwards, underlined,
 * with a bar at the composition caret. It is an overlay: the buffer
 * underneath is untouched and the committed text replaces it. */
static void
gut_draw_preedit(gut_window *w, const struct gut_buf *b)
{
    const unsigned char *s = (const unsigned char *)w->preedit;
    size_t len = strlen(w->preedit);
    int row = b->cursor_row, col = b->cursor_col;
    int index = 0;
    float s1 = (float)w->scale;

    while (len > 0 && row < w->rows) {
        uint32_t cp;
        int n = gut_utf8_decode(&cp, s, len);
        int width = gut_rune_width(cp);
        struct gut_cell cell;
        float x, y;

        s += n;
        len -= (size_t)n;
        if (width <= 0)
            continue;
        if (col + width > w->cols) {
            row++;
            col = 0;
            if (row >= w->rows)
                break;
        }
        x = (float)(col * w->cell_w);
        y = (float)(row * w->cell_h);
        if (index == w->preedit_cursor)
            gut_rect(w, x, y, s1, (float)w->cell_h, w->def_fg);
        memset(&cell, 0, sizeof(cell));
        cell.cp = cp;
        cell.width = (uint8_t)width;
        cell.attrs = GUT_ATTR_UNDERLINE;
        gut_rect(w, x, y, (float)(w->cell_w * width), (float)w->cell_h,
                 w->def_bg);
        gut_draw_cell(w, &cell, row, col, 0);
        col += width;
        index++;
    }
    if (index == w->preedit_cursor && row < w->rows && col < w->cols)
        gut_rect(w, (float)(col * w->cell_w), (float)(row * w->cell_h), s1,
                 (float)w->cell_h, w->def_fg);
}

/* Tell the platform where the cursor is so an IME can place its
 * candidate list next to it. Coordinates are window points. */
static void
gut_text_input_area(gut_window *w, const struct gut_buf *b)
{
    float density = SDL_GetWindowPixelDensity(w->win);
    SDL_Rect r;

    if (density <= 0.0f)
        density = 1.0f;
    r.x = (int)(b->cursor_col * w->cell_w / density);
    r.y = (int)(b->cursor_row * w->cell_h / density);
    r.w = (int)(w->cell_w / density);
    r.h = (int)(w->cell_h / density);
    SDL_SetTextInputArea(w->win, &r, 0);
}

void
gut_present(gut_window *w, struct gut_buf *b)
{
    int rows = b->rows < w->rows ? b->rows : w->rows;
    int cols = b->cols < w->cols ? b->cols : w->cols;

    SDL_GL_MakeCurrent(w->win, w->ctx);
    w->nverts = 0;

    /* backgrounds first so glyph overhang (bold shift) stays on top */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const struct gut_cell *cell = &b->cells[r * b->cols + c];
            uint32_t fg, bg;

            if (cell->width == 0)
                continue;
            gut_cell_colors(w, cell, &fg, &bg);
            if (gut_sel_contains(&w->sel, r, c))
                bg = fg;
            if (bg == w->def_bg)
                continue;
            gut_rect(w, (float)(c * w->cell_w), (float)(r * w->cell_h),
                     (float)(w->cell_w * cell->width), (float)w->cell_h,
                     bg);
        }
    }
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const struct gut_cell *cell = &b->cells[r * b->cols + c];
            int cursor = 0;

            if (b->cursor_visible && r == b->cursor_row &&
                c == b->cursor_col)
                cursor = b->cursor_shape + 1;
            gut_draw_cell(w, cell, r, c, cursor);
        }
    }
    if (w->overlay && w->preedit && w->preedit[0])
        gut_draw_preedit(w, b);

    GUT_GL(glViewport)(0, 0, w->px_w, w->px_h);
    GUT_GL(glClearColor)(((w->def_bg >> 16) & 0xFF) / 255.0f,
                         ((w->def_bg >> 8) & 0xFF) / 255.0f,
                         (w->def_bg & 0xFF) / 255.0f, 1.0f);
    GUT_GL(glClear)(GUT_GL_COLOR_BUFFER_BIT);
    if (w->nverts > 0) {
        GUT_GL(glUseProgram)(w->prog);
        GUT_GL(glUniform2f)(w->u_screen, (float)w->px_w, (float)w->px_h);
        GUT_GL(glActiveTexture)(GUT_GL_TEXTURE0);
        GUT_GL(glBindTexture)(GUT_GL_TEXTURE_2D, w->tex);
        GUT_GL(glUniform1i)(w->u_tex, 0);
        GUT_GL(glBindBuffer)(GUT_GL_ARRAY_BUFFER, w->vbo);
        GUT_GL(glBufferData)(GUT_GL_ARRAY_BUFFER,
                             (gut_GLsizeiptr)(w->nverts * sizeof(*w->verts)),
                             w->verts, GUT_GL_STREAM_DRAW);
        GUT_GL(glEnableVertexAttribArray)(0);
        GUT_GL(glEnableVertexAttribArray)(1);
        GUT_GL(glEnableVertexAttribArray)(2);
        GUT_GL(glVertexAttribPointer)(0, 2, GUT_GL_FLOAT, GUT_GL_FALSE,
                                      sizeof(struct gut_vertex),
                                      (const void *)0);
        GUT_GL(glVertexAttribPointer)(1, 2, GUT_GL_FLOAT, GUT_GL_FALSE,
                                      sizeof(struct gut_vertex),
                                      (const void *)(2 * sizeof(float)));
        GUT_GL(glVertexAttribPointer)(2, 4, GUT_GL_UNSIGNED_BYTE, GUT_GL_TRUE,
                                      sizeof(struct gut_vertex),
                                      (const void *)(4 * sizeof(float)));
        GUT_GL(glDrawArrays)(GUT_GL_TRIANGLES, 0, (gut_GLsizei)w->nverts);
    }
    SDL_GL_SwapWindow(w->win);
    memset(b->dirty, 0, (size_t)b->rows);
    gut_text_input_area(w, b);
}

/****************************************************************
 * Events
 ****************************************************************/

static int
gut_mods_from_sdl(SDL_Keymod m)
{
    int mods = 0;

    if (m & SDL_KMOD_SHIFT)
        mods |= GUT_MOD_SHIFT;
    if (m & SDL_KMOD_CTRL)
        mods |= GUT_MOD_CTRL;
    if (m & SDL_KMOD_ALT)
        mods |= GUT_MOD_ALT;
    if (m & SDL_KMOD_GUI)
        mods |= GUT_MOD_SUPER;
    return mods;
}

static int
gut_key_from_sdl(SDL_Keycode kc)
{
    switch (kc) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return GUT_KEY_ENTER;
    case SDLK_ESCAPE:    return GUT_KEY_ESCAPE;
    case SDLK_BACKSPACE: return GUT_KEY_BACKSPACE;
    case SDLK_TAB:       return GUT_KEY_TAB;
    case SDLK_DELETE:    return GUT_KEY_DELETE;
    case SDLK_INSERT:    return GUT_KEY_INSERT;
    case SDLK_UP:        return GUT_KEY_UP;
    case SDLK_DOWN:      return GUT_KEY_DOWN;
    case SDLK_LEFT:      return GUT_KEY_LEFT;
    case SDLK_RIGHT:     return GUT_KEY_RIGHT;
    case SDLK_HOME:      return GUT_KEY_HOME;
    case SDLK_END:       return GUT_KEY_END;
    case SDLK_PAGEUP:    return GUT_KEY_PAGEUP;
    case SDLK_PAGEDOWN:  return GUT_KEY_PAGEDOWN;
    case SDLK_F1:  return GUT_KEY_F1;
    case SDLK_F2:  return GUT_KEY_F2;
    case SDLK_F3:  return GUT_KEY_F3;
    case SDLK_F4:  return GUT_KEY_F4;
    case SDLK_F5:  return GUT_KEY_F5;
    case SDLK_F6:  return GUT_KEY_F6;
    case SDLK_F7:  return GUT_KEY_F7;
    case SDLK_F8:  return GUT_KEY_F8;
    case SDLK_F9:  return GUT_KEY_F9;
    case SDLK_F10: return GUT_KEY_F10;
    case SDLK_F11: return GUT_KEY_F11;
    case SDLK_F12: return GUT_KEY_F12;
    default:
        break;
    }
    if (kc & SDLK_SCANCODE_MASK)
        return GUT_KEY_NONE;
    if (kc >= 0x20 && kc < GUT_KEY_SPECIAL)
        return (int)kc;
    return GUT_KEY_NONE;
}

static void
gut_mouse_pos(const gut_window *w, float px, float py, struct gut_event *ev)
{
    int ww = 1, wh = 1;
    float sx, sy;

    SDL_GetWindowSize(w->win, &ww, &wh);
    sx = ww > 0 ? (float)w->px_w / (float)ww : 1.0f;
    sy = wh > 0 ? (float)w->px_h / (float)wh : 1.0f;
    ev->x = (int)(px * sx);
    ev->y = (int)(py * sy);
    ev->col = ev->x / w->cell_w;
    ev->row = ev->y / w->cell_h;
}

/* Hand text to the program through data and len, owned by the window
 * until the next gut_poll(). */
static void
gut_set_event_text(gut_window *w, struct gut_event *ev, const char *text)
{
    size_t len = strlen(text);
    char *copy = gut_strdup(text);

    free(w->event_text);
    w->event_text = NULL;
    if (!copy) {
        ev->data = "";
        ev->len = 0;
        return;
    }
    w->event_text = copy;
    ev->data = copy;
    ev->len = len;
}

static int
gut_is_paste_chord(const struct gut_event *ev)
{
    if (ev->key == GUT_KEY_INSERT && ev->mods == GUT_MOD_SHIFT)
        return 1;
#if defined(__APPLE__)
    return ev->key == 'v' && ev->mods == GUT_MOD_SUPER;
#else
    return ev->key == 'v' && ev->mods == (GUT_MOD_CTRL | GUT_MOD_SHIFT);
#endif
}

/* Turn the event into a paste of text, when there is any. The mouse or
 * key fields already filled stay, so a primary paste reports its cell. */
static int
gut_paste_event(gut_window *w, struct gut_event *ev, const char *text,
                int primary)
{
    if (!text || !text[0])
        return 0;
    ev->type = GUT_EVENT_PASTE;
    ev->primary = primary;
    gut_set_event_text(w, ev, text);
    return 1;
}

static int
gut_translate(gut_window *w, const SDL_Event *e, struct gut_event *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->cursor = -1;
    ev->mods = gut_mods_from_sdl(SDL_GetModState());

    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        ev->type = GUT_EVENT_QUIT;
        return 1;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED: {
        int cols = w->cols, rows = w->rows;

        gut_update_grid(w);
        if (cols == w->cols && rows == w->rows)
            return 0;
        ev->type = GUT_EVENT_RESIZE;
        ev->cols = w->cols;
        ev->rows = w->rows;
        return 1;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        w->focused = 1;
        ev->type = GUT_EVENT_FOCUS_IN;
        return 1;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        w->focused = 0;
        ev->type = GUT_EVENT_FOCUS_OUT;
        return 1;
    case SDL_EVENT_KEY_DOWN:
        ev->key = gut_key_from_sdl(e->key.key);
        if (ev->key == GUT_KEY_NONE)
            return 0;
        ev->type = GUT_EVENT_KEY;
        ev->mods = gut_mods_from_sdl(e->key.mod);
        ev->repeat = e->key.repeat ? 1 : 0;
        if (w->paste_keys && gut_is_paste_chord(ev) &&
            gut_paste_event(w, ev, gut_clipboard_get(w), 0))
            return 1;
        return 1;
    case SDL_EVENT_TEXT_INPUT:
        ev->type = GUT_EVENT_TEXT;
        snprintf(ev->text, sizeof(ev->text), "%s", e->text.text);
        gut_set_event_text(w, ev, e->text.text);
        free(w->preedit);
        w->preedit = NULL;
        return 1;
    case SDL_EVENT_TEXT_EDITING:
        free(w->preedit);
        w->preedit = e->edit.text && e->edit.text[0] ? gut_strdup(e->edit.text)
                                                     : NULL;
        w->preedit_cursor = e->edit.start;
        ev->type = GUT_EVENT_COMPOSE;
        ev->cursor = e->edit.start;
        gut_set_event_text(w, ev, e->edit.text ? e->edit.text : "");
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        ev->type = GUT_EVENT_MOUSE_MOVE;
        gut_mouse_pos(w, e->motion.x, e->motion.y, ev);
        if (e->motion.state & SDL_BUTTON_LMASK)
            ev->button = GUT_BUTTON_LEFT;
        else if (e->motion.state & SDL_BUTTON_MMASK)
            ev->button = GUT_BUTTON_MIDDLE;
        else if (e->motion.state & SDL_BUTTON_RMASK)
            ev->button = GUT_BUTTON_RIGHT;
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ev->type = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN
                   ? GUT_EVENT_MOUSE_DOWN : GUT_EVENT_MOUSE_UP;
        gut_mouse_pos(w, e->button.x, e->button.y, ev);
        ev->button = e->button.button == SDL_BUTTON_LEFT ? GUT_BUTTON_LEFT
                   : e->button.button == SDL_BUTTON_MIDDLE ? GUT_BUTTON_MIDDLE
                   : e->button.button == SDL_BUTTON_RIGHT ? GUT_BUTTON_RIGHT
                   : 0;
        ev->clicks = e->button.clicks;
        if (w->paste_keys && ev->type == GUT_EVENT_MOUSE_DOWN &&
            ev->button == GUT_BUTTON_MIDDLE &&
            gut_paste_event(w, ev, gut_primary_get(w), 1))
            return 1;
        return 1;
    case SDL_EVENT_MOUSE_WHEEL:
        ev->type = GUT_EVENT_MOUSE_WHEEL;
        gut_mouse_pos(w, e->wheel.mouse_x, e->wheel.mouse_y, ev);
        ev->dx = (int)e->wheel.x;
        ev->dy = (int)e->wheel.y;
        return 1;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return gut_translate_pad(w, e, ev);
    default:
        return 0;
    }
}

int
gut_poll(gut_window *w, struct gut_event *ev, int timeout_ms)
{
    uint64_t deadline = SDL_GetTicks() + (uint64_t)(timeout_ms > 0 ?
                                                    timeout_ms : 0);
    SDL_Event e;

    /* text handed out with the previous event expires now */
    free(w->event_text);
    w->event_text = NULL;

    for (;;) {
        int wait;

        if (timeout_ms < 0) {
            wait = -1;
        } else {
            uint64_t now = SDL_GetTicks();

            wait = now >= deadline ? 0 : (int)(deadline - now);
        }
        if (!SDL_WaitEventTimeout(&e, wait)) {
            ev->type = GUT_EVENT_NONE;
            return 0;
        }
        if (gut_translate(w, &e, ev)) {
            /* A fast mouse queues motion faster than a program that
             * presents per event can drain it. Consecutive motion
             * events collapse into the latest one; anything else in
             * between keeps its place. */
            while (ev->type == GUT_EVENT_MOUSE_MOVE &&
                   SDL_PeepEvents(&e, 1, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                                  SDL_EVENT_LAST) == 1 &&
                   e.type == SDL_EVENT_MOUSE_MOTION) {
                SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                               SDL_EVENT_LAST);
                gut_translate(w, &e, ev);
            }
            return 1;
        }
        if (timeout_ms == 0 && !SDL_PollEvent(NULL)) {
            ev->type = GUT_EVENT_NONE;
            return 0;
        }
    }
}

#endif /* GUTERM_NO_WINDOW */

#ifndef GUTERM_NO_VT

/****************************************************************
 * VT layer. The parser follows the VT500 state diagram; the dispatch
 * covers the VT100 and xterm sequences that full screen programs use.
 ****************************************************************/

enum {
    GUT_ST_GROUND,
    GUT_ST_ESCAPE,
    GUT_ST_ESCAPE_INTERMED,
    GUT_ST_CSI_ENTRY,
    GUT_ST_CSI_PARAM,
    GUT_ST_CSI_INTERMED,
    GUT_ST_CSI_IGNORE,
    GUT_ST_OSC_STRING,
    GUT_ST_DCS_PASSTHRU,
};

/* DEC special graphics, 0x5F to 0x7E */
static const uint32_t gut_dec_graphics[] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

/* ---- state helpers ---- */

static void
gut_vt_reply_str(struct gut_vt *vt, const char *s, size_t len)
{
    if (vt->reply)
        vt->reply(vt->reply_ctx, s, len);
}

static void
gut_vt_tab_reset(struct gut_vt *vt)
{
    for (int c = 0; c < vt->buf->cols; c++)
        vt->tabstops[c] = (c % 8) == 0;
}

static int
gut_vt_tab_next(const struct gut_vt *vt, int col)
{
    int cols = vt->buf->cols;

    for (int c = col + 1; c < cols; c++)
        if (vt->tabstops[c])
            return c;
    return cols - 1;
}

static int
gut_vt_tab_prev(const struct gut_vt *vt, int col)
{
    for (int c = col - 1; c > 0; c--)
        if (vt->tabstops[c])
            return c;
    return 0;
}

static void
gut_vt_clamp(struct gut_vt *vt)
{
    int rows = vt->buf->rows, cols = vt->buf->cols;

    if (vt->row < 0)
        vt->row = 0;
    if (vt->row >= rows)
        vt->row = rows - 1;
    if (vt->col < 0)
        vt->col = 0;
    if (vt->col >= cols)
        vt->col = cols - 1;
    vt->wrap_pending = 0;
}

/* Cursor row bounds for absolute moves under origin mode. */
static void
gut_vt_set_row(struct gut_vt *vt, int n)
{
    if (vt->modes & GUT_VT_MODE_ORIGIN) {
        vt->row = vt->scroll_top + n;
        if (vt->row >= vt->scroll_bot)
            vt->row = vt->scroll_bot - 1;
    } else {
        vt->row = n;
    }
}

/* ---- scrollback ---- */

static struct gut_vt_line *
gut_vt_sb_line(const struct gut_vt *vt, int i)
{
    return &vt->sb[(vt->sb_head + i) % vt->sb_cap];
}

/* Store a copy of a row, trimmed of trailing default blanks. */
static void
gut_vt_sb_push(struct gut_vt *vt, const struct gut_cell *row, int cols)
{
    struct gut_vt_line *line;
    struct gut_cell blank;
    int n = cols;

    if (vt->sb_cap == 0)
        return;
    gut_cell_erase(&blank, gut_color_default());
    while (n > 0 && memcmp(&row[n - 1], &blank, sizeof(blank)) == 0)
        n--;
    if (vt->sb_len == vt->sb_cap) {
        line = gut_vt_sb_line(vt, 0);
        free(line->cells);
        vt->sb_head = (vt->sb_head + 1) % vt->sb_cap;
    } else {
        line = gut_vt_sb_line(vt, vt->sb_len);
        vt->sb_len++;
    }
    line->cells = NULL;
    line->n = 0;
    if (n > 0) {
        line->cells = malloc((size_t)n * sizeof(*line->cells));
        if (line->cells) {
            memcpy(line->cells, row, (size_t)n * sizeof(*line->cells));
            line->n = n;
        }
    }
    /* keep a scrolled back view on the same lines */
    if (vt->view > 0 && vt->view < vt->sb_len)
        vt->view++;
}

/* Copy a scrollback line into a row of b, clipped or padded to width. */
static void
gut_vt_sb_unpack(const struct gut_vt *vt, int i, struct gut_buf *b, int row)
{
    const struct gut_vt_line *line = gut_vt_sb_line(vt, i);
    struct gut_cell *dst = &b->cells[row * b->cols];
    int n = line->n < b->cols ? line->n : b->cols;

    if (n > 0)
        memcpy(dst, line->cells, (size_t)n * sizeof(*dst));
    for (int c = n; c < b->cols; c++)
        gut_cell_erase(&dst[c], gut_color_default());
    gut_buf_repair_row(b, row);
    b->dirty[row] = 1;
}

/* Remove the newest line, after it was unpacked. */
static void
gut_vt_sb_drop_newest(struct gut_vt *vt)
{
    struct gut_vt_line *line = gut_vt_sb_line(vt, vt->sb_len - 1);

    free(line->cells);
    line->cells = NULL;
    line->n = 0;
    vt->sb_len--;
}

static void
gut_vt_sb_free_lines(struct gut_vt *vt)
{
    for (int i = 0; i < vt->sb_len; i++)
        free(gut_vt_sb_line(vt, i)->cells);
    vt->sb_len = 0;
    vt->sb_head = 0;
}

/* Fill the caller's buffer with the view: scrollback lines on top, the
 * live screen below. */
static void
gut_vt_compose(struct gut_vt *vt)
{
    struct gut_buf *out = vt->out;
    const struct gut_buf *live = &vt->live;

    if (vt->view == 0)
        return;
    for (int r = 0; r < out->rows; r++) {
        if (r < vt->view) {
            gut_vt_sb_unpack(vt, vt->sb_len - vt->view + r, out, r);
        } else {
            memcpy(&out->cells[r * out->cols],
                   &live->cells[(r - vt->view) * live->cols],
                   (size_t)out->cols * sizeof(*out->cells));
            out->dirty[r] = 1;
        }
    }
    out->cursor_visible = 0;
    out->cursor_row = 0;
    out->cursor_col = 0;
}

static int
gut_vt_view_apply(struct gut_vt *vt, int offset)
{
    struct gut_buf *out = vt->out;
    size_t bytes;

    if (offset > vt->sb_len)
        offset = vt->sb_len;
    if (offset < 0 || (vt->modes & GUT_VT_MODE_ALTSCREEN))
        offset = 0;
    if (offset == vt->view)
        return vt->view;
    bytes = (size_t)out->rows * (size_t)out->cols * sizeof(*out->cells);
    if (vt->view == 0) {
        /* park the live screen so the emulator keeps writing to it */
        if (gut_buf_init(&vt->live, out->rows, out->cols) != 0)
            return 0;
        memcpy(vt->live.cells, out->cells, bytes);
        vt->live.cursor_row = out->cursor_row;
        vt->live.cursor_col = out->cursor_col;
        vt->live.cursor_visible = out->cursor_visible;
        vt->live.cursor_shape = out->cursor_shape;
        vt->buf = &vt->live;
    }
    vt->view = offset;
    if (offset == 0) {
        memcpy(out->cells, vt->live.cells, bytes);
        out->cursor_row = vt->live.cursor_row;
        out->cursor_col = vt->live.cursor_col;
        out->cursor_visible = vt->live.cursor_visible;
        out->cursor_shape = vt->live.cursor_shape;
        gut_buf_dirty_all(out);
        gut_buf_free(&vt->live);
        vt->buf = out;
    } else {
        gut_vt_compose(vt);
    }
    return vt->view;
}

/* Scroll the region up, feeding the scrollback when the top of the
 * primary screen leaves. */
static void
gut_vt_scroll_up(struct gut_vt *vt, int count)
{
    struct gut_buf *b = vt->buf;

    if (vt->scroll_top == 0 && !(vt->modes & GUT_VT_MODE_ALTSCREEN)) {
        int n = count < vt->scroll_bot ? count : vt->scroll_bot;

        for (int r = 0; r < n; r++)
            gut_vt_sb_push(vt, &b->cells[r * b->cols], b->cols);
    }
    gut_buf_scroll(b, vt->scroll_top, vt->scroll_bot, count, vt->bg);
}

static void
gut_vt_index(struct gut_vt *vt)
{
    if (vt->row == vt->scroll_bot - 1)
        gut_vt_scroll_up(vt, 1);
    else if (vt->row < vt->buf->rows - 1)
        vt->row++;
}

static void
gut_vt_reverse_index(struct gut_vt *vt)
{
    if (vt->row == vt->scroll_top)
        gut_buf_scroll(vt->buf, vt->scroll_top, vt->scroll_bot, -1, vt->bg);
    else if (vt->row > 0)
        vt->row--;
}

static void
gut_vt_save_cursor(struct gut_vt *vt)
{
    vt->saved.row = vt->row;
    vt->saved.col = vt->col;
    vt->saved.attrs = vt->attrs;
    vt->saved.fg = vt->fg;
    vt->saved.bg = vt->bg;
}

static void
gut_vt_restore_cursor(struct gut_vt *vt)
{
    vt->row = vt->saved.row;
    vt->col = vt->saved.col;
    vt->attrs = vt->saved.attrs;
    vt->fg = vt->saved.fg;
    vt->bg = vt->saved.bg;
    gut_vt_clamp(vt);
}

static void
gut_vt_altscreen_enter(struct gut_vt *vt)
{
    struct gut_buf *b;
    size_t n;

    if (vt->modes & GUT_VT_MODE_ALTSCREEN)
        return;
    gut_vt_view_apply(vt, 0);
    b = vt->buf;
    n = (size_t)b->rows * (size_t)b->cols;
    free(vt->alt_saved);
    vt->alt_saved = malloc(n * sizeof(*vt->alt_saved));
    if (vt->alt_saved) {
        memcpy(vt->alt_saved, b->cells, n * sizeof(*vt->alt_saved));
        vt->alt_rows = b->rows;
        vt->alt_cols = b->cols;
    }
    vt->alt_cursor.row = vt->row;
    vt->alt_cursor.col = vt->col;
    vt->alt_cursor.attrs = vt->attrs;
    vt->alt_cursor.fg = vt->fg;
    vt->alt_cursor.bg = vt->bg;
    vt->modes |= GUT_VT_MODE_ALTSCREEN;
    gut_buf_clear(b, gut_color_default());
}

static void
gut_vt_altscreen_leave(struct gut_vt *vt)
{
    struct gut_buf *b = vt->buf;

    if (!(vt->modes & GUT_VT_MODE_ALTSCREEN))
        return;
    vt->modes &= ~GUT_VT_MODE_ALTSCREEN;
    if (vt->alt_saved && vt->alt_rows == b->rows && vt->alt_cols == b->cols)
        memcpy(b->cells, vt->alt_saved,
               (size_t)b->rows * (size_t)b->cols * sizeof(*b->cells));
    else
        gut_buf_clear(b, gut_color_default());
    gut_buf_dirty_all(b);
    free(vt->alt_saved);
    vt->alt_saved = NULL;
    vt->row = vt->alt_cursor.row;
    vt->col = vt->alt_cursor.col;
    vt->attrs = vt->alt_cursor.attrs;
    vt->fg = vt->alt_cursor.fg;
    vt->bg = vt->alt_cursor.bg;
    gut_vt_clamp(vt);
}

/* ---- print ---- */

static void
gut_vt_putchar(struct gut_vt *vt, uint32_t cp, int width)
{
    struct gut_buf *b = vt->buf;
    int cols = b->cols;

    if (width == 0) {
        /* combining mark: attach to the previous cell by replacing it
         * with the base alone is all this renderer can do, so drop it */
        return;
    }
    if (vt->wrap_pending || vt->col + width > cols) {
        if (vt->modes & GUT_VT_MODE_AUTOWRAP) {
            vt->col = 0;
            vt->wrap_pending = 0;
            gut_vt_index(vt);
        } else {
            vt->col = cols - width;
            vt->wrap_pending = 0;
        }
    }
    if (vt->modes & GUT_VT_MODE_INSERT) {
        struct gut_cell *row = &b->cells[vt->row * cols];

        for (int i = cols - 1; i >= vt->col + width; i--)
            row[i] = row[i - width];
        gut_buf_repair_row(b, vt->row);
    }
    gut_buf_put(b, vt->row, vt->col, cp, vt->fg, vt->bg, vt->attrs);
    vt->col += width;
    if (vt->col >= cols) {
        vt->col = cols - 1;
        vt->wrap_pending = 1;
    }
}

static void
gut_vt_print(struct gut_vt *vt, uint32_t cp)
{
    int w;

    if (vt->g_set[vt->charset] == 1 && cp >= 0x5F && cp <= 0x7E)
        cp = gut_dec_graphics[cp - 0x5F];
    w = gut_rune_width(cp);
    if (w < 0)
        w = 1;
    gut_vt_putchar(vt, cp, w);
}

/* ---- C0 execute ---- */

static void
gut_vt_execute(struct gut_vt *vt, unsigned char c)
{
    switch (c) {
    case 0x07:
        if (vt->bell)
            vt->bell(vt->bell_ctx);
        break;
    case 0x08:
        if (vt->col > 0)
            vt->col--;
        vt->wrap_pending = 0;
        break;
    case 0x09:
        vt->col = gut_vt_tab_next(vt, vt->col);
        vt->wrap_pending = 0;
        break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        gut_vt_index(vt);
        vt->wrap_pending = 0;
        break;
    case 0x0D:
        vt->col = 0;
        vt->wrap_pending = 0;
        break;
    case 0x0E:
        vt->charset = 1;
        break;
    case 0x0F:
        vt->charset = 0;
        break;
    }
}

/* ---- CSI ---- */

static int
gut_vt_param(const struct gut_vt *vt, int idx, int def)
{
    if (idx < vt->nparam && vt->params[idx] >= 0)
        return vt->params[idx];
    return def;
}

static void
gut_vt_sgr(struct gut_vt *vt)
{
    if (vt->nparam == 0) {
        vt->attrs = 0;
        vt->fg = gut_color_default();
        vt->bg = gut_color_default();
        return;
    }
    for (int i = 0; i < vt->nparam; i++) {
        int p = vt->params[i] < 0 ? 0 : vt->params[i];

        switch (p) {
        case 0:
            vt->attrs = 0;
            vt->fg = gut_color_default();
            vt->bg = gut_color_default();
            break;
        case 1: vt->attrs |= GUT_ATTR_BOLD; break;
        case 2: vt->attrs |= GUT_ATTR_DIM; break;
        case 3: vt->attrs |= GUT_ATTR_ITALIC; break;
        case 4: vt->attrs |= GUT_ATTR_UNDERLINE; break;
        case 5: vt->attrs |= GUT_ATTR_BLINK; break;
        case 7: vt->attrs |= GUT_ATTR_REVERSE; break;
        case 8: vt->attrs |= GUT_ATTR_HIDDEN; break;
        case 9: vt->attrs |= GUT_ATTR_STRIKE; break;
        case 21: vt->attrs &= (uint16_t)~GUT_ATTR_BOLD; break;
        case 22: vt->attrs &= (uint16_t)~(GUT_ATTR_BOLD | GUT_ATTR_DIM); break;
        case 23: vt->attrs &= (uint16_t)~GUT_ATTR_ITALIC; break;
        case 24: vt->attrs &= (uint16_t)~GUT_ATTR_UNDERLINE; break;
        case 25: vt->attrs &= (uint16_t)~GUT_ATTR_BLINK; break;
        case 27: vt->attrs &= (uint16_t)~GUT_ATTR_REVERSE; break;
        case 28: vt->attrs &= (uint16_t)~GUT_ATTR_HIDDEN; break;
        case 29: vt->attrs &= (uint16_t)~GUT_ATTR_STRIKE; break;
        case 38:
        case 48: {
            struct gut_color *dst = p == 38 ? &vt->fg : &vt->bg;

            if (i + 2 < vt->nparam && vt->params[i + 1] == 5) {
                *dst = gut_color_indexed(vt->params[i + 2]);
                i += 2;
            } else if (i + 4 < vt->nparam && vt->params[i + 1] == 2) {
                *dst = gut_color_rgb(vt->params[i + 2] & 0xFF,
                                     vt->params[i + 3] & 0xFF,
                                     vt->params[i + 4] & 0xFF);
                i += 4;
            }
            break;
        }
        case 39: vt->fg = gut_color_default(); break;
        case 49: vt->bg = gut_color_default(); break;
        default:
            if (p >= 30 && p <= 37)
                vt->fg = gut_color_indexed(p - 30);
            else if (p >= 40 && p <= 47)
                vt->bg = gut_color_indexed(p - 40);
            else if (p >= 90 && p <= 97)
                vt->fg = gut_color_indexed(p - 90 + 8);
            else if (p >= 100 && p <= 107)
                vt->bg = gut_color_indexed(p - 100 + 8);
            break;
        }
    }
}

static void
gut_vt_erase_cols(struct gut_vt *vt, int row, int from, int to)
{
    struct gut_buf *b = vt->buf;

    if (from < 0)
        from = 0;
    if (to > b->cols)
        to = b->cols;
    for (int c = from; c < to; c++) {
        gut_buf_unwide(b, row, c);
        gut_cell_erase(&b->cells[row * b->cols + c], vt->bg);
    }
    if (from < to)
        b->dirty[row] = 1;
}

static void
gut_vt_erase_display(struct gut_vt *vt, int mode)
{
    struct gut_buf *b = vt->buf;

    switch (mode) {
    case 0:
        gut_vt_erase_cols(vt, vt->row, vt->col, b->cols);
        gut_buf_clear_rows(b, vt->row + 1, b->rows, vt->bg);
        break;
    case 1:
        gut_buf_clear_rows(b, 0, vt->row, vt->bg);
        gut_vt_erase_cols(vt, vt->row, 0, vt->col + 1);
        break;
    case 2:
        gut_buf_clear_rows(b, 0, b->rows, vt->bg);
        break;
    case 3:
        if (!(vt->modes & GUT_VT_MODE_ALTSCREEN))
            gut_vt_clear_scrollback(vt);
        break;
    }
}

static void
gut_vt_erase_line(struct gut_vt *vt, int mode)
{
    int cols = vt->buf->cols;

    switch (mode) {
    case 0: gut_vt_erase_cols(vt, vt->row, vt->col, cols); break;
    case 1: gut_vt_erase_cols(vt, vt->row, 0, vt->col + 1); break;
    case 2: gut_vt_erase_cols(vt, vt->row, 0, cols); break;
    }
}

static void
gut_vt_insert_chars(struct gut_vt *vt, int count)
{
    struct gut_buf *b = vt->buf;
    struct gut_cell *row = &b->cells[vt->row * b->cols];
    int cols = b->cols;

    if (count > cols - vt->col)
        count = cols - vt->col;
    for (int i = cols - 1; i >= vt->col + count; i--)
        row[i] = row[i - count];
    gut_vt_erase_cols(vt, vt->row, vt->col, vt->col + count);
    gut_buf_repair_row(b, vt->row);
}

static void
gut_vt_delete_chars(struct gut_vt *vt, int count)
{
    struct gut_buf *b = vt->buf;
    struct gut_cell *row = &b->cells[vt->row * b->cols];
    int cols = b->cols;

    if (count > cols - vt->col)
        count = cols - vt->col;
    for (int i = vt->col; i < cols - count; i++)
        row[i] = row[i + count];
    gut_vt_erase_cols(vt, vt->row, cols - count, cols);
    gut_buf_repair_row(b, vt->row);
}

static void
gut_vt_decset(struct gut_vt *vt, int n, int on)
{
    unsigned bit = 0;

    switch (n) {
    case 1: bit = GUT_VT_MODE_APP_CURSOR; break;
    case 6:
        bit = GUT_VT_MODE_ORIGIN;
        vt->row = on ? vt->scroll_top : 0;
        vt->col = 0;
        vt->wrap_pending = 0;
        break;
    case 7: bit = GUT_VT_MODE_AUTOWRAP; break;
    case 25:
        vt->buf->cursor_visible = on;
        return;
    case 47:
    case 1047:
    case 1049:
        if (on) {
            if (n == 1049)
                gut_vt_save_cursor(vt);
            gut_vt_altscreen_enter(vt);
        } else {
            gut_vt_altscreen_leave(vt);
            if (n == 1049)
                gut_vt_restore_cursor(vt);
        }
        return;
    case 1000:
    case 1002:
    case 1003:
        vt->mouse_mode = on ? n : 0;
        vt->mouse_row = -1;
        vt->mouse_col = -1;
        bit = GUT_VT_MODE_MOUSE;
        break;
    case 1004: bit = GUT_VT_MODE_FOCUS; break;
    case 1006: bit = GUT_VT_MODE_MOUSE_SGR; break;
    case 2004: bit = GUT_VT_MODE_BRACKETPASTE; break;
    default:
        return;
    }
    if (on)
        vt->modes |= bit;
    else
        vt->modes &= ~bit;
}

static void
gut_vt_csi(struct gut_vt *vt, int final)
{
    struct gut_buf *b = vt->buf;
    int rows = b->rows, cols = b->cols;
    int n, m;
    char rep[32];

    if (vt->intermed == '?') {
        if (final == 'h' || final == 'l')
            for (int i = 0; i < vt->nparam; i++)
                gut_vt_decset(vt, vt->params[i], final == 'h');
        return;
    }
    if (vt->intermed == ' ' && final == 'q') {
        n = gut_vt_param(vt, 0, 0);
        b->cursor_shape = (n == 3 || n == 4) ? GUT_CURSOR_UNDERLINE
                        : (n == 5 || n == 6) ? GUT_CURSOR_BAR
                        : GUT_CURSOR_BLOCK;
        return;
    }
    if (vt->intermed != 0)
        return;

    switch (final) {
    case 'A':
        /* stop at the top margin when starting inside the region */
        n = vt->row >= vt->scroll_top ? vt->scroll_top : 0;
        vt->row -= gut_vt_param(vt, 0, 1);
        if (vt->row < n)
            vt->row = n;
        gut_vt_clamp(vt);
        break;
    case 'B':
        n = vt->row < vt->scroll_bot ? vt->scroll_bot - 1 : rows - 1;
        vt->row += gut_vt_param(vt, 0, 1);
        if (vt->row > n)
            vt->row = n;
        gut_vt_clamp(vt);
        break;
    case 'C':
        vt->col += gut_vt_param(vt, 0, 1);
        gut_vt_clamp(vt);
        break;
    case 'D':
        vt->col -= gut_vt_param(vt, 0, 1);
        gut_vt_clamp(vt);
        break;
    case 'E':
        vt->row += gut_vt_param(vt, 0, 1);
        vt->col = 0;
        gut_vt_clamp(vt);
        break;
    case 'F':
        vt->row -= gut_vt_param(vt, 0, 1);
        vt->col = 0;
        gut_vt_clamp(vt);
        break;
    case 'G':
    case '`':
        vt->col = gut_vt_param(vt, 0, 1) - 1;
        gut_vt_clamp(vt);
        break;
    case 'H':
    case 'f':
        n = gut_vt_param(vt, 0, 1);
        m = gut_vt_param(vt, 1, 1);
        gut_vt_set_row(vt, n - 1);
        vt->col = m - 1;
        gut_vt_clamp(vt);
        break;
    case 'J':
        gut_vt_erase_display(vt, gut_vt_param(vt, 0, 0));
        break;
    case 'K':
        gut_vt_erase_line(vt, gut_vt_param(vt, 0, 0));
        break;
    case 'L':
        if (vt->row >= vt->scroll_top && vt->row < vt->scroll_bot)
            gut_buf_scroll(b, vt->row, vt->scroll_bot,
                           -gut_vt_param(vt, 0, 1), vt->bg);
        break;
    case 'M':
        if (vt->row >= vt->scroll_top && vt->row < vt->scroll_bot)
            gut_buf_scroll(b, vt->row, vt->scroll_bot,
                           gut_vt_param(vt, 0, 1), vt->bg);
        break;
    case 'P':
        gut_vt_delete_chars(vt, gut_vt_param(vt, 0, 1));
        break;
    case 'S':
        gut_vt_scroll_up(vt, gut_vt_param(vt, 0, 1));
        break;
    case 'T':
        gut_buf_scroll(b, vt->scroll_top, vt->scroll_bot,
                       -gut_vt_param(vt, 0, 1), vt->bg);
        break;
    case 'X':
        n = gut_vt_param(vt, 0, 1);
        gut_vt_erase_cols(vt, vt->row, vt->col, vt->col + n);
        break;
    case '@':
        gut_vt_insert_chars(vt, gut_vt_param(vt, 0, 1));
        break;
    case 'c':
        if (gut_vt_param(vt, 0, 0) == 0)
            gut_vt_reply_str(vt, "\033[?1;2c", 7);
        break;
    case 'n':
        n = gut_vt_param(vt, 0, 0);
        if (n == 6) {
            int len = snprintf(rep, sizeof(rep), "\033[%d;%dR",
                               vt->row + 1, vt->col + 1);

            gut_vt_reply_str(vt, rep, (size_t)len);
        } else if (n == 5) {
            gut_vt_reply_str(vt, "\033[0n", 4);
        }
        break;
    case 'd':
        gut_vt_set_row(vt, gut_vt_param(vt, 0, 1) - 1);
        gut_vt_clamp(vt);
        break;
    case 'h':
    case 'l':
        if (gut_vt_param(vt, 0, 0) == 4) {
            if (final == 'h')
                vt->modes |= GUT_VT_MODE_INSERT;
            else
                vt->modes &= ~GUT_VT_MODE_INSERT;
        }
        break;
    case 'm':
        gut_vt_sgr(vt);
        break;
    case 'r':
        n = gut_vt_param(vt, 0, 1);
        m = gut_vt_param(vt, 1, rows);
        if (n < 1)
            n = 1;
        if (m > rows)
            m = rows;
        if (n < m) {
            vt->scroll_top = n - 1;
            vt->scroll_bot = m;
        }
        vt->row = (vt->modes & GUT_VT_MODE_ORIGIN) ? vt->scroll_top : 0;
        vt->col = 0;
        vt->wrap_pending = 0;
        break;
    case 's':
        gut_vt_save_cursor(vt);
        break;
    case 'u':
        gut_vt_restore_cursor(vt);
        break;
    case 'g':
        n = gut_vt_param(vt, 0, 0);
        if (n == 0)
            vt->tabstops[vt->col] = 0;
        else if (n == 3)
            memset(vt->tabstops, 0, (size_t)cols);
        break;
    case 'Z':
        for (n = gut_vt_param(vt, 0, 1); n > 0; n--)
            vt->col = gut_vt_tab_prev(vt, vt->col);
        vt->wrap_pending = 0;
        break;
    }
}

/* ---- ESC ---- */

static void
gut_vt_esc(struct gut_vt *vt, int final)
{
    if (vt->intermed == '(' || vt->intermed == ')') {
        int g = vt->intermed == '(' ? 0 : 1;

        if (final == 'B')
            vt->g_set[g] = 0;
        else if (final == '0')
            vt->g_set[g] = 1;
        return;
    }
    if (vt->intermed != 0)
        return;
    switch (final) {
    case '7': gut_vt_save_cursor(vt); break;
    case '8': gut_vt_restore_cursor(vt); break;
    case 'D': gut_vt_index(vt); vt->wrap_pending = 0; break;
    case 'E':
        vt->col = 0;
        gut_vt_index(vt);
        vt->wrap_pending = 0;
        break;
    case 'H': vt->tabstops[vt->col] = 1; break;
    case 'M': gut_vt_reverse_index(vt); vt->wrap_pending = 0; break;
    case '=': vt->modes |= GUT_VT_MODE_APP_KEYPAD; break;
    case '>': vt->modes &= ~GUT_VT_MODE_APP_KEYPAD; break;
    case 'c': gut_vt_reset(vt); break;
    }
}

/* ---- OSC ---- */

static const char gut_b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
gut_b64_value(char c)
{
    const char *p = c ? strchr(gut_b64_chars, c) : NULL;

    return p ? (int)(p - gut_b64_chars) : -1;
}

/* Decode into out (at least 3 * len / 4 + 1 bytes), ignoring
 * whitespace. Returns the length, NUL terminated, or -1 when the input
 * is not base64. */
static long
gut_b64_decode(const char *in, size_t len, char *out)
{
    unsigned acc = 0;
    int bits = 0;
    long n = 0;

    for (size_t i = 0; i < len; i++) {
        int v;

        if (in[i] == '=' )
            break;
        if (in[i] == ' ' || in[i] == '\n' || in[i] == '\r' ||
            in[i] == '\t')
            continue;
        v = gut_b64_value(in[i]);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[n] = '\0';
    return n;
}

/* Encode len bytes; out needs 4 * ((len + 2) / 3) + 1 bytes. */
static size_t
gut_b64_encode(const unsigned char *in, size_t len, char *out)
{
    size_t n = 0;

    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        size_t rest = len - i;

        if (rest > 1)
            v |= (unsigned)in[i + 1] << 8;
        if (rest > 2)
            v |= in[i + 2];
        out[n++] = gut_b64_chars[(v >> 18) & 63];
        out[n++] = gut_b64_chars[(v >> 12) & 63];
        out[n++] = rest > 1 ? gut_b64_chars[(v >> 6) & 63] : '=';
        out[n++] = rest > 2 ? gut_b64_chars[v & 63] : '=';
    }
    out[n] = '\0';
    return n;
}

/* OSC 52 ; Pc ; Pd : Pc names selections, c clipboard and p or s
 * primary, empty meaning primary; Pd is base64 text to store, ? to
 * query, anything else clears. */
static void
gut_vt_osc52(struct gut_vt *vt, const char *arg, size_t len)
{
    const char *semi = memchr(arg, ';', len);
    const char *pc = arg, *pd;
    size_t pc_len, pd_len;
    int which[2], nwhich = 0;

    if (!semi)
        return;
    pc_len = (size_t)(semi - arg);
    pd = semi + 1;
    pd_len = len - pc_len - 1;
    for (size_t i = 0; i < pc_len && nwhich < 2; i++) {
        int w = pc[i] == 'c' ? GUT_CLIP_CLIPBOARD
              : pc[i] == 'p' || pc[i] == 's' ? GUT_CLIP_PRIMARY : -1;

        if (w >= 0 && !(nwhich == 1 && which[0] == w))
            which[nwhich++] = w;
    }
    if (nwhich == 0)
        which[nwhich++] = GUT_CLIP_PRIMARY;

    if (pd_len == 1 && pd[0] == '?') {
        const char *text = NULL;
        size_t tlen;
        char *reply;
        size_t n;

        if (!vt->clip_get || !vt->reply)
            return;
        for (int i = 0; i < nwhich && !text; i++)
            text = vt->clip_get(vt->clip_ctx, which[i]);
        if (!text)
            text = "";
        tlen = strlen(text);
        reply = malloc(pc_len + 4 * ((tlen + 2) / 3) + 16);
        if (!reply)
            return;
        n = (size_t)snprintf(reply, pc_len + 8, "\033]52;%.*s;",
                             (int)pc_len, pc);
        n += gut_b64_encode((const unsigned char *)text, tlen, reply + n);
        memcpy(reply + n, "\033\\", 2);
        gut_vt_reply_str(vt, reply, n + 2);
        free(reply);
        return;
    }
    if (vt->clip_set) {
        char *text = malloc(3 * (pd_len / 4) + 4);
        long n;

        if (!text)
            return;
        n = gut_b64_decode(pd, pd_len, text);
        if (n < 0) {
            n = 0;
            text[0] = '\0';
        }
        for (int i = 0; i < nwhich; i++)
            vt->clip_set(vt->clip_ctx, which[i], text, (size_t)n);
        free(text);
    }
}

static void
gut_vt_osc(struct gut_vt *vt)
{
    const char *data = vt->osc;
    size_t len = vt->osc_len;
    const char *semi = data ? memchr(data, ';', len) : NULL;
    int num = 0;

    if (!data || !semi)
        return;
    for (const char *p = data; p < semi; p++) {
        if (*p < '0' || *p > '9')
            return;
        num = num * 10 + (*p - '0');
    }
    semi++;
    if (vt->osc_overflow)
        return;
    vt->osc[len] = '\0';    /* the buffer keeps a spare byte */
    if ((num == 0 || num == 2) && vt->title)
        vt->title(vt->title_ctx, semi);
    else if (num == 52)
        gut_vt_osc52(vt, semi, len - (size_t)(semi - data));
}

/* Append to the OSC string, growing the buffer up to the limit. */
static void
gut_vt_osc_put(struct gut_vt *vt, char c)
{
    if (vt->osc_overflow)
        return;
    if (vt->osc_len + 1 >= vt->osc_cap) {
        size_t cap = vt->osc_cap ? vt->osc_cap * 2 : 256;
        char *p;

        if (vt->osc_len + 1 >= GUT_VT_OSC_MAX) {
            vt->osc_overflow = 1;
            return;
        }
        p = realloc(vt->osc, cap);
        if (!p) {
            vt->osc_overflow = 1;
            return;
        }
        vt->osc = p;
        vt->osc_cap = cap;
    }
    vt->osc[vt->osc_len++] = c;
}

/* ---- parser ---- */

static void
gut_vt_csi_reset(struct gut_vt *vt)
{
    vt->nparam = 0;
    vt->cur_param = 0;
    vt->has_digit = 0;
    vt->intermed = 0;
}

static void
gut_vt_finish_param(struct gut_vt *vt)
{
    if (vt->nparam < GUT_VT_MAX_PARAMS)
        vt->params[vt->nparam++] = vt->has_digit ? vt->cur_param : -1;
    vt->cur_param = 0;
    vt->has_digit = 0;
}

static void
gut_vt_ground(struct gut_vt *vt, unsigned char c)
{
    uint32_t cp;

    if (vt->utf8_need > 0) {
        if ((c & 0xC0) == 0x80) {
            vt->utf8_buf[vt->utf8_len++] = c;
            if (vt->utf8_len >= vt->utf8_need) {
                gut_utf8_decode(&cp, vt->utf8_buf, (size_t)vt->utf8_len);
                gut_vt_print(vt, cp);
                vt->utf8_need = 0;
                vt->utf8_len = 0;
            }
            return;
        }
        gut_vt_print(vt, GUT_RUNE_ERROR);
        vt->utf8_need = 0;
        vt->utf8_len = 0;
    }
    if (c < 0x20) {
        gut_vt_execute(vt, c);
        return;
    }
    if (c == 0x7F)
        return;
    if (c < 0x80) {
        gut_vt_print(vt, c);
        return;
    }
    vt->utf8_buf[0] = c;
    vt->utf8_len = 1;
    if ((c & 0xE0) == 0xC0)
        vt->utf8_need = 2;
    else if ((c & 0xF0) == 0xE0)
        vt->utf8_need = 3;
    else if ((c & 0xF8) == 0xF0)
        vt->utf8_need = 4;
    else {
        gut_vt_print(vt, GUT_RUNE_ERROR);
        vt->utf8_need = 0;
        vt->utf8_len = 0;
    }
}

static void
gut_vt_byte(struct gut_vt *vt, unsigned char c)
{
    if (c == 0x1B) {
        if (vt->state == GUT_ST_OSC_STRING)
            gut_vt_osc(vt);
        vt->utf8_need = 0;
        vt->utf8_len = 0;
        vt->state = GUT_ST_ESCAPE;
        vt->intermed = 0;
        return;
    }
    if (vt->state != GUT_ST_GROUND && vt->state != GUT_ST_OSC_STRING &&
        vt->state != GUT_ST_DCS_PASSTHRU) {
        if (c == 0x07 || (c >= 0x08 && c <= 0x0D)) {
            gut_vt_execute(vt, c);
            return;
        }
    }

    switch (vt->state) {
    case GUT_ST_GROUND:
        gut_vt_ground(vt, c);
        break;
    case GUT_ST_ESCAPE:
        if (c == '[') {
            gut_vt_csi_reset(vt);
            vt->state = GUT_ST_CSI_ENTRY;
        } else if (c == ']') {
            vt->osc_len = 0;
            vt->osc_overflow = 0;
            vt->state = GUT_ST_OSC_STRING;
        } else if (c == 'P' || c == 'X' || c == '^' || c == '_') {
            vt->state = GUT_ST_DCS_PASSTHRU;
        } else if (c >= 0x20 && c <= 0x2F) {
            vt->intermed = c;
            vt->state = GUT_ST_ESCAPE_INTERMED;
        } else if (c >= 0x30 && c <= 0x7E) {
            vt->state = GUT_ST_GROUND;
            gut_vt_esc(vt, c);
        } else {
            vt->state = GUT_ST_GROUND;
        }
        break;
    case GUT_ST_ESCAPE_INTERMED:
        if (c >= 0x20 && c <= 0x2F) {
            vt->intermed = c;
        } else if (c >= 0x30 && c <= 0x7E) {
            vt->state = GUT_ST_GROUND;
            gut_vt_esc(vt, c);
        } else {
            vt->state = GUT_ST_GROUND;
        }
        break;
    case GUT_ST_CSI_ENTRY:
        if (c >= '0' && c <= '9') {
            vt->cur_param = c - '0';
            vt->has_digit = 1;
            vt->state = GUT_ST_CSI_PARAM;
        } else if (c == ';') {
            gut_vt_finish_param(vt);
            vt->state = GUT_ST_CSI_PARAM;
        } else if (c >= 0x3C && c <= 0x3F) {
            vt->intermed = c;
            vt->state = GUT_ST_CSI_PARAM;
        } else if (c >= 0x20 && c <= 0x2F) {
            vt->intermed = c;
            vt->state = GUT_ST_CSI_INTERMED;
        } else if (c >= 0x40 && c <= 0x7E) {
            gut_vt_finish_param(vt);
            vt->state = GUT_ST_GROUND;
            gut_vt_csi(vt, c);
        } else {
            vt->state = GUT_ST_GROUND;
        }
        break;
    case GUT_ST_CSI_PARAM:
        if (c >= '0' && c <= '9') {
            if (vt->cur_param < 65535)
                vt->cur_param = vt->cur_param * 10 + (c - '0');
            vt->has_digit = 1;
        } else if (c == ';' || c == ':') {
            gut_vt_finish_param(vt);
        } else if (c >= 0x20 && c <= 0x2F) {
            gut_vt_finish_param(vt);
            vt->intermed = c;
            vt->state = GUT_ST_CSI_INTERMED;
        } else if (c >= 0x40 && c <= 0x7E) {
            gut_vt_finish_param(vt);
            vt->state = GUT_ST_GROUND;
            gut_vt_csi(vt, c);
        } else if (c >= 0x3C && c <= 0x3F) {
            vt->state = GUT_ST_CSI_IGNORE;
        } else {
            vt->state = GUT_ST_GROUND;
        }
        break;
    case GUT_ST_CSI_INTERMED:
        if (c >= 0x20 && c <= 0x2F) {
            /* further intermediates: keep the first */
        } else if (c >= 0x40 && c <= 0x7E) {
            vt->state = GUT_ST_GROUND;
            gut_vt_csi(vt, c);
        } else {
            vt->state = GUT_ST_CSI_IGNORE;
        }
        break;
    case GUT_ST_CSI_IGNORE:
        if (c >= 0x40 && c <= 0x7E)
            vt->state = GUT_ST_GROUND;
        break;
    case GUT_ST_DCS_PASSTHRU:
        /* absorbed until ESC \ arrives; ESC is handled above */
        break;
    case GUT_ST_OSC_STRING:
        if (c == 0x07) {
            gut_vt_osc(vt);
            vt->state = GUT_ST_GROUND;
        } else {
            gut_vt_osc_put(vt, (char)c);
        }
        break;
    }
}

void
gut_vt_feed(struct gut_vt *vt, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        gut_vt_byte(vt, (unsigned char)data[i]);
    vt->buf->cursor_row = vt->row;
    vt->buf->cursor_col = vt->col;
    gut_vt_compose(vt);
}

/* ---- lifecycle ---- */

void
gut_vt_reset(struct gut_vt *vt)
{
    struct gut_buf *b;

    if (vt->modes & GUT_VT_MODE_ALTSCREEN)
        gut_vt_altscreen_leave(vt);
    gut_vt_view_apply(vt, 0);
    gut_vt_sb_free_lines(vt);
    b = vt->buf;
    vt->modes = GUT_VT_MODE_AUTOWRAP;
    vt->mouse_mode = 0;
    vt->mouse_row = -1;
    vt->mouse_col = -1;
    vt->attrs = 0;
    vt->fg = gut_color_default();
    vt->bg = gut_color_default();
    vt->charset = 0;
    vt->g_set[0] = 0;
    vt->g_set[1] = 0;
    vt->scroll_top = 0;
    vt->scroll_bot = b->rows;
    vt->row = 0;
    vt->col = 0;
    vt->wrap_pending = 0;
    memset(&vt->saved, 0, sizeof(vt->saved));
    gut_vt_tab_reset(vt);
    vt->state = GUT_ST_GROUND;
    vt->utf8_need = 0;
    vt->utf8_len = 0;
    vt->osc_len = 0;
    b->cursor_visible = 1;
    b->cursor_shape = GUT_CURSOR_BLOCK;
    gut_buf_clear(b, gut_color_default());
    b->cursor_row = 0;
    b->cursor_col = 0;
}

int
gut_vt_init(struct gut_vt *vt, struct gut_buf *buf)
{
    memset(vt, 0, sizeof(*vt));
    vt->buf = buf;
    vt->out = buf;
    vt->tabstops = calloc((size_t)buf->cols, 1);
    if (!vt->tabstops)
        return -1;
    if (gut_vt_set_scrollback(vt, GUT_VT_SCROLLBACK_DEFAULT) != 0) {
        free(vt->tabstops);
        vt->tabstops = NULL;
        return -1;
    }
    gut_vt_reset(vt);
    return 0;
}

void
gut_vt_free(struct gut_vt *vt)
{
    gut_vt_sb_free_lines(vt);
    free(vt->sb);
    if (vt->view > 0)
        gut_buf_free(&vt->live);
    free(vt->tabstops);
    free(vt->alt_saved);
    free(vt->osc);
    vt->sb = NULL;
    vt->sb_cap = 0;
    vt->view = 0;
    vt->buf = vt->out;
    vt->tabstops = NULL;
    vt->alt_saved = NULL;
    vt->osc = NULL;
    vt->osc_cap = 0;
    vt->osc_len = 0;
}

/* Lines above the cursor move into the scrollback when the screen
 * shrinks below it, and come back when it grows. */
static void
gut_vt_resize_shrink(struct gut_vt *vt, int rows)
{
    struct gut_buf *b = vt->buf;
    int excess = vt->row - (rows - 1);

    if (excess <= 0 || (vt->modes & GUT_VT_MODE_ALTSCREEN))
        return;
    for (int r = 0; r < excess; r++)
        gut_vt_sb_push(vt, &b->cells[r * b->cols], b->cols);
    gut_buf_scroll(b, 0, b->rows, excess, gut_color_default());
    vt->row -= excess;
    vt->saved.row -= excess;
    if (vt->saved.row < 0)
        vt->saved.row = 0;
}

static void
gut_vt_resize_grow(struct gut_vt *vt, int old_rows)
{
    struct gut_buf *b = vt->buf;
    int pull = b->rows - old_rows;

    if (pull > vt->sb_len)
        pull = vt->sb_len;
    if (pull <= 0 || (vt->modes & GUT_VT_MODE_ALTSCREEN))
        return;
    gut_buf_scroll(b, 0, b->rows, -pull, gut_color_default());
    for (int r = pull - 1; r >= 0; r--) {
        gut_vt_sb_unpack(vt, vt->sb_len - 1, b, r);
        gut_vt_sb_drop_newest(vt);
    }
    vt->row += pull;
    vt->saved.row += pull;
}

int
gut_vt_resize(struct gut_vt *vt, int rows, int cols)
{
    uint8_t *tabs;
    int view = vt->view;
    int old_rows, old_cols;
    int full_region;

    if (rows < 1 || cols < 1)
        return -1;
    if (rows == vt->out->rows && cols == vt->out->cols)
        return 0;
    tabs = calloc((size_t)cols, 1);
    if (!tabs)
        return -1;
    gut_vt_view_apply(vt, 0);
    old_rows = vt->buf->rows;
    old_cols = vt->buf->cols;
    full_region = vt->scroll_top == 0 && vt->scroll_bot == old_rows;
    gut_vt_resize_shrink(vt, rows);
    if (gut_buf_resize(vt->buf, rows, cols) != 0) {
        free(tabs);
        return -1;
    }
    for (int c = 0; c < cols; c++)
        tabs[c] = c < old_cols ? vt->tabstops[c] : (c % 8) == 0;
    free(vt->tabstops);
    vt->tabstops = tabs;
    if (full_region || vt->scroll_bot > rows) {
        vt->scroll_top = 0;
        vt->scroll_bot = rows;
    }
    gut_vt_resize_grow(vt, old_rows);
    gut_vt_clamp(vt);
    if (vt->saved.row >= rows)
        vt->saved.row = rows - 1;
    if (vt->saved.col >= cols)
        vt->saved.col = cols - 1;
    vt->buf->cursor_row = vt->row;
    vt->buf->cursor_col = vt->col;
    gut_vt_view_apply(vt, view);
    return 0;
}

int
gut_vt_set_scrollback(struct gut_vt *vt, int lines)
{
    struct gut_vt_line *ring = NULL;
    int keep;

    if (lines < 0)
        return -1;
    if (lines > 0) {
        ring = calloc((size_t)lines, sizeof(*ring));
        if (!ring)
            return -1;
    }
    keep = vt->sb_len < lines ? vt->sb_len : lines;
    for (int i = 0; i < vt->sb_len - keep; i++)
        free(gut_vt_sb_line(vt, i)->cells);
    for (int i = 0; i < keep; i++)
        ring[i] = *gut_vt_sb_line(vt, vt->sb_len - keep + i);
    free(vt->sb);
    vt->sb = ring;
    vt->sb_cap = lines;
    vt->sb_head = 0;
    vt->sb_len = keep;
    if (vt->view > keep)
        gut_vt_view_apply(vt, keep);
    return 0;
}

int
gut_vt_scrollback_lines(const struct gut_vt *vt)
{
    return vt->sb_len;
}

void
gut_vt_clear_scrollback(struct gut_vt *vt)
{
    gut_vt_view_apply(vt, 0);
    gut_vt_sb_free_lines(vt);
}

int
gut_vt_set_view(struct gut_vt *vt, int offset)
{
    return gut_vt_view_apply(vt, offset);
}

int
gut_vt_scroll_view(struct gut_vt *vt, int delta)
{
    long offset = (long)vt->view + delta;

    if (offset > vt->sb_len)
        offset = vt->sb_len;
    if (offset < 0)
        offset = 0;
    return gut_vt_view_apply(vt, (int)offset);
}

int
gut_vt_view_offset(const struct gut_vt *vt)
{
    return vt->view;
}

unsigned
gut_vt_modes(const struct gut_vt *vt)
{
    return vt->modes;
}

int
gut_vt_encode_flags(const struct gut_vt *vt)
{
    int flags = 0;

    if (vt->modes & GUT_VT_MODE_APP_CURSOR)
        flags |= GUT_ENC_APP_CURSOR;
    if (vt->modes & GUT_VT_MODE_BRACKETPASTE)
        flags |= GUT_ENC_BRACKET_PASTE;
    if (vt->modes & GUT_VT_MODE_MOUSE) {
        flags |= vt->mouse_mode == 1003 ? GUT_ENC_MOUSE_ANY
               : vt->mouse_mode == 1002 ? GUT_ENC_MOUSE_DRAG
               : GUT_ENC_MOUSE_BTN;
    }
    if (vt->modes & GUT_VT_MODE_MOUSE_SGR)
        flags |= GUT_ENC_MOUSE_SGR;
    if (vt->modes & GUT_VT_MODE_FOCUS)
        flags |= GUT_ENC_FOCUS;
    return flags;
}

size_t
gut_vt_mouse(struct gut_vt *vt, const struct gut_event *ev, char *out,
             size_t n)
{
    int flags = gut_vt_encode_flags(vt);

    if (n)
        out[0] = '\0';
    if (ev->type == GUT_EVENT_FOCUS_IN || ev->type == GUT_EVENT_FOCUS_OUT)
        return gut_encode_event(ev, out, n, flags);
    if (ev->type < GUT_EVENT_MOUSE_DOWN || ev->type > GUT_EVENT_MOUSE_WHEEL)
        return 0;
    if (!(vt->modes & GUT_VT_MODE_MOUSE)) {
        /* alternate scroll: the wheel drives full screen programs */
        if (ev->type == GUT_EVENT_MOUSE_WHEEL && ev->dy != 0 &&
            (vt->modes & GUT_VT_MODE_ALTSCREEN)) {
            struct gut_event key;
            struct gut_sink s = { out, n, 0 };
            char one[16];
            size_t len;

            memset(&key, 0, sizeof(key));
            key.type = GUT_EVENT_KEY;
            key.key = ev->dy > 0 ? GUT_KEY_UP : GUT_KEY_DOWN;
            len = gut_encode_event(&key, one, sizeof(one), flags);
            for (int i = 0; i < 3 * (ev->dy > 0 ? ev->dy : -ev->dy); i++)
                gut_sink_put(&s, one, len);
            gut_sink_end(&s);
            return s.len;
        }
        return 0;
    }
    if (ev->mods & GUT_MOD_SHIFT)
        return 0;
    if (ev->type == GUT_EVENT_MOUSE_MOVE) {
        if (ev->row == vt->mouse_row && ev->col == vt->mouse_col)
            return 0;
        vt->mouse_row = ev->row;
        vt->mouse_col = ev->col;
    }
    return gut_encode_event(ev, out, n, flags);
}

void
gut_vt_set_reply(struct gut_vt *vt,
                 void (*fn)(void *ctx, const char *data, size_t len),
                 void *ctx)
{
    vt->reply = fn;
    vt->reply_ctx = ctx;
}

void
gut_vt_set_title_cb(struct gut_vt *vt,
                    void (*fn)(void *ctx, const char *title), void *ctx)
{
    vt->title = fn;
    vt->title_ctx = ctx;
}

void
gut_vt_set_bell_cb(struct gut_vt *vt, void (*fn)(void *ctx), void *ctx)
{
    vt->bell = fn;
    vt->bell_ctx = ctx;
}

void
gut_vt_set_clipboard_cb(struct gut_vt *vt,
                        void (*set)(void *ctx, int which, const char *text,
                                    size_t len),
                        const char *(*get)(void *ctx, int which), void *ctx)
{
    vt->clip_set = set;
    vt->clip_get = get;
    vt->clip_ctx = ctx;
}

#endif /* GUTERM_NO_VT */

#endif /* GUTERM_IMPLEMENTATION */
