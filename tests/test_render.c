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

/* The tag stack: Ctrl-] jumps to a tag, then vi Ctrl-T pops back to where the
 * jump started. Driven through the event loop in vi normal mode. */
static void
t_tag_stack(Test *t)
{
	char dir[] = "/tmp/vedit_tsXXXXXX";
	char caller[PATH_MAX], target[PATH_MAX], tags[PATH_MAX];
	char cfgtext[PATH_MAX + 64];
	const char keys[] = "\x1d\x14";		/* Ctrl-] then Ctrl-T */
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(caller, sizeof(caller), "%s/caller.c", dir);
	snprintf(target, sizeof(target), "%s/sock.c", dir);
	snprintf(tags, sizeof(tags), "%s/tags", dir);

	f = fopen(caller, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("sock_open();\n", f);
	fclose(f);
	f = fopen(target, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("/* hdr */\nint sock_open(void)\n{\n}\n", f);
	fclose(f);
	f = fopen(tags, "w");
	TAP_ASSERT(t, f != NULL);
	fprintf(f, "sock_open\t%s\t/^int sock_open(void)$/;\"\tf\n", target);
	fclose(f);

	snprintf(cfgtext, sizeof(cfgtext), "[tags]\n\tfile = %s\n", tags);
	cfg = cfg_from_text(cfgtext);
	TAP_ASSERT(t, cfg != NULL);

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	TAP_ASSERT(t, vedit_open(v, caller) == 0);
	v->e.mode = MODE_NORMAL;	/* so Ctrl-T pops, not the modeless picker */
	v->e.cy = 0;
	v->e.cx = 0;

	vedit_run(v);

	/* popped back to the caller after the jump into sock.c */
	TAP_CHECK(t, v->e.has_name &&
	    strcmp(v->e.path + strlen(v->e.path) - 8, "caller.c") == 0);
	TAP_CHECKF(t, v->e.cy == 0, "returned to line %zu", v->e.cy);
	TAP_CHECKF(t, v->e.tag_sp == 0, "stack depth %d after pop", v->e.tag_sp);

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	vedit_cfg_free(cfg);
	unlink(caller);
	unlink(target);
	unlink(tags);
	rmdir(dir);
}

/* gf: on an #include line, open the header found through a compile_commands.json
 * include path. Driven through the event loop in vi normal mode. */
static void
t_gf_header(Test *t)
{
	char dir[] = "/tmp/vedit_gfXXXXXX";
	char src[PATH_MAX], inc[PATH_MAX], hdr[PATH_MAX], db[PATH_MAX];
	char cfgtext[PATH_MAX + 64];
	const char keys[] = "gf";
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(inc, sizeof(inc), "%s/inc", dir);
	TAP_ASSERT(t, mkdir(inc, 0700) == 0);
	snprintf(src, sizeof(src), "%s/foo.c", dir);
	snprintf(hdr, sizeof(hdr), "%s/inc/bar.h", dir);	/* only under inc/ */
	snprintf(db, sizeof(db), "%s/compile_commands.json", dir);

	f = fopen(src, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("#include \"bar.h\"\n", f);
	fclose(f);
	f = fopen(hdr, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("#define BAR 1\n", f);
	fclose(f);
	f = fopen(db, "w");
	TAP_ASSERT(t, f != NULL);
	fprintf(f, "[{\"directory\":\"%s\",\"file\":\"foo.c\","
	    "\"arguments\":[\"cc\",\"-I\",\"inc\",\"-c\",\"foo.c\"]}]\n", dir);
	fclose(f);

	snprintf(cfgtext, sizeof(cfgtext), "[cc]\n\tfile = %s\n", db);
	cfg = cfg_from_text(cfgtext);
	TAP_ASSERT(t, cfg != NULL);

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	TAP_ASSERT(t, vedit_open(v, src) == 0);
	v->e.mode = MODE_NORMAL;	/* gf is a vi normal-mode command */
	v->e.cy = 0;
	v->e.cx = 0;

	vedit_run(v);

	TAP_CHECK(t, v->e.has_name &&
	    strcmp(v->e.path + strlen(v->e.path) - 5, "bar.h") == 0);

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	vedit_cfg_free(cfg);
	unlink(db);
	unlink(hdr);
	unlink(src);
	rmdir(inc);
	rmdir(dir);
}

/* Opening the same file by a second spelling (here a symlink) switches to the
 * open buffer instead of making a duplicate; a distinct file still adds one. */
static void
t_buf_dedup(Test *t)
{
	char dir[] = "/tmp/vedit_dedupXXXXXX";
	char a[PATH_MAX], b[PATH_MAX], sym[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;
	int n0;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(a, sizeof(a), "%s/a.c", dir);
	snprintf(b, sizeof(b), "%s/b.c", dir);
	snprintf(sym, sizeof(sym), "%s/link.c", dir);
	f = fopen(a, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("a\n", f);
	fclose(f);
	f = fopen(b, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("b\n", f);
	fclose(f);
	TAP_ASSERT(t, symlink(a, sym) == 0);

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, a) == 0);
	vedit_run(v);			/* registers the initial buffer */
	n0 = v->e.nbuf;
	TAP_CHECKF(t, n0 == 1, "initial buffer count %d", n0);

	/* the symlink resolves to the open buffer: no new buffer */
	TAP_CHECK(t, buf_open(&v->e, sym) >= 0);
	TAP_CHECKF(t, v->e.nbuf == n0, "symlink added a buffer: %d", v->e.nbuf);

	/* a genuinely different file does add one */
	TAP_CHECK(t, buf_open(&v->e, b) >= 0);
	TAP_CHECKF(t, v->e.nbuf == n0 + 1, "distinct file not added: %d",
	    v->e.nbuf);

	vedit_free(v);
	memio_free(&m);
	unlink(sym);
	unlink(b);
	unlink(a);
	rmdir(dir);
}

/* Runtime config reload: re-reading the file picks up a changed setting, keeps a
 * valid syntax pointer, and reports no config when none backs the session. */
static void
t_reload_config(Test *t)
{
	char dir[] = "/tmp/vedit_rlXXXXXX";
	char cfgp[PATH_MAX], src[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(cfgp, sizeof(cfgp), "%s/config", dir);
	snprintf(src, sizeof(src), "%s/a.c", dir);
	f = fopen(src, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("int x;\n", f);
	fclose(f);
	f = fopen(cfgp, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("[ui]\nwrap = on\n", f);
	fclose(f);

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, src) == 0);
	vedit_run(v);				/* register the buffer */

	/* a .c buffer has a syntax; it must survive a grammar reload */
	TAP_CHECK(t, v->e.syn != NULL);

	/* no path recorded yet: reload is a reported no-op */
	ed_reload_config(&v->e);
	TAP_CHECKF(t, strstr(v->e.status, "no config") != NULL,
	    "status: %s", v->e.status);

	/* record the path, reload, and the file's wrap = on takes effect */
	vedit_set_config_path(v, cfgp);
	ed_reload_config(&v->e);
	TAP_CHECK(t, v->e.wrap == 1);
	TAP_CHECK(t, v->e.syn != NULL);		/* re-pointed, not dangling */
	TAP_CHECKF(t, strstr(v->e.status, "reloaded") != NULL,
	    "status: %s", v->e.status);

	/* edit the file and reload again: the new value wins */
	f = fopen(cfgp, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("[ui]\nwrap = off\n", f);
	fclose(f);
	ed_reload_config(&v->e);
	TAP_CHECK(t, v->e.wrap == 0);

	vedit_free(v);
	memio_free(&m);
	unlink(cfgp);
	unlink(src);
	rmdir(dir);
}

static int vline_is(struct vedit *v, size_t y, const char *want);

/* Lay down a swap file for path holding body, as a crashed prior session
 * would have left behind. Uses a throwaway editor so the on-disk format
 * matches exactly what swap_write produces. */
static void
plant_swap(const char *path, const char *body)
{
	Editor e;

	editor_init(&e);			/* sets swap_enabled = 1 */
	e.t = text_new();
	text_insert(e.t, 0, 0, body, strlen(body));
	e.t->final_newline = 1;
	snprintf(e.path, sizeof(e.path), "%s", path);
	e.has_name = 1;
	swap_write(&e);				/* writes <dir>/.<base>.swp */
	text_free(e.t);				/* no teardown: keep the swap */
}

/* A dirty buffer gets a swap snapshot on the idle tick, and a clean quit
 * (teardown) removes it. */
static void
t_swap_file_created(Test *t)
{
	char dir[] = "/tmp/vedit_swcXXXXXX";
	char path[PATH_MAX], sp[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/doc.txt", dir);
	f = fopen(path, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("hi\n", f);
	fclose(f);
	TAP_ASSERT(t, swap_path_for(path, sp, sizeof(sp)));

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	vedit_run(v);				/* registers; no swap yet (clean) */
	TAP_CHECK(t, access(sp, F_OK) != 0);

	text_insert(v->e.t, 0, 0, "Z", 1);	/* dirty the buffer */
	swap_maybe_write(&v->e);		/* the idle tick would do this */
	TAP_CHECK(t, access(sp, F_OK) == 0);	/* snapshot on disk */

	vedit_free(v);				/* clean exit */
	TAP_CHECK(t, access(sp, F_OK) != 0);	/* swap removed */
	memio_free(&m);
	unlink(path);
	rmdir(dir);
}

/* Opening a file with a swap beside it and answering 'r' recovers the swap's
 * contents into a dirty buffer. */
static void
t_swap_recover_key(Test *t)
{
	char dir[] = "/tmp/vedit_swrXXXXXX";
	char path[PATH_MAX], sp[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/doc.txt", dir);
	f = fopen(path, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("orig\n", f);
	fclose(f);
	plant_swap(path, "recovered");
	TAP_ASSERT(t, swap_path_for(path, sp, sizeof(sp)));
	TAP_ASSERT(t, access(sp, F_OK) == 0);

	memio_init(&m, "r", 1, 24, 80);	/* answer the recovery prompt */
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	vedit_run(v);

	TAP_CHECK(t, vline_is(v, 0, "recovered"));	/* swap body, not "orig" */
	TAP_CHECK(t, text_dirty(v->e.t));		/* unsaved recovery */

	vedit_free(v);
	memio_free(&m);
	unlink(sp);			/* best effort if teardown kept it */
	unlink(path);
	rmdir(dir);
}

/* Answering 'd' to the recovery prompt deletes the swap and keeps the file. */
static void
t_swap_recover_delete(Test *t)
{
	char dir[] = "/tmp/vedit_swdXXXXXX";
	char path[PATH_MAX], sp[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/doc.txt", dir);
	f = fopen(path, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("orig\n", f);
	fclose(f);
	plant_swap(path, "recovered");
	TAP_ASSERT(t, swap_path_for(path, sp, sizeof(sp)));

	memio_init(&m, "d", 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	vedit_run(v);

	TAP_CHECK(t, vline_is(v, 0, "orig"));	/* original kept */
	TAP_CHECK(t, access(sp, F_OK) != 0);	/* swap deleted */

	vedit_free(v);
	memio_free(&m);
	unlink(path);
	rmdir(dir);
}

/* :set ignorecase makes a search match regardless of case. */
static void
t_search_icase(Test *t)
{
	static const char *const L[] = { "alpha", "BetaGAMMA" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 2);

	v->e.cy = v->e.cx = 0;
	v->e.search_icase = 0;
	ed_find_dir(&v->e, "gamma", 1);
	TAP_CHECKF(t, v->e.cy == 0, "case-sensitive should miss, cy=%zu",
	    v->e.cy);

	v->e.cy = v->e.cx = 0;
	v->e.search_icase = 1;
	ed_find_dir(&v->e, "gamma", 1);
	TAP_CHECKF(t, v->e.cy == 1, "ignorecase should hit line 1, cy=%zu",
	    v->e.cy);

	vedit_free(v);
	memio_free(&m);
}

/* The * word search matches whole words only: it skips "foobar" and lands on
 * the standalone "foo". */
static void
t_search_word(Test *t)
{
	static const char *const L[] = { "foo", "foobar", "zz foo" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);

	v->e.cy = v->e.cx = 0;
	vi_search_word(&v->e, 1);		/* like pressing * on "foo" */
	TAP_CHECKF(t, v->e.cy == 2, "* should skip 'foobar', cy=%zu", v->e.cy);

	vedit_free(v);
	memio_free(&m);
}

/* Put bytes into register a (as if recorded), for the @ playback tests. */
static void
set_reg_a(struct vedit *v, const char *bytes, size_t n)
{
	char *b = malloc(n);

	if (b) {
		memcpy(b, bytes, n);
		free(v->e.vi_regs[0].bytes);
		v->e.vi_regs[0].bytes = b;
		v->e.vi_regs[0].len = n;
		v->e.vi_regs[0].linewise = 0;
	}
}

/* @a replays register a as keystrokes, including an embedded Esc that leaves
 * insert mode; a count repeats the whole macro. */
static void
t_macro_play(Test *t)
{
	static const char *const L[] = { "X" };
	static const char *const L2[] = { "abcd" };
	const char ins[] = { 'i', 'h', 'i', 0x1b };	/* insert "hi", then Esc */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	/* a macro that switches to insert mode and back */
	memio_init(&m, "@a", 2, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 1);
	v->e.mode = MODE_NORMAL;
	set_reg_a(v, ins, sizeof(ins));
	vedit_run(v);
	TAP_CHECK(t, vline_is(v, 0, "hiX"));
	vedit_free(v);
	memio_free(&m);

	/* a count repeats the macro: 2@a deletes two characters */
	memio_init(&m, "2@a", 3, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L2, 1);
	v->e.mode = MODE_NORMAL;
	set_reg_a(v, "x", 1);
	vedit_run(v);
	TAP_CHECK(t, vline_is(v, 0, "cd"));
	vedit_free(v);
	memio_free(&m);
}

/* q records typed keys into a register (dropping the closing q), and @ replays
 * them. Record "qax" then stop with "q": register a holds "x"; "@a" deletes the
 * next character, so two chars are gone overall. */
static void
t_macro_record(Test *t)
{
	static const char *const L[] = { "abcdef" };
	const char keys[] = "qaxq@a";
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 1);
	v->e.mode = MODE_NORMAL;

	vedit_run(v);

	/* the register holds exactly the recorded keystroke, not the stop q */
	TAP_CHECKF(t, v->e.vi_regs[0].len == 1, "reg len %zu",
	    v->e.vi_regs[0].len);
	TAP_CHECK(t, v->e.vi_regs[0].bytes && v->e.vi_regs[0].bytes[0] == 'x');
	/* x while recording removed 'a', @a removed 'b' */
	TAP_CHECK(t, vline_is(v, 0, "cdef"));

	vedit_free(v);
	memio_free(&m);
}

/* The automatic special marks: '> on the last visual selection, '. on the last
 * change, and '^ where insert mode stopped. */
static void
t_marks_special(Test *t)
{
	static const char *const L[] = { "abcde", "fghij", "klmno" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	/* '>: select (0,0)..(1,1), leave with v, then `> jumps to the end */
	memio_init(&m, "vjlv`>", 6, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 1 && v->e.cx == 1, "'> at %zu,%zu",
	    v->e.cy, v->e.cx);
	vedit_free(v);
	memio_free(&m);

	/* '.: x changes line 0, G leaves, `. returns to the change */
	memio_init(&m, "xG`.", 4, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 0, "'. at line %zu", v->e.cy);
	vedit_free(v);
	memio_free(&m);

	/* '^: insert Z then Esc (Esc last, so a canned feed cannot fold it into
	 * the next key); the mark lands where insert stopped. */
	memio_init(&m, "iZ\x1b", 3, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECK(t, (v->e.vi_marks_set & ((uint64_t)1 << MARK_INSERT)) != 0);
	TAP_CHECKF(t, v->e.vi_mark_y[MARK_INSERT] == 0 &&
	    v->e.vi_mark_x[MARK_INSERT] == 0, "'^ at %zu,%zu",
	    v->e.vi_mark_y[MARK_INSERT], v->e.vi_mark_x[MARK_INSERT]);
	vedit_free(v);
	memio_free(&m);
}

/* The jump list: G records the origin, Ctrl-O walks back to it and Ctrl-I
 * forward again, and `` toggles between a jump's two ends. */
static void
t_jumplist(Test *t)
{
	static const char *const L[] = { "l0", "l1", "l2", "l3", "l4",
	    "l5", "l6", "l7", "l8", "l9" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	/* G jumps to the last line, Ctrl-O (0x0f) returns to the origin */
	memio_init(&m, "G\x0f", 2, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 10);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 0, "Ctrl-O landed on line %zu", v->e.cy);
	vedit_free(v);
	memio_free(&m);

	/* ... and Ctrl-I (Tab, 0x09) goes forward again */
	memio_init(&m, "G\x0f\x09", 3, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 10);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 9, "Ctrl-I landed on line %zu", v->e.cy);
	vedit_free(v);
	memio_free(&m);

	/* a committed / search is a jump: Ctrl-O returns to where it started */
	memio_init(&m, "/l5\r\x0f", 5, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 10);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 0, "Ctrl-O after search at line %zu", v->e.cy);
	vedit_free(v);
	memio_free(&m);

	/* `` returns to the pre-jump spot; a second `` toggles back */
	memio_init(&m, "G````", 5, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 10);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 9, "`` toggled to line %zu", v->e.cy);
	vedit_free(v);
	memio_free(&m);
}

/* :delmarks clears a named mark; :marks lists them and jumps to the chosen one. */
static void
t_marks_ex(Test *t)
{
	static const char *const L[] = { "l0", "l1", "l2", "l3", "l4" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	/* set mark a, then :delmarks a clears it */
	memio_init(&m, "ma:delmarks a\r", 14, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 5);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECK(t, (v->e.vi_marks_set & ((uint64_t)1 << 0)) == 0);
	vedit_free(v);
	memio_free(&m);

	/* set mark a on line 2, go home, :marks then Enter jumps back to it */
	memio_init(&m, "jjmagg:marks\r\r", 14, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 5);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 2, ":marks jumped to line %zu", v->e.cy);
	vedit_free(v);
	memio_free(&m);
}

/* '[ and '] bracket the last change, yank, or put. */
static void
t_marks_bracket(Test *t)
{
	static const char *const L[] = { "abcdef" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	/* yank three chars: '] is the last of them (col 2) */
	memio_init(&m, "y3l$`]", 6, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 1);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 0 && v->e.cx == 2, "'] after yank at %zu,%zu",
	    v->e.cy, v->e.cx);
	vedit_free(v);
	memio_free(&m);

	/* insert "XY" at the start: '[ at the start, '] on the last inserted */
	memio_init(&m, "iXY\x1b", 4, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 1);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECK(t, v->e.vi_mark_y[MARK_LBRACK] == 0 &&
	    v->e.vi_mark_x[MARK_LBRACK] == 0);
	TAP_CHECKF(t, v->e.vi_mark_y[MARK_RBRACK] == 0 &&
	    v->e.vi_mark_x[MARK_RBRACK] == 1, "'] after insert at %zu,%zu",
	    v->e.vi_mark_y[MARK_RBRACK], v->e.vi_mark_x[MARK_RBRACK]);
	vedit_free(v);
	memio_free(&m);

	/* yank a char and put it: '[ is the pasted char (col 1) */
	memio_init(&m, "ylp$`[", 6, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 1);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);
	TAP_CHECKF(t, v->e.cy == 0 && v->e.cx == 1, "'[ after put at %zu,%zu",
	    v->e.cy, v->e.cx);
	vedit_free(v);
	memio_free(&m);
}

/* gv reselects the previous visual range after leaving visual mode. */
static void
t_gv_reselect(Test *t)
{
	static const char *const L[] = { "abcd", "efgh", "ijkl" };
	/* select (0,0)-(1,1), leave with v (not Esc, which a canned feed would
	 * coalesce with the following g into Alt-g), then gv to reselect */
	const char keys[] = "vjlvgv";
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);

	TAP_CHECKF(t, v->e.vi_visual == 'v', "not in visual: %d",
	    v->e.vi_visual);
	TAP_CHECK(t, v->e.sel_active);
	TAP_CHECKF(t, v->e.ay == 0 && v->e.ax == 0, "anchor %zu,%zu",
	    v->e.ay, v->e.ax);
	TAP_CHECKF(t, v->e.cy == 1 && v->e.cx == 1, "cursor %zu,%zu",
	    v->e.cy, v->e.cx);

	vedit_free(v);
	memio_free(&m);
}

/* :g/re/y yanks every matching line (linewise) into the clipboard. */
static void
t_global_yank(Test *t)
{
	static const char *const L[] = { "keep1", "drop", "keep2" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);

	vi_ex_global(&v->e, 0, 0, 0, "/keep/y", 0);
	TAP_CHECK(t, v->e.clip_linewise);
	TAP_CHECKF(t, v->e.clip_len == 12 &&
	    memcmp(v->e.clip, "keep1\nkeep2\n", 12) == 0,
	    "clip len %zu", v->e.clip_len);

	vedit_free(v);
	memio_free(&m);
}

/* :g/re/> shifts every matching line, leaving the rest alone. */
static void
t_global_shift(Test *t)
{
	static const char *const L[] = { "aa", "bb", "ac" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);
	v->e.expand_tabs = 0;
	v->e.shiftwidth = 0;			/* one tab */

	vi_ex_global(&v->e, 0, 0, 0, "/^a/>", 0);
	TAP_CHECK(t, vline_is(v, 0, "\taa"));	/* matched: shifted */
	TAP_CHECK(t, vline_is(v, 1, "bb"));	/* no match: untouched */
	TAP_CHECK(t, vline_is(v, 2, "\tac"));	/* matched: shifted */

	vedit_free(v);
	memio_free(&m);
}

/* shiftwidth controls the >> indent: spaces when expandtab is on, a tab by
 * default, and the chosen column width. */
static void
t_shiftwidth(Test *t)
{
	static const char *const L[] = { "x" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 1);

	v->e.expand_tabs = 1;
	v->e.shiftwidth = 4;
	vi_shift_lines(&v->e, 0, 0, 1);
	TAP_CHECK(t, vline_is(v, 0, "    x"));		/* four spaces */
	vi_shift_lines(&v->e, 0, 0, -1);
	TAP_CHECK(t, vline_is(v, 0, "x"));		/* and back */

	v->e.expand_tabs = 0;
	v->e.shiftwidth = 0;				/* default: one tab */
	vi_shift_lines(&v->e, 0, 0, 1);
	TAP_CHECK(t, vline_is(v, 0, "\tx"));

	vedit_free(v);
	memio_free(&m);
}

/* True when line y of the buffer equals the NUL-terminated want. */
static int
vline_is(struct vedit *v, size_t y, const char *want)
{
	size_t len = 0;
	const char *s = text_line(v->e.t, y, &len);

	return s && len == strlen(want) && memcmp(s, want, len) == 0;
}

/* Ctrl-V block delete removes the column range from every spanned row. */
static void
t_vblock_delete(Test *t)
{
	static const char *const L[] = { "abcdef", "ghijkl", "mnopqr" };
	const char keys[] = "\x16jjld";	/* Ctrl-V, down, down, right, delete */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);
	v->e.mode = MODE_NORMAL;
	vedit_run(v);

	TAP_CHECK(t, vline_is(v, 0, "cdef"));	/* columns 0-1 removed */
	TAP_CHECK(t, vline_is(v, 1, "ijkl"));
	TAP_CHECK(t, vline_is(v, 2, "opqr"));

	vedit_free(v);
	memio_free(&m);
}

/* Ctrl-V then I inserts typed text down the whole block; A appends past it. */
static void
t_vblock_insert(Test *t)
{
	static const char *const L[] = { "abcdef", "ghijkl", "mnopqr" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	/* I at the left edge: "X" prepended to all three rows */
	{
		const char keys[] = "\x16jjIX\x1b";	/* block, down x2, I, 'X', Esc */

		memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
		memio_bind(&io, &m);
		v = vedit_new(&io);
		TAP_ASSERT(t, v != NULL);
		fill_lines(v->e.t, L, 3);
		v->e.mode = MODE_NORMAL;
		vedit_run(v);
		TAP_CHECK(t, vline_is(v, 0, "Xabcdef"));
		TAP_CHECK(t, vline_is(v, 1, "Xghijkl"));
		TAP_CHECK(t, vline_is(v, 2, "Xmnopqr"));
		vedit_free(v);
		memio_free(&m);
	}

	/* A past the right edge of a 3-wide block (cols 0-2): "Z" at column 3 */
	{
		const char keys[] = "\x16jjllAZ\x1b";

		memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
		memio_bind(&io, &m);
		v = vedit_new(&io);
		TAP_ASSERT(t, v != NULL);
		fill_lines(v->e.t, L, 3);
		v->e.mode = MODE_NORMAL;
		vedit_run(v);
		TAP_CHECK(t, vline_is(v, 0, "abcZdef"));
		TAP_CHECK(t, vline_is(v, 1, "ghiZjkl"));
		TAP_CHECK(t, vline_is(v, 2, "mnoZpqr"));
		vedit_free(v);
		memio_free(&m);
	}
}

/* Block yank fills a blockwise register; block put inserts the rectangle. */
static void
t_vblock_yank_put(Test *t)
{
	static const char *const L[] = { "abcdef", "ghijkl", "mnopqr" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 3);

	/* select columns 1-2 over all three rows and yank */
	v->e.ay = 0;
	v->e.ax = 1;
	v->e.cy = 2;
	v->e.cx = 2;
	v->e.sel_active = 1;
	v->e.sel_block = 1;
	vi_block_yank(&v->e);
	TAP_CHECK(t, v->e.clip_block == 1);
	TAP_CHECKF(t, v->e.clip_len == 8 &&
	    memcmp(v->e.clip, "bc\nhi\nno", 8) == 0, "clip '%.*s'",
	    (int)v->e.clip_len, v->e.clip);

	/* put it inserted at column 0 of the top row, spreading down */
	v->e.cy = 0;
	v->e.cx = 0;
	vi_block_put(&v->e, 0);
	TAP_CHECK(t, vline_is(v, 0, "bcabcdef"));
	TAP_CHECK(t, vline_is(v, 1, "highijkl"));
	TAP_CHECK(t, vline_is(v, 2, "nomnopqr"));

	vedit_free(v);
	memio_free(&m);
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

static int g_fake_filter_rc;		/* exit status run_filter returns */
static int g_fake_filter_called;	/* run_filter was invoked */

/* A fake format filter: uppercases whatever it is fed, so a test can see the
 * buffer go through it. A nonzero g_fake_filter_rc simulates a failing
 * formatter (emitting nothing). */
static int
fake_filter(void *ctx, const char *cmd, const char *dir,
    const char *input, size_t inlen,
    void (*emit)(void *sink, const char *buf, size_t n), void *sink)
{
	size_t i;
	char *up;

	(void)ctx;
	(void)dir;
	g_fake_filter_called = 1;
	snprintf(g_fake_cmd, sizeof(g_fake_cmd), "%s", cmd);
	if (g_fake_filter_rc != 0)
		return g_fake_filter_rc;
	up = malloc(inlen ? inlen : 1);
	if (!up)
		return -1;
	for (i = 0; i < inlen; i++)
		up[i] = (char)toupper((unsigned char)input[i]);
	emit(sink, up, inlen);
	free(up);
	return 0;
}

static const struct vedit_tool_api fake_tools = {
	NULL, fake_capture, fake_foreground, fake_filter,
};

/* On exit the editor parks the cursor on the last row and scrolls up one, so a
 * client without the alternate screen gets a clean prompt line. */
static void
t_exit_cursor(Test *t)
{
	static const char *const L[] = { "hello" };
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 1);
	vedit_run(v);
	vedit_free(v);				/* teardown emits the exit sequence */

	TAP_CHECK(t, m.out && strstr(m.out, "\033[24;1H\r\n") != NULL);
	memio_free(&m);
}

/* :!cmd runs through the tool runner and shows the output in the pane. */
static void
t_shell_cmd(Test *t)
{
	const char keys[] = ":!echo hi\r";	/* ex line, then Enter */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	g_fake_output = "line one\nline two\n";
	g_fake_rc = 0;
	g_fake_cmd[0] = '\0';
	g_fake_fg = 0;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.mode = MODE_NORMAL;
	vedit_set_tools(v, &fake_tools);
	vedit_run(v);

	TAP_CHECKF(t, strcmp(g_fake_cmd, "echo hi") == 0, "ran '%s'",
	    g_fake_cmd);
	TAP_CHECK(t, g_fake_fg == 0);		/* captured, not foreground */
	TAP_CHECK(t, m.out && strstr(m.out, "line one") != NULL);
	TAP_CHECKF(t, strstr(v->e.status, "exited 0") != NULL, "status '%s'",
	    v->e.status);

	vedit_free(v);
	memio_free(&m);
}

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
		/* severity was classified from the "error:" / "warning:" word */
		TAP_CHECK(t, v->e.tool_errs[0].sev == TSEV_ERROR &&
		    v->e.tool_errs[1].sev == TSEV_WARN);
	}

	/* the build landed on the first error, and the summary counts both */
	TAP_CHECKF(t, v->e.tool_curerr == 0, "curerr %d", v->e.tool_curerr);
	TAP_CHECKF(t, strstr(v->e.status, "1 error") != NULL &&
	    strstr(v->e.status, "1 warning") != NULL, "status '%s'", v->e.status);

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

/* :format / the Format menu item pipe the buffer through command.<lang>.format
 * and replace it with the result; a failing formatter leaves the buffer alone. */
static void
t_format(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;
	const char *s;
	size_t len = 0;

	cfg = cfg_from_text("[command \"c\"]\n\tformat = fmt\n");
	TAP_ASSERT(t, cfg != NULL);
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	vedit_set_tools(v, &fake_tools);
	vedit_open(v, "x.c");			/* new named .c buffer: lang c */
	text_insert(v->e.t, 0, 0, "abc", 3);

	TAP_CHECK(t, menu_item_enabled(&v->e, MA_FORMAT));	/* formatter ready */

	g_fake_filter_rc = 0;
	g_fake_filter_called = 0;
	TAP_CHECK(t, ed_format(&v->e) == 1);
	TAP_CHECK(t, g_fake_filter_called);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, len == 3 && memcmp(s, "ABC", 3) == 0,
	    "after format: '%.*s'", (int)len, s ? s : "");
	TAP_CHECK(t, text_can_undo(v->e.t));	/* the reformat is undoable */

	/* a failing formatter must not disturb the buffer */
	g_fake_filter_rc = 2;
	TAP_CHECK(t, ed_format(&v->e) == 0);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECK(t, len == 3 && memcmp(s, "ABC", 3) == 0);

	g_fake_filter_rc = 0;
	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	vedit_cfg_free(cfg);
}

/* With format-on-save enabled, saving runs the formatter first, so the file on
 * disk holds the formatted text. */
static void
t_format_on_save(Test *t)
{
	char dir[] = "/tmp/vedit_fmtXXXXXX";
	char path[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;
	Text *rd;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/s.c", dir);

	cfg = cfg_from_text("[command \"c\"]\n\tformat = fmt\n");
	TAP_ASSERT(t, cfg != NULL);
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	vedit_set_tools(v, &fake_tools);
	vedit_open(v, path);
	text_insert(v->e.t, 0, 0, "hello", 5);
	v->e.t->final_newline = 1;
	v->e.format_on_save = 1;

	g_fake_filter_rc = 0;
	TAP_CHECK(t, ed_save_file(&v->e) == OK);

	rd = text_new();
	TAP_ASSERT(t, rd && text_load(rd, path) == OK);
	{
		size_t len = 0;
		const char *s = text_line(rd, 0, &len);

		TAP_CHECKF(t, len == 5 && s && memcmp(s, "HELLO", 5) == 0,
		    "saved: '%.*s'", (int)len, s ? s : "");
	}
	text_free(rd);

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	vedit_cfg_free(cfg);
	unlink(path);
	rmdir(dir);
}
#endif /* VEDIT_NO_TOOLS */

/* Write a small C file with three top-level functions and open it, so the
 * symbol picker (Ctrl-T) lists exactly alpha/bravo/charlie at lines 0/3/6. */
static struct vedit *
open_syms(Memio *m, struct vedit_io *io, char *dir, size_t dirsz, char *src,
    size_t srcsz)
{
	struct vedit *v;
	FILE *f;

	snprintf(dir, dirsz, "/tmp/vedit_pkXXXXXX");
	if (!mkdtemp(dir))
		return NULL;
	snprintf(src, srcsz, "%s/sym.c", dir);
	f = fopen(src, "w");
	if (!f)
		return NULL;
	fputs("int alpha(void)\n{\n}\nint bravo(void)\n{\n}\n"
	    "int charlie(void)\n{\n}\n", f);
	fclose(f);

	memio_bind(io, m);
	v = vedit_new(io);
	if (!v)
		return NULL;
	if (vedit_open(v, src) != 0) {
		vedit_free(v);
		return NULL;
	}
	v->e.cy = 0;
	v->e.cx = 0;
	return v;
}

/* The symbol picker, driven through the modal loop: Ctrl-T opens it, Down moves
 * to the second row (bravo at line 3), Enter chooses it and jumps there. */
static void
t_pick_symbol_choose(Test *t)
{
	char dir[PATH_MAX], src[PATH_MAX];
	const char keys[] = "\x14\033[B\r";	/* Ctrl-T, Down, Enter */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	v = open_syms(&m, &io, dir, sizeof(dir), src, sizeof(src));
	TAP_ASSERT(t, v != NULL);

	vedit_run(v);

	TAP_CHECKF(t, v->e.cy == 3, "chose row 1 -> line %zu", v->e.cy);

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	unlink(src);
	rmdir(dir);
}

/* Esc cancels the picker: after a Down, Esc closes it and the cursor stays put
 * (no row was chosen, so no jump). */
static void
t_pick_symbol_cancel(Test *t)
{
	char dir[PATH_MAX], src[PATH_MAX];
	const char keys[] = "\x14\033[B\033";	/* Ctrl-T, Down, Esc */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 24, 80);
	v = open_syms(&m, &io, dir, sizeof(dir), src, sizeof(src));
	TAP_ASSERT(t, v != NULL);

	vedit_run(v);

	TAP_CHECKF(t, v->e.cy == 0, "cancelled; cursor at line %zu", v->e.cy);

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	unlink(src);
	rmdir(dir);
}

/* The file picker's entry line, reached through the menu: Alt-F opens the File
 * menu, 'o' picks Open, Tab moves focus to the entry field, a typed absolute
 * path is submitted with Enter, and the editor opens that file. Exercises the
 * entry focus toggle, pick_entry_key typing, and submit -> PICK_DONE. */
static void
t_pick_open_entry(Test *t)
{
	char dir[] = "/tmp/vedit_poXXXXXX";
	char start[PATH_MAX], target[PATH_MAX];
	char keys[PATH_MAX + 16];
	size_t klen;
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(start, sizeof(start), "%s/start.txt", dir);
	snprintf(target, sizeof(target), "%s/target.txt", dir);
	f = fopen(start, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("x\n", f);
	fclose(f);
	f = fopen(target, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("hello\n", f);
	fclose(f);

	/* Alt-F, 'o', Tab, <absolute path>, Enter */
	klen = (size_t)snprintf(keys, sizeof(keys), "\033fo\t%s\r", target);

	memio_init(&m, keys, klen, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, start) == 0);

	vedit_run(v);

	TAP_CHECK(t, v->e.has_name &&
	    strcmp(v->e.path + strlen(v->e.path) - 10, "target.txt") == 0);
	{
		size_t len = 0;
		const char *line = text_line(v->e.t, 0, &len);

		TAP_CHECK(t, line && len == 5 && memcmp(line, "hello", 5) == 0);
	}

	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;
	unlink(start);
	unlink(target);
	rmdir(dir);
}

/* scr_resize reallocates the grid to the new size, marks every row dirty, and
 * drops shadow_valid so the next present repaints the whole screen. */
static void
t_resize_grid(Test *t)
{
	Memio m;
	struct vedit_io io;
	Scrbuf *sb;
	Screen *d;
	int rows, cols;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	sb = scr_new_io(&io);
	TAP_ASSERT(t, sb != NULL);
	d = scr_new(sb);
	TAP_ASSERT(t, d != NULL);

	scr_size(d, &rows, &cols);
	TAP_CHECKF(t, rows == 24 && cols == 80, "initial size %dx%d", rows, cols);

	sb->shadow_valid = 1;		/* pretend the screen is in sync */
	scr_resize(d, 40, 120);
	scr_size(d, &rows, &cols);
	TAP_CHECKF(t, rows == 40 && cols == 120, "resized to %dx%d", rows, cols);
	TAP_CHECK(t, sb->shadow_valid == 0);	/* forces a full repaint */
	TAP_CHECK(t, sb->rowdirty[0] == 1 && sb->rowdirty[39] == 1);

	scr_resize(d, 0, 0);		/* degenerate size clamps to 1x1 */
	scr_size(d, &rows, &cols);
	TAP_CHECKF(t, rows == 1 && cols == 1, "clamped to %dx%d", rows, cols);

	scr_free(d);
	memio_free(&m);
}

/* A SIGWINCH (g_winch) only flags that the window changed: scr_wait re-reads
 * the live size via getsize, resizes to it, and returns EVENT_RESIZE without
 * consuming input. */
static void
t_resize_signal(Test *t)
{
	Memio m;
	struct vedit_io io;
	Scrbuf *sb;
	Screen *d;
	Event ev;
	int rows, cols, rc;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	sb = scr_new_io(&io);
	TAP_ASSERT(t, sb != NULL);
	d = scr_new(sb);
	TAP_ASSERT(t, d != NULL);

	m.rows = 50;			/* the window changed under us */
	m.cols = 100;
	g_winch = 1;			/* as the SIGWINCH handler would set it */

	memset(&ev, 0, sizeof(ev));
	rc = scr_wait(d, &ev);
	TAP_CHECKF(t, rc == EVENT_RESIZE, "scr_wait returned %d", rc);
	TAP_CHECK(t, ev.type == EVENT_RESIZE);
	TAP_CHECK(t, g_winch == 0);
	scr_size(d, &rows, &cols);
	TAP_CHECKF(t, rows == 50 && cols == 100, "size after signal %dx%d",
	    rows, cols);

	scr_free(d);
	memio_free(&m);
}

/* A host-driven resize (vedit_set_size sets want_resize after applying its own
 * size) is reported by scr_wait as EVENT_RESIZE without re-querying getsize, so
 * a stale getsize cannot override the host's dimensions. */
static void
t_resize_event(Test *t)
{
	Memio m;
	struct vedit_io io;
	Scrbuf *sb;
	Screen *d;
	Event ev;
	int rows, cols, rc;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	sb = scr_new_io(&io);
	TAP_ASSERT(t, sb != NULL);
	d = scr_new(sb);
	TAP_ASSERT(t, d != NULL);

	scr_resize(d, 50, 100);		/* the host applied its size */
	m.rows = 24;			/* getsize is stale and must be ignored */
	m.cols = 80;
	sb->want_resize = 1;		/* as vedit_set_size would set it */

	memset(&ev, 0, sizeof(ev));
	rc = scr_wait(d, &ev);
	TAP_CHECKF(t, rc == EVENT_RESIZE, "scr_wait returned %d", rc);
	TAP_CHECK(t, ev.type == EVENT_RESIZE);
	TAP_CHECK(t, sb->want_resize == 0);
	scr_size(d, &rows, &cols);
	TAP_CHECKF(t, rows == 50 && cols == 100,
	    "host size overridden to %dx%d", rows, cols);

	scr_free(d);
	memio_free(&m);
}

/* Shrinking the window keeps the cursor on screen: after vedit_set_size the
 * next render re-clamps the scroll offset around the cursor. */
static void
t_resize_reflow(Test *t)
{
	static const char *const L[] = {
		"l1", "l2", "l3", "l4", "l5", "l6", "l7", "l8", "l9", "l10",
		"l11", "l12", "l13", "l14", "l15", "l16", "l17", "l18", "l19",
		"l20"
	};
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int rows, cols, th;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 20);

	/* Put the cursor on the last line and lay out at the full height. */
	v->e.cy = 19;
	render_body(&v->e, v->e.d);
	TAP_CHECK(t, v->e.cy >= v->e.top);

	/* Shrink the window; the grid and the editor dimensions both follow. */
	vedit_set_size(v, 10, 40);
	scr_size(v->e.d, &rows, &cols);
	TAP_CHECKF(t, rows == 10 && cols == 40, "grid %dx%d after shrink",
	    rows, cols);
	TAP_CHECKF(t, v->e.rows == 10 && v->e.cols == 40,
	    "editor %dx%d after shrink", v->e.rows, v->e.cols);

	/* Re-render at the new height: the cursor must stay within the text
	 * area, so the scroll offset has to move down with it. */
	render_body(&v->e, v->e.d);
	th = text_height(&v->e);
	TAP_CHECK(t, v->e.cy >= v->e.top);
	TAP_CHECKF(t, v->e.cy < v->e.top + (size_t)th,
	    "cursor %zu off screen: top %zu height %d",
	    v->e.cy, v->e.top, th);

	vedit_free(v);
	memio_free(&m);
}

/* Index of the menu with this exact title (with the '&' mnemonic), or -1. */
static int
find_menu(const char *title)
{
	int i;

	for (i = 0; i < MENU_COUNT; i++)
		if (strcmp(MENUS[i].title, title) == 0)
			return i;
	return -1;
}

/* menu_item_enabled tracks the editor context it reads. */
static void
t_menu_item_enabled(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	char *clip;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);

	TAP_CHECK(t, menu_item_enabled(&v->e, MA_NEW) == 1);	/* always on */
	TAP_CHECK(t, menu_item_enabled(&v->e, MA_SEP) == 0);	/* never */

	TAP_CHECK(t, menu_item_enabled(&v->e, MA_PASTE) == 0);	/* empty clip */
	clip = malloc(2);
	TAP_ASSERT(t, clip != NULL);
	clip[0] = 'x';
	clip_set(&v->e, clip, 1);				/* owns clip */
	TAP_CHECK(t, menu_item_enabled(&v->e, MA_PASTE) == 1);

	TAP_CHECK(t, menu_item_enabled(&v->e, MA_CUT) == 0);	/* no selection */
	v->e.sel_active = 1;
	TAP_CHECK(t, menu_item_enabled(&v->e, MA_CUT) == 1);

	TAP_CHECK(t, menu_item_enabled(&v->e, MA_FIND_NEXT) == 0);
	v->e.last_find[0] = 'x';
	TAP_CHECK(t, menu_item_enabled(&v->e, MA_FIND_NEXT) == 1);

	vedit_free(v);
	memio_free(&m);
}

#ifndef VEDIT_NO_TOOLS
/* The Compile menu is hidden with no build command and shown once one exists. */
static void
t_menu_hide_tools(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Cfg *cfg;
	int ci, ri;

	ci = find_menu("&Compile");
	ri = find_menu("&Run");
	TAP_ASSERT(t, ci >= 0 && ri >= 0);

	/* no tools, no config: both tool menus hidden, their mnemonics dead */
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_CHECK(t, !menu_visible(&v->e, ci));
	TAP_CHECK(t, !menu_visible(&v->e, ri));
	TAP_CHECK(t, menu_col(&v->e, ci) == -1);
	TAP_CHECK(t, menu_title_by_mnemonic(&v->e, 'c') == -1);
	vedit_free(v);
	memio_free(&m);

	/* a build command for C makes the Compile menu appear for a .c file */
	cfg = cfg_from_text("[command \"c\"]\n\tbuild = make\n");
	TAP_ASSERT(t, cfg != NULL);
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_set_config(v, cfg);
	vedit_set_tools(v, &fake_tools);
	vedit_open(v, "test.c");
	TAP_CHECK(t, menu_visible(&v->e, ci));		/* MA_MAKE is ready */
	TAP_CHECK(t, menu_title_by_mnemonic(&v->e, 'c') == ci);
	TAP_CHECK(t, !menu_visible(&v->e, ri));		/* no run cmd/output yet */
	vedit_free(v);
	memio_free(&m);
	g_cfg = NULL;			/* editor borrowed it; drop before free */
	vedit_cfg_free(cfg);
}
#endif

/* With the tool and terminal menus hidden, the bar packs to the same columns as
 * before this feature, and Help stays right-aligned. */
static void
t_menu_col_pack(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);

	TAP_CHECK(t, menu_col(&v->e, find_menu("&File")) == 1);
	TAP_CHECK(t, menu_col(&v->e, find_menu("&Edit")) == 7);
	TAP_CHECK(t, menu_col(&v->e, find_menu("&Search")) == 13);
	TAP_CHECK(t, menu_col(&v->e, find_menu("&View")) == 21);
	TAP_CHECK(t, menu_col(&v->e, find_menu("&Options")) == 27);
	TAP_CHECK(t, menu_col(&v->e, MENU_HELP) == v->e.cols - 5);

	vedit_free(v);
	memio_free(&m);
}

/* On a narrow bar a title that would collide with the right-aligned Help label
 * is dropped whole instead of overprinting it, and it stops swallowing clicks,
 * but it stays reachable by its mnemonic. A wide bar draws them all. */
static void
t_menu_narrow(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int fi, ei, si, hi;

	fi = find_menu("&File");
	ei = find_menu("&Edit");
	si = find_menu("&Search");
	hi = MENU_HELP;
	TAP_ASSERT(t, fi >= 0 && ei >= 0 && si >= 0);

	/* wide: every standard title is drawn */
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_CHECK(t, menu_title_drawn(&v->e, fi));
	TAP_CHECK(t, menu_title_drawn(&v->e, ei));
	TAP_CHECK(t, menu_title_drawn(&v->e, si));
	TAP_CHECK(t, menu_title_drawn(&v->e, hi));
	vedit_free(v);
	memio_free(&m);

	/* narrow: File and Edit fit, Search collides with Help so it is hidden */
	memio_init(&m, "", 0, 24, 20);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_CHECK(t, menu_title_drawn(&v->e, fi));
	TAP_CHECK(t, menu_title_drawn(&v->e, ei));
	TAP_CHECK(t, !menu_title_drawn(&v->e, si));	/* would overprint Help */
	TAP_CHECK(t, menu_title_drawn(&v->e, hi));	/* Help still fits */
	/* a click where Search used to sit no longer opens it */
	TAP_CHECK(t, menu_hit(&v->e, menu_col(&v->e, si)) != si);
	/* but Alt+S still reaches it */
	TAP_CHECK(t, menu_title_by_mnemonic(&v->e, 's') == si);
	vedit_free(v);
	memio_free(&m);
}

/* The key-bindings screen scrolls, so an entry past the first screen is reached
 * by paging down. Drives F2 (vi keys), F1 (help), then Space to page down on a
 * short window, and checks a near-bottom vi entry becomes visible. */
static void
t_help_scroll(Test *t)
{
	const char keys[] = "\033OQ\033OP    ";	/* F2, F1, four page-downs */
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	memio_init(&m, keys, sizeof(keys) - 1, 12, 80);	/* short: must scroll */
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_run(v);
	/* ":reload" is one of the last vi-help rows, off the first screen at 12
	 * rows, so seeing it proves both the vi table and the scrolling. */
	TAP_CHECK(t, strstr(m.out, "Re-read the config") != NULL);
	vedit_free(v);
	memio_free(&m);
}

/* A pending termination signal unwinds the run like end-of-input: scr_wait
 * reports EOF while g_sig_quit is set, so vedit_run returns promptly. (The
 * command-line handler, self-pipe, and re-raise are CLI-only and not exercised
 * here.) */
static void
t_sig_quit_unwinds(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int rc;

	/* a long scripted input that would otherwise keep the loop busy */
	memio_init(&m, "iiiiiiiiii", 10, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);

	g_sig_quit = SIGTERM;		/* as the fatal handler would set it */
	rc = vedit_run(v);		/* scr_wait returns EOF at once */
	g_sig_quit = 0;			/* do not leak into later tests */
	TAP_CHECK(t, rc == 1);

	vedit_free(v);
	memio_free(&m);
}

/* Below the minimum usable size the frame is replaced by a centered notice; at
 * the floor the real framed editor renders. */
static void
t_win_too_small(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;

	/* too short: notice shown, no status bar */
	memio_init(&m, "", 0, WIN_MIN_ROWS - 1, 40);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_run(v);
	TAP_CHECK(t, strstr(m.out, "window too small") != NULL);
	TAP_CHECK(t, strstr(m.out, "F1") == NULL);	/* no status/menu bar */
	vedit_free(v);
	memio_free(&m);

	/* too narrow: a shorter message that fits is used */
	memio_init(&m, "", 0, 24, WIN_MIN_COLS - 1);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_run(v);
	TAP_CHECK(t, strstr(m.out, "too small") != NULL);
	vedit_free(v);
	memio_free(&m);

	/* exactly at the floor: the framed editor renders, no notice */
	memio_init(&m, "", 0, WIN_MIN_ROWS, WIN_MIN_COLS);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	vedit_run(v);
	TAP_CHECK(t, strstr(m.out, "too small") == NULL);
	TAP_CHECK(t, strstr(m.out, "Press F1") != NULL);	/* the status bar is drawn */
	vedit_free(v);
	memio_free(&m);
}

#ifdef VEDIT_TERM
/* The Terminal menu follows the host's fd multiplexer, and when shown it packs
 * into the slot the hidden Compile/Run left open. */
static void
t_menu_terminal(Test *t)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	int ti, oi, want;

	ti = find_menu("&Terminal");
	oi = find_menu("&Options");
	TAP_ASSERT(t, ti >= 0 && oi >= 0);

	/* no poll_fds: can neither open a terminal nor is one active -> hidden */
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_CHECK(t, !menu_visible(&v->e, ti));
	TAP_CHECK(t, menu_col(&v->e, ti) == -1);
	vedit_free(v);
	memio_free(&m);

	/* with a multiplexing host the menu shows, packed right after Options
	 * (Compile/Run stay hidden) */
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	memio_enable_fds(&io);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_CHECK(t, menu_visible(&v->e, ti));
	want = menu_col(&v->e, oi) + menu_disp_w("&Options") + 2;
	TAP_CHECKF(t, menu_col(&v->e, ti) == want,
	    "terminal col %d, want %d", menu_col(&v->e, ti), want);
	vedit_free(v);
	memio_free(&m);
}
#endif

/* A .csv opens with the table view attached: records joined, the BOM noted,
 * the columns counted; a .tsv uses tabs; :table toggles and :colwidth sets. */
static void
t_tbl_attach(Test *t)
{
	char dir[] = "/tmp/vedit-tbl-XXXXXX";
	char path[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;
	Scrbuf *sb;
	char bar[81];
	int i;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/a.csv", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("\xef\xbb\xbfid;name;note\r\n1;Ann;\"two\r\nlines\"\r\n2;Bob;x;extra\r\n", f);
	fclose(f);

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.tbl != NULL);
	TAP_CHECKF(t, v->e.tbl->delim == ';', "delimiter %d", v->e.tbl->delim);
	TAP_CHECKF(t, v->e.tbl->ncols == 4, "%d columns", v->e.tbl->ncols);
	TAP_CHECK(t, v->e.tbl->bom == 3 && v->e.tbl->header);
	TAP_CHECKF(t, text_lines(v->e.t) == 3, "%zu lines", text_lines(v->e.t));
	TAP_CHECK(t, !text_dirty(v->e.t));
	TAP_CHECK(t, tbl_width(v->e.tbl, 0) == TBL_WIDTH_DEFAULT);

	/* the status line names the cell, without the BOM */
	ed_render(&v->e, v->e.d);
	sb = v->e.d->t;
	for (i = 0; i < 80; i++)
		bar[i] = (char)sb->cur[(size_t)(sb->rows - 1) * sb->cols + i].codepoint;
	bar[80] = '\0';
	TAP_CHECKF(t, strstr(bar, "-- TABLE --  A1: id") != NULL, "bar [%s]", bar);

	/* saving writes the joined record back as it was */
	TAP_ASSERT(t, save_editor(&v->e) == 0);
	f = fopen(path, "rb");
	TAP_ASSERT(t, f != NULL);
	{
		char back[128];
		size_t n = fread(back, 1, sizeof(back) - 1, f);

		back[n] = '\0';
		TAP_CHECKF(t, strcmp(back,
		    "\xef\xbb\xbfid;name;note\r\n1;Ann;\"two\r\nlines\"\r\n2;Bob;x;extra\r\n") == 0,
		    "saved [%s]", back);
	}
	fclose(f);

	/* :colwidth, :table off, :table back on with a forced delimiter */
	tbl_colwidth(&v->e, "20");
	TAP_CHECK(t, tbl_width(v->e.tbl, 0) == 20 && tbl_width(v->e.tbl, 1) == 10);
	tbl_colwidth(&v->e, "7 all");
	TAP_CHECK(t, tbl_width(v->e.tbl, 0) == 7 && tbl_width(v->e.tbl, 1) == 7);
	tbl_colwidth(&v->e, "0");
	TAP_CHECK(t, strncmp(v->e.status, "E474", 4) == 0);
	tbl_colwidth(&v->e, "fit");		/* A: "id" vs "1", "2": 2 wide */
	TAP_CHECKF(t, tbl_width(v->e.tbl, 0) == 2, "fitted A to %d", tbl_width(v->e.tbl, 0));
	tbl_colwidth(&v->e, "fit all");
	TAP_CHECKF(t, tbl_width(v->e.tbl, 1) == 4 && tbl_width(v->e.tbl, 2) == 10 &&
	    tbl_width(v->e.tbl, 3) == 5, "fitted widths %d %d %d", tbl_width(v->e.tbl, 1),
	    tbl_width(v->e.tbl, 2), tbl_width(v->e.tbl, 3));
	TAP_CHECK(t, menu_item_enabled(&v->e, MA_TBL_FIT) == 1);
	tbl_command(&v->e, "off");
	TAP_CHECK(t, v->e.tbl == NULL);
	tbl_colwidth(&v->e, "5");
	TAP_CHECK(t, strncmp(v->e.status, "not a table", 11) == 0);
	tbl_command(&v->e, "pipe");
	TAP_ASSERT(t, v->e.tbl != NULL);
	TAP_CHECK(t, v->e.tbl->delim == '|' && v->e.tbl->ncols == 1);
	tbl_command(&v->e, "noheader");
	TAP_CHECK(t, !v->e.tbl->header);
	tbl_command(&v->e, "");
	TAP_CHECK(t, v->e.tbl == NULL);
	tbl_command(&v->e, "bogus");
	TAP_CHECK(t, v->e.tbl == NULL && strncmp(v->e.status, "E474", 4) == 0);

	/* the table state parks with its buffer */
	tbl_command(&v->e, ";");
	TAP_ASSERT(t, buf_slot(&v->e) == 0);
	buf_save(&v->e, &v->e.bufs[0]);
	TAP_ASSERT(t, buf_open(&v->e, NULL) == 1);
	TAP_CHECK(t, v->e.tbl == NULL && v->e.bufs[0].tbl != NULL);
	buf_switch(&v->e, 0);
	TAP_CHECK(t, v->e.tbl != NULL && v->e.tbl->delim == ';');

	vedit_free(v);
	memio_free(&m);

	/* a .tsv: tabs, and a typed tab stays a tab */
	snprintf(path, sizeof(path), "%s/b.tsv", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("a\tb\n1\t2\n", f);
	fclose(f);
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.expand_tabs = 1;
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.tbl != NULL);
	TAP_CHECK(t, v->e.tbl->delim == '\t' && v->e.tbl->ncols == 2 && !v->e.expand_tabs);
	vedit_free(v);
	memio_free(&m);

	/* table.view = off leaves the file as text */
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	v->e.tbl_on = 0;
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_CHECK(t, v->e.tbl == NULL);
	vedit_free(v);
	memio_free(&m);

	unlink(path);
	snprintf(path, sizeof(path), "%s/a.csv", dir);
	unlink(path);
	rmdir(dir);
}

/* The grid: a label row, a row-number gutter, the frozen header, cells
 * truncated with a marker, the current cell reversed; keys move by cell in
 * both personalities and the columns scroll as whole columns. */
static void
t_tbl_grid(Test *t)
{
	char dir[] = "/tmp/vedit-tbl-XXXXXX";
	char path[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;
	Scrbuf *sb;
	struct tkbd_seq seq;
	int r0 = CHROME_TOP, c0 = CHROME_LEFT, gw, i;
	char row[81];

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/g.csv", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("id,name,note,amount,e,f\n1,Ann,\"two\nlines\",10.50,,\n", f);
	for (i = 2; i <= 30; i++)
		fprintf(f, "%d,Bob,a rather long note that will not fit,7,,\n", i);
	fclose(f);

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.tbl != NULL && v->e.tbl->ncols == 6);
	gw = tbl_gutter(&v->e);
	TAP_CHECKF(t, gw == 3, "gutter %d", gw);

	ed_render(&v->e, v->e.d);
	sb = v->e.d->t;
#define ROW(y) do { int x_; for (x_ = 0; x_ < 80; x_++) \
	row[x_] = (char)(sb->cur[(size_t)(y) * sb->cols + x_].codepoint < 128 ? \
	    sb->cur[(size_t)(y) * sb->cols + x_].codepoint : '#'); row[80] = '\0'; } while (0)
#define CELL(y, x) (sb->cur[(size_t)(y) * sb->cols + (x)])
	ROW(r0);
	TAP_CHECKF(t, strncmp(row + c0 + gw, "A          B", 12) == 0, "labels [%s]", row);
	ROW(r0 + 1);
	TAP_CHECKF(t, strncmp(row + c0, " 1 id         name", 18) == 0, "header [%s]", row);
	TAP_CHECK(t, CELL(r0 + 1, c0 + gw).attrs & ATTR_BOLD);
	TAP_CHECK(t, CELL(r0 + 1, c0 + gw).attrs & ATTR_REVERSE);	/* A1 current */
	ROW(r0 + 2);
	TAP_CHECKF(t, strncmp(row + c0 + gw + 22, "two#lines", 9) == 0, "row 2 [%s]", row);
	TAP_CHECK(t, CELL(r0 + 2, c0 + gw + 25).codepoint == 0x21b5);	/* the newline */
	ROW(r0 + 3);
	TAP_CHECKF(t, strncmp(row + c0 + gw + 22, "a rather #", 10) == 0, "row 3 [%s]", row);
	TAP_CHECK(t, CELL(r0 + 3, c0 + gw + 31).codepoint == 0x2026);	/* truncated */
	TAP_CHECK(t, sb->cursor_r == r0 + 1 && sb->cursor_c == c0 + gw);

	/* modeless keys */
	memset(&seq, 0, sizeof(seq));
	seq.type = TKBD_KEY;
	seq.ch = TKBD_CH_NONE;
	seq.key = TKBD_KEY_RIGHT;
	run_req(&v->e, tbl_key(&v->e, &seq));
	seq.key = TKBD_KEY_DOWN;
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.cy == 1 && v->e.tbl->cx == 1);
	seq.key = TKBD_KEY_TAB;
	seq.mod = TKBD_MOD_SHIFT;
	run_req(&v->e, tbl_key(&v->e, &seq));
	seq.mod = 0;
	TAP_CHECK(t, v->e.tbl->cx == 0);
	seq.key = TKBD_KEY_END;
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.tbl->cx == 5);
	seq.key = TKBD_KEY_PGDN;
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECKF(t, v->e.cy == 18, "PgDn to row %zu", v->e.cy);
	seq.key = TKBD_KEY_END;
	seq.mod = TKBD_MOD_CTRL;
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECKF(t, v->e.cy == 30 && v->e.tbl->cx == 0, "Ctrl+End: row %zu col %d", v->e.cy, v->e.tbl->cx);
	seq.key = TKBD_KEY_HOME;
	run_req(&v->e, tbl_key(&v->e, &seq));
	seq.mod = 0;
	TAP_CHECK(t, v->e.cy == 0);

	/* the header stays while the rows scroll */
	v->e.cy = 25;
	ed_render(&v->e, v->e.d);
	ROW(r0 + 1);
	TAP_CHECKF(t, strncmp(row + c0, " 1 id", 5) == 0, "frozen header [%s]", row);
	ROW(r0 + 2);
	TAP_CHECKF(t, v->e.top > 1 && atoi(row + c0) == (int)v->e.top + 1, "first row [%s] top %zu",
	    row, v->e.top);
	TAP_CHECK(t, sb->cursor_r == r0 + 2 + (int)(v->e.cy - v->e.top));

	/* wide columns scroll by whole columns; the gutter and label row stay */
	tbl_colwidth(&v->e, "30 all");
	v->e.tbl->cx = 3;
	ed_render(&v->e, v->e.d);
	TAP_CHECKF(t, v->e.tbl->left == 2, "left column %d", v->e.tbl->left);
	ROW(r0);
	TAP_CHECKF(t, strncmp(row + c0 + gw, "C", 1) == 0, "labels [%s]", row);
	TAP_CHECK(t, sb->cursor_c == c0 + gw + 31);

	/* vi keys */
	v->e.mode = MODE_NORMAL;
	seq.key = TKBD_KEY_H;
	seq.ch = 'h';
	run_req(&v->e, tbl_key(&v->e, &seq));
	seq.key = TKBD_KEY_K;
	seq.ch = 'k';
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.cy == 24 && v->e.tbl->cx == 2);
	seq.key = TKBD_KEY_G;
	seq.ch = 'g';
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.tbl->pending == 'g');
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.cy == 0 && v->e.tbl->pending == 0);
	seq.ch = '$';
	seq.key = '$';
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.tbl->cx == 5);
	seq.ch = ':';
	seq.key = ':';
	TAP_CHECK(t, tbl_key(&v->e, &seq) == REQ_VI_COLON);

	/* leaving the view puts the text cursor on the cell */
	v->e.tbl->cx = 2;
	v->e.cy = 1;
	tbl_command(&v->e, "off");
	TAP_CHECK(t, v->e.tbl == NULL && v->e.cx == 6 && v->e.top == 0);
