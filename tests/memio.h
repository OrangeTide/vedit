/*
 * memio: an in-memory vedit_io for headless tests. It feeds the editor a
 * scripted stream of input bytes and captures everything the editor writes, so
 * a test can drive vedit_run() (or any renderer path) without a terminal. When
 * the scripted input runs out, read() returns -1, which the editor treats as
 * end of input and quits, so a run always terminates.
 *
 * Include this after vedit.c, so struct vedit_io is in scope.
 */
#ifndef MEMIO_H
#define MEMIO_H

#include <stdlib.h>
#include <string.h>

typedef struct {
	const unsigned char *in;	/* scripted input */
	size_t		inlen, inpos;
	char		*out;		/* captured output (NUL-terminated) */
	size_t		outlen, outcap;
	int		rows, cols;
} Memio;

static long
memio_read(void *ctx, void *buf, long n)
{
	Memio *m = ctx;
	size_t avail = m->inlen - m->inpos;

	if (avail == 0)
		return -1;		/* end of input: the editor quits */
	if (n < 0)
		n = 0;
	if ((size_t)n > avail)
		n = (long)avail;
	memcpy(buf, m->in + m->inpos, (size_t)n);
	m->inpos += (size_t)n;
	return n;
}

static long
memio_write(void *ctx, const void *buf, long n)
{
	Memio *m = ctx;

	if (n < 0)
		return -1;
	if (m->outlen + (size_t)n + 1 > m->outcap) {
		size_t cap = m->outcap ? m->outcap : 8192;
		char *p;

		while (cap < m->outlen + (size_t)n + 1)
			cap *= 2;
		p = realloc(m->out, cap);
		if (!p)
			return -1;
		m->out = p;
		m->outcap = cap;
	}
	memcpy(m->out + m->outlen, buf, (size_t)n);
	m->outlen += (size_t)n;
	m->out[m->outlen] = '\0';
	return n;
}

/* Always "ready": read() then reports data or end of input, so the run loop
 * never blocks and always reaches the end-of-input quit. */
static int
memio_poll(void *ctx, int timeout_ms)
{
	(void)ctx;
	(void)timeout_ms;
	return 1;
}

static int
memio_getsize(void *ctx, int *rows, int *cols)
{
	Memio *m = ctx;

	*rows = m->rows;
	*cols = m->cols;
	return 0;
}

static void
memio_init(Memio *m, const char *keys, size_t keylen, int rows, int cols)
{
	memset(m, 0, sizeof(*m));
	m->in = (const unsigned char *)keys;
	m->inlen = keylen;
	m->rows = rows;
	m->cols = cols;
}

static void
memio_bind(struct vedit_io *io, Memio *m)
{
	memset(io, 0, sizeof(*io));
	io->ctx = m;
	io->read = memio_read;
	io->write = memio_write;
	io->poll = memio_poll;
	io->getsize = memio_getsize;
}

static void
memio_free(Memio *m)
{
	free(m->out);
	m->out = NULL;
}

#ifdef VEDIT_TERM
#include <sys/select.h>

/* A poll_fds that waits on the terminal PTY fds (the keyboard is the scripted
 * input, reported ready while any remains). Lets a test drive term_open, which
 * refuses without a multiplexing host. */
static int
memio_poll_fds(void *ctx, int timeout_ms, const int *extra, int nextra,
    int *ready, int *nready)
{
	Memio *m = ctx;
	fd_set rf;
	struct timeval tv, *ptv = NULL;
	int i, maxfd = -1, r;

	FD_ZERO(&rf);
	for (i = 0; i < nextra; i++) {
		if (extra[i] < 0)
			continue;
		FD_SET(extra[i], &rf);
		if (extra[i] > maxfd)
			maxfd = extra[i];
	}
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		ptv = &tv;
	}
	*nready = 0;
	if (maxfd >= 0) {
		r = select(maxfd + 1, &rf, NULL, NULL, ptv);
		if (r > 0)
			for (i = 0; i < nextra; i++)
				if (extra[i] >= 0 && FD_ISSET(extra[i], &rf))
					ready[(*nready)++] = extra[i];
	}
	return (m->inpos < m->inlen) ? 1 : 0;
}

/* Not every test that includes this header multiplexes fds (the torture suite,
 * for one), so the terminal now being built by default leaves this unused in
 * those units. */
__attribute__((unused)) static void
memio_enable_fds(struct vedit_io *io)
{
	io->poll_fds = memio_poll_fds;
}
#endif /* VEDIT_TERM */

#endif /* MEMIO_H */
