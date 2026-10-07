/*
 * Terminal-buffer tests (VEDIT_TERM). Built with -DVEDIT_TERM -DVEDIT_TEST so
 * the emulator and the fork-free term_attach injection point are compiled in.
 * term_attach wires a plain pipe where a PTY master would be, so feeding bytes
 * and draining them is deterministic with no child process. One case does use
 * a real forkpty via term_open, and skips itself if the spawn fails.
 */
#include "test.h"

#define main test_main
#include "../vedit.c"
#undef main

#include "memio.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* Build an editor over memio with keys as the scripted keyboard input. Drops
 * the empty initial text so a terminal buffer is the only buffer and nothing
 * leaks at teardown. With mux set, memio also offers poll_fds, which the input
 * loop (scr_pump -> in_refill) needs. */
static struct vedit *
term_editor_in(Memio *m, struct vedit_io *io, const char *keys, size_t klen,
    int mux)
{
	struct vedit *v;

	memio_init(m, keys, klen, 24, 80);
	memio_bind(io, m);
	if (mux)
		memio_enable_fds(io);
	v = vedit_new(io);
	if (v) {
		text_free(v->e.t);	/* replaced by the terminal's own text */
		v->e.t = NULL;
	}
	return v;
}

static struct vedit *
term_editor(Memio *m, struct vedit_io *io)
{
	return term_editor_in(m, io, "", 0, 0);
}

/* Attach a terminal to one end of a socketpair and hand the test the other end
 * (the "child"). A socketpair is bidirectional, so the loop can both forward
 * keystrokes to the child and read what the child writes back. The editor owns
 * its end (freed at teardown); the caller closes *child. Returns -1 on error. */
static int
term_pair(struct vedit *v, int *child, int rows, int cols)
{
	int sv[2], fl;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return -1;
	fl = fcntl(sv[0], F_GETFL);
	fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
	fl = fcntl(sv[1], F_GETFL);
	fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
	if (term_attach(&v->e, sv[0], rows, cols) < 0) {
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	*child = sv[1];
	return 0;
}

/* Attach a pipe as a terminal, feed it an escape stream, and check the grid,
 * the rendered output, and the exit-on-EOF path. */
static void
t_term_attach_render(Test *t)
{
	static const char feed[] = "\033[31mHELLO\033[0m world";
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int pr[2], i, fl;
	struct vt_cell *cell;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, pipe(pr) == 0);
	fl = fcntl(pr[0], F_GETFL);		/* read end non-blocking */
	fcntl(pr[0], F_SETFL, fl | O_NONBLOCK);

	i = term_attach(&v->e, pr[0], 10, 40);
	TAP_ASSERT(t, i >= 0);
	TAP_CHECK(t, v->e.kind == BUF_TERM);
	TAP_CHECK(t, v->e.vterm != NULL);
	TAP_CHECK(t, term_is_active(&v->e));

	TAP_ASSERT(t, write(pr[1], feed, sizeof(feed) - 1) == (ssize_t)(sizeof(feed) - 1));
	term_drain(&v->e, pr[0]);		/* parse the fed bytes */

	/* the grid holds the text, with the first cell colored red (index 1) */
	cell = vt_buf_cell(v->e.vterm->vt->buf, 0, 0);
	TAP_ASSERT(t, cell != NULL);
	TAP_CHECKF(t, cell->codepoint == 'H', "cell0 cp=%u", cell->codepoint);
	TAP_CHECKF(t, cell->fg.type == COLOR_INDEXED && cell->fg.index == 1,
	    "cell0 fg type=%d idx=%d", cell->fg.type, cell->fg.index);

	/* the rendered frame carries the visible text */
	ed_render(&v->e, v->e.d);
	TAP_CHECK(t, strstr(m.out, "HELLO") != NULL);
	TAP_CHECK(t, strstr(m.out, "world") != NULL);

	/* closing the write end is EOF: the child is considered gone */
	close(pr[1]);
	term_drain(&v->e, pr[0]);
	TAP_CHECK(t, v->e.vterm->dead == 1);
	TAP_CHECK(t, v->e.vterm->master_fd == -1);	/* reaped: fd closed */

	vedit_free(v);
	memio_free(&m);
}

/* term_collect reports the live PTY fd for the active terminal buffer. */
static void
t_term_collect(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int pr[2], fds[VEDIT_TERM_MAX], n;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, pipe(pr) == 0);
	TAP_ASSERT(t, term_attach(&v->e, pr[0], 10, 40) >= 0);

	n = term_collect(&v->e, fds, VEDIT_TERM_MAX);
	TAP_CHECKF(t, n == 1, "collect returned %d", n);
	TAP_CHECK(t, n == 1 && fds[0] == pr[0]);

	close(pr[1]);
	vedit_free(v);			/* closes pr[0] via term_buf_free */
	memio_free(&m);
}

/* term_resize_all resizes the emulator grid to the text area. */
static void
t_term_resize(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int pr[2];

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, pipe(pr) == 0);
	TAP_ASSERT(t, term_attach(&v->e, pr[0], 10, 40) >= 0);
	TAP_CHECK(t, v->e.vterm->rows == 10);

	v->e.rows = 40;			/* taller window */
	v->e.cols = 100;
	term_resize_all(&v->e);
	TAP_CHECKF(t, v->e.vterm->rows == text_height(&v->e),
	    "grid rows %d != text_height %d", v->e.vterm->rows,
	    text_height(&v->e));
	TAP_CHECK(t, vt_buf_rows(v->e.vterm->vt->buf) == v->e.vterm->rows);

	close(pr[1]);
	vedit_free(v);
	memio_free(&m);
}

/* OSC title becomes the window label; DECTCEM hides the cursor. */
static void
t_term_title_cursor(Test *t)
{
	static const char feed[] = "\033]0;MyTitle\a\033[?25l";
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int pr[2], fl;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, pipe(pr) == 0);
	fl = fcntl(pr[0], F_GETFL);
	fcntl(pr[0], F_SETFL, fl | O_NONBLOCK);
	TAP_ASSERT(t, term_attach(&v->e, pr[0], 10, 40) >= 0);

	TAP_ASSERT(t, write(pr[1], feed, sizeof(feed) - 1) == (ssize_t)(sizeof(feed) - 1));
	term_drain(&v->e, pr[0]);

	/* title recorded and surfaced as the label; cursor mode cleared */
	TAP_CHECK(t, strcmp(term_label(&v->e), "MyTitle") == 0);
	TAP_CHECK(t, !(v->e.vterm->vt->modes & VT_MODE_CURSOR_VIS));

	/* render: the border carries the title and the cursor is hidden */
	ed_render(&v->e, v->e.d);
	TAP_CHECK(t, strstr(m.out, "MyTitle") != NULL);
	TAP_CHECK(t, strstr(m.out, "\033[?25l") != NULL);	/* DECTCEM off */

	/* the child shows the cursor again */
	TAP_ASSERT(t, write(pr[1], "\033[?25h", 6) == 6);
	term_drain(&v->e, pr[0]);
	TAP_CHECK(t, (v->e.vterm->vt->modes & VT_MODE_CURSOR_VIS) != 0);
	ed_render(&v->e, v->e.d);
	TAP_CHECK(t, strstr(m.out, "\033[?25h") != NULL);	/* DECTCEM on */

	close(pr[1]);
	vedit_free(v);
	memio_free(&m);
}

/* A real child over a PTY: run printf and confirm its output reaches the grid.
 * Skips (no failure) if the spawn is unavailable in this environment. */