#undef ROW
#undef CELL

	vedit_free(v);
	memio_free(&m);
	unlink(path);
	rmdir(dir);
}

/* The prompt editor moves its cursor; a cell edit commits with canonical
 * quoting, pads short rows, clears, copies and pastes; a search lands on
 * the cell; :cell jumps by label. */
static void
t_tbl_edit(Test *t)
{
	char dir[] = "/tmp/vedit-tbl-XXXXXX";
	char path[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;
	struct tkbd_seq seq;
	size_t len;
	const char *line;
	char buf[64];

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/e.csv", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("id,name,note\n1,Ann,x\n2,Bob\n3,\"Cy, jr\",needle here\n", f);
	fclose(f);

	/* the prompt: Left twice, insert, Alt+Enter, Delete, End, type, Enter */
	memio_init(&m, "\033[D\033[DX\033\r\033[3~\033[FZ\r", 18, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	snprintf(buf, sizeof(buf), "abc");
	TAP_CHECK(t, prompt_edit(&v->e, "> ", buf, sizeof(buf), 1, 3, 1) == 1);
	TAP_CHECKF(t, strcmp(buf, "aX\ncZ") == 0, "prompt gave [%s]", buf);
	vedit_free(v);
	memio_free(&m);

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.tbl != NULL);

	/* set: quoting only when needed, a short row padded, one undo step */
	TAP_CHECK(t, tbl_set_cell(&v->e, 1, 1, "Ann, PhD", 8) == 0);
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,\"Ann, PhD\",x") == 0, "row 2 [%s]", line);
	TAP_CHECK(t, tbl_set_cell(&v->e, 1, 1, "Ann", 3) == 0);
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,Ann,x") == 0, "row 2 [%s]", line);
	TAP_CHECK(t, tbl_set_cell(&v->e, 2, 2, "say \"hi\"\nthere", 14) == 0);
	line = text_line(v->e.t, 2, &len);
	TAP_CHECKF(t, strcmp(line, "2,Bob,\"say \"\"hi\"\"\nthere\"") == 0, "row 3 [%s]", line);
	TAP_CHECK(t, text_undo(v->e.t, &v->e.cy, &v->e.cx) == 0);
	line = text_line(v->e.t, 2, &len);
	TAP_CHECKF(t, strcmp(line, "2,Bob") == 0, "after undo [%s]", line);
	TAP_CHECK(t, text_dirty(v->e.t));

	/* keys: Delete clears; typing replaces through the prompt; Ctrl+C/V */
	memset(&seq, 0, sizeof(seq));
	seq.type = TKBD_KEY;
	seq.ch = TKBD_CH_NONE;
	v->e.cy = 1;
	v->e.tbl->cx = 2;
	seq.key = TKBD_KEY_DEL;
	run_req(&v->e, tbl_key(&v->e, &seq));
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,Ann,") == 0, "cleared [%s]", line);
	v->e.tbl->cx = 1;
	seq.key = TKBD_KEY_C;
	seq.mod = TKBD_MOD_CTRL;
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.clip && v->e.clip_len == 3 && memcmp(v->e.clip, "Ann", 3) == 0);
	v->e.tbl->cx = 2;
	seq.key = TKBD_KEY_V;
	run_req(&v->e, tbl_key(&v->e, &seq));
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,Ann,Ann") == 0, "pasted [%s]", line);
	seq.mod = 0;
	vedit_free(v);
	memio_free(&m);

	/* typing 'Q' then "x" Enter replaces the cell with Qx (modeless) */
	memio_init(&m, "x\r", 2, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	v->e.cy = 1;
	v->e.tbl->cx = 1;
	seq.key = TKBD_KEY_Q;
	seq.ch = 'Q';
	run_req(&v->e, tbl_key(&v->e, &seq));
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,Qx,x") == 0, "replaced [%s]", line);
	vedit_free(v);
	memio_free(&m);

	/* vi: 'a' appends through the prompt; 'x' clears; search lands on a cell */
	memio_init(&m, "!\r", 2, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	v->e.mode = MODE_NORMAL;
	v->e.cy = 1;
	v->e.tbl->cx = 1;
	seq.key = TKBD_KEY_A;
	seq.ch = 'a';
	run_req(&v->e, tbl_key(&v->e, &seq));
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,Ann!,x") == 0, "appended [%s]", line);
	seq.key = TKBD_KEY_X;
	seq.ch = 'x';
	run_req(&v->e, tbl_key(&v->e, &seq));
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,,x") == 0, "x cleared [%s]", line);
	snprintf(v->e.last_find, sizeof(v->e.last_find), "needle");
	v->e.vi_search_dir = 1;
	seq.key = TKBD_KEY_N;
	seq.ch = 'n';
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECKF(t, v->e.cy == 3 && v->e.tbl->cx == 2, "search landed on row %zu col %d",
	    v->e.cy, v->e.tbl->cx);
	TAP_CHECK(t, tbl_col_at(&v->e, 3, 0) == 0 && tbl_col_at(&v->e, 3, 5) == 1 &&
	    tbl_col_at(&v->e, 3, 99) == 2);

	/* :cell */
	tbl_cell_goto(&v->e, "B2");
	TAP_CHECK(t, v->e.cy == 1 && v->e.tbl->cx == 1);
	tbl_cell_goto(&v->e, "c");
	TAP_CHECK(t, v->e.cy == 1 && v->e.tbl->cx == 2);
	tbl_cell_goto(&v->e, "99");
	TAP_CHECK(t, v->e.cy == 3 && v->e.tbl->cx == 2);
	tbl_cell_goto(&v->e, "ZZ1");
	TAP_CHECK(t, v->e.cy == 0 && v->e.tbl->cx == 2);
	tbl_cell_goto(&v->e, "7x");
	TAP_CHECK(t, strncmp(v->e.status, "E474", 4) == 0);
	TAP_CHECK(t, tbl_label_col("A") == 0 && tbl_label_col("aa") == 26 &&
	    tbl_label_col("ZZ") == 701 && tbl_label_col("A1") == -1);

	vedit_free(v);
	memio_free(&m);
	unlink(path);
	rmdir(dir);
}

/* Through the main loop: Ctrl+F searches from the grid and lands on the
 * cell, Enter edits, Ctrl+Q quits a clean buffer. */
static void
t_tbl_loop(Test *t)
{
	char dir[] = "/tmp/vedit-tbl-XXXXXX";
	char path[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;
	size_t len;
	const char *line;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/l.csv", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("id,name,note\n1,Ann,x\n2,Bob,y\n", f);
	fclose(f);

	/* Ctrl+F "Bob" Enter; Right; Enter "!" Enter; Ctrl+S; Ctrl+Q */
	memio_init(&m, "\006Bob\r\033[C\r!\r\023\021", 13, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.tbl != NULL);
	TAP_CHECK(t, vedit_run(v) == 0);
	TAP_CHECKF(t, v->e.cy == 2 && v->e.tbl->cx == 2, "ended on row %zu col %d",
	    v->e.cy, v->e.tbl->cx);
	line = text_line(v->e.t, 2, &len);
	TAP_CHECKF(t, strcmp(line, "2,Bob,y!") == 0, "row 3 [%s]", line);
	TAP_CHECK(t, !text_dirty(v->e.t));
	vedit_free(v);
	memio_free(&m);
	unlink(path);
	rmdir(dir);
}

/* Rows and columns: insert above and below, delete with copy, paste rows,
 * insert and delete columns with short rows and a BOM, the ex forms with
 * a bang and a count, and undo keeping the cell cursor. */
static void
t_tbl_rows(Test *t)
{
	char dir[] = "/tmp/vedit-tbl-XXXXXX";
	char path[PATH_MAX];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;
	struct tkbd_seq seq;
	char exbuf[32];
	size_t len;
	const char *line;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/r.csv", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("\xef\xbb\xbfid,name,note\n1,Ann,x\n2\n3,Cy,z\n", f);
	fclose(f);
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.tbl != NULL && v->e.tbl->ncols == 3);

	/* rows: above, below, delete, paste back */
	v->e.cy = 1;
	tbl_row_add(&v->e, 2, 0);
	TAP_CHECKF(t, text_lines(v->e.t) == 6 && v->e.cy == 1, "%zu lines, row %zu",
	    text_lines(v->e.t), v->e.cy);
	line = text_line(v->e.t, 3, &len);
	TAP_CHECK(t, strcmp(line, "1,Ann,x") == 0 && text_line_len(v->e.t, 1) == 0);
	TAP_CHECK(t, text_undo(v->e.t, &v->e.cy, &v->e.cx) == 0 && text_lines(v->e.t) == 4);
	v->e.cy = 3;
	tbl_row_add(&v->e, 1, 1);
	TAP_CHECK(t, text_lines(v->e.t) == 5 && v->e.cy == 4 && text_line_len(v->e.t, 4) == 0);
	v->e.cy = 1;
	tbl_row_del(&v->e, 2);
	TAP_CHECKF(t, text_lines(v->e.t) == 3 && v->e.cy == 1, "%zu lines after rowdel",
	    text_lines(v->e.t));
	line = text_line(v->e.t, 1, &len);
	TAP_CHECK(t, strcmp(line, "3,Cy,z") == 0);
	TAP_CHECK(t, v->e.clip_len == 10 && memcmp(v->e.clip, "1,Ann,x\n2\n", 10) == 0);
	tbl_row_paste(&v->e, 0);		/* above: back where they were */
	TAP_CHECK(t, text_lines(v->e.t) == 5 && v->e.cy == 1);
	line = text_line(v->e.t, 2, &len);
	TAP_CHECKF(t, strcmp(line, "2") == 0, "pasted row 3 [%s]", line);
	line = text_line(v->e.t, 3, &len);
	TAP_CHECK(t, strcmp(line, "3,Cy,z") == 0);
	v->e.cy = 4;
	tbl_row_paste(&v->e, 1);		/* below the last row */
	TAP_CHECK(t, text_lines(v->e.t) == 7 && v->e.cy == 5);
	TAP_CHECK(t, text_undo(v->e.t, &v->e.cy, &v->e.cx) == 0 && text_lines(v->e.t) == 5);

	/* columns: insert left of B; the short row "2" is untouched */
	v->e.cy = 0;
	v->e.tbl->cx = 1;
	tbl_col_add(&v->e, 1, 0);
	TAP_CHECK(t, v->e.tbl->ncols == 4 && v->e.tbl->cx == 1);
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(line, "\xef\xbb\xbfid,,name,note") == 0, "header [%s]", line);
	line = text_line(v->e.t, 2, &len);
	TAP_CHECK(t, strcmp(line, "2") == 0);
	/* insert right of A on row 0 keeps the BOM ahead of A */
	v->e.tbl->cx = 0;
	tbl_set_width(v->e.tbl, 0, 5);
	tbl_set_width(v->e.tbl, 1, 7);
	tbl_col_add(&v->e, 1, 1);
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(line, "\xef\xbb\xbfid,,,name,note") == 0, "header [%s]", line);
	TAP_CHECK(t, v->e.tbl->cx == 1 && v->e.tbl->ncols == 5);
	TAP_CHECK(t, tbl_width(v->e.tbl, 0) == 5 && tbl_width(v->e.tbl, 1) == 10 &&
	    tbl_width(v->e.tbl, 2) == 7);
	/* delete the two empty columns; then the last column; then the only one */
	tbl_col_del(&v->e, 2);
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(line, "\xef\xbb\xbfid,name,note") == 0, "header [%s]", line);
	TAP_CHECK(t, v->e.tbl->ncols == 3 && tbl_width(v->e.tbl, 1) == 10 && tbl_width(v->e.tbl, 0) == 5);
	v->e.tbl->cx = 2;
	tbl_col_del(&v->e, 1);
	line = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(line, "1,Ann") == 0, "row 2 [%s]", line);
	TAP_CHECK(t, v->e.tbl->ncols == 2 && v->e.tbl->cx == 1);
	tbl_col_del(&v->e, 5);
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(line, "\xef\xbb\xbfid") == 0, "header [%s]", line);
	v->e.tbl->cx = 0;
	tbl_col_del(&v->e, 1);			/* the only column: cleared */
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(line, "\xef\xbb\xbf") == 0 && v->e.tbl->ncols == 1, "header [%s]", line);

	/* undo from the keys keeps the cell cursor sane */
	memset(&seq, 0, sizeof(seq));
	seq.type = TKBD_KEY;
	seq.ch = TKBD_CH_NONE;
	seq.key = TKBD_KEY_Z;
	seq.mod = TKBD_MOD_CTRL;
	run_req(&v->e, tbl_key(&v->e, &seq));
	line = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(line, "\xef\xbb\xbfid") == 0, "after undo [%s]", line);
	run_req(&v->e, tbl_key(&v->e, &seq));
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECKF(t, v->e.tbl->ncols == 3, "ncols %d after undos", v->e.tbl->ncols);
	TAP_CHECK(t, v->e.tbl->cx < v->e.tbl->ncols);

	/* ex: the bang form and a count, the vi keys o O dd yy p */
	v->e.cy = 1;
	v->e.tbl->cx = 0;
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "rowadd! 2")) == REQ_CONTINUE);
	TAP_CHECK(t, text_lines(v->e.t) == 7 && v->e.cy == 2);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "rowdel 2")) == REQ_CONTINUE);
	TAP_CHECK(t, text_lines(v->e.t) == 5 && v->e.cy == 2);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "coladd")) == REQ_CONTINUE && v->e.tbl->ncols == 4);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "coldel")) == REQ_CONTINUE && v->e.tbl->ncols == 3);
	v->e.mode = MODE_NORMAL;
	seq.mod = 0;
	seq.key = TKBD_KEY_O;
	seq.ch = 'O';
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, text_lines(v->e.t) == 6 && v->e.cy == 2 && text_line_len(v->e.t, 2) == 0);
	seq.ch = 'd';
	seq.key = TKBD_KEY_D;
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.tbl->pending == 'd');
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, text_lines(v->e.t) == 5 && v->e.tbl->pending == 0);
	seq.ch = 'y';
	seq.key = TKBD_KEY_Y;
	run_req(&v->e, tbl_key(&v->e, &seq));
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, v->e.clip_len == 2 && memcmp(v->e.clip, "2\n", 2) == 0);
	seq.ch = 'p';
	seq.key = TKBD_KEY_P;
	run_req(&v->e, tbl_key(&v->e, &seq));
	TAP_CHECK(t, text_lines(v->e.t) == 6 && v->e.cy == 3);
	line = text_line(v->e.t, 3, &len);
	TAP_CHECK(t, strcmp(line, "2") == 0);

	/* the Edit menu knows the table */
	TAP_CHECK(t, menu_item_enabled(&v->e, MA_TBL_ROWADD) == 1);
	tbl_command(&v->e, "off");
	TAP_CHECK(t, menu_item_enabled(&v->e, MA_TBL_COLDEL) == 0);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "rowadd")) == REQ_CONTINUE &&
	    strncmp(v->e.status, "not a table", 11) == 0);

	vedit_free(v);
	memio_free(&m);
	unlink(path);
	rmdir(dir);
}

