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

/* The Tab key with expandtab on inserts spaces to the next stop, driven through
 * the event loop. */
static void
t_tab_key_expand(Test *t)
{
	const char keys[] = "\t";	/* one Tab */
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	size_t len = 0;
	const char *s;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.expand_tabs = 1;
	vedit_run(v);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, s && len == 8 && memcmp(s, "        ", 8) == 0,
	    "expandtab produced %zu bytes", len);
	vedit_free(v);
	memio_free(&m);
}

/* Load a one-section config from an in-memory string (cfg_load_mem mutates its
 * input, so it is handed a private copy). */
static Cfg *
cfg_from_text(const char *text)
{
	Cfg *c = vedit_cfg_new();
	char *dup;

	if (!c)
		return NULL;
	dup = cfg_dup(text);
	if (!dup) {
		vedit_cfg_free(c);
		return NULL;
	}
	cfg_load_mem(c, dup);
	free(dup);
	return c;
}

/* Ctrl-] jumps to the tag under the cursor: with a tags file and one match it
 * opens the target file and positions the cursor, driven through the loop. */
static void
t_tag_jump(Test *t)
{
	char dir[] = "/tmp/vedit_tjXXXXXX";
	char src[PATH_MAX], tags[PATH_MAX], cfgtext[PATH_MAX + 64];
	const char keys[] = "\x1d";		/* Ctrl-] */
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(src, sizeof(src), "%s/sock.c", dir);
	snprintf(tags, sizeof(tags), "%s/tags", dir);

	f = fopen(src, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("/* hdr */\nint sock_open(void)\n{\n\treturn 0;\n}\n", f);
	fclose(f);

	f = fopen(tags, "w");
	TAP_ASSERT(t, f != NULL);
	/* absolute file path so the resolved target is exact */
	fprintf(f, "sock_open\t%s\t/^int sock_open(void)$/;\"\tf\n", src);
	fclose(f);

	snprintf(cfgtext, sizeof(cfgtext), "[tags]\n\tfile = %s\n", tags);
	cfg = cfg_from_text(cfgtext);
	TAP_ASSERT(t, cfg != NULL);

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	text_insert(v->e.t, 0, 0, "sock_open", 9);	/* identifier under cursor */
	v->e.cy = 0;
	v->e.cx = 0;

	vedit_run(v);

	TAP_CHECK(t, v->e.has_name &&
	    strcmp(v->e.path + strlen(v->e.path) - 6, "sock.c") == 0);
	TAP_CHECKF(t, v->e.cy == 1, "cursor line %zu, want 1", v->e.cy);

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	vedit_cfg_free(cfg);
	unlink(tags);
	unlink(src);
	rmdir(dir);
}

#ifndef VEDIT_NO_TOOLS
/* A fake tool runner, so the IDE-command tests drive the whole event loop
 * (key -> dispatch -> command -> output pane) without forking a shell. */
static const char *g_fake_output;	/* bytes run_capture emits */
static int g_fake_rc;			/* exit status it returns */
static char g_fake_cmd[256];		/* the command line it was handed */
static char g_fake_dir[PATH_MAX];	/* the directory it was handed */
static int g_fake_fg;			/* run_foreground was called */

static int
fake_capture(void *ctx, const char *cmd, const char *dir,
    void (*emit)(void *sink, const char *buf, size_t n), void *sink)
{
	(void)ctx;
	snprintf(g_fake_cmd, sizeof(g_fake_cmd), "%s", cmd);
	snprintf(g_fake_dir, sizeof(g_fake_dir), "%s", dir ? dir : "");
	if (g_fake_output)
		emit(sink, g_fake_output, strlen(g_fake_output));
	return g_fake_rc;
}

static int
fake_foreground(void *ctx, const char *cmd, const char *dir)
{
	(void)ctx;
	(void)dir;
	g_fake_fg = 1;
	snprintf(g_fake_cmd, sizeof(g_fake_cmd), "%s", cmd);
	return 0;
}

static const struct vedit_tool_api fake_tools = {
	NULL, fake_capture, fake_foreground,
};

/* F9 (Make): the whole path end to end. The key reaches the dispatcher, the
 * per-language build command is expanded and run, the captured output is parsed
 * into the quickfix list, and the output pane renders it. The pane and then the
 * loop both exit on end of input. */
static void
t_tool_f9_make(Test *t)
{
	const char keys[] = "\033[20~";	/* F9 */
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;

	g_fake_output =
	    "gcc -c test.c\n"
	    "test.c:3:12: error: 'bad' undeclared\n"
	    "other.c:7: warning: unused variable\n";
	g_fake_rc = 1;
	g_fake_cmd[0] = '\0';
	g_fake_fg = 0;

	cfg = cfg_from_text("[command \"c\"]\n\tbuild = make $(filenoext)\n");
	TAP_ASSERT(t, cfg != NULL);

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	vedit_set_tools(v, &fake_tools);
	vedit_open(v, "test.c");		/* named .c buffer: syntax is C */

	vedit_run(v);

	/* $(filenoext) expanded to the base name without extension */
	TAP_CHECKF(t, strcmp(g_fake_cmd, "make test") == 0,
	    "ran command '%s'", g_fake_cmd);
	TAP_CHECKF(t, g_fake_fg == 0, "capture command must not run foreground");

	/* both diagnostics were parsed, with and without a column */
	TAP_CHECKF(t, v->e.tool_nerr == 2, "diagnostics %d", v->e.tool_nerr);
	if (v->e.tool_nerr >= 2) {
		TAP_CHECKF(t, strcmp(v->e.tool_errs[0].file, "test.c") == 0 &&
		    v->e.tool_errs[0].line == 3 && v->e.tool_errs[0].col == 12,
		    "err0 %s:%zu:%zu", v->e.tool_errs[0].file,
		    v->e.tool_errs[0].line, v->e.tool_errs[0].col);
		TAP_CHECKF(t, strcmp(v->e.tool_errs[1].file, "other.c") == 0 &&
		    v->e.tool_errs[1].line == 7 && v->e.tool_errs[1].col == 0,
		    "err1 %s:%zu:%zu", v->e.tool_errs[1].file,
		    v->e.tool_errs[1].line, v->e.tool_errs[1].col);
	}

	/* the output pane rendered the captured text */
	TAP_CHECK(t, m.out && strstr(m.out, "undeclared") != NULL);

	vedit_free(v);
	memio_free(&m);
	/* vedit_set_config keeps the config in a global; drop that reference
	 * before freeing it, since the next case builds another editor. */
	g_cfg = NULL;
	vedit_cfg_free(cfg);
}

/* Ctrl+F9 (Run) with an interactive command takes the foreground branch: it
 * runs through run_foreground and opens no capture pane. */
static void
t_tool_ctrl_f9_run(Test *t)
{
	const char keys[] = "\033[20;5~";	/* Ctrl+F9 */
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;

	g_fake_output = NULL;
	g_fake_rc = 0;
	g_fake_cmd[0] = '\0';
	g_fake_fg = 0;

	cfg = cfg_from_text("[command \"c\"]\n"
	    "\trun = ./$(filenoext)\n"
	    "\trun.interactive = on\n");
	TAP_ASSERT(t, cfg != NULL);

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	vedit_set_tools(v, &fake_tools);
	vedit_open(v, "test.c");

	vedit_run(v);

	TAP_CHECKF(t, g_fake_fg == 1, "run.interactive did not run foreground");
	TAP_CHECKF(t, strcmp(g_fake_cmd, "./test") == 0,
	    "ran command '%s'", g_fake_cmd);
	TAP_CHECKF(t, v->e.tool_nerr == 0, "foreground run left %d diagnostics",
	    v->e.tool_nerr);

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;			/* see t_tool_f9_make */
	vedit_cfg_free(cfg);
}
#endif /* VEDIT_NO_TOOLS */

const Case tap_cases[] = {
	{ "cursor_end_home", t_cursor_end_home },
	{ "cursor_home", t_cursor_home },
	{ "wrap_shows_tail", t_wrap_shows_tail },
	{ "nowrap_truncates_tail", t_nowrap_truncates_tail },
	{ "gutter_numbers", t_gutter_numbers },
	{ "status_flags", t_status_flags },
	{ "tab_key_expand", t_tab_key_expand },
	{ "tag_jump", t_tag_jump },
#ifndef VEDIT_NO_TOOLS
	{ "tool_f9_make", t_tool_f9_make },
	{ "tool_ctrl_f9_run", t_tool_ctrl_f9_run },
#endif
	{ NULL, NULL },
};
