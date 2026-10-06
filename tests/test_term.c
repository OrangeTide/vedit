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
#include <unistd.h>

/* Build an editor over memio (no terminal) and drop the empty initial text, so
 * a terminal buffer created with term_attach/term_open is the only buffer and
 * nothing leaks at teardown. */
static struct vedit *
term_editor(Memio *m, struct vedit_io *io)
{
	struct vedit *v;

	memio_init(m, "", 0, 24, 80);
	memio_bind(io, m);
	v = vedit_new(io);
	if (v) {
		text_free(v->e.t);	/* replaced by the terminal's own text */
		v->e.t = NULL;
	}
	return v;
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

const Case tap_cases[] = {
	{ "term_attach_render", t_term_attach_render },
	{ "term_collect", t_term_collect },
	{ "term_resize", t_term_resize },
	{ "term_title_cursor", t_term_title_cursor },
	{ "term_fork_smoke", t_term_fork_smoke },
	{ NULL, NULL },
};