/* :sort: strings, decimal keys, ignore case, reverse, a key column, a
 * stable order, the table's data rows, and the dialog's key handler. */
static void
t_sort(Test *t)
{
	static const char *const L[] = {
		"pear 10", "Apple 2", "fig 2", "apple 1", "zoo x", "fig 100" };
	char dir[] = "/tmp/vedit-tbl-XXXXXX";
	char path[PATH_MAX], exbuf[32];
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	FILE *f;
	Sortctx c;
	Event ev;
	Modal md;
	size_t len;
	const char *s;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	fill_lines(v->e.t, L, 6);

	/* plain: bytewise, so capitals first; then ignore case; then reverse */
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort")) == REQ_CONTINUE);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(s, "Apple 2") == 0, "line 0 [%s]", s);
	s = text_line(v->e.t, 1, &len);
	TAP_CHECK(t, strcmp(s, "apple 1") == 0);
	s = text_line(v->e.t, 5, &len);
	TAP_CHECK(t, strcmp(s, "zoo x") == 0);
	TAP_CHECK(t, text_dirty(v->e.t));
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort i")) == REQ_CONTINUE);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(s, "apple 1") == 0, "icase line 0 [%s]", s);
	s = text_line(v->e.t, 1, &len);
	TAP_CHECK(t, strcmp(s, "Apple 2") == 0);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort!")) == REQ_CONTINUE);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(s, "zoo x") == 0, "reverse line 0 [%s]", s);
	TAP_CHECK(t, strncmp(v->e.status, "sorted 6 lines", 14) == 0);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort!")) == REQ_CONTINUE);
	TAP_CHECK(t, strcmp(v->e.status, "already in order") == 0);

	/* a numeric key at column 4 (after the name); non-numbers last */
	{
		static const char *const N[] = {
			"aa 10", "bb 2", "cc 100", "dd x", "ee 1" };

		vedit_free(v);
		memio_free(&m);
		memio_init(&m, "", 0, 24, 80);
		memio_bind(&io, &m);
		v = vedit_new(&io);
		TAP_ASSERT(t, v != NULL);
		fill_lines(v->e.t, N, 5);
	}
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort n 4")) == REQ_CONTINUE);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(s, "ee 1") == 0, "numeric line 0 [%s]", s);
	s = text_line(v->e.t, 2, &len);
	TAP_CHECKF(t, strcmp(s, "aa 10") == 0, "numeric line 2 [%s]", s);
	s = text_line(v->e.t, 3, &len);
	TAP_CHECK(t, strcmp(s, "cc 100") == 0);
	s = text_line(v->e.t, 4, &len);
	TAP_CHECK(t, strcmp(s, "dd x") == 0);
	/* a range sorts only those lines; undo restores one step */
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "1,3sort! n 4")) == REQ_CONTINUE);
	s = text_line(v->e.t, 4, &len);
	TAP_CHECK(t, strcmp(s, "dd x") == 0);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECKF(t, strcmp(s, "aa 10") == 0, "range line 0 [%s]", s);
	TAP_CHECK(t, text_undo(v->e.t, &v->e.cy, &v->e.cx) == 0);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECK(t, strcmp(s, "ee 1") == 0);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort q")) == REQ_CONTINUE &&
	    strncmp(v->e.status, "E474", 4) == 0);
	vedit_free(v);
	memio_free(&m);

	/* a table: the header stays, the key is the cursor column or a label */
	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/s.csv", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("name,qty\n\"Smith, J\",10\nadams,9\nBrown,100\n", f);
	fclose(f);
	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	TAP_ASSERT(t, v != NULL);
	TAP_ASSERT(t, vedit_open(v, path) == 0);
	TAP_ASSERT(t, v->e.tbl != NULL);
	v->e.tbl->cx = 1;
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort n")) == REQ_CONTINUE);
	s = text_line(v->e.t, 0, &len);
	TAP_CHECK(t, strcmp(s, "name,qty") == 0);
	s = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(s, "adams,9") == 0, "table row 2 [%s]", s);
	s = text_line(v->e.t, 3, &len);
	TAP_CHECK(t, strcmp(s, "Brown,100") == 0);
	TAP_CHECK(t, vi_ex_exec(&v->e, strcpy(exbuf, "sort i A")) == REQ_CONTINUE);
	s = text_line(v->e.t, 1, &len);
	TAP_CHECKF(t, strcmp(s, "adams,9") == 0, "by name [%s]", s);
	s = text_line(v->e.t, 3, &len);
	TAP_CHECKF(t, strcmp(s, "\"Smith, J\",10") == 0, "by name last [%s]", s);

	/* the dialog's keys: Tab to the kind, Right picks decimal, r reverses,
	 * then Enter; a letter in the column field is a label */
	memset(&c, 0, sizeof(c));
	c.table = 1;
	snprintf(c.col, sizeof(c.col), "B");
	memset(&ev, 0, sizeof(ev));
	ev.type = EVENT_KEY;
	ev.key.type = TKBD_KEY;
	ev.key.ch = TKBD_CH_NONE;
	ev.key.key = TKBD_KEY_TAB;
	TAP_CHECK(t, dlg_sort_key(&v->e, &md, &ev, &c) == 0 && c.focus == 1);
	ev.key.key = TKBD_KEY_RIGHT;
	TAP_CHECK(t, dlg_sort_key(&v->e, &md, &ev, &c) == 0 && c.numeric);
	ev.key.key = TKBD_KEY_NONE;
	ev.key.ch = 'r';
	TAP_CHECK(t, dlg_sort_key(&v->e, &md, &ev, &c) == 0 && c.reverse);
	ev.key.key = TKBD_KEY_ENTER;
	ev.key.ch = TKBD_CH_NONE;
	TAP_CHECK(t, dlg_sort_key(&v->e, &md, &ev, &c) == 1 && c.ok);
	c.focus = 5;
	ev.key.key = TKBD_KEY_ESC;
	c.ok = 0;
	TAP_CHECK(t, dlg_sort_key(&v->e, &md, &ev, &c) == 1 && !c.ok);
	c.focus = 0;
	ev.key.key = TKBD_KEY_NONE;
	ev.key.ch = 'A';
	TAP_CHECK(t, dlg_sort_key(&v->e, &md, &ev, &c) == 0 && strcmp(c.col, "BA") == 0);
	ev.key.key = TKBD_KEY_BACKSPACE;
	ev.key.ch = TKBD_CH_NONE;
	TAP_CHECK(t, dlg_sort_key(&v->e, &md, &ev, &c) == 0 && strcmp(c.col, "B") == 0);

	vedit_free(v);
	memio_free(&m);
	unlink(path);
	rmdir(dir);
}

