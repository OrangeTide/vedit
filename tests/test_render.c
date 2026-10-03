/*
 * Integration tests: drive the editor through its public API over an in-memory
 * io (no terminal), then inspect the resulting editor state and the bytes it
 * emitted. The editor is included as one translation unit so struct vedit and
 * the buffer helpers are reachable.
 */
#include "test.h"

#define main test_main
#include "../vedit.c"
#undef main

#include "memio.h"

/* Fill a fresh buffer (one empty line) with n whole lines. */
static void
fill_lines(Text *tx, const char *const *lines, int n)
{
	int i;

	if (n <= 0)
		return;
	text_insert(tx, 0, 0, lines[0], strlen(lines[0]));
	for (i = 1; i < n; i++)
		lines_insert_at(tx, (size_t)i, lines[i], strlen(lines[i]));
}

static void
t_cursor_end_home(Test *t)
{
	static const char *const L[] = {
		"l1", "l2", "l3", "l4", "l5", "l6", "l7", "l8", "l9", "l10"
	};
	const char keys[] = "\033[1;5F";	/* Ctrl+End: jump to last line */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 10);
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 9, "Ctrl+End left cy at %zu, want 9", v->e.cy);
	vedit_free(v);
	memio_free(&m);
}

static void
t_cursor_home(Test *t)
{
	static const char *const L[] = { "a", "b", "c", "d", "e" };
	const char keys[] = "\033[1;5F\033[1;5H";	/* end, then home */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 5);
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 0, "Ctrl+Home left cy at %zu, want 0",
	    v->e.cy);
	vedit_free(v);
	memio_free(&m);
}

/* A line that is clearly wider than the 78-column text area, ending in a marker
 * that is only reachable once it wraps. */
static const char *const LONG =
	"alpha beta gamma delta epsilon zeta eta theta iota kappa lambda "
	"mu nu xi omicron pi rho sigma ENDMARK";

static void
t_wrap_shows_tail(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);		/* no keys: one frame, then EOF */
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.wrap = 1;
	text_insert(v->e.t, 0, 0, LONG, strlen(LONG));
	vedit_run(v);
	TAP_CHECK(t, strstr(m.out, "ENDMARK") != NULL);	/* tail is on a wrap row */
	vedit_free(v);
	memio_free(&m);
}

static void
t_nowrap_truncates_tail(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.wrap = 0;
	text_insert(v->e.t, 0, 0, LONG, strlen(LONG));
	vedit_run(v);
	/* without wrap the far tail is off the right edge, so never emitted */
	TAP_CHECK(t, strstr(m.out, "ENDMARK") == NULL);
	vedit_free(v);
	memio_free(&m);
}

static void
t_gutter_numbers(Test *t)
{
	static const char *const L[] = { "one", "two", "three" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.show_lineno = 1;
	fill_lines(v->e.t, L, 3);
	vedit_run(v);
	TAP_CHECK(t, strstr(m.out, "  1 ") != NULL);	/* right-aligned gutter */
	TAP_CHECK(t, strstr(m.out, "  2 ") != NULL);
	TAP_CHECK(t, strstr(m.out, "  3 ") != NULL);
	vedit_free(v);
	memio_free(&m);
}

static void
t_status_flags(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.wrap = 1;
	v->e.show_lineno = 1;
	text_insert(v->e.t, 0, 0, "hi", 2);
	vedit_run(v);
	TAP_CHECK(t, strstr(m.out, "WRAP") != NULL);
	TAP_CHECK(t, strstr(m.out, "NUM") != NULL);
	vedit_free(v);
	memio_free(&m);
}

const Case tap_cases[] = {
	{ "cursor_end_home", t_cursor_end_home },
	{ "cursor_home", t_cursor_home },
	{ "wrap_shows_tail", t_wrap_shows_tail },
	{ "nowrap_truncates_tail", t_nowrap_truncates_tail },
	{ "gutter_numbers", t_gutter_numbers },
	{ "status_flags", t_status_flags },
	{ NULL, NULL },
};
