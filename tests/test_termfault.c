/*
 * Terminal allocation-failure paths, exercised via the linker's --wrap so no
 * change to vedit.c is needed. Built with -Wl,--wrap=calloc,--wrap=realloc:
 * calls to those names in the linked objects go to __wrap_calloc/__wrap_realloc,
 * and the real ones are __real_calloc/__real_realloc.
 *
 * libvt allocates through the abort-on-OOM x* wrappers, so failing one of its
 * allocations would abort the process rather than return NULL. The terminal
 * code's own allocations are plain: the Term handle (calloc), the buffer text
 * and its line array (calloc/realloc), and the buffer slot (realloc). A case
 * therefore arms "fail the next calloc" or "fail the next realloc" immediately
 * before the call, where that next allocation is the plain one; the x* ones run
 * afterward and succeed.
 */
#include "test.h"

#define main test_main
#include "../vedit.c"
#undef main

#include "memio.h"

#include <unistd.h>

void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);

static int fail_calloc;		/* fail (and disarm) the next calloc */
static int fail_realloc;	/* fail (and disarm) the next realloc */

void *
__wrap_calloc(size_t n, size_t s)
{
	if (fail_calloc) {
		fail_calloc = 0;
		return NULL;
	}
	return __real_calloc(n, s);
}

void *
__wrap_realloc(void *p, size_t n)
{
	if (fail_realloc) {
		fail_realloc = 0;
		return NULL;
	}
	return __real_realloc(p, n);
}

/* Fresh editor over memio, initial text dropped. With mux, memio offers the
 * poll_fds hook that term_open requires. */
static struct vedit *
fault_editor(Memio *m, struct vedit_io *io, int mux)
{
	struct vedit *v;

	memio_init(m, "", 0, 24, 80);
	memio_bind(io, m);
	if (mux)
		memio_enable_fds(io);
	v = vedit_new(io);
	if (v) {
		text_free(v->e.t);
		v->e.t = NULL;
	}
	return v;
}

/* term_attach fails cleanly when the Term handle's calloc fails. */
static void
t_fault_attach_make(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int r;

	v = fault_editor(&m, &io, 0);
	TAP_ASSERT(t, v != NULL);
	fail_calloc = 1;
	r = term_attach(&v->e, -1, 10, 40);
	fail_calloc = 0;
	TAP_CHECK(t, r < 0);
	TAP_CHECK(t, v->e.kind != BUF_TERM);

	vedit_free(v);
	memio_free(&m);
}

/* term_attach fails cleanly and discards the handle when the buffer setup
 * (text line array or buffer slot) fails to allocate. */
static void
t_fault_attach_install(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int r;

	v = fault_editor(&m, &io, 0);
	TAP_ASSERT(t, v != NULL);
	fail_realloc = 1;
	r = term_attach(&v->e, -1, 10, 40);
	fail_realloc = 0;
	TAP_CHECK(t, r < 0);
	TAP_CHECK(t, v->e.kind != BUF_TERM);

	vedit_free(v);
	memio_free(&m);
}

/* term_open reports out of memory when the Term handle's calloc fails, before
 * it spawns anything. */
static void
t_fault_open_make(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int r;

	v = fault_editor(&m, &io, 1);
	TAP_ASSERT(t, v != NULL);
	fail_calloc = 1;
	r = term_open(&v->e, NULL);
	fail_calloc = 0;
	TAP_CHECK(t, r < 0);
	TAP_CHECK(t, strstr(v->e.status, "out of memory") != NULL);

	vedit_free(v);
	memio_free(&m);
}

/* term_open spawns, then the buffer setup fails: the child is hung up and
 * reaped and the open reports failure. */
static void
t_fault_open_install(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int r;

	v = fault_editor(&m, &io, 1);
	TAP_ASSERT(t, v != NULL);
	fail_realloc = 1;
	r = term_open(&v->e, NULL);	/* forks a shell, then install fails */
	fail_realloc = 0;
	TAP_CHECK(t, r < 0);
	TAP_CHECK(t, v->e.kind != BUF_TERM);

	vedit_free(v);
	memio_free(&m);
}

const Case tap_cases[] = {
	{ "fault_attach_make", t_fault_attach_make },
	{ "fault_attach_install", t_fault_attach_install },
	{ "fault_open_make", t_fault_open_make },
	{ "fault_open_install", t_fault_open_install },
	{ NULL, NULL },
};
