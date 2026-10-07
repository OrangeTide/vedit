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
	/* Optional. Wait up to timeout_ms (negative to block) for the transport
	 * input or any of the nextra extra fds to be readable. On return
	 * ready[0..*nready) lists the readable extra fds (ready has room for
	 * nextra). Returns 1 if the transport input is readable, 0 if not, -1 on
	 * error. When NULL, the editor falls back to poll and cannot watch extra
	 * fds, which disables the terminal panel. The standalone binary provides
	 * this; an embedding host wires it to its own event loop to allow
	 * terminal buffers. */
	int	(*poll_fds)(void *ctx, int timeout_ms, const int *extra,
		    int nextra, int *ready, int *nready);
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

/* Optional configuration file (gitconfig-style key/value). The editor core
 * never opens a file itself: build a config with vedit_cfg_new(), fill it from
 * one or more files with vedit_cfg_load(), hand it to vedit_set_config() before
 * vedit_run(), and free it with vedit_cfg_free() after vedit_run(). Recognized
 * keys include ui.box, ui.colors, ui.scroll, ui.scheme, ui.wrap, ui.number,
 * edit.mode, edit.swap, edit.swapdir, edit.backup, edit.backupdir, and
 * syntax.enable (see README). Config values rank below the VEDIT_*
 * environment variables and the explicit setters above. */
struct cfg;
struct cfg *vedit_cfg_new(void);
int vedit_cfg_load(struct cfg *c, const char *path);
void vedit_cfg_free(struct cfg *c);
void vedit_set_config(struct vedit *v, const struct cfg *c);

/* Record the path the config was read from, so the user can re-read it during a
 * session with the ":reload" ex command or Options > Reload Config. The reloaded
 * config is owned by the editor. Without this, reload reports there is no config
 * file. Pass NULL to clear. */
void vedit_set_config_path(struct vedit *v, const char *path);

#ifndef VEDIT_NO_TOOLS
/*
 * External tool commands (compile / make / run), the "primitive IDE" layer.
 * The editor resolves a per-language command from the config (a [command
 * "<lang>"] section), expands its $(...) variables, and hands the final shell
 * command line to this vtable. The core itself spawns nothing: the standalone
 * binary installs a default implementation that forks a shell, and an
 * embedding host can install its own (or leave it NULL to disable building).
 * Define VEDIT_NO_TOOLS before including vedit.c to drop the whole subsystem.
 */
struct vedit_tool_api {
	void	*ctx;
	/* Run cmd (a shell command line) in directory dir, capturing combined
	 * stdout and stderr by calling emit(sink, buf, n) for each chunk.
	 * Returns the command's exit status (>=0), or -1 if it could not run. */
	int	(*run_capture)(void *ctx, const char *cmd, const char *dir,
		    void (*emit)(void *sink, const char *buf, size_t n),
		    void *sink);
	/* Run cmd in directory dir connected to the real terminal (foreground),
	 * for an interactive program. Returns the exit status, or -1. May be
	 * NULL, in which case interactive commands fall back to run_capture. */
	int	(*run_foreground)(void *ctx, const char *cmd, const char *dir);
	/* Run cmd in directory dir as a filter: feed it input[0..inlen) on
	 * stdin and capture its stdout by calling emit(sink, buf, n) for each
	 * chunk (stderr is discarded). Returns the exit status (>=0), or -1 if
	 * it could not run. Used by format-on-save. May be NULL, in which case
	 * formatting is unavailable. */
	int	(*run_filter)(void *ctx, const char *cmd, const char *dir,
		    const char *input, size_t inlen,
		    void (*emit)(void *sink, const char *buf, size_t n),
		    void *sink);
};

/* Install the tool runner. Pass NULL to disable the build/run commands. The
 * api is borrowed, not copied, so it must outlive the editor. Call before
 * vedit_run(). The standalone binary installs a default shell-spawning runner;
 * an embedding host that wants the IDE commands installs its own. */
void vedit_set_tools(struct vedit *v, const struct vedit_tool_api *api);
#endif /* VEDIT_NO_TOOLS */

#ifndef VEDIT_NO_MAIL
/*
 * Mail (the alpine-style reader and composer). The editor core never touches a
 * mailbox itself: it talks to this vtable, which is shaped like IMAP (folders,
 * message lists, flags, fetch / store / move / append as transactions) rather
 * than files, so an embedding host can serve its own server's mail through it.
 * The standalone binary installs a Maildir++ backend when mail.dir is set in
 * the config. Define VEDIT_NO_MAIL before including vedit.c to drop the whole
 * subsystem.
 *
 * Folder names are UTF-8 strings as the backend presents them ("INBOX",
 * "Sent", "lists/vedit"). A uid is an opaque string naming one message within
 * its folder for the life of the session.
 */
#define VEDIT_MAIL_SEEN		0x01
#define VEDIT_MAIL_ANSWERED	0x02
#define VEDIT_MAIL_FLAGGED	0x04
#define VEDIT_MAIL_DRAFT	0x08
#define VEDIT_MAIL_TRASHED	0x10

/* One row of a folder listing. The strings are the raw header values. */
struct vedit_mail_summary {
	const char	*uid;
	unsigned	 flags;
	const char	*from;
	const char	*subject;
	const char	*date;
	long		 size;
};

struct vedit_mail_api {
	void	*ctx;
	/* List the folders, calling emit(sink, name) for each. Returns 0, or
	 * -1 with errno set. */
	int	(*folders)(void *ctx, int (*emit)(void *sink, const char *name),
		    void *sink);
	/* List the messages of folder, calling emit(sink, m) for each; m and
	 * its strings are valid only during the call. Returns 0 or -1. */
	int	(*list)(void *ctx, const char *folder,
		    int (*emit)(void *sink, const struct vedit_mail_summary *m),
		    void *sink);
	/* Fetch the raw RFC 5322 message into a malloc'd buffer the caller
	 * frees. Returns 0, or -1 with errno set (ENOENT: no such uid). */
	int	(*fetch)(void *ctx, const char *folder, const char *uid,
		    char **data, size_t *len);
	/* Set and clear flags (VEDIT_MAIL_*) on a message. */
	int	(*store)(void *ctx, const char *folder, const char *uid,
		    unsigned set, unsigned clear);
	/* Move a message to another folder. */
	int	(*move)(void *ctx, const char *folder, const char *uid,
		    const char *dest);
	/* Add a complete message to a folder (a draft, a sent copy). The
	 * folder is created when the backend can. */
	int	(*append)(void *ctx, const char *folder, const char *data,
		    size_t len, unsigned flags);
	/* Hand a complete message over for delivery. The backend decides what
	 * that means (queue it, submit it); 0 when accepted. */
	int	(*send)(void *ctx, const char *data, size_t len);
};

/* Install the mail backend. The api is borrowed, not copied, so it must
 * outlive the editor. Call before vedit_run(). With none installed (the
 * default) the Mail menu and commands report that there is no mail backend. */
void vedit_set_mail(struct vedit *v, const struct vedit_mail_api *api);
#endif /* VEDIT_NO_MAIL */

/* Run the editor to completion. Returns 0 on a normal quit, 1 on end of input
 * or vi ':cq'. */
int vedit_run(struct vedit *v);

/* Free an editor and everything it owns (including the draw surface). */
void vedit_free(struct vedit *v);

#endif /* VEDIT_H */