static void
t_term_fork_smoke(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int tries;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	memio_enable_fds(&io);		/* term_open needs a multiplexing host */
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	text_free(v->e.t);
	v->e.t = NULL;

	if (term_open(&v->e, "printf SMOKE123") < 0) {
		vedit_free(v);		/* spawn unavailable: skip */
		memio_free(&m);
		return;
	}

	/* wait for the child to write and exit, draining as data arrives */
	for (tries = 0; tries < 200 && !v->e.vterm->dead; tries++) {
		fd_set rf;
		struct timeval tv = { 0, 20000 };	/* 20 ms */
		int fd = v->e.vterm->master_fd;

		if (fd < 0)
			break;
		FD_ZERO(&rf);
		FD_SET(fd, &rf);
		if (select(fd + 1, &rf, NULL, NULL, &tv) > 0)
			term_drain(&v->e, fd);
	}

	ed_render(&v->e, v->e.d);
	TAP_CHECK(t, strstr(m.out, "SMOKE123") != NULL);
	TAP_CHECK(t, v->e.vterm->dead == 1);
	TAP_CHECKF(t, v->e.vterm->exit_status == 0, "exit status %d",
	    v->e.vterm->exit_status);

	vedit_free(v);
	memio_free(&m);
}

/* Drain everything currently readable from a non-blocking fd. */
static int
read_all(int fd, char *buf, int max)
{
	int n = 0;
	ssize_t r;

	while (n < max && (r = read(fd, buf + n, (size_t)(max - n))) > 0)
		n += (int)r;
	return n;
}

