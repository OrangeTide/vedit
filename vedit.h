/*
 * vedit.h : public interface for embedding the vedit editor.
 *
 * vedit is a single-file visual text editor for primitive terminals. It can be
 * built as a command-line program (see main() in vedit.c) or embedded in a
 * host such as a MUD server, which drives it through the read()/write()/
 * select()-style callbacks in struct vedit_io.
 *
 * Embedding sketch:
 *
 *     struct vedit_io io = {0};
 *     io.ctx     = player;
 *     io.read    = player_read;    // pull bytes from the player's socket
 *     io.write   = player_write;   // push bytes to the player's socket
 *     io.poll    = player_poll;    // wait for the socket to be readable
 *     // io.begin / io.end stay NULL: no raw mode on an embedded socket
 *     // io.getsize optional; otherwise call vedit_set_size()
 *
 *     struct vedit *v = vedit_new(&io);
 *     vedit_open(v, "note.txt");
 *     vedit_set_size(v, rows, cols);   // from telnet NAWS
 *     int rc = vedit_run(v);           // blocks until the player quits
 *     vedit_free(v);
 */

#ifndef VEDIT_H
#define VEDIT_H

#include <stddef.h>

/*
 * Terminal I/O, modeled on read/write/select. The editor owns no file
 * descriptors and installs no signal handlers of its own; everything passes
 * through this vtable. The command-line binding fills it over a tty (with raw
 * mode in begin/end and SIGWINCH); an embedding host fills it over its own
 * transport, where begin/end are a no-op and resizes arrive by a call to
 * vedit_set_size().
 */
struct vedit_io {
	void	*ctx;
	/* Read up to n bytes. Returns the count, 0 when no data is ready
	 * (would block), or -1 at end of input or on error. */
	long	(*read)(void *ctx, void *buf, long n);
	/* Write n bytes. The host must accept all of them (block or buffer).
	 * Returns the count written, or -1 on error. */
	long	(*write)(void *ctx, const void *buf, long n);
	/* Wait up to timeout_ms (negative to block) for input. Returns 1 when
	 * readable, 0 on timeout, -1 on error. NULL means "always readable". */
	int	(*poll)(void *ctx, int timeout_ms);
	/* Enter and leave the drawing session (raw mode on a tty). Optional;
	 * a no-op for an embedded host. */
	void	(*begin)(void *ctx);
	void	(*end)(void *ctx);
	/* Report the window size. Returns 0 and fills rows/cols, or -1 when
	 * unknown. Optional; a host can use vedit_set_size() instead. */
	int	(*getsize)(void *ctx, int *rows, int *cols);
};

/* How the frame, scrollbars, and menus are drawn. A host picks this from what
 * it knows about the client (for example its telnet terminal type): UTF-8
 * box-drawing, DEC VT100 line-drawing (single-byte, widely supported by older
 * clients), or plain ASCII for the most limited ones. */
enum vedit_box_mode {
	VEDIT_BOX_UTF8,
	VEDIT_BOX_DEC,
	VEDIT_BOX_ASCII,
};

/* An editor instance. Opaque to a host. */
struct vedit;

/* Create an editor bound to the host's io vtable. Returns NULL on failure. */
struct vedit *vedit_new(const struct vedit_io *io);

/* Load a file before vedit_run(). A missing file opens as an empty, named
 * buffer. Returns 0, or -1 on a read error other than "not found". */
int vedit_open(struct vedit *v, const char *path);

/* Deliver a new terminal size (for example from telnet NAWS or a caught
 * resize). The run loop picks up the change and repaints. */
void vedit_set_size(struct vedit *v, int rows, int cols);

/* Choose how box-drawing is rendered. Call before vedit_run(). When never
 * called, the mode is chosen from the environment (UTF-8 locale -> UTF-8,
 * otherwise ASCII; VEDIT_BOX=utf8|dec|ascii and VEDIT_ASCII override). */
void vedit_set_box_mode(struct vedit *v, enum vedit_box_mode mode);

/* Tell the editor the client's color depth: 256 for the full palette, or 16
 * (any value < 256) to map colors to the nearest of the 16 ANSI colors and emit
 * the classic SGR codes, for a primitive client. Call before vedit_run(). When
 * never called, the depth is taken from TERM/COLORTERM (VEDIT_COLORS overrides),
 * defaulting to 16 when nothing indicates 256. */
void vedit_set_colors(struct vedit *v, int colors);

/* Turn the VT100 scroll-region fast path on (nonzero) or off. When on, the
 * editor scrolls the text area with the terminal's scroll region instead of
 * repainting every row, which is much cheaper on a slow link when scrolling or
 * inserting lines. It needs a client that supports the scroll region (most do;
 * the most primitive line-at-a-time clients do not), so it is off by default.
 * Call before vedit_run(). When never called, it is taken from VEDIT_SCROLL. */
void vedit_set_scroll(struct vedit *v, int on);

/* Run the editor to completion. Returns 0 on a normal quit, 1 on end of input
 * or vi ':cq'. */
int vedit_run(struct vedit *v);

/* Free an editor and everything it owns (including the draw surface). */
void vedit_free(struct vedit *v);

#endif /* VEDIT_H */