const Case tap_cases[] = {
	{ "resize_grid", t_resize_grid },
	{ "resize_signal", t_resize_signal },
	{ "resize_event", t_resize_event },
	{ "resize_reflow", t_resize_reflow },
	{ "cursor_end_home", t_cursor_end_home },
	{ "cursor_home", t_cursor_home },
	{ "wrap_shows_tail", t_wrap_shows_tail },
	{ "nowrap_truncates_tail", t_nowrap_truncates_tail },
	{ "gutter_numbers", t_gutter_numbers },
	{ "status_flags", t_status_flags },
	{ "tab_key_expand", t_tab_key_expand },
	{ "tag_jump", t_tag_jump },
	{ "tag_stack", t_tag_stack },
	{ "gf_header", t_gf_header },
	{ "buf_dedup", t_buf_dedup },
	{ "reload_config", t_reload_config },
	{ "swap_file_created", t_swap_file_created },
	{ "swap_recover_key", t_swap_recover_key },
	{ "swap_recover_delete", t_swap_recover_delete },
	{ "search_icase", t_search_icase },
	{ "search_word", t_search_word },
	{ "shiftwidth", t_shiftwidth },
	{ "macro_play", t_macro_play },
	{ "macro_record", t_macro_record },
	{ "marks_special", t_marks_special },
	{ "jumplist", t_jumplist },
	{ "marks_ex", t_marks_ex },
	{ "marks_bracket", t_marks_bracket },
	{ "shell_cmd", t_shell_cmd },
	{ "exit_cursor", t_exit_cursor },
	{ "gv_reselect", t_gv_reselect },
	{ "global_yank", t_global_yank },
	{ "global_shift", t_global_shift },
	{ "vblock_delete", t_vblock_delete },
	{ "vblock_insert", t_vblock_insert },
	{ "vblock_yank_put", t_vblock_yank_put },
	{ "pick_symbol_choose", t_pick_symbol_choose },
	{ "pick_symbol_cancel", t_pick_symbol_cancel },
	{ "pick_open_entry", t_pick_open_entry },
	{ "menu_item_enabled", t_menu_item_enabled },
	{ "menu_col_pack", t_menu_col_pack },
	{ "menu_narrow", t_menu_narrow },
	{ "win_too_small", t_win_too_small },
	{ "help_scroll", t_help_scroll },
	{ "sig_quit_unwinds", t_sig_quit_unwinds },
#ifndef VEDIT_NO_TOOLS
	{ "tool_f9_make", t_tool_f9_make },
	{ "tool_ctrl_f9_run", t_tool_ctrl_f9_run },
	{ "menu_hide_tools", t_menu_hide_tools },
	{ "format", t_format },
	{ "format_on_save", t_format_on_save },
#endif
#ifdef VEDIT_TERM
	{ "menu_terminal", t_menu_terminal },
	{ "tbl_attach", t_tbl_attach },
	{ "tbl_grid", t_tbl_grid },
	{ "tbl_edit", t_tbl_edit },
	{ "tbl_loop", t_tbl_loop },
	{ "tbl_rows", t_tbl_rows },
	{ "sort", t_sort },
#endif
	{ NULL, NULL },
};