/* The input loop forwards raw keystrokes to the child. */
static void
t_term_loop_input(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, n;
	char buf[64];

	v = term_editor_in(&m, &io, "hello", 5, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	n = read_all(child, buf, sizeof(buf));
	TAP_CHECKF(t, n == 5 && memcmp(buf, "hello", 5) == 0,
	    "child got %d bytes", n);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W Ctrl-W sends one literal Ctrl-W to the child. */
static void
t_term_loop_literal(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, n;
	char buf[8];

	v = term_editor_in(&m, &io, "\027\027", 2, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	n = read_all(child, buf, sizeof(buf));
	TAP_CHECKF(t, n == 1 && buf[0] == 0x17, "child got %d bytes", n);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W w is a window command (buffer cycle); it does not reach the child. */
static void
t_term_loop_cmd(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, n;
	char buf[8];

	v = term_editor_in(&m, &io, "\027w", 2, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	n = read_all(child, buf, sizeof(buf));
	TAP_CHECKF(t, n == 0, "window command leaked %d bytes to child", n);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W q on the only buffer asks the editor to quit. */
static void
t_term_loop_quit(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;

	v = term_editor_in(&m, &io, "\027q", 2, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_QUIT);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* With no keystrokes, the loop drains child output through the multiplexer and
 * updates the grid (the idle path). */
static void
t_term_loop_output(Test *t)
{
	static const char out[] = "\033[32mGRID\033[0m";
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;
	struct vt_cell *cell;

	v = term_editor_in(&m, &io, "", 0, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_ASSERT(t, write(child, out, sizeof(out) - 1) == (ssize_t)(sizeof(out) - 1));
	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);

	cell = vt_buf_cell(v->e.vterm->vt->buf, 0, 0);
	TAP_ASSERT(t, cell != NULL);
	TAP_CHECKF(t, cell->codepoint == 'G', "grid cp=%u", cell->codepoint);
	TAP_CHECK(t, cell->fg.type == COLOR_INDEXED && cell->fg.index == 2);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* A pending resize is handled by the terminal loop (resizes the PTY + grid). */
static void
t_term_loop_resize(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;

	v = term_editor_in(&m, &io, "", 0, 1);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	g_winch = 1;				/* a window-change arrived */
	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECK(t, g_winch == 0);		/* the handler consumed it */
	TAP_CHECK(t, v->e.vterm->rows == text_height(&v->e));

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* The native poll_fds binding reports the keyboard and the extra PTY fds. */
static void
t_tty_poll_fds(Test *t)
{
	Ttyio tt;
	int a[2], b[2], extra[1], ready[VEDIT_TERM_MAX], nready, r;

	TAP_ASSERT(t, pipe(a) == 0);
	TAP_ASSERT(t, pipe(b) == 0);
	memset(&tt, 0, sizeof(tt));
	tt.in_fd = a[0];			/* stands in for the keyboard */
	extra[0] = b[0];			/* stands in for a PTY master */

	r = tty_poll_fds(&tt, 0, extra, 1, ready, &nready);
	TAP_CHECKF(t, r == 0 && nready == 0, "idle: r=%d nready=%d", r, nready);

	TAP_ASSERT(t, write(b[1], "x", 1) == 1);	/* PTY readable */
	r = tty_poll_fds(&tt, 0, extra, 1, ready, &nready);
	TAP_CHECKF(t, r == 0 && nready == 1 && ready[0] == b[0],
	    "pty: r=%d nready=%d", r, nready);

	TAP_ASSERT(t, write(a[1], "k", 1) == 1);	/* keyboard readable */
	r = tty_poll_fds(&tt, 0, extra, 1, ready, &nready);
	TAP_CHECKF(t, r == 1, "kbd: r=%d", r);

	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
}

/* With no title set, the label falls back to a generic name. */
static void
t_term_label_default(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);
	TAP_CHECK(t, strcmp(term_label(&v->e), "terminal") == 0);
	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* Two terminals: collect reports both, and a background one is found by fd. */
static void
t_term_two_bg(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int c0, c1, fds[VEDIT_TERM_MAX], n;
	struct vt_cell *cell;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, term_pair(v, &c0, 10, 40) == 0);		/* buffer 0 */
	TAP_ASSERT(t, term_pair(v, &c1, 10, 40) == 0);		/* buffer 1 (active) */

	n = term_collect(&v->e, fds, VEDIT_TERM_MAX);
	TAP_CHECKF(t, n == 2, "collect returned %d", n);

	/* feed the background buffer; term_drain must find it by fd */
	TAP_ASSERT(t, write(c0, "\033[33mBG\033[0m", 10) == 10);
	term_drain(&v->e, v->e.bufs[0].vterm->master_fd);
	cell = vt_buf_cell(v->e.bufs[0].vterm->vt->buf, 0, 0);
	TAP_ASSERT(t, cell != NULL);
	TAP_CHECK(t, cell->codepoint == 'B');

	close(c0);
	close(c1);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W W switches to the previous buffer. */
static void
t_term_loop_prev(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int c0, c1;

	v = term_editor_in(&m, &io, "\027W", 2, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &c0, 10, 40) == 0);
	TAP_ASSERT(t, term_pair(v, &c1, 10, 40) == 0);
	TAP_CHECK(t, v->e.cur == 1);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECKF(t, v->e.cur == 0, "prev left cur at %d", v->e.cur);

	close(c0);
	close(c1);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W c closes the active terminal when another buffer remains. */
static void
t_term_loop_close(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int c0, c1;

	v = term_editor_in(&m, &io, "\027c", 2, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &c0, 10, 40) == 0);
	TAP_ASSERT(t, term_pair(v, &c1, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECKF(t, v->e.nbuf == 1, "close left nbuf at %d", v->e.nbuf);

	close(c0);			/* the closed buffer's end */
	close(c1);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W <digit> switches to that buffer by number. */
static void
t_term_loop_digit(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int c0, c1;

	v = term_editor_in(&m, &io, "\0271", 2, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &c0, 10, 40) == 0);
	TAP_ASSERT(t, term_pair(v, &c1, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECKF(t, v->e.cur == 0, "digit left cur at %d", v->e.cur);

	close(c0);
	close(c1);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W m opens the menu bar; Esc backs out of it and leaves the terminal
 * focused, with nothing leaked to the child. */
static void
t_term_loop_menu(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, n;
	char buf[8];

	v = term_editor_in(&m, &io, "\027m\033", 3, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECK(t, term_is_active(&v->e));
	TAP_CHECK(t, v->e.term_prefix == 0);
	n = read_all(child, buf, sizeof(buf));
	TAP_CHECKF(t, n == 0, "menu command leaked %d bytes to child", n);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W : opens the ex command line; the command runs and the terminal
 * stays focused, with nothing leaked to the child. */
static void
t_term_loop_colon(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, n;
	char buf[8];

	v = term_editor_in(&m, &io, "\027:set ph=7\r", 12, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECK(t, term_is_active(&v->e));
	TAP_CHECKF(t, v->e.pane_rows == 7, "pane_rows %d", v->e.pane_rows);
	n = read_all(child, buf, sizeof(buf));
	TAP_CHECKF(t, n == 0, "ex command leaked %d bytes to child", n);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* Bytes typed before Ctrl-W are flushed to the child before the command. */
static void
t_term_loop_flush(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, n;
	char buf[8];

	v = term_editor_in(&m, &io, "ab\027\027", 4, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	n = read_all(child, buf, sizeof(buf));
	TAP_CHECKF(t, n == 3 && buf[0] == 'a' && buf[1] == 'b' && buf[2] == 0x17,
	    "child got %d bytes", n);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* ---- build commands in a tool terminal ---- */

/* Write a small file at dir/name. */
static void
write_file(const char *dir, const char *name, const char *text)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void
remove_file(const char *dir, const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
}

/* Output through a tool terminal is captured, stripped of colors and CRs,
 * parsed when the child end closes, and the editor lands on the first error.
 * Driven through a socketpair so no child is forked. */
static void
t_tool_term_capture(Test *t)
{
	static const char out[] =
	    "\033[1m\033[Ka.c:2:1:\033[m\033[K \033[01;31m\033[Kerror:\033[m"
	    "\033[K boom\r\n"
	    "a.c:3:1: warning: meh\r\n";
	char dir[] = "/tmp/vedit-tool-XXXXXX";
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, tries;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	write_file(dir, "a.c", "int a;\nint b;\nint c;\n");

	v = term_editor_in(&m, &io, "", 0, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);
	v->e.vterm->tool = 1;
	snprintf(v->e.vterm->tool_label, sizeof(v->e.vterm->tool_label), "Make");
	snprintf(v->e.tool_title, sizeof(v->e.tool_title), "Make");
	snprintf(v->e.tool_dir, sizeof(v->e.tool_dir), "%s", dir);

	TAP_ASSERT(t, write(child, out, sizeof(out) - 1) == (ssize_t)(sizeof(out) - 1));
	close(child);				/* "the command exited" */
	for (tries = 0; tries < 20 && term_is_active(&v->e); tries++)
		TAP_ASSERT(t, term_loop_step(&v->e) == TERM_CONT);

	TAP_CHECKF(t, v->e.tool_rawlen == sizeof(out) - 1, "captured %zu bytes",
	    v->e.tool_rawlen);
	TAP_CHECKF(t, v->e.tool_nerr == 2, "parsed %d diagnostics", v->e.tool_nerr);
	if (v->e.tool_nerr == 2) {
		TAP_CHECKF(t, strcmp(v->e.tool_errs[0].file, "a.c") == 0 &&
		    v->e.tool_errs[0].line == 2 && v->e.tool_errs[0].sev == TSEV_ERROR,
		    "first: %s:%zu sev %d", v->e.tool_errs[0].file,
		    v->e.tool_errs[0].line, v->e.tool_errs[0].sev);
		TAP_CHECK(t, v->e.tool_errs[1].sev == TSEV_WARN);
	}
	TAP_CHECKF(t, v->e.tool_nlines == 2 &&
	    strcmp(v->e.tool_lines[0], "a.c:2:1: error: boom") == 0,
	    "line 0 '%s'", v->e.tool_nlines ? v->e.tool_lines[0] : "");
	/* landed on a.c line 2 in a text buffer; the terminal stays open */
	TAP_CHECKF(t, v->e.kind == BUF_TEXT && v->e.cy == 1,
	    "kind %d cy %zu", v->e.kind, v->e.cy);
	TAP_CHECK(t, v->e.has_name && strstr(v->e.path, "a.c") != NULL);
	TAP_CHECK(t, v->e.nbuf == 2 && tool_term_find(&v->e, NULL) != NULL);
	TAP_CHECKF(t, strcmp(v->e.tool_result,
	    "Make exited 0, 1 error, 1 warning") == 0, "result '%s'",
	    v->e.tool_result);
	TAP_CHECKF(t, strcmp(v->e.status, v->e.tool_result) == 0, "status '%s'",
	    v->e.status);

	/* View Output switches back to the tool terminal, labelled by command */
	tool_view_output(&v->e);
	TAP_CHECK(t, term_is_active(&v->e) && v->e.vterm->tool);
	TAP_CHECKF(t, strcmp(term_label(&v->e), "Make") == 0, "label '%s'",
	    term_label(&v->e));
	ed_render(&v->e, v->e.d);		/* dead tool terminal: result in the status */
	TAP_CHECKF(t, strstr(v->e.status, "[Make exited 0, 1 error, 1 warning]") != NULL,
	    "status '%s'", v->e.status);

	vedit_free(v);
	memio_free(&m);
	remove_file(dir, "a.c");
	rmdir(dir);
}

/* tool_term_start forks the command through the shell in dir; the exit status
 * reaches the result line, and a second start replaces the finished terminal
 * rather than adding one. Skipped when a pty cannot be opened. */
static void
t_tool_term_start(Test *t)
{
	char dir[] = "/tmp/vedit-tool-XXXXXX";
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Term *old, *pt;
	int tries, idx = -1;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	write_file(dir, "b.c", "int a;\nint b;\nint c;\n");

	v = term_editor_in(&m, &io, "", 0, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	snprintf(v->e.tool_dir, sizeof(v->e.tool_dir), "%s", dir);

	if (tool_term_start(&v->e, "printf 'b.c:3:1: error: x\\n'; exit 3", dir,
	    "Make") < 0) {
		vedit_free(v);			/* no pty here: skip */
		memio_free(&m);
		remove_file(dir, "b.c");
		rmdir(dir);
		return;
	}
	TAP_CHECK(t, term_is_active(&v->e) && v->e.vterm->tool);
	for (tries = 0; tries < 200 && term_is_active(&v->e); tries++)
		if (term_loop_step(&v->e) != TERM_CONT)
			break;
	TAP_CHECKF(t, strcmp(v->e.tool_result, "Make exited 3, 1 error, 0 warnings") == 0,
	    "result '%s' after %d steps", v->e.tool_result, tries);
	TAP_CHECKF(t, v->e.kind == BUF_TEXT && v->e.cy == 2 &&
	    strstr(v->e.path, "b.c") != NULL, "kind %d cy %zu path %s",
	    v->e.kind, v->e.cy, v->e.path);
	old = tool_term_find(&v->e, &idx);
	TAP_CHECK(t, old != NULL && old->dead && old->exit_status == 3);
	TAP_CHECKF(t, v->e.nbuf == 2, "nbuf %d", v->e.nbuf);	/* term, b.c */

	/* a second build replaces the finished terminal; with a text buffer
	 * current it opens in the pane below and the text keeps the focus */
	TAP_CHECK(t, tool_term_start(&v->e, "exit 0", dir, "Make") == 0);
	TAP_CHECKF(t, v->e.nbuf == 2, "nbuf %d after restart", v->e.nbuf);
	pt = tool_term_find(&v->e, NULL);
	TAP_CHECK(t, pt != NULL && pt != old && pt->pane);
	TAP_CHECK(t, v->e.kind == BUF_TEXT && pane_shown(&v->e) && !v->e.pane_focus);
	TAP_CHECK(t, v->e.tool_nerr == 0);	/* the capture was cleared */
	v->e.pane_focus = 1;			/* so the loop step pumps it */
	for (tries = 0; tries < 200 && !pt->dead; tries++)
		if (term_loop_step(&v->e) != TERM_CONT)
			break;
	for (tries = 0; tries < 5 && v->e.tool_done_pending; tries++)
		term_loop_step(&v->e);
	TAP_CHECKF(t, strcmp(v->e.tool_result, "Make exited 0") == 0,
	    "result '%s'", v->e.tool_result);
	TAP_CHECK(t, v->e.kind == BUF_TEXT && pane_shown(&v->e));	/* output stays below */

	/* a running build refuses a second start */
	TAP_CHECK(t, tool_term_start(&v->e, "sleep 5", dir, "Make") == 0);
	TAP_CHECK(t, tool_term_start(&v->e, "exit 0", dir, "Make") < 0);
	TAP_CHECK(t, strstr(v->e.status, "still running") != NULL);

	vedit_free(v);
	memio_free(&m);
	remove_file(dir, "b.c");
	rmdir(dir);
}

/* :!cmd runs in a tool terminal labelled with the command, and its output is
 * parsed like a build's. Skipped when a pty cannot be opened. */
static void
t_tool_term_shell(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int tries;

	v = term_editor_in(&m, &io, "", 0, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	v->e.tools = &cli_tools;
	ed_shell_cmd(&v->e, "printf 'nothing to see\\n'; exit 4");
	if (!term_is_active(&v->e)) {
		vedit_free(v);			/* no pty here: skip */
		memio_free(&m);
		return;
	}
	TAP_CHECKF(t, strcmp(term_label(&v->e), "! printf 'nothing to see\\n'; exit 4") == 0,
	    "label '%s'", term_label(&v->e));
	for (tries = 0; tries < 200 && !v->e.vterm->dead; tries++)
		if (term_loop_step(&v->e) != TERM_CONT)
			break;
	for (tries = 0; tries < 5 && v->e.tool_done_pending; tries++)
		term_loop_step(&v->e);
	TAP_CHECKF(t, strcmp(v->e.tool_result, "! printf 'nothing to see\\n'; exit 4 exited 4") == 0,
	    "result '%s'", v->e.tool_result);
	TAP_CHECK(t, term_is_active(&v->e));	/* no error: stays on the output */
	TAP_CHECK(t, v->e.tool_nlines == 1 &&
	    strcmp(v->e.tool_lines[0], "nothing to see") == 0);

	vedit_free(v);
	memio_free(&m);
}

/* ---- the bottom pane ---- */

/* A command opened with :split runs in a pane under the text buffer, which
 * keeps the keys until Ctrl-W w moves the focus; the pane's output lands in
 * the rows under the separator; Ctrl-W w from the pane returns to the text
 * and pane_close removes it. Skipped when a pty cannot be opened. */
static void
t_pane_split(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Term *pt;
	Scrbuf *sb;
	int idx = -1, full, ph, tries, row;

	v = term_editor_in(&m, &io, "w", 1, 1);	/* "w" answers Ctrl-W */
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, buf_open(&v->e, NULL) == 0);	/* a text buffer */
	TAP_CHECK(t, v->e.kind == BUF_TEXT && !pane_shown(&v->e));
	full = text_height_full(&v->e);
	TAP_CHECKF(t, text_height(&v->e) == full, "no pane: %d of %d",
	    text_height(&v->e), full);

	pane_run(&v->e, "cat");
	pt = pane_term(&v->e, &idx);
	if (!pt) {
		vedit_free(v);			/* no pty here: skip */
		memio_free(&m);
		return;
	}
	TAP_CHECK(t, v->e.kind == BUF_TEXT && idx != v->e.cur);
	TAP_CHECK(t, pane_shown(&v->e) && !v->e.pane_focus);
	ph = pane_height(&v->e);
	TAP_CHECKF(t, text_height(&v->e) == full - ph - 1,
	    "text %d, full %d, pane %d", text_height(&v->e), full, ph);
	TAP_CHECKF(t, pt->rows == ph, "pane grid %d rows, want %d", pt->rows, ph);

	/* Ctrl-W + and - and :set paneheight resize the pane and its grid */
	pane_resize(&v->e, 2);
	TAP_CHECKF(t, pane_height(&v->e) == ph + 2 && pt->rows == ph + 2 &&
	    text_height(&v->e) == full - ph - 3, "grown to %d, grid %d",
	    pane_height(&v->e), pt->rows);
	pane_resize(&v->e, -1);
	TAP_CHECK(t, pane_height(&v->e) == ph + 1 && pt->rows == ph + 1);
	pane_set_rows(&v->e, 1);			/* clamps to the minimum */
	TAP_CHECK(t, pane_height(&v->e) == PANE_MIN_ROWS && pt->rows == PANE_MIN_ROWS);
	pane_set_rows(&v->e, 0);			/* back to a third */
	TAP_CHECK(t, pane_height(&v->e) == ph && pt->rows == ph &&
	    strstr(v->e.status, "a third") != NULL);

	/* Ctrl-W then the scripted "w" focuses the pane */
	pane_key(&v->e);
	TAP_CHECK(t, v->e.pane_focus == 1 && term_focus(&v->e) == pt);

	/* what cat echoes shows up in the pane rows */
	term_write(pt, "hi\r", 3);
	for (tries = 0; tries < 50; tries++) {
		TAP_ASSERT(t, term_loop_step(&v->e) == TERM_CONT);
		if (vt_buf_cell(pt->vt->buf, 1, 0) &&
		    vt_buf_cell(pt->vt->buf, 1, 0)->codepoint == 'h')
			break;
	}
	ed_render(&v->e, v->e.d);
	sb = v->e.d->t;
	row = CHROME_TOP + text_height(&v->e) + 1 + 1;	/* separator, echo, output */
	TAP_CHECKF(t, sb->cur[(size_t)row * sb->cols + CHROME_LEFT].codepoint == 'h',
	    "pane row %d col 1 is U+%04X", row,
	    sb->cur[(size_t)row * sb->cols + CHROME_LEFT].codepoint);
	TAP_CHECK(t, sb->cur[(size_t)(row - 2) * sb->cols + 0].codepoint == GL_H);

	/* Ctrl-W w from the pane: focus returns to the text */
	scr_raw_unread(v->e.d, (const unsigned char *)"\027w", 2);
	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECK(t, v->e.pane_focus == 0 && term_focus(&v->e) == NULL);

	pane_close(&v->e);
	TAP_CHECK(t, pane_term(&v->e, NULL) == NULL && v->e.nbuf == 1);
	TAP_CHECK(t, text_height(&v->e) == full);
	TAP_CHECK(t, strcmp(v->e.status, "pane closed") == 0);

	/* a new shell leaves the focus in the text; Ctrl-W s again moves in */
	pane_run(&v->e, NULL);
	pt = pane_term(&v->e, NULL);
	TAP_ASSERT(t, pt != NULL);
	TAP_CHECK(t, !v->e.pane_focus && strstr(v->e.status, "shell") != NULL);
	pane_run(&v->e, NULL);
	TAP_CHECK(t, v->e.pane_focus == 1);
	v->e.pane_focus = 0;
	pane_close(&v->e);

	vedit_free(v);
	memio_free(&m);
}

/* A text buffer in the pane: Ctrl-W b puts the current buffer below with the
 * previous one above, Ctrl-W w moves the focus (the current buffer) between
 * them while the layout stays, both buffers paint in their rows, and
 * closing the pane keeps the buffer. ui.paneheight sets the pane's rows. */
static void
t_pane_text(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Scrbuf *sb;
	int full, ph, prow0, cur_row;

	memio_init(&m, "wwc", 3, 24, 80);	/* answers for three Ctrl-Ws */
	memio_bind(&io, &m);
	memio_enable_fds(&io);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, buf_slot(&v->e) == 0);	/* register the initial buffer */
	buf_save(&v->e, &v->e.bufs[0]);
	text_insert(v->e.t, 0, 0, "top line", 8);
	TAP_ASSERT(t, buf_open(&v->e, NULL) == 1);
	text_insert(v->e.t, 0, 0, "pane line", 9);
	TAP_CHECK(t, v->e.cur == 1 && v->e.nbuf == 2 && !pane_shown(&v->e));
	full = text_height_full(&v->e);

	pane_buffer(&v->e);			/* buffer 1 below, buffer 0 above */
	TAP_CHECK(t, v->e.in_pane && pane_shown(&v->e));
	TAP_CHECKF(t, pane_top_idx(&v->e) == 0 && pane_text_idx(&v->e) == 1,
	    "top %d pane %d", pane_top_idx(&v->e), pane_text_idx(&v->e));
	ph = pane_height(&v->e);
	TAP_CHECKF(t, text_height(&v->e) == full - ph - 1, "text %d full %d ph %d",
	    text_height(&v->e), full, ph);
	prow0 = CHROME_TOP + text_height(&v->e) + 1;

	ed_render(&v->e, v->e.d);
	sb = v->e.d->t;
	TAP_CHECKF(t, sb->cur[(size_t)CHROME_TOP * sb->cols + CHROME_LEFT].codepoint == 't',
	    "top row shows U+%04X", sb->cur[(size_t)CHROME_TOP * sb->cols + CHROME_LEFT].codepoint);
	TAP_CHECKF(t, sb->cur[(size_t)prow0 * sb->cols + CHROME_LEFT].codepoint == 'p',
	    "pane row shows U+%04X", sb->cur[(size_t)prow0 * sb->cols + CHROME_LEFT].codepoint);
	TAP_CHECK(t, sb->cur[(size_t)(prow0 - 1) * sb->cols + 0].codepoint == GL_H);
	cur_row = sb->cursor_r;
	TAP_CHECKF(t, cur_row == prow0, "cursor row %d, want pane row %d", cur_row, prow0);
	TAP_CHECK(t, v->e.cur == 1);	/* the flat state was restored */

	/* Ctrl-W w: the focus goes up; the layout stays */
	pane_key(&v->e);
	TAP_CHECKF(t, v->e.cur == 0 && !v->e.in_pane && v->e.bufs[1].in_pane,
	    "after Ctrl-W w: cur %d", v->e.cur);
	TAP_CHECK(t, pane_shown(&v->e) && pane_top_idx(&v->e) == 0);
	ed_render(&v->e, v->e.d);
	TAP_CHECK(t, sb->cur[(size_t)CHROME_TOP * sb->cols + CHROME_LEFT].codepoint == 't');
	TAP_CHECK(t, sb->cur[(size_t)prow0 * sb->cols + CHROME_LEFT].codepoint == 'p');
	TAP_CHECKF(t, sb->cursor_r == CHROME_TOP, "cursor row %d", sb->cursor_r);

	/* and back down; the top is remembered */
	pane_key(&v->e);
	TAP_CHECKF(t, v->e.cur == 1 && v->e.in_pane && v->e.bufs[0].top_last,
	    "after second Ctrl-W w: cur %d", v->e.cur);

	/* Ctrl-W c: the pane goes, the buffer stays, and fills the frame */
	pane_key(&v->e);
	TAP_CHECK(t, !pane_shown(&v->e) && v->e.nbuf == 2 && v->e.cur == 1);
	TAP_CHECK(t, text_height(&v->e) == full);
	TAP_CHECK(t, strcmp(v->e.status, "pane closed") == 0);

	/* a configured height wins over the third */
	v->e.pane_rows = 5;
	TAP_CHECKF(t, pane_height(&v->e) == 5, "pane rows %d", pane_height(&v->e));
	v->e.pane_rows = 0;

	vedit_free(v);
	memio_free(&m);
}

/* Without a multiplexing host, in_refill uses the plain poll fallback. */
static void
t_term_poll_fallback(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;

	v = term_editor(&m, &io);		/* no memio_enable_fds: poll only */
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);
	TAP_CHECK(t, v->e.d->t->io.poll_fds == NULL);

	/* memio has no input, so the fallback path reads EOF */
	TAP_CHECK(t, scr_pump(v->e.d, 0) < 0);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* term_discard frees a handle that was never installed on a buffer. */
static void
t_term_discard(Test *t)
{
	Term *h = term_make(10, 40);
	int p[2];

	TAP_ASSERT(t, h != NULL);
	TAP_ASSERT(t, pipe(p) == 0);
	h->master_fd = p[1];		/* a real fd to close; no child to reap */
	term_discard(h);		/* closes the fd, frees vt/parser/handle */
	close(p[0]);
}

/* term_open refuses without a multiplexing host. */
static void
t_term_open_nomux(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	v = term_editor(&m, &io);		/* no memio_enable_fds */
	TAP_ASSERT(t, v != NULL);
	v->e.t = text_new();			/* term_open refuses before using it */
	TAP_CHECK(t, term_open(&v->e, NULL) < 0);
	TAP_CHECK(t, v->e.kind != BUF_TERM);

	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-W n opens a second terminal (a forked child). */
static void
t_term_loop_newwin(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child, before;

	v = term_editor_in(&m, &io, "\027n", 2, 1);
	TAP_ASSERT(t, v != NULL);
	g_winch = 0;
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);
	before = v->e.nbuf;

	if (term_loop_step(&v->e) != TERM_CONT) {
		close(child);
		vedit_free(v);
		memio_free(&m);
		return;
	}
	/* a new terminal buffer appears (unless the spawn failed) */
	TAP_CHECK(t, v->e.nbuf >= before);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* A resize to a different size resizes the PTY and grid through the loop. */
static void
t_term_loop_resize_grow(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;

	v = term_editor_in(&m, &io, "", 0, 1);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, term_pair(v, &child, 10, 40) == 0);

	m.rows = 40;				/* getsize now reports a new size */
	m.cols = 100;
	g_winch = 1;
	TAP_CHECK(t, term_loop_step(&v->e) == TERM_CONT);
	TAP_CHECK(t, v->e.rows == 40 && v->e.cols == 100);
	TAP_CHECK(t, v->e.vterm->rows == text_height(&v->e));

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* A cursor reported past the visible area is clamped into it. */
static void
t_term_cursor_clamp(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	/* grid taller than the text area so the child can park the cursor
	 * below the last visible row, forcing the clamp */
	TAP_ASSERT(t, term_pair(v, &child, 40, 40) == 0);

	TAP_ASSERT(t, write(child, "\033[40;40H", 8) == 8);
	term_drain(&v->e, v->e.vterm->master_fd);
	/* render_body, not ed_render: the latter first resizes the grid to
	 * the area, which would remove the case being tested */
	render_body(&v->e, v->e.d);		/* must not place the cursor off-area */
	TAP_CHECK(t, v->e.vterm->vt->cursor_row >= text_height(&v->e));

	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* Freeing a buffer whose child is still alive hangs it up and reaps it. */
static void
t_term_kill(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	v = term_editor_in(&m, &io, "", 0, 1);
	TAP_ASSERT(t, v != NULL);
	if (term_open(&v->e, "sleep 30") < 0) {
		vedit_free(v);			/* spawn unavailable: skip */
		memio_free(&m);
		return;
	}
	TAP_CHECK(t, v->e.vterm->child_pid > 0);
	TAP_CHECK(t, !v->e.vterm->dead);
	vedit_free(v);				/* term_buf_free: SIGHUP + reap */
	memio_free(&m);
}

/* term_label, term_buf_free, and term_discard all no-op on a non-terminal
 * buffer or a NULL handle. */
static void
t_term_nonterm(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Buf b;

	v = term_editor(&m, &io);		/* a plain text buffer, no attach */
	TAP_ASSERT(t, v != NULL);
	TAP_CHECK(t, v->e.kind != BUF_TERM);
	TAP_CHECK(t, strcmp(term_label(&v->e), "terminal") == 0);

	memset(&b, 0, sizeof(b));		/* kind == BUF_TEXT */
	term_buf_free(&b);			/* no vterm: nothing to free */
	term_discard(NULL);			/* NULL handle: no-op */

	vedit_free(v);
	memio_free(&m);
}

/* term_drain ignores an fd that no terminal owns. */
static void
t_term_drain_unknown(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int pr[2];

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, pipe(pr) == 0);
	TAP_ASSERT(t, term_attach(&v->e, pr[0], 10, 40) >= 0);

	term_drain(&v->e, pr[1]);		/* an fd no terminal reads from */

	close(pr[1]);
	vedit_free(v);
	memio_free(&m);
}

/* A cursor reported above or left of the visible area is clamped into it, as
 * is a column past the right edge. */
static void
t_term_cursor_clamp_edges(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, term_pair(v, &child, 40, 200) == 0);

	v->e.vterm->vt->cursor_row = -5;	/* above the top row */
	v->e.vterm->vt->cursor_col = -5;	/* left of the first column */
	ed_render(&v->e, v->e.d);		/* must clamp to (0,0) */

	v->e.vterm->vt->cursor_row = 0;
	v->e.vterm->vt->cursor_col = 1000;	/* past the right edge */
	ed_render(&v->e, v->e.d);		/* must clamp to text_width - 1 */

	TAP_CHECK(t, 1);			/* reached here without an off-area cursor */
	close(child);
	vedit_free(v);
	memio_free(&m);
}

/* term_attach treats a non-positive rows/cols as one. */
static void
t_term_attach_tiny(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int pr[2];

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, pipe(pr) == 0);
	TAP_ASSERT(t, term_attach(&v->e, pr[0], 0, 0) >= 0);
	TAP_CHECK(t, v->e.vterm->rows == 1 && v->e.vterm->cols == 1);

	close(pr[1]);
	vedit_free(v);
	memio_free(&m);
}

/* term_resize_all skips a terminal already at the target size and one whose
 * child has gone, and term_write to a dead terminal is a no-op. */
static void
t_term_resize_skips(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int pr[2];

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, pipe(pr) == 0);
	TAP_ASSERT(t, term_attach(&v->e, pr[0], 10, 40) >= 0);

	term_resize_all(&v->e);			/* sets the grid to the text area */
	term_resize_all(&v->e);			/* same size: nothing to do */

	close(pr[1]);				/* EOF: the terminal goes dead */
	term_drain(&v->e, pr[0]);
	TAP_CHECK(t, v->e.vterm->dead == 1);
	term_resize_all(&v->e);			/* dead/no-fd: skipped */
	term_write(v->e.vterm, "x", 1);		/* dead: no-op */

	vedit_free(v);
	memio_free(&m);
}

/* ---- the art view ---- */

static void
art_press(Editor *e, uint16_t key, uint32_t ch, int mod)
{
	struct tkbd_seq seq;

	memset(&seq, 0, sizeof(seq));
	seq.type = TKBD_KEY;
	seq.key = key;
	seq.ch = ch;
	seq.mod = mod;
	run_req(e, art_key(e, &seq));
}

static void
art_type(Editor *e, const char *s)
{
	for (; *s; s++)
		art_press(e, (uint16_t)toupper((unsigned char)*s),
		    (uint32_t)(unsigned char)*s, 0);
}

/* A .ans file opens as a grid, renders through the emulator's cells, and
 * saves back as SGR-coloured UTF-8 with blank rows and cells dropped. */
static void
t_art_roundtrip(Test *t)
{
	char dir[] = "/tmp/vedit-art-XXXXXX";
	char path[PATH_MAX], back[256];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Scrbuf *sb;
	const Cell *c;
	FILE *f;
	size_t n;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	write_file(dir, "a.ans",
	    "\033[1;31mRed\033[0m x\r\n"
	    "\r\n"
	    "\xe6\x97\xa5 \033[44m  \033[0m\r\n"
	    "\r\n\r\n");
	snprintf(path, sizeof(path), "%s/a.ans", dir);

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.art != NULL);
	TAP_CHECKF(t, v->e.art->cols == 80 && v->e.art->rows == 6,
	    "grid %dx%d", v->e.art->cols, v->e.art->rows);
	TAP_CHECK(t, text_eol(v->e.t) == EOL_CRLF);

	c = art_cell(v->e.art, 0, 0);
	TAP_CHECK(t, c->codepoint == 'R' && (c->attrs & ATTR_BOLD) &&
	    c->fg.type == COLOR_INDEXED && c->fg.index == 1);
	c = art_cell(v->e.art, 0, 4);
	TAP_CHECK(t, c->codepoint == 'x' && art_cell_plain(c));
	c = art_cell(v->e.art, 2, 0);
	TAP_CHECK(t, c->codepoint == 0x65e5 && c->width == 2);
	c = art_cell(v->e.art, 2, 1);
	TAP_CHECK(t, c->width == 0);
	c = art_cell(v->e.art, 2, 3);
	TAP_CHECK(t, c->codepoint == ' ' && c->bg.type == COLOR_INDEXED &&
	    c->bg.index == 4);

	/* the frame shows the cells with their colours */
	ed_render(&v->e, v->e.d);
	sb = v->e.d->t;
	TAP_CHECK(t, sb->cur[(size_t)CHROME_TOP * sb->cols + CHROME_LEFT].codepoint == 'R');
	TAP_CHECK(t, sb->cur[(size_t)CHROME_TOP * sb->cols + CHROME_LEFT].fg.index == 1);
	TAP_CHECK(t, sb->cur[(size_t)(CHROME_TOP + 2) * sb->cols + CHROME_LEFT + 3].bg.index == 4);
	TAP_CHECK(t, !text_dirty(v->e.t));

	/* an untouched grid saves back the same picture */
	TAP_ASSERT(t, save_editor(&v->e) == 0);
	f = fopen(path, "rb");
	TAP_ASSERT(t, f != NULL);
	n = fread(back, 1, sizeof(back) - 1, f);
	fclose(f);
	back[n] = '\0';
	TAP_CHECKF(t, strcmp(back,
	    "\033[0;1;31mRed\033[0m x\r\n"
	    "\r\n"
	    "\xe6\x97\xa5 \033[0;44m  \033[0m\r\n") == 0, "saved [%s]", back);
	TAP_CHECK(t, !text_dirty(v->e.t) && v->e.art != NULL);

	vedit_free(v);
	memio_free(&m);
	remove_file(dir, "a.ans");
	rmdir(dir);
}

/* Keys edit cells with the pen; the rectangle, box and undo work on cells. */
static void
t_art_edit(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Art *a;
	const Cell *c;
	size_t len;
	const char *line;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	snprintf(v->e.path, sizeof(v->e.path), "new.ans");
	v->e.has_name = 1;
	art_sync_file(&v->e);
	TAP_ASSERT(t, v->e.art != NULL);
	a = v->e.art;
	TAP_CHECKF(t, a->rows == 1 && a->cols == 80, "grid %dx%d", a->cols, a->rows);

	/* Alt+Up picks fg 0, Alt+Right bg 0, Alt+b bold; typing uses them */
	art_press(&v->e, TKBD_KEY_UP, TKBD_CH_NONE, TKBD_MOD_ALT);
	art_press(&v->e, TKBD_KEY_RIGHT, TKBD_CH_NONE, TKBD_MOD_ALT);
	art_press(&v->e, 'b', 'b', TKBD_MOD_ALT);
	TAP_CHECK(t, a->fg.type == COLOR_INDEXED && a->fg.index == 0 &&
	    a->bg.index == 0 && (a->attrs & ATTR_BOLD));
	art_type(&v->e, "hi");
	TAP_CHECK(t, a->cx == 2 && text_dirty(v->e.t));
	c = art_cell(a, 0, 0);
	TAP_CHECK(t, c->codepoint == 'h' && c->fg.index == 0 && (c->attrs & ATTR_BOLD));

	/* Enter is a carriage return; rows grow under the cursor */
	art_press(&v->e, TKBD_KEY_ENTER, TKBD_CH_NONE, 0);
	art_press(&v->e, TKBD_KEY_DOWN, TKBD_CH_NONE, 0);
	TAP_CHECK(t, a->cy == 2 && a->cx == 0 && a->rows == 1);
	art_press(&v->e, 'r', 'r', TKBD_MOD_ALT);	/* plain pen again */
	art_type(&v->e, "z");
	TAP_CHECKF(t, a->rows == 3, "rows %d", a->rows);

	/* Shift+arrows mark 3x2 from (2,0); Ctrl-B draws a box in it */
	art_press(&v->e, TKBD_KEY_LEFT, TKBD_CH_NONE, 0);
	art_press(&v->e, TKBD_KEY_RIGHT, TKBD_CH_NONE, TKBD_MOD_SHIFT);
	art_press(&v->e, TKBD_KEY_RIGHT, TKBD_CH_NONE, TKBD_MOD_SHIFT);
	art_press(&v->e, TKBD_KEY_DOWN, TKBD_CH_NONE, TKBD_MOD_SHIFT);
	TAP_CHECK(t, v->e.sel_active && a->cy == 3 && a->cx == 2);
	art_press(&v->e, TKBD_KEY_B, 0x02, TKBD_MOD_CTRL);
	TAP_CHECK(t, !v->e.sel_active && a->rows == 4);
	TAP_CHECK(t, art_cell(a, 2, 0)->codepoint == 0x250c &&
	    art_cell(a, 2, 1)->codepoint == 0x2500 &&
	    art_cell(a, 2, 2)->codepoint == 0x2510 &&
	    art_cell(a, 3, 0)->codepoint == 0x2514 &&
	    art_cell(a, 3, 2)->codepoint == 0x2518);

	/* undo restores the 'z' and the row count; redo brings the box back */
	art_press(&v->e, TKBD_KEY_Z, 0x1a, TKBD_MOD_CTRL);
	TAP_CHECKF(t, a->rows == 3 && art_cell(a, 2, 0)->codepoint == 'z',
	    "after undo: rows %d cell U+%04X", a->rows, art_cell(a, 2, 0)->codepoint);
	art_press(&v->e, TKBD_KEY_Y, 0x19, TKBD_MOD_CTRL);
	TAP_CHECK(t, a->rows == 4 && art_cell(a, 2, 0)->codepoint == 0x250c);

	/* copy the box, paste it at the top right */
	a->cy = 2;
	a->cx = 0;
	art_press(&v->e, TKBD_KEY_RIGHT, TKBD_CH_NONE, TKBD_MOD_SHIFT);
	art_press(&v->e, TKBD_KEY_RIGHT, TKBD_CH_NONE, TKBD_MOD_SHIFT);
	art_press(&v->e, TKBD_KEY_DOWN, TKBD_CH_NONE, TKBD_MOD_SHIFT);
	art_press(&v->e, TKBD_KEY_C, 0x03, TKBD_MOD_CTRL);
	TAP_CHECK(t, v->e.art_clip_w == 3 && v->e.art_clip_h == 2);
	a->cy = 0;
	a->cx = 4;
	art_press(&v->e, TKBD_KEY_V, 0x16, TKBD_MOD_CTRL);
	TAP_CHECK(t, art_cell(a, 0, 4)->codepoint == 0x250c &&
	    art_cell(a, 1, 6)->codepoint == 0x2518);

	/* Backspace erases the cell to the left; Delete the one under */
	a->cy = 0;
	a->cx = 2;
	art_press(&v->e, TKBD_KEY_BACKSPACE, TKBD_CH_NONE, 0);
	TAP_CHECK(t, a->cx == 1 && art_cell(a, 0, 1)->codepoint == ' ' &&
	    art_cell_plain(art_cell(a, 0, 1)));

	/* the saved form: a bold black-on-black h, the pasted box, and so on */
	TAP_ASSERT(t, art_export(&v->e) == 0);
	TAP_CHECKF(t, text_lines(v->e.t) == 4, "%zu lines", text_lines(v->e.t));
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(line, "\033[0;1;30;40mh\033[0m   \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90") == 0,
	    "line 0 [%s]", line);
	line = text_line(v->e.t, 2, &len);
	TAP_CHECKF(t, strcmp(line, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90") == 0,
	    "line 2 [%s]", line);

	vedit_free(v);
	memio_free(&m);
}

/* Alt+digit inserts from the active glyph set, in draw mode over the text and
 * in the art view with the pen; the colour grid sets the pen. */
static void
t_palettes(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	struct tkbd_seq seq;
	Modal md;
	Event ev;
	Colorctx cc;
	size_t len;
	const char *line;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);

	/* draw mode: Alt+1 of the first set is a horizontal line glyph */
	draw_toggle(&v->e);
	memset(&seq, 0, sizeof(seq));
	seq.type = TKBD_KEY;
	seq.mod = TKBD_MOD_ALT;
	seq.ch = '1';
	seq.key = '1';
	run_req(&v->e, draw_key(&v->e, &seq));
	v->e.glyph_set = 4;			/* Blocks: slot 0 is a full block */
	seq.ch = '0';
	seq.key = '0';
	run_req(&v->e, draw_key(&v->e, &seq));
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, len == 6 && memcmp(line, "\xe2\x94\x80\xe2\x96\xa1", 6) == 0,
	    "draw line [%.*s]", (int)len, line);
	TAP_CHECK(t, v->e.cx == 6);		/* a byte offset in the text */
	draw_toggle(&v->e);

	/* the art view: the glyph takes the pen */
	snprintf(v->e.path, sizeof(v->e.path), "p.ans");
	v->e.has_name = 1;
	art_sync_file(&v->e);
	TAP_ASSERT(t, v->e.art != NULL);
	v->e.art->fg = art_idx(3);
	v->e.glyph_set = 0;
	seq.ch = '2';
	seq.key = '2';
	run_req(&v->e, art_key(&v->e, &seq));
	TAP_CHECK(t, art_cell(v->e.art, 0, 0)->codepoint == 0x2502 &&
	    art_cell(v->e.art, 0, 0)->fg.index == 3 && v->e.art->cx == 1);

	/* the colour grid: start on the pen, move, Enter sets both */
	cc.fy = art_pal_index(v->e.art->fg);
	cc.fx = art_pal_index(v->e.art->bg);
	cc.pick = 0;
	TAP_CHECK(t, cc.fy == 4 && cc.fx == 0);
	memset(&ev, 0, sizeof(ev));
	ev.type = EVENT_KEY;
	ev.key.type = TKBD_KEY;
	ev.key.ch = TKBD_CH_NONE;
	ev.key.key = TKBD_KEY_DOWN;
	TAP_CHECK(t, dlg_color_key(&v->e, &md, &ev, &cc) == 0);
	ev.key.key = TKBD_KEY_RIGHT;
	TAP_CHECK(t, dlg_color_key(&v->e, &md, &ev, &cc) == 0);
	ev.key.key = TKBD_KEY_ENTER;
	TAP_CHECK(t, dlg_color_key(&v->e, &md, &ev, &cc) == 1 && cc.pick == 1);
	TAP_CHECK(t, cc.fy == 5 && cc.fx == 1);
	v->e.art->fg = art_pal_color(cc.fy);
	v->e.art->bg = art_pal_color(cc.fx);
	TAP_CHECK(t, v->e.art->fg.index == 4 && v->e.art->bg.type == COLOR_INDEXED &&
	    v->e.art->bg.index == 0);
	ev.key.key = TKBD_KEY_NONE;
	ev.key.ch = 'f';
	TAP_CHECK(t, dlg_color_key(&v->e, &md, &ev, &cc) == 1 && cc.pick == 2);

	/* the glyph dialog's keys: a digit picks that slot and closes */
	{
		Glyphctx g = { 0, -1 };

		ev.key.ch = TKBD_CH_NONE;
		ev.key.key = TKBD_KEY_DOWN;
		TAP_CHECK(t, dlg_glyph_key(&v->e, &md, &ev, &g) == 0 &&
		    v->e.glyph_set == 1);
		ev.key.key = TKBD_KEY_NONE;
		ev.key.ch = '3';
		TAP_CHECK(t, dlg_glyph_key(&v->e, &md, &ev, &g) == 1 && g.insert == 2);
	}

	vedit_free(v);
	memio_free(&m);
}

/* Repost copies the scrollback and the screen into a new buffer: as text with
 * wrapped rows rejoined and trailing blanks dropped, or as art with colours. */
static void
t_term_repost(Test *t)
{
	static const char out[] =
	    "one\r\ntwo\r\nthree\r\n"
	    "abcdefghijklmnopqrstuvwxy\r\n"	/* wraps at 20 columns */
	    "\033[1;32mgreen\033[0m end\r\n";
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int child;
	size_t len;
	const char *line;

	v = term_editor(&m, &io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, term_pair(v, &child, 4, 20) == 0);
	TAP_ASSERT(t, write(child, out, sizeof(out) - 1) == (ssize_t)(sizeof(out) - 1));
	term_drain(&v->e, v->e.vterm->master_fd);
	TAP_CHECKF(t, vt_buf_scrollback_lines(v->e.vterm->vt->buf) == 3,
	    "%d lines scrolled off", vt_buf_scrollback_lines(v->e.vterm->vt->buf));

	TAP_ASSERT(t, term_repost_text(&v->e) == 0);
	TAP_CHECK(t, v->e.nbuf == 2 && v->e.cur == 1 && v->e.kind == BUF_TEXT);
	TAP_CHECKF(t, text_lines(v->e.t) == 5, "%zu lines", text_lines(v->e.t));
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, len == 3 && memcmp(line, "one", 3) == 0, "line 0 [%.*s]", (int)len, line);
	line = text_line(v->e.t, 3, &len);
	TAP_CHECKF(t, len == 25 && memcmp(line, "abcdefghijklmnopqrstuvwxy", 25) == 0,
	    "line 3 [%.*s]", (int)len, line);
	line = text_line(v->e.t, 4, &len);
	TAP_CHECKF(t, len == 9 && memcmp(line, "green end", 9) == 0, "line 4 [%.*s]", (int)len, line);
	TAP_CHECK(t, text_dirty(v->e.t) && v->e.art == NULL);

	/* from the text buffer there is no terminal to read */
	TAP_CHECK(t, term_repost_src(&v->e) == NULL);
	TAP_CHECK(t, term_repost_art(&v->e) < 0);

	buf_switch(&v->e, 0);
	TAP_ASSERT(t, term_is_active(&v->e));
	TAP_ASSERT(t, term_repost_art(&v->e) == 0);
	TAP_CHECK(t, v->e.nbuf == 3 && v->e.cur == 2 && v->e.art != NULL);
	TAP_CHECKF(t, v->e.art->rows == 6 && v->e.art->cols == 20, "art %dx%d",
	    v->e.art->cols, v->e.art->rows);
	TAP_CHECK(t, art_cell(v->e.art, 0, 0)->codepoint == 'o');
	TAP_CHECK(t, art_cell(v->e.art, 5, 0)->codepoint == 'g' &&
	    art_cell(v->e.art, 5, 0)->fg.index == 2 &&
	    (art_cell(v->e.art, 5, 0)->attrs & ATTR_BOLD));
	TAP_CHECK(t, art_cell_plain(art_cell(v->e.art, 5, 6)));
	TAP_CHECK(t, v->e.bufs[2].art == v->e.art);

	close(child);
	vedit_free(v);
	memio_free(&m);
}

const Case tap_cases[] = {
	{ "term_attach_render", t_term_attach_render },
	{ "term_collect", t_term_collect },
	{ "term_resize", t_term_resize },
	{ "term_title_cursor", t_term_title_cursor },
	{ "term_fork_smoke", t_term_fork_smoke },
	{ "term_loop_input", t_term_loop_input },
	{ "term_loop_literal", t_term_loop_literal },
	{ "term_loop_cmd", t_term_loop_cmd },
	{ "term_loop_quit", t_term_loop_quit },
	{ "term_loop_output", t_term_loop_output },
	{ "term_loop_resize", t_term_loop_resize },
	{ "tty_poll_fds", t_tty_poll_fds },
	{ "term_label_default", t_term_label_default },
	{ "term_two_bg", t_term_two_bg },
	{ "term_loop_prev", t_term_loop_prev },
	{ "term_loop_close", t_term_loop_close },
	{ "term_loop_digit", t_term_loop_digit },
	{ "term_loop_flush", t_term_loop_flush },
	{ "term_loop_menu", t_term_loop_menu },
	{ "tool_term_capture", t_tool_term_capture },
	{ "tool_term_start", t_tool_term_start },
	{ "tool_term_shell", t_tool_term_shell },
	{ "term_loop_colon", t_term_loop_colon },
	{ "pane_split", t_pane_split },
	{ "pane_text", t_pane_text },
	{ "term_poll_fallback", t_term_poll_fallback },
	{ "term_discard", t_term_discard },
	{ "term_open_nomux", t_term_open_nomux },
	{ "term_loop_newwin", t_term_loop_newwin },
	{ "term_loop_resize_grow", t_term_loop_resize_grow },
	{ "term_cursor_clamp", t_term_cursor_clamp },
	{ "term_kill", t_term_kill },
	{ "term_nonterm", t_term_nonterm },
	{ "term_drain_unknown", t_term_drain_unknown },
	{ "term_cursor_clamp_edges", t_term_cursor_clamp_edges },
	{ "term_attach_tiny", t_term_attach_tiny },
	{ "term_resize_skips", t_term_resize_skips },
	{ "art_roundtrip", t_art_roundtrip },
	{ "art_edit", t_art_edit },
	{ "palettes", t_palettes },
	{ "term_repost", t_term_repost },
	{ NULL, NULL },
};
