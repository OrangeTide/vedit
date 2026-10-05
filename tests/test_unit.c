/*
 * Unit tests for vedit's internal helpers. The whole editor is included as a
 * single translation unit (with main renamed out of the way), so the static
 * functions are reachable directly.
 */
#include "test.h"
#include <utime.h>

#define main test_main
#include "../vedit.c"
#undef main

static void
t_utf8_roundtrip(Test *t)
{
	static const uint32_t cps[] = { 'A', 0x00e9, 0x4e00, 0x1f600 };
	size_t k;

	for (k = 0; k < sizeof(cps) / sizeof(cps[0]); k++) {
		unsigned char buf[4];
		uint32_t got = 0;
		int n = utf8_encode(buf, cps[k]);
		int m = utf8_decode(&got, buf, (size_t)n);

		TAP_CHECKF(t, n == m, "len mismatch for U+%04X: %d vs %d",
		    cps[k], n, m);
		TAP_CHECKF(t, got == cps[k], "U+%04X decoded as U+%04X",
		    cps[k], got);
	}
}

static void
t_rune_width(Test *t)
{
	TAP_CHECK(t, rune_width('A') == 1);
	TAP_CHECK(t, rune_width(0x4e00) == 2);		/* CJK: wide */
	TAP_CHECK(t, rune_width(0x0301) == 0);		/* combining acute */
}

static void
t_disp_cols(Test *t)
{
	TAP_CHECK(t, disp_cols("abc", 3) == 3);
	/* a tab from column 1 advances to the next multiple of 8 */
	TAP_CHECK(t, disp_cols("a\tb", 3) == 9);
	TAP_CHECK(t, disp_cols("", 0) == 0);
}

static void
t_color256_to_16(Test *t)
{
	int i;

	for (i = 0; i < 16; i++)
		TAP_CHECKF(t, color256_to_16(i) == i, "index %d not identity", i);
	TAP_CHECK(t, color256_to_16(16) == 0);		/* cube 0,0,0 -> black */
	TAP_CHECK(t, color256_to_16(231) == 15);	/* cube max -> white */
}

static void
t_rgb_to_ansi16(Test *t)
{
	TAP_CHECK(t, rgb_to_ansi16(0, 0, 0) == 0);
	TAP_CHECK(t, rgb_to_ansi16(255, 255, 255) == 15);
	TAP_CHECK(t, rgb_to_ansi16(255, 0, 0) == 9);	/* bright red */
}

static void
t_wrap_next(Test *t)
{
	const char *s = "hello world";		/* 11 bytes */
	size_t next = 0;
	int nextcol = -1;
	size_t end = wrap_next(s, 11, 0, 0, 8, &next, &nextcol);

	TAP_CHECKF(t, end == 5, "break at %zu, want 5", end);	/* "hello" */
	TAP_CHECKF(t, next == 6, "next at %zu, want 6", next);	/* past space */
	TAP_CHECK(t, nextcol == 6);
}

static void
t_line_rows(Test *t)
{
	TAP_CHECK(t, line_rows("", 0, 80) == 1);
	TAP_CHECK(t, line_rows("hi", 2, 80) == 1);
	TAP_CHECK(t, line_rows("hello world", 11, 8) == 2);
	/* a word longer than the window still makes progress, one row per chunk */
	TAP_CHECK(t, line_rows("aaaaaaaaaa", 10, 4) >= 3);
}

static void
t_seg_index_of(Test *t)
{
	const char *s = "hello world";

	TAP_CHECK(t, seg_index_of(s, 11, 0, 8) == 0);
	TAP_CHECK(t, seg_index_of(s, 11, 6, 8) == 1);	/* into "world" */
	TAP_CHECK(t, seg_index_of(s, 11, 11, 8) == 1);	/* end of line */
}

/* Class index by name for a grammar reached through its Syntax wrapper. */
static int
fsm_class(const Jsf *j, const char *name)
{
	int k;

	for (k = 0; k < j->nclasses; k++)
		if (strcmp(j->classname[k], name) == 0)
			return k;
	return -1;
}

static void
t_syntax_c(Test *t)
{
	const Syntax *sy = syn_for_ext("c");	/* the built-in default grammar */
	uint8_t out[32];
	int type, kw, com, pre;

	TAP_ASSERT(t, sy != NULL && sy->fsm != NULL);
	type = fsm_class(sy->fsm, "type");
	kw = fsm_class(sy->fsm, "keyword");
	com = fsm_class(sy->fsm, "comment");
	pre = fsm_class(sy->fsm, "preproc");
	TAP_ASSERT(t, type > 0 && kw > 0 && com > 0 && pre > 0);

	syn_line(sy, sy->start, "int", 3, out);
	TAP_CHECKF(t, out[0] == type, "int -> %d", out[0]);

	syn_line(sy, sy->start, "if", 2, out);
	TAP_CHECKF(t, out[0] == kw, "if -> %d", out[0]);

	syn_line(sy, sy->start, "// hi", 5, out);
	TAP_CHECKF(t, out[0] == com, "// -> %d", out[0]);

	syn_line(sy, sy->start, "#include", 8, out);
	TAP_CHECKF(t, out[0] == pre, "# -> %d", out[0]);

	/* .sh resolves to the shell grammar, an unknown extension to nothing */
	TAP_CHECK(t, syn_for_ext("sh") != NULL);
	TAP_CHECK(t, syn_for_ext("xyz") == NULL);
}

static void
t_syntax_block_comment_carry(Test *t)
{
	const Syntax *sy = syn_for_ext("c");
	uint8_t out[32];
	uint16_t st;
	int com;

	TAP_ASSERT(t, sy != NULL && sy->fsm != NULL);
	com = fsm_class(sy->fsm, "comment");

	/* an unterminated block comment carries its state to the next line */
	st = syn_line(sy, sy->start, "/* open", 7, out);
	TAP_CHECKF(t, out[0] == com, "open -> %d", out[0]);
	TAP_CHECKF(t, st != sy->start, "carry should be in-comment: %u", st);
	st = syn_line(sy, st, "still */ x", 10, out);
	TAP_CHECKF(t, out[0] == com, "cont -> %d", out[0]);	/* still comment */
	TAP_CHECK(t, st == sy->start);			/* closed on this line */
}

static void
t_syntax_refine(Test *t)
{
	const Syntax *c = syn_for_ext("c");
	const Syntax *sh = syn_for_ext("sh");
	uint8_t out[32];
	uint16_t st;
	int pre, str, var;

	TAP_ASSERT(t, c && c->fsm && sh && sh->fsm);
	pre = fsm_class(c->fsm, "preproc");
	str = fsm_class(c->fsm, "string");

	/* a string literal inside a preprocessor line is colored as a string */
	syn_line(c, c->start, "#include \"x.h\"", 14, out);
	TAP_CHECKF(t, out[0] == pre, "directive -> %d", out[0]);
	TAP_CHECKF(t, out[9] == str && out[13] == str, "inc target [%d %d]",
	    out[9], out[13]);

	/* a trailing backslash continues the preprocessor line onto the next */
	st = syn_line(c, c->start, "#define A \\", 11, out);
	TAP_CHECKF(t, st != c->start, "continuation carry: %u", st);
	syn_line(c, st, "cont", 4, out);
	TAP_CHECKF(t, out[0] == pre, "continued line -> %d", out[0]);

	/* a $var inside a double-quoted shell string is colored as a variable */
	str = fsm_class(sh->fsm, "string");
	var = fsm_class(sh->fsm, "variable");
	syn_line(sh, sh->start, "\"$HOME\"", 7, out);
	TAP_CHECKF(t, out[0] == str && out[6] == str, "quotes [%d %d]",
	    out[0], out[6]);
	TAP_CHECKF(t, out[1] == var && out[5] == var, "var [%d %d]",
	    out[1], out[5]);
}

static void
t_text_edit_undo(Test *t)
{
	Text *tx = text_new();
	size_t line = 0, col = 0, len = 0;
	const char *s;

	TAP_ASSERT(t, tx != NULL);

	text_insert(tx, 0, 0, "hello", 5);
	s = text_line(tx, 0, &len);
	TAP_CHECKF(t, len == 5 && memcmp(s, "hello", 5) == 0,
	    "insert gave len %zu", len);

	text_delete(tx, 0, 0, 1);		/* drop the 'h' */
	s = text_line(tx, 0, &len);
	TAP_CHECKF(t, len == 4 && memcmp(s, "ello", 4) == 0,
	    "delete gave len %zu", len);

	TAP_CHECK(t, text_undo(tx, &line, &col) == 0);	/* restore the 'h' */
	s = text_line(tx, 0, &len);
	TAP_CHECK(t, len == 5 && memcmp(s, "hello", 5) == 0);

	TAP_CHECK(t, text_redo(tx, &line, &col) == 0);	/* re-delete it */
	s = text_line(tx, 0, &len);
	TAP_CHECK(t, len == 4 && memcmp(s, "ello", 4) == 0);

	text_free(tx);
}

static void
t_multiline_buffer(Test *t)
{
	Text *tx = text_new();

	TAP_ASSERT(t, tx != NULL);
	text_insert(tx, 0, 0, "a", 1);
	lines_insert_at(tx, 1, "b", 1);
	lines_insert_at(tx, 2, "c", 1);
	TAP_CHECKF(t, text_lines(tx) == 3, "line count %zu", text_lines(tx));
	text_free(tx);
}

static void
t_pick_fit(Test *t)
{
	char buf[16];
	int w;

	w = pick_fit(buf, sizeof(buf), "hello", 10);
	TAP_CHECKF(t, w == 5 && strcmp(buf, "hello") == 0, "fit full: %s", buf);
	w = pick_fit(buf, sizeof(buf), "hello world", 5);
	TAP_CHECKF(t, w == 5 && strcmp(buf, "hello") == 0, "fit clip: %s", buf);
	w = pick_fit(buf, sizeof(buf), "abc", 0);
	TAP_CHECKF(t, w == 0 && buf[0] == '\0', "fit zero: '%s'", buf);
}

static void
t_filepick_cmp(Test *t)
{
	Fpent e[4] = {
		{ (char *)"zebra", 0 },
		{ (char *)"apple/", 1 },
		{ (char *)"beta", 0 },
		{ (char *)"alpha/", 1 },
	};

	qsort(e, 4, sizeof(e[0]), filepick_cmp);
	TAP_CHECKF(t, e[0].isdir && strcmp(e[0].name, "alpha/") == 0,
	    "first %s", e[0].name);
	TAP_CHECKF(t, e[1].isdir && strcmp(e[1].name, "apple/") == 0,
	    "second %s", e[1].name);
	TAP_CHECKF(t, !e[2].isdir && strcmp(e[2].name, "beta") == 0,
	    "third %s", e[2].name);
	TAP_CHECKF(t, !e[3].isdir && strcmp(e[3].name, "zebra") == 0,
	    "fourth %s", e[3].name);
}

/* A picker source backed by a fixed string array, for pick_jump. */
static const char *t_jump_rows[] = { "alpha", "beta", "banana", "gamma" };

static int
t_jump_count(void *ctx)
{
	(void)ctx;
	return 4;
}

static const char *
t_jump_label(void *ctx, int i)
{
	(void)ctx;
	return t_jump_rows[i];
}

static void
t_pick_jump(Test *t)
{
	Picksrc src = { .count = t_jump_count, .label = t_jump_label };
	Picker pk;

	memset(&pk, 0, sizeof(pk));
	pk.src = &src;
	pk.sel = 0;
	pick_jump(&pk, 4, 'b');			/* -> beta */
	TAP_CHECKF(t, pk.sel == 1, "first b at %d", pk.sel);
	pick_jump(&pk, 4, 'b');			/* -> banana (next b) */
	TAP_CHECKF(t, pk.sel == 2, "second b at %d", pk.sel);
	pick_jump(&pk, 4, 'g');			/* -> gamma */
	TAP_CHECKF(t, pk.sel == 3, "g at %d", pk.sel);
	pick_jump(&pk, 4, 'z');			/* no match, stay */
	TAP_CHECKF(t, pk.sel == 3, "no match stays %d", pk.sel);
}

static void
t_filepick_load(Test *t)
{
	char tmpl[] = "/tmp/vedit_fpXXXXXX";
	char *dir = mkdtemp(tmpl);
	char path[PATH_MAX];
	Filepick fp;
	int i, saw_sub = 0, saw_file = 0;

	TAP_ASSERT(t, dir != NULL);
	snprintf(path, sizeof(path), "%s/sub", dir);
	TAP_ASSERT(t, mkdir(path, 0700) == 0);
	snprintf(path, sizeof(path), "%s/zfile", dir);
	TAP_ASSERT(t, fclose(fopen(path, "w")) == 0);

	memset(&fp, 0, sizeof(fp));
	snprintf(fp.dir, sizeof(fp.dir), "%s", dir);
	filepick_load(&fp);
	/* .. and sub (dirs) sort before zfile */
	TAP_CHECKF(t, fp.n == 3, "entry count %d", fp.n);
	TAP_CHECK(t, fp.ent[fp.n - 1].isdir == 0);	/* file last */
	for (i = 0; i < fp.n; i++) {
		if (strcmp(fp.ent[i].name, "sub/") == 0 && fp.ent[i].isdir)
			saw_sub = 1;
		if (strcmp(fp.ent[i].name, "zfile") == 0 && !fp.ent[i].isdir)
			saw_file = 1;
	}
	TAP_CHECK(t, saw_sub && saw_file);
	filepick_clear(&fp);
	free(fp.ent);

	snprintf(path, sizeof(path), "%s/sub", dir);
	rmdir(path);
	snprintf(path, sizeof(path), "%s/zfile", dir);
	unlink(path);
	rmdir(dir);
}

static void
t_filepick_start_dir(Test *t)
{
	char tmpl[] = "/tmp/vedit_sdXXXXXX";
	char *dir = mkdtemp(tmpl);
	char sub[PATH_MAX], file[PATH_MAX], real[PATH_MAX], out[PATH_MAX];

	TAP_ASSERT(t, dir != NULL);
	TAP_ASSERT(t, realpath(dir, real) != NULL);	/* canonical form */
	snprintf(sub, sizeof(sub), "%s/sub", dir);
	TAP_ASSERT(t, mkdir(sub, 0700) == 0);
	snprintf(file, sizeof(file), "%s/afile", dir);
	TAP_ASSERT(t, fclose(fopen(file, "w")) == 0);

	/* a directory hint resolves to itself */
	filepick_start_dir(dir, out, sizeof(out));
	TAP_CHECKF(t, strcmp(out, real) == 0, "dir hint -> %s", out);

	/* a file-path hint resolves to its containing directory */
	filepick_start_dir(file, out, sizeof(out));
	TAP_CHECKF(t, strcmp(out, real) == 0, "file hint -> %s", out);

	/* a missing file under a real dir still resolves to that dir */
	snprintf(file, sizeof(file), "%s/nope.txt", dir);
	filepick_start_dir(file, out, sizeof(out));
	TAP_CHECKF(t, strcmp(out, real) == 0, "new-file hint -> %s", out);

	/* NULL falls back to the current directory */
	filepick_start_dir(NULL, out, sizeof(out));
	TAP_CHECK(t, realpath(".", real) && strcmp(out, real) == 0);

	rmdir(sub);
	snprintf(file, sizeof(file), "%s/afile", dir);
	unlink(file);
	rmdir(dir);
}

/* Write text to a temp file and parse it; caller frees *out and unlinks path. */
static Cfg *
load_cfg_text(const char *text, char *path, size_t pathsz)
{
	int fd;
	FILE *f;
	Cfg *c;

	snprintf(path, pathsz, "/tmp/vedit_cfgXXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		return NULL;
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		return NULL;
	}
	fputs(text, f);
	fclose(f);
	c = vedit_cfg_new();
	if (c && vedit_cfg_load(c, path) != 0) {
		vedit_cfg_free(c);
		c = NULL;
	}
	return c;
}

static void
t_cfg_parse(Test *t)
{
	static const char *text =
	    "# a comment\n"
	    "\n"			/* a blank line must not fail the parse */
	    "ui.scheme = dos\n"		/* shorthand, overwritten below */
	    "edit.mode = vi\n"
	    "\n"
	    "[ui]\n"
	    "  scheme = black ; trailing comment\n"
	    "  wrap = on\n"
	    "\t\n"			/* a whitespace-only line is also blank */
	    "[theme \"midnight\"]\n"
	    "  content.fg = 250\n"
	    "  title.fg = \"#ff8000\"\n";	/* quoted: '#' is not a comment */
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const char *v;

	TAP_ASSERT(t, c != NULL);
	v = cfg_get(c, "edit.mode");
	TAP_CHECKF(t, v && strcmp(v, "vi") == 0, "edit.mode=%s", v ? v : "(nil)");
	v = cfg_get(c, "ui.scheme");			/* section wins, overwrite */
	TAP_CHECKF(t, v && strcmp(v, "black") == 0, "ui.scheme=%s",
	    v ? v : "(nil)");
	v = cfg_get(c, "ui.wrap");
	TAP_CHECKF(t, v && strcmp(v, "on") == 0, "ui.wrap=%s", v ? v : "(nil)");
	v = cfg_get(c, "theme.midnight.content.fg");
	TAP_CHECKF(t, v && strcmp(v, "250") == 0, "theme fg=%s",
	    v ? v : "(nil)");
	v = cfg_get(c, "theme.midnight.title.fg");	/* quotes stripped, # kept */
	TAP_CHECKF(t, v && strcmp(v, "#ff8000") == 0, "quoted hex=%s",
	    v ? v : "(nil)");
	TAP_CHECK(t, cfg_get(c, "comment") == NULL);	/* comment not a key */
	TAP_CHECK(t, cfg_bool(c, "ui.wrap", 0) == 1);
	TAP_CHECK(t, cfg_bool(c, "missing", 1) == 1);	/* default when unset */

	vedit_cfg_free(c);
	unlink(path);
}

static void
t_cfg_resolve(Test *t)
{
	static const char *text =
	    "[ui]\n"
	    "box = dec\n"
	    "colors = 256\n"
	    "scroll = on\n"
	    "scheme = black\n"
	    "wrap = on\n"
	    "number = on\n"
	    "[edit]\n"
	    "mode = vi\n"
	    "[syntax]\n"
	    "enable = off\n";
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;
	int sb = g_box_force, sc = g_colors_force, ss = g_scroll_force;
	Editor e;

	TAP_ASSERT(t, c != NULL);
	unsetenv("VEDIT_BOX");
	unsetenv("VEDIT_ASCII");
	unsetenv("VEDIT_COLORS");
	unsetenv("VEDIT_SCROLL");
	g_box_force = g_colors_force = g_scroll_force = -1;
	g_cfg = c;

	/* config beats auto-detect for the startup knobs */
	TAP_CHECK(t, box_default() == VEDIT_BOX_DEC);
	TAP_CHECK(t, color_default() == 256);
	TAP_CHECK(t, scroll_default() == 1);
	/* a flag still beats config */
	g_box_force = VEDIT_BOX_ASCII;
	TAP_CHECK(t, box_default() == VEDIT_BOX_ASCII);

	/* editor-level toggles */
	editor_init(&e);
	ed_apply_config(&e);
	TAP_CHECK(t, e.scheme == SCHEME_BLACK);
	TAP_CHECK(t, e.wrap == 1);
	TAP_CHECK(t, e.show_lineno == 1);
	TAP_CHECK(t, e.mode == MODE_NORMAL);
	TAP_CHECK(t, e.hl_on == 0);

	g_cfg = old;
	g_box_force = sb;
	g_colors_force = sc;
	g_scroll_force = ss;
	vedit_cfg_free(c);
	unlink(path);
}

static void
t_cfg_color(Test *t)
{
	Color c;

	TAP_CHECK(t, cfg_color("default", &c) && c.type == COLOR_DEFAULT);
	TAP_CHECK(t, cfg_color("250", &c) && c.type == COLOR_INDEXED &&
	    c.index == 250);
	TAP_CHECK(t, cfg_color("#ff8000", &c) && c.type == COLOR_RGB &&
	    c.rgb.r == 0xff && c.rgb.g == 0x80 && c.rgb.b == 0);
	TAP_CHECK(t, cfg_color("blue", &c) && c.type == COLOR_INDEXED &&
	    c.index == 4);
	TAP_CHECK(t, cfg_color("bright-red", &c) && c.type == COLOR_INDEXED &&
	    c.index == 9);
	TAP_CHECK(t, !cfg_color("nope", &c));
	TAP_CHECK(t, !cfg_color("300", &c));
}

static void
t_cfg_theme(Test *t)
{
	static const char *text =
	    "[ui]\n"
	    "scheme = midnight\n"
	    "[theme \"midnight\"]\n"
	    "base = black\n"		/* borderless preset... */
	    "content.fg = 250\n"
	    "bar.bg = 244\n"
	    "borderless = off\n";	/* ...overridden back on */
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;
	const Pal *p;
	Editor e;
	int ti;

	TAP_ASSERT(t, c != NULL);
	g_cfg = c;
	themes_load_cfg(c);
	ti = theme_by_name("midnight");
	TAP_ASSERT(t, ti >= 0);

	editor_init(&e);
	ed_apply_config(&e);
	TAP_CHECK(t, e.scheme == SCHEME_COUNT + ti);	/* ui.scheme resolves */
	p = ed_chrome(&e);
	TAP_CHECK(t, p->content_fg.type == COLOR_INDEXED &&
	    p->content_fg.index == 250);
	TAP_CHECK(t, p->bar_bg.type == COLOR_INDEXED && p->bar_bg.index == 244);
	TAP_CHECK(t, p->content_bg.type == COLOR_DEFAULT);	/* from black base */
	TAP_CHECK(t, chrome_right(&e) == CHROME_RIGHT);	/* borderless=off wins */

	g_cfg = old;
	themes_load_cfg(NULL);			/* clear for other tests */
	vedit_cfg_free(c);
	unlink(path);
}

static void
t_jsf_charset(Test *t)
{
	uint8_t set[32];

	TAP_CHECK(t, jsf_charset("*", set) && (set[0] == 0xff));
	TAP_ASSERT(t, jsf_charset("\"a-c\"", set));
	TAP_CHECK(t, (set['a' >> 3] & (1 << ('a' & 7))));
	TAP_CHECK(t, (set['c' >> 3] & (1 << ('c' & 7))));
	TAP_CHECK(t, !(set['d' >> 3] & (1 << ('d' & 7))));
	TAP_ASSERT(t, jsf_charset("\"0-9\\t\"", set));
	TAP_CHECK(t, (set['5' >> 3] & (1 << ('5' & 7))));
	TAP_CHECK(t, (set['\t' >> 3] & (1 << ('\t' & 7))));
}

static int
jsf_class_of(int lang, const char *name)
{
	int k;

	for (k = 0; k < g_user.lang[lang].nclasses; k++)
		if (strcmp(g_user.lang[lang].classname[k], name) == 0)
			return k;
	return -1;
}

static void
t_jsf_highlight(Test *t)
{
	static const char *text =
	    "[language \"mini\"]\n"
	    "[color \"mini\"]\n"
	    "  kw  = yellow\n"
	    "  num = cyan\n"
	    "[words \"mini.w\"]\n"
	    "  list = if while\n"
	    "[state \"mini.idle\"]\n"
	    "  color = text\n"
	    "  rule = \"0-9\" num recolor\n"
	    "  rule = \"a-z\" word buffer\n"
	    "  rule = * idle\n"
	    "[state \"mini.num\"]\n"
	    "  color = num\n"
	    "  rule = \"0-9\" num\n"
	    "  rule = * idle noeat\n"
	    "[state \"mini.word\"]\n"
	    "  color = text\n"
	    "  rule = \"a-z\" word\n"
	    "  rule = * idle noeat kw=mini.w:kw\n";
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;
	const Syntax *sy;
	const char *line = "if 42x";
	uint8_t out[16];
	int lang, kw, num, txt;

	TAP_ASSERT(t, c != NULL);
	g_cfg = c;
	syntax_load_cfg(&g_user, c);
	lang = jsf_find(&g_user, "mini");
	TAP_ASSERT(t, lang >= 0);
	kw = jsf_class_of(lang, "kw");
	num = jsf_class_of(lang, "num");
	txt = jsf_class_of(lang, "text");
	TAP_ASSERT(t, kw > 0 && num > 0 && txt >= 0);

	sy = syn_for_ext("mini");		/* resolves to the FSM language */
	TAP_ASSERT(t, sy && sy->fsm);
	memset(out, 0xee, sizeof(out));
	syn_line(sy, sy->start, line, strlen(line), out);

	/* "if 42x" -> keyword, keyword, text, num, num, text */
	TAP_CHECKF(t, out[0] == kw && out[1] == kw, "kw [%d %d]", out[0], out[1]);
	TAP_CHECK(t, out[2] == txt);
	TAP_CHECKF(t, out[3] == num && out[4] == num, "num [%d %d]",
	    out[3], out[4]);
	TAP_CHECK(t, out[5] == txt);

	g_cfg = old;
	syntax_load_cfg(&g_user, NULL);			/* clear registry for other tests */
	vedit_cfg_free(c);
	unlink(path);
}

static void
t_jsf_linecomment(Test *t)
{
	static const char *text =
	    "[color \"lc\"]\n"
	    "  kw  = yellow\n"
	    "  com = green\n"
	    "[words \"lc.w\"]\n"
	    "  list = if\n"
	    "[state \"lc.idle\"]\n"
	    "  color = text\n"
	    "  rule = \"/\" slash\n"
	    "  rule = \"a-z\" word buffer\n"
	    "  rule = * idle\n"
	    "[state \"lc.slash\"]\n"
	    "  color = text\n"
	    "  rule = \"/\" com recolor=2\n"
	    "  rule = * idle noeat\n"
	    "[state \"lc.com\"]\n"
	    "  color = com\n"
	    "  rule = \"\\n\" idle\n"
	    "  rule = * com\n"
	    "[state \"lc.word\"]\n"
	    "  color = text\n"
	    "  rule = \"a-z\" word\n"
	    "  rule = * idle noeat kw=lc.w:kw\n";
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;
	const Syntax *sy;
	uint8_t out[32];
	uint16_t carry;
	int lang, kw, com;

	TAP_ASSERT(t, c != NULL);
	g_cfg = c;
	syntax_load_cfg(&g_user, c);
	lang = jsf_find(&g_user, "lc");
	TAP_ASSERT(t, lang >= 0);
	kw = jsf_class_of(lang, "kw");
	com = jsf_class_of(lang, "com");
	sy = syn_for_ext("lc");
	TAP_ASSERT(t, sy && sy->fsm);

	/* line 1 ends in a line comment; the virtual newline must return the
	 * carry state to idle so the comment does not leak onto line 2 */
	memset(out, 0, sizeof(out));
	carry = syn_line(sy, sy->start, "a // x", 6, out);
	TAP_CHECKF(t, out[3] == com && out[5] == com, "comment [%d %d]",
	    out[3], out[5]);
	TAP_CHECKF(t, carry == sy->start, "carry leaked: %u", carry);

	/* line 2: a bare "if" at end of line matches the keyword via the
	 * end-of-line token flush */
	memset(out, 0, sizeof(out));
	syn_line(sy, carry, "if", 2, out);
	TAP_CHECKF(t, out[0] == kw && out[1] == kw, "eol kw [%d %d]",
	    out[0], out[1]);

	g_cfg = old;
	syntax_load_cfg(&g_user, NULL);
	vedit_cfg_free(c);
	unlink(path);
}

static void
t_jsf_recolormark(Test *t)
{
	/* A "<...>" region whose length is unknown until ">" is seen: mark at
	 * "<", then recolormark repaints the whole span once the end matches. */
	static const char *text =
	    "[color \"rm\"]\n"
	    "  reg = magenta\n"
	    "[state \"rm.idle\"]\n"
	    "  color = text\n"
	    "  rule = \"<\" open mark\n"
	    "  rule = * idle\n"
	    "[state \"rm.open\"]\n"
	    "  color = text\n"
	    "  rule = \">\" close recolormark\n"
	    "  rule = * open\n"
	    "[state \"rm.close\"]\n"
	    "  color = reg\n"
	    "  rule = * idle noeat\n";
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;
	const Syntax *sy;
	const char *line = "a<bcd>e";
	uint8_t out[16];
	int lang, reg, txt;

	TAP_ASSERT(t, c != NULL);
	g_cfg = c;
	syntax_load_cfg(&g_user, c);
	lang = jsf_find(&g_user, "rm");
	TAP_ASSERT(t, lang >= 0);
	reg = jsf_class_of(lang, "reg");
	txt = jsf_class_of(lang, "text");
	TAP_ASSERT(t, reg > 0 && txt >= 0);
	sy = syn_for_ext("rm");
	TAP_ASSERT(t, sy && sy->fsm);

	memset(out, 0xee, sizeof(out));
	syn_line(sy, sy->start, line, strlen(line), out);

	/* "a<bcd>e": the "<bcd" span is repainted reg, ">" and the rest are not */
	TAP_CHECKF(t, out[0] == txt, "pre [%d]", out[0]);
	TAP_CHECKF(t, out[1] == reg && out[2] == reg && out[3] == reg &&
	    out[4] == reg, "region [%d %d %d %d]",
	    out[1], out[2], out[3], out[4]);
	TAP_CHECKF(t, out[5] == txt && out[6] == txt, "post [%d %d]",
	    out[5], out[6]);

	g_cfg = old;
	syntax_load_cfg(&g_user, NULL);
	vedit_cfg_free(c);
	unlink(path);
}

static void
t_jsf_include(Test *t)
{
	/* State "sa" has no rules of its own; it includes "shared", so it
	 * borrows shared's transitions while keeping its own color. */
	static const char *text =
	    "[language \"inc\"]\n"
	    "  start = sa\n"
	    "[color \"inc\"]\n"
	    "  a = red\n"
	    "  b = green\n"
	    "[state \"inc.sa\"]\n"
	    "  color = a\n"
	    "  include = shared\n"
	    "[state \"inc.sb\"]\n"
	    "  color = b\n"
	    "  rule = \"x\" sa\n"
	    "[state \"inc.shared\"]\n"
	    "  color = text\n"
	    "  rule = \"x\" sb\n"
	    "  rule = * shared\n";
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;
	const Syntax *sy;
	uint8_t out[16];
	int lang, ca, cb;

	TAP_ASSERT(t, c != NULL);
	g_cfg = c;
	syntax_load_cfg(&g_user, c);
	lang = jsf_find(&g_user, "inc");
	TAP_ASSERT(t, lang >= 0);
	ca = jsf_class_of(lang, "a");
	cb = jsf_class_of(lang, "b");
	TAP_ASSERT(t, ca > 0 && cb > 0);
	sy = syn_for_ext("inc");
	TAP_ASSERT(t, sy && sy->fsm);

	memset(out, 0xee, sizeof(out));
	syn_line(sy, sy->start, "xy", 2, out);

	/* "x" matches shared's borrowed rule but is colored with sa's own
	 * class, then transitions to sb; "y" falls through sb as sb's color */
	TAP_CHECKF(t, out[0] == ca, "own color on borrowed rule [%d]", out[0]);
	TAP_CHECKF(t, out[1] == cb, "transitioned via borrowed rule [%d]",
	    out[1]);

	g_cfg = old;
	syntax_load_cfg(&g_user, NULL);
	vedit_cfg_free(c);
	unlink(path);
}

static void
t_replace(Test *t)
{
	Editor e;
	size_t y, x, my, mx, mlen, count, len;
	const char *s;
	rx_t *re;

	editor_init(&e);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);

	/* two lines, three occurrences of "foo" */
	text_insert(e.t, 0, 0, "foo foo", 7);
	lines_insert_at(e.t, 1, "x foo y", 7);

	/* replace-all driven by the same primitives the interactive loop uses */
	re = rx_compile("foo", 0, NULL);
	TAP_ASSERT(t, re != NULL);
	y = x = count = 0;
	while (replace_next(&e, re, y, x, &my, &mx, &mlen)) {
		replace_at(&e, my, mx, mlen, "BARS", 4);
		count++;
		y = my;
		x = mx + 4;
	}
	rx_free(re);
	TAP_CHECKF(t, count == 3, "count %zu", count);
	s = text_line(e.t, 0, &len);
	TAP_CHECKF(t, len == 9 && memcmp(s, "BARS BARS", 9) == 0,
	    "grow line 0: '%.*s'", (int)len, s);
	s = text_line(e.t, 1, &len);
	TAP_CHECKF(t, len == 8 && memcmp(s, "x BARS y", 8) == 0,
	    "grow line 1: '%.*s'", (int)len, s);

	/* an empty replacement deletes the match */
	re = rx_compile("BARS", 0, NULL);
	TAP_ASSERT(t, re != NULL);
	TAP_ASSERT(t, replace_next(&e, re, 1, 0, &my, &mx, &mlen));
	replace_at(&e, my, mx, mlen, "", 0);
	rx_free(re);
	s = text_line(e.t, 1, &len);
	TAP_CHECKF(t, len == 4 && memcmp(s, "x  y", 4) == 0,
	    "delete: '%.*s'", (int)len, s);

	/* no match past the end returns 0 */
	re = rx_compile("zzz", 0, NULL);
	TAP_ASSERT(t, re != NULL);
	TAP_CHECK(t, !replace_next(&e, re, 0, 0, &my, &mx, &mlen));
	rx_free(re);

	text_free(e.t);
}

/* The vendored rx engine, as the editor drives it: a regex match with a
 * variable-width span, and a replacement template with a backreference. */
static void
t_regex_engine(Test *t)
{
	rx_t *re;
	const char *s = "size_t n = 0;";
	rx_match m[3];
	char *out;

	re = rx_compile("([a-z_]+)_t", 0, NULL);
	TAP_ASSERT(t, re != NULL);
	TAP_CHECKF(t, rx_exec(re, s, strlen(s), 0, m, 3) == 1 &&
	    m[0].so == 0 && m[0].eo == 6, "match span %ld..%ld",
	    m[0].so, m[0].eo);
	TAP_CHECKF(t, m[1].so == 0 && m[1].eo == 4, "group1 %ld..%ld",
	    m[1].so, m[1].eo);
	rx_free(re);

	/* backreference and the whole-match & in a replacement template */
	re = rx_compile("(\\w+)@(\\w+)", 0, NULL);
	TAP_ASSERT(t, re != NULL);
	out = rx_replace(re, "user@host", 9, "\\2.\\1", 0);
	TAP_ASSERT(t, out != NULL);
	TAP_CHECKF(t, strcmp(out, "host.user") == 0, "swap: '%s'", out);
	free(out);
	rx_free(re);
}

/* Base64 encoder used for the OSC 52 terminal clipboard, against the RFC 4648
 * test vectors (padding and all). */
static void
t_base64(Test *t)
{
	static const struct { const char *in; const char *out; } v[] = {
		{ "", "" },
		{ "f", "Zg==" },
		{ "fo", "Zm8=" },
		{ "foo", "Zm9v" },
		{ "foob", "Zm9vYg==" },
		{ "fooba", "Zm9vYmE=" },
		{ "foobar", "Zm9vYmFy" },
	};
	size_t k;

	for (k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
		char buf[16];
		size_t n = b64_encode((const unsigned char *)v[k].in,
		    strlen(v[k].in), buf);

		TAP_CHECKF(t, n == strlen(v[k].out) &&
		    strcmp(buf, v[k].out) == 0, "b64('%s') = '%s' (want '%s')",
		    v[k].in, buf, v[k].out);
	}
}

/* Write raw bytes to a fresh temp file, returning its path in `path`. */
static int
write_tmp(const char *bytes, size_t n, char *path, size_t pathsz)
{
	int fd;
	FILE *f;

	snprintf(path, pathsz, "/tmp/vedit_eolXXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		return -1;
	}
	if (n)
		fwrite(bytes, 1, n, f);
	fclose(f);
	return 0;
}

/* Read a whole file into buf; returns the byte count, or -1. */
static long
read_file(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, cap, f);
	fclose(f);
	return (long)n;
}

/* Line-ending detection on load and emission on save, for LF, CRLF, and NUL. */
static void
t_eol(Test *t)
{
	char path[64], buf[32];
	Text *tx;
	size_t l0 = 0, l1 = 0;
	const char *s0, *s1;

	/* CRLF is detected and the trailing CR is stripped from each line */
	TAP_ASSERT(t, write_tmp("a\r\nbb\r\n", 7, path, sizeof(path)) == 0);
	tx = text_new();
	TAP_ASSERT(t, tx != NULL && text_load(tx, path) == OK);
	TAP_CHECKF(t, text_eol(tx) == EOL_CRLF, "eol %d", text_eol(tx));
	TAP_CHECKF(t, text_lines(tx) == 2, "lines %zu", text_lines(tx));
	s0 = text_line(tx, 0, &l0);
	s1 = text_line(tx, 1, &l1);
	TAP_CHECKF(t, l0 == 1 && s0[0] == 'a', "line0 len %zu", l0);
	TAP_CHECKF(t, l1 == 2 && s1[0] == 'b', "line1 len %zu", l1);
	TAP_CHECK(t, text_final_newline(tx));
	remove(path);
	text_free(tx);

	/* a NUL byte marks NUL-separated records */
	TAP_ASSERT(t, write_tmp("x\0yz\0", 5, path, sizeof(path)) == 0);
	tx = text_new();
	TAP_ASSERT(t, tx != NULL && text_load(tx, path) == OK);
	TAP_CHECKF(t, text_eol(tx) == EOL_NUL, "eol %d", text_eol(tx));
	TAP_CHECKF(t, text_lines(tx) == 2, "lines %zu", text_lines(tx));
	remove(path);
	text_free(tx);

	/* plain LF, no trailing terminator */
	TAP_ASSERT(t, write_tmp("one\ntwo", 7, path, sizeof(path)) == 0);
	tx = text_new();
	TAP_ASSERT(t, tx != NULL && text_load(tx, path) == OK);
	TAP_CHECK(t, text_eol(tx) == EOL_LF);
	TAP_CHECKF(t, text_lines(tx) == 2, "lines %zu", text_lines(tx));
	TAP_CHECK(t, !text_final_newline(tx));

	/* set CRLF and save: the content keeps no trailing terminator */
	text_set_eol(tx, EOL_CRLF);
	TAP_CHECK(t, text_dirty(tx));		/* the change dirtied the buffer */
	TAP_ASSERT(t, text_save(tx, path) == OK);
	TAP_CHECKF(t, read_file(path, buf, sizeof(buf)) == 8 &&
	    memcmp(buf, "one\r\ntwo", 8) == 0, "crlf save");

	/* set NUL and save */
	text_set_eol(tx, EOL_NUL);
	TAP_ASSERT(t, text_save(tx, path) == OK);
	TAP_CHECKF(t, read_file(path, buf, sizeof(buf)) == 7 &&
	    memcmp(buf, "one\0two", 7) == 0, "nul save");

	remove(path);
	text_free(tx);
}

/* Compare line y of e->t against a NUL-terminated expected string. */
static int
line_is(Editor *e, size_t y, const char *want)
{
	size_t len = 0;
	const char *s = text_line(e->t, y, &len);

	return s && len == strlen(want) && memcmp(s, want, len) == 0;
}

/* The Tab key and auto-indent: literal tabs vs spaces, and copying indent. */
static void
t_indent(Test *t)
{
	Editor e;

	editor_init(&e);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);

	/* Tab with expandtab off inserts a literal tab */
	e.expand_tabs = 0;
	e.cy = e.cx = 0;
	ed_indent_tab(&e);
	TAP_CHECK(t, line_is(&e, 0, "\t"));

	/* Tab with expandtab on fills spaces to the next 8-column stop. The line
	 * already holds one tab (8 columns), so this adds a full 8 spaces. */
	e.expand_tabs = 1;
	ed_indent_tab(&e);
	TAP_CHECK(t, line_is(&e, 0, "\t        "));

	/* a tab three columns in adds five spaces to reach column 8 */
	text_free(e.t);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "abc", 3);
	e.cy = 0;
	e.cx = 3;
	e.expand_tabs = 1;
	ed_indent_tab(&e);
	TAP_CHECK(t, line_is(&e, 0, "abc     "));	/* abc + 5 spaces */

	/* auto-indent copies the leading whitespace onto the new line */
	text_free(e.t);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "\t\tcode", 6);
	e.auto_indent = 1;
	e.cy = 0;
	e.cx = text_line_len(e.t, 0);		/* end of line */
	ed_newline_indent(&e);
	TAP_CHECK(t, text_lines(e.t) == 2);
	TAP_CHECK(t, line_is(&e, 1, "\t\t"));
	TAP_CHECKF(t, e.cy == 1 && e.cx == 2, "cursor %zu,%zu", e.cy, e.cx);

	/* with auto-indent off the new line starts empty */
	text_free(e.t);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "\t\tcode", 6);
	e.auto_indent = 0;
	e.cy = 0;
	e.cx = text_line_len(e.t, 0);
	ed_newline_indent(&e);
	TAP_CHECK(t, line_is(&e, 1, ""));

	text_free(e.t);
}

/* Convert tabs to spaces and indentation back to tabs. */
static void
t_retab(Test *t)
{
	Editor e;

	editor_init(&e);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);

	/* a leading tab expands to eight spaces; a tab after content fills to the
	 * next stop (here "ab" + a tab -> "ab" + six spaces) */
	text_insert(e.t, 0, 0, "\tab\tc", 5);
	TAP_CHECK(t, ed_retab_range(&e, 0, 0, 1) == 1);
	TAP_CHECK(t, line_is(&e, 0, "        ab      c"));	/* 8 + ab + 6 + c */

	/* convert indentation back to tabs: eight leading spaces -> one tab, and
	 * spaces inside the line are left alone */
	text_free(e.t);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "        ab c", 12);
	TAP_CHECK(t, ed_retab_range(&e, 0, 0, 0) == 1);
	TAP_CHECK(t, line_is(&e, 0, "\tab c"));

	/* ten leading spaces -> one tab plus two spaces (8 + 2) */
	text_free(e.t);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "          x", 11);
	TAP_CHECK(t, ed_retab_range(&e, 0, 0, 0) == 1);
	TAP_CHECK(t, line_is(&e, 0, "\t  x"));

	text_free(e.t);
}

/* Parse an Exuberant/Universal ctags "tags" file: pseudo-tags skipped, a
 * pattern address unescaped and de-anchored, a numeric address, and kinds. */
static void
t_tags(Test *t)
{
	char dir[] = "/tmp/vedit_tagsXXXXXX";
	char tagspath[PATH_MAX], target[PATH_MAX];
	Tagdb db;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(tagspath, sizeof(tagspath), "%s/tags", dir);
	f = fopen(tagspath, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("!_TAG_FILE_FORMAT\t2\t//\n", f);		/* pseudo-tag: skipped */
	fputs("sock_open\tnet/sock.c\t/^int sock_open(void)$/;\"\tf\n", f);
	fputs("MAX\tnet/sock.c\t12;\"\td\n", f);		/* numeric address */
	fputs("Conn\tnet/sock.c\t/^struct Conn {/;\"\tkind:s\n", f);
	fclose(f);

	memset(&db, 0, sizeof(db));
	TAP_ASSERT(t, tags_load(&db, tagspath) == 0);
	TAP_CHECKF(t, db.n == 3, "entries %d", db.n);
	if (db.n >= 3) {
		TAP_CHECK(t, strcmp(db.ent[0].name, "sock_open") == 0 &&
		    db.ent[0].kind == 'f' && db.ent[0].line == 0 &&
		    strcmp(db.ent[0].pattern, "int sock_open(void)") == 0);
		TAP_CHECK(t, strcmp(db.ent[1].name, "MAX") == 0 &&
		    db.ent[1].kind == 'd' && db.ent[1].line == 12);
		TAP_CHECK(t, strcmp(db.ent[2].name, "Conn") == 0 &&
		    db.ent[2].kind == 's' &&
		    strcmp(db.ent[2].pattern, "struct Conn {") == 0);
	}
	/* a relative tagfile resolves against the tags file's directory */
	tag_resolve(&db, db.ent[0].file_idx, target, sizeof(target));
	TAP_CHECKF(t, strncmp(target, dir, strlen(dir)) == 0 &&
	    strcmp(target + strlen(target) - 10, "net/sock.c") == 0,
	    "resolved '%s'", target);

	tagdb_free(&db);
	unlink(tagspath);
	rmdir(dir);
}

/* ex command names resolve by the vi abbreviation rule: any prefix of the full
 * name at least `min` long, with the standard minimums not colliding. */
static void
t_ex_abbrev(Test *t)
{
	static const struct { const char *w; int id; } v[] = {
		{ "s", EX_SUBST }, { "su", EX_SUBST }, { "substitute", EX_SUBST },
		{ "se", EX_SET }, { "set", EX_SET },
		{ "sy", EX_SYNTAX }, { "syntax", EX_SYNTAX },
		{ "e", EX_EDIT }, { "ed", EX_EDIT }, { "edit", EX_EDIT },
		{ "en", EX_NONE }, { "ene", EX_ENEW }, { "enew", EX_ENEW },
		{ "w", EX_WRITE }, { "write", EX_WRITE },
		{ "wq", EX_WQ }, { "wqa", EX_WQALL }, { "wqall", EX_WQALL },
		{ "x", EX_XIT }, { "ex", EX_XIT }, { "exit", EX_XIT },
		{ "xa", EX_WQALL }, { "xall", EX_WQALL },
		{ "q", EX_QUIT }, { "quit", EX_QUIT },
		{ "qa", EX_QALL }, { "qall", EX_QALL }, { "quitall", EX_QALL },
		{ "cq", EX_CQUIT }, { "cquit", EX_CQUIT },
		{ "d", EX_DELETE }, { "delete", EX_DELETE },
		{ "y", EX_YANK }, { "g", EX_GLOBAL }, { "v", EX_VGLOBAL },
		{ "r", EX_READ }, { "re", EX_READ }, { "read", EX_READ },
		{ "ret", EX_RETAB }, { "retab", EX_RETAB },
		{ "b", EX_BUFFER }, { "bu", EX_BUFFER }, { "buffer", EX_BUFFER },
		{ "buffers", EX_LS }, { "ls", EX_LS }, { "files", EX_LS },
		{ "bn", EX_BNEXT }, { "bp", EX_BPREV }, { "bN", EX_BPREV },
		{ "bd", EX_BDELETE },
		{ "ta", EX_TAG }, { "tag", EX_TAG },
		{ "po", EX_POP }, { "pop", EX_POP },
		{ "dr", EX_DRAW }, { "draw", EX_DRAW },
		{ "zzz", EX_NONE }, { "", EX_NONE },
	};
	size_t k;

	for (k = 0; k < sizeof(v) / sizeof(v[0]); k++)
		TAP_CHECKF(t, ex_lookup(v[k].w) == v[k].id,
		    "lookup '%s' = %d (want %d)", v[k].w, ex_lookup(v[k].w),
		    v[k].id);
}

/* A substitute works spelled short (:s) or in full (:substitute), proving the
 * delimiter is read from after the command word, not a fixed offset. */
static void
t_ex_subst(Test *t)
{
	Editor e;
	char cmd[64];

	editor_init(&e);
	e.rows = 24;
	e.cols = 80;
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "foo foo", 7);
	e.cy = e.cx = 0;

	snprintf(cmd, sizeof(cmd), "s/foo/bar/");
	vi_ex_exec(&e, cmd);
	TAP_CHECK(t, line_is(&e, 0, "bar foo"));

	snprintf(cmd, sizeof(cmd), "%%substitute/foo/baz/g");
	vi_ex_exec(&e, cmd);
	TAP_CHECK(t, line_is(&e, 0, "bar baz"));

	text_free(e.t);
}

/* The #include target parser: "name" / <name>, spaces, and non-matches. */
static void
t_include_target(Test *t)
{
	char out[PATH_MAX];
	int angle;

	TAP_CHECK(t, include_target("#include \"foo.h\"", 16, out,
	    sizeof(out), &angle) == 5 && angle == 0 &&
	    strcmp(out, "foo.h") == 0);
	TAP_CHECK(t, include_target("#include <a/b.h>", 16, out,
	    sizeof(out), &angle) == 5 && angle == 1 &&
	    strcmp(out, "a/b.h") == 0);
	TAP_CHECK(t, include_target("   #  include   \"x.h\"", 21, out,
	    sizeof(out), &angle) == 3 && strcmp(out, "x.h") == 0);
	TAP_CHECK(t, include_target("int x;", 6, out, sizeof(out),
	    &angle) == 0);
	TAP_CHECK(t, include_target("#include foo", 12, out, sizeof(out),
	    &angle) == 0);
	TAP_CHECK(t, include_target("#include \"bad", 13, out, sizeof(out),
	    &angle) == 0);
}

/* The shell-style splitter for a "command" string: quotes and escapes. */
static void
t_cc_split(Test *t)
{
	char cmd[] = "cc -I /a \"b c\" -DX=\\\"y\\\" foo.c";
	int argc = 0;
	char **argv = cc_split(cmd, &argc);

	TAP_ASSERT(t, argv != NULL);
	TAP_CHECKF(t, argc == 6, "argc %d", argc);
	TAP_CHECK(t, strcmp(argv[0], "cc") == 0);
	TAP_CHECK(t, strcmp(argv[1], "-I") == 0);
	TAP_CHECK(t, strcmp(argv[2], "/a") == 0);
	TAP_CHECK(t, strcmp(argv[3], "b c") == 0);
	TAP_CHECK(t, strcmp(argv[4], "-DX=\"y\"") == 0);
	TAP_CHECK(t, strcmp(argv[5], "foo.c") == 0);
	free(argv);
}

/* End to end: cc_resolve finds the current file's entry in a temp
 * compile_commands.json and resolves its -I dir relative to directory. */
static void
t_cc_db(Test *t)
{
	char tmpl[] = "/tmp/vedit_ccXXXXXX";
	char *dir = mkdtemp(tmpl);
	char src[PATH_MAX], inc[PATH_MAX], hdr[PATH_MAX];
	char db[PATH_MAX], cfgpath[PATH_MAX], json[1024], cfgtext[PATH_MAX + 32];
	char cand[PATH_MAX];
	Cfg *c;
	Editor e;
	CcIncludes ci;

	TAP_ASSERT(t, dir != NULL);
	snprintf(inc, sizeof(inc), "%s/inc", dir);
	TAP_ASSERT(t, mkdir(inc, 0700) == 0);
	snprintf(src, sizeof(src), "%s/foo.c", dir);
	TAP_ASSERT(t, fclose(fopen(src, "w")) == 0);
	snprintf(hdr, sizeof(hdr), "%s/inc/bar.h", dir);
	TAP_ASSERT(t, fclose(fopen(hdr, "w")) == 0);

	/* An entry with a relative file and a relative -I, plus a decoy. */
	snprintf(json, sizeof(json),
	    "[\n"
	    " { \"directory\": \"%s\", \"file\": \"other.c\",\n"
	    "   \"arguments\": [\"cc\", \"-Iwrong\", \"-c\", \"other.c\"] },\n"
	    " { \"directory\": \"%s\", \"file\": \"foo.c\",\n"
	    "   \"arguments\": [\"cc\", \"-I\", \"inc\", \"-c\", \"foo.c\"] }\n"
	    "]\n", dir, dir);
	snprintf(db, sizeof(db), "%s/compile_commands.json", dir);
	{
		FILE *f = fopen(db, "w");

		TAP_ASSERT(t, f != NULL);
		fputs(json, f);
		fclose(f);
	}

	snprintf(cfgtext, sizeof(cfgtext), "[cc]\nfile = %s\n", db);
	c = load_cfg_text(cfgtext, cfgpath, sizeof(cfgpath));
	TAP_ASSERT(t, c != NULL);

	memset(&e, 0, sizeof(e));
	e.has_name = 1;
	snprintf(e.path, sizeof(e.path), "%s", src);

	g_cfg = c;
	TAP_CHECK(t, cc_resolve(&e, &ci) == 0);
	TAP_CHECKF(t, ci.found && ci.ninc == 1, "found %d ninc %d",
	    ci.found, ci.ninc);
	/* the resolved -I dir holds bar.h */
	snprintf(cand, sizeof(cand), "%.4000s/bar.h", ci.inc[0]);
	TAP_CHECK(t, access(cand, R_OK) == 0);
	g_cfg = NULL;
	vedit_cfg_free(c);

	unlink(db);
	unlink(hdr);
	unlink(src);
	rmdir(inc);
	rmdir(dir);
	unlink(cfgpath);
}

/* cc_resolve caches one result: rewriting the database with equal-length but
 * different content while holding its mtime fixed is not re-read (the stale
 * result stands), and bumping the mtime invalidates the cache. */
static void
t_cc_cache(Test *t)
{
	char tmpl[] = "/tmp/vedit_cccXXXXXX";
	char *dir = mkdtemp(tmpl);
	char src[PATH_MAX], db[PATH_MAX], cfgpath[PATH_MAX];
	char cfgtext[PATH_MAX + 32], j1[512], j2[512];
	struct stat st0;
	struct utimbuf ut;
	Cfg *c;
	Editor e;
	CcIncludes ci;
	FILE *f;
	size_t n;

	TAP_ASSERT(t, dir != NULL);
	snprintf(src, sizeof(src), "%s/foo.c", dir);
	TAP_ASSERT(t, fclose(fopen(src, "w")) == 0);
	snprintf(db, sizeof(db), "%s/compile_commands.json", dir);

	/* j1 and j2 differ only inc <-> oth, so their byte length is identical. */
	snprintf(j1, sizeof(j1), "[{\"directory\":\"%s\",\"file\":\"foo.c\","
	    "\"arguments\":[\"cc\",\"-Iinc\",\"-c\",\"foo.c\"]}]\n", dir);
	snprintf(j2, sizeof(j2), "[{\"directory\":\"%s\",\"file\":\"foo.c\","
	    "\"arguments\":[\"cc\",\"-Ioth\",\"-c\",\"foo.c\"]}]\n", dir);
	f = fopen(db, "w");
	TAP_ASSERT(t, f != NULL);
	fputs(j1, f);
	fclose(f);

	snprintf(cfgtext, sizeof(cfgtext), "[cc]\nfile = %s\n", db);
	c = load_cfg_text(cfgtext, cfgpath, sizeof(cfgpath));
	TAP_ASSERT(t, c != NULL);
	g_cfg = c;

	memset(&e, 0, sizeof(e));
	e.has_name = 1;
	snprintf(e.path, sizeof(e.path), "%s", src);

	TAP_CHECK(t, cc_resolve(&e, &ci) == 0 && ci.ninc == 1);
	n = strlen(ci.inc[0]);
	TAP_CHECK(t, n >= 4 && strcmp(ci.inc[0] + n - 4, "/inc") == 0);
	TAP_ASSERT(t, stat(db, &st0) == 0);

	/* rewrite with the other dir but restore the mtime: a cache hit */
	f = fopen(db, "w");
	TAP_ASSERT(t, f != NULL);
	fputs(j2, f);
	fclose(f);
	ut.actime = st0.st_atime;
	ut.modtime = st0.st_mtime;
	TAP_ASSERT(t, utime(db, &ut) == 0);
	TAP_CHECK(t, cc_resolve(&e, &ci) == 0 && ci.ninc == 1);
	n = strlen(ci.inc[0]);
	TAP_CHECKF(t, n >= 4 && strcmp(ci.inc[0] + n - 4, "/inc") == 0,
	    "cache hit should keep stale '%s'", ci.inc[0]);

	/* bump the mtime: the cache is now stale and the new content is read */
	ut.modtime = st0.st_mtime + 5;
	TAP_ASSERT(t, utime(db, &ut) == 0);
	TAP_CHECK(t, cc_resolve(&e, &ci) == 0 && ci.ninc == 1);
	n = strlen(ci.inc[0]);
	TAP_CHECKF(t, n >= 4 && strcmp(ci.inc[0] + n - 4, "/oth") == 0,
	    "mtime bump should reparse to '%s'", ci.inc[0]);

	g_cfg = NULL;
	vedit_cfg_free(c);
	unlink(db);
	unlink(src);
	rmdir(dir);
	unlink(cfgpath);
}

#ifndef VEDIT_NO_TOOLS
/* The $(...) substitution the per-language tool commands use. */
static void
t_tool_expand(Test *t)
{
	char *s;

	/* Commands run in the file's directory, so filename/filenoext are the
	 * bare basename; file keeps the full path and dir names the directory. */
	s = tool_expand("gcc -c $(filename) -o $(filenoext).o", "src/main.c");
	TAP_ASSERT(t, s != NULL);
	TAP_CHECKF(t, strcmp(s, "gcc -c main.c -o main.o") == 0,
	    "expand: '%s'", s);
	free(s);

	s = tool_expand("$(file) $(fileext) $(dir)", "src/main.c");
	TAP_ASSERT(t, s != NULL);
	TAP_CHECKF(t, strcmp(s, "src/main.c c src") == 0, "parts: '%s'", s);
	free(s);

	/* no directory: dir is ".", so $(file) gets a leading "./" */
	s = tool_expand("$(file) $(dir)", "note.txt");
	TAP_ASSERT(t, s != NULL);
	TAP_CHECKF(t, strcmp(s, "./note.txt .") == 0, "nodir: '%s'", s);
	free(s);

	/* an unknown $(name) is copied through verbatim */
	s = tool_expand("x $(bogus) y", "a.c");
	TAP_ASSERT(t, s != NULL);
	TAP_CHECKF(t, strcmp(s, "x $(bogus) y") == 0, "unknown: '%s'", s);
	free(s);
}

/* The gcc/clang diagnostic parser that feeds the quickfix list. */
static void
t_tool_parse(Test *t)
{
	Editor e;
	const char *out =
	    "gcc -c main.c\n"
	    "main.c:10:5: error: 'x' undeclared\n"
	    "util.h:3: warning: unused\n"
	    "make: *** [all] Error 1\n";

	editor_init(&e);
	sb_append(&e.tool_raw, &e.tool_rawlen, &e.tool_rawcap, out, strlen(out));
	tool_parse_output(&e);

	TAP_CHECKF(t, e.tool_nlines == 4, "lines %d", e.tool_nlines);
	TAP_CHECKF(t, e.tool_nerr == 2, "errors %d", e.tool_nerr);
	if (e.tool_nerr == 2) {
		TAP_CHECKF(t, strcmp(e.tool_errs[0].file, "main.c") == 0 &&
		    e.tool_errs[0].line == 10 && e.tool_errs[0].col == 5 &&
		    e.tool_errs[0].outline == 1, "err0 %s:%zu:%zu@%d",
		    e.tool_errs[0].file, e.tool_errs[0].line,
		    e.tool_errs[0].col, e.tool_errs[0].outline);
		TAP_CHECKF(t, strcmp(e.tool_errs[1].file, "util.h") == 0 &&
		    e.tool_errs[1].line == 3 && e.tool_errs[1].col == 0 &&
		    e.tool_errs[1].outline == 2, "err1 %s:%zu:%zu@%d",
		    e.tool_errs[1].file, e.tool_errs[1].line,
		    e.tool_errs[1].col, e.tool_errs[1].outline);
	}
	tool_free(&e);
}
/* The standalone binary's shell spawner: capture stdout, and the exit status. */
struct capbuf { char data[256]; size_t len; };

static void
cap_emit(void *sink, const char *buf, size_t n)
{
	struct capbuf *c = sink;

	if (c->len + n < sizeof(c->data)) {
		memcpy(c->data + c->len, buf, n);
		c->len += n;
		c->data[c->len] = '\0';
	}
}

static void
t_tool_run(Test *t)
{
	struct capbuf c = { {0}, 0 };
	int rc;

	rc = cli_run_capture(NULL, "printf 'hi:%s\\n' there", ".", cap_emit, &c);
	TAP_CHECKF(t, rc == 0, "printf exit %d", rc);
	TAP_CHECKF(t, strcmp(c.data, "hi:there\n") == 0, "out '%s'", c.data);

	/* the child's directory is the dir argument */
	c.len = 0;
	c.data[0] = '\0';
	rc = cli_run_capture(NULL, "basename \"$(pwd)\"", "/tmp", cap_emit, &c);
	TAP_CHECKF(t, rc == 0 && strcmp(c.data, "tmp\n") == 0, "pwd '%s'", c.data);

	/* a nonzero exit status is reported */
	c.len = 0;
	c.data[0] = '\0';
	rc = cli_run_capture(NULL, "exit 3", ".", cap_emit, &c);
	TAP_CHECKF(t, rc == 3, "exit-status %d", rc);
}

/* Diagnostic severity is read from the word after "file:line:col: ". */
static void
t_tool_sev(Test *t)
{
	TAP_CHECK(t, tool_sev("error: 'x' undeclared") == TSEV_ERROR);
	TAP_CHECK(t, tool_sev("warning: unused") == TSEV_WARN);
	TAP_CHECK(t, tool_sev("note: expanded from") == TSEV_NOTE);
	TAP_CHECK(t, tool_sev("fatal error: no such file") == TSEV_ERROR);
	TAP_CHECK(t, tool_sev("  WARNING: C4244") == TSEV_WARN);	/* ci + ws */
	TAP_CHECK(t, tool_sev("undefined reference to foo") == TSEV_ERROR);
	TAP_CHECK(t, strcmp(tool_sev_name(TSEV_ERROR), "error") == 0);
	TAP_CHECK(t, strcmp(tool_sev_name(TSEV_WARN), "warning") == 0);
	TAP_CHECK(t, strcmp(tool_sev_name(TSEV_NOTE), "note") == 0);
}

/* The count, first-error, and nav-floor helpers over a diagnostics list. */
static void
t_tool_nav(Test *t)
{
	Editor e;
	Toolerr errs[3];
	int ne, nw;

	memset(&e, 0, sizeof(e));
	memset(errs, 0, sizeof(errs));
	errs[0].sev = TSEV_WARN;
	errs[1].sev = TSEV_ERROR;
	errs[2].sev = TSEV_NOTE;
	e.tool_errs = errs;
	e.tool_nerr = 3;

	tool_counts(&e, &ne, &nw);
	TAP_CHECKF(t, ne == 1 && nw == 1, "counts %d/%d", ne, nw);
	TAP_CHECKF(t, tool_first_error(&e) == 1, "first %d",
	    tool_first_error(&e));
	/* an error is present, so stepping floors at errors */
	TAP_CHECK(t, tool_nav_floor(&e) == TSEV_ERROR);

	/* with no errors, the floor drops to warnings */
	errs[1].sev = TSEV_WARN;
	TAP_CHECK(t, tool_first_error(&e) == -1);
	TAP_CHECK(t, tool_nav_floor(&e) == TSEV_WARN);
}
#endif /* VEDIT_NO_TOOLS */

static void
t_bufpick(Test *t)
{
	Editor e;
	Buf bufs[2];
	Bufpick bp;
	const char *l0, *l1;

	editor_init(&e);
	memset(bufs, 0, sizeof(bufs));
	e.bufs = bufs;
	e.nbuf = 2;
	e.cur = 0;

	/* buffer 0: active, named, edited (so dirty) */
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "hello", 5);
	e.has_name = 1;
	snprintf(e.path, sizeof(e.path), "/tmp/alpha.c");
	bufs[0].t = e.t;
	bufs[0].has_name = 1;
	snprintf(bufs[0].path, sizeof(bufs[0].path), "/tmp/alpha.c");

	/* buffer 1: parked, unnamed, untouched */
	bufs[1].t = text_new();
	TAP_ASSERT(t, bufs[1].t != NULL);
	bufs[1].has_name = 0;

	memset(&bp, 0, sizeof(bp));
	bp.e = &e;

	TAP_CHECK(t, bufpick_count(&bp) == 2);

	l0 = bufpick_label(&bp, 0);
	TAP_CHECKF(t, strstr(l0, "alpha.c") && strstr(l0, "*") &&
	    strstr(l0, "[+]"), "active label: %s", l0);

	l1 = bufpick_label(&bp, 1);
	TAP_CHECKF(t, strstr(l1, "[No Name]") && !strstr(l1, "*") &&
	    !strstr(l1, "[+]"), "parked label: %s", l1);

	/* choosing a valid row reports it; an out-of-range row stays open */
	bp.chosen = -1;
	TAP_CHECK(t, bufpick_choose(&bp, 1) == PICK_DONE && bp.chosen == 1);
	TAP_CHECK(t, bufpick_choose(&bp, 9) == PICK_STAY);

	text_free(bufs[0].t);
	text_free(bufs[1].t);
}

static char
classify(const char *s, char *name, size_t namesz)
{
	name[0] = '\0';
	return sym_classify(s, strlen(s), name, namesz);
}

static void
t_sym_classify(Test *t)
{
	char nm[80];

	/* functions: BSD split style (name at column 0) and same-line style */
	TAP_CHECKF(t, classify("jsf_line(const Jsf *j,", nm, sizeof(nm)) == 'f'
	    && strcmp(nm, "jsf_line") == 0, "bsd func: '%s'", nm);
	TAP_CHECKF(t, classify("int main(int argc, char **argv)", nm,
	    sizeof(nm)) == 'f' && strcmp(nm, "main") == 0, "inline func: '%s'",
	    nm);
	TAP_CHECKF(t, classify("foo() {", nm, sizeof(nm)) == 'f' &&
	    strcmp(nm, "foo") == 0, "shell/paren func: '%s'", nm);

	/* not functions */
	TAP_CHECK(t, classify("static int", nm, sizeof(nm)) == 0);
	TAP_CHECK(t, classify("int foo(void);", nm, sizeof(nm)) == 0);
	TAP_CHECK(t, classify("\tbar(void)", nm, sizeof(nm)) == 0);
	TAP_CHECK(t, classify("if (x) {", nm, sizeof(nm)) == 0);
	TAP_CHECK(t, classify("while (y)", nm, sizeof(nm)) == 0);

	/* macros */
	TAP_CHECKF(t, classify("#define MAX 10", nm, sizeof(nm)) == 'd' &&
	    strcmp(nm, "MAX") == 0, "define: '%s'", nm);
	TAP_CHECKF(t, classify("#  define FOO(x) (x)", nm, sizeof(nm)) == 'd' &&
	    strcmp(nm, "FOO") == 0, "define spaced: '%s'", nm);
	TAP_CHECK(t, classify("#ifndef GUARD_H", nm, sizeof(nm)) == 0);

	/* aggregates and typedef aliases */
	TAP_CHECKF(t, classify("typedef struct jsf_rule {", nm, sizeof(nm))
	    == 's' && strcmp(nm, "jsf_rule") == 0, "typedef struct: '%s'", nm);
	TAP_CHECKF(t, classify("struct point {", nm, sizeof(nm)) == 's' &&
	    strcmp(nm, "point") == 0, "struct: '%s'", nm);
	TAP_CHECK(t, classify("struct fwd;", nm, sizeof(nm)) == 0);
	TAP_CHECKF(t, classify("enum req {", nm, sizeof(nm)) == 's' &&
	    strcmp(nm, "req") == 0, "enum: '%s'", nm);
	TAP_CHECKF(t, classify("class Widget : public Base {", nm, sizeof(nm))
	    == 'c' && strcmp(nm, "Widget") == 0, "class: '%s'", nm);
	TAP_CHECKF(t, classify("} Jsfrule;", nm, sizeof(nm)) == 't' &&
	    strcmp(nm, "Jsfrule") == 0, "alias: '%s'", nm);
	TAP_CHECK(t, classify("};", nm, sizeof(nm)) == 0);
}

static void
t_symscan(Test *t)
{
	Editor e;
	Sympick sp;

	editor_init(&e);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "#define N 3", 11);
	lines_insert_at(e.t, 1, "struct point {", 14);
	lines_insert_at(e.t, 2, "\tint x;", 7);
	lines_insert_at(e.t, 3, "};", 2);
	lines_insert_at(e.t, 4, "int add(int a, int b)", 21);
	lines_insert_at(e.t, 5, "{", 1);

	memset(&sp, 0, sizeof(sp));
	sp.e = &e;
	symscan(&sp);

	TAP_CHECKF(t, sp.n == 3, "symbol count %d", sp.n);
	TAP_CHECKF(t, sp.ent[0].kind == 'd' && sp.ent[0].line == 0 &&
	    strcmp(sp.ent[0].name, "N") == 0, "sym0 %c L%zu %s",
	    sp.ent[0].kind, sp.ent[0].line, sp.ent[0].name);
	TAP_CHECKF(t, sp.ent[1].kind == 's' && sp.ent[1].line == 1 &&
	    strcmp(sp.ent[1].name, "point") == 0, "sym1 %c L%zu %s",
	    sp.ent[1].kind, sp.ent[1].line, sp.ent[1].name);
	TAP_CHECKF(t, sp.ent[2].kind == 'f' && sp.ent[2].line == 4 &&
	    strcmp(sp.ent[2].name, "add") == 0, "sym2 %c L%zu %s",
	    sp.ent[2].kind, sp.ent[2].line, sp.ent[2].name);

	free(sp.ent);
	text_free(e.t);
}

static void
t_isearch(Test *t)
{
	Editor e;
	size_t my, mx;

	editor_init(&e);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	text_insert(e.t, 0, 0, "foo bar", 7);
	lines_insert_at(e.t, 1, "baz foo", 7);

	/* from the very start, the first match is in place */
	TAP_CHECK(t, isearch_scan_dir(&e, "foo", 0, 0, 1, &my, &mx) &&
	    my == 0 && mx == 0);

	/* past the first match, the next is on line 2 */
	TAP_CHECKF(t, isearch_scan_dir(&e, "foo", 0, 1, 1, &my, &mx) &&
	    my == 1 && mx == 4, "forward: L%zu C%zu", my, mx);

	/* past the last match, it wraps back to the first */
	TAP_CHECKF(t, isearch_scan_dir(&e, "foo", 1, 5, 1, &my, &mx) &&
	    my == 0 && mx == 0, "wrap: L%zu C%zu", my, mx);

	/* a miss and an empty query both report nothing */
	TAP_CHECK(t, !isearch_scan_dir(&e, "zzz", 0, 0, 1, &my, &mx));
	TAP_CHECK(t, !isearch_scan_dir(&e, "", 0, 0, 1, &my, &mx));

	/* the pattern is a regex: anchors and classes work */
	TAP_CHECKF(t, isearch_scan_dir(&e, "^baz", 0, 0, 1, &my, &mx) &&
	    my == 1 && mx == 0, "anchor: L%zu C%zu", my, mx);
	TAP_CHECKF(t, isearch_scan_dir(&e, "b.r", 0, 0, 1, &my, &mx) &&
	    my == 0 && mx == 4, "class: L%zu C%zu", my, mx);
	/* a half-typed, invalid pattern matches nothing rather than erroring */
	TAP_CHECK(t, !isearch_scan_dir(&e, "(", 0, 0, 1, &my, &mx));

	/* backward: nearest match before the origin, then wrapping */
	TAP_CHECKF(t, isearch_scan_dir(&e, "foo", 1, 7, -1, &my, &mx) &&
	    my == 1 && mx == 4, "back from (1,7): L%zu C%zu", my, mx);
	TAP_CHECKF(t, isearch_scan_dir(&e, "foo", 1, 4, -1, &my, &mx) &&
	    my == 0 && mx == 0, "back from (1,4): L%zu C%zu", my, mx);
	TAP_CHECKF(t, isearch_scan_dir(&e, "foo", 0, 0, -1, &my, &mx) &&
	    my == 1 && mx == 4, "back wrap from (0,0): L%zu C%zu", my, mx);

	text_free(e.t);
}

const Case tap_cases[] = {
	{ "utf8_roundtrip", t_utf8_roundtrip },
	{ "rune_width", t_rune_width },
	{ "disp_cols", t_disp_cols },
	{ "color256_to_16", t_color256_to_16 },
	{ "rgb_to_ansi16", t_rgb_to_ansi16 },
	{ "wrap_next", t_wrap_next },
	{ "line_rows", t_line_rows },
	{ "seg_index_of", t_seg_index_of },
	{ "syntax_c", t_syntax_c },
	{ "syntax_block_comment_carry", t_syntax_block_comment_carry },
	{ "syntax_refine", t_syntax_refine },
	{ "text_edit_undo", t_text_edit_undo },
	{ "multiline_buffer", t_multiline_buffer },
	{ "pick_fit", t_pick_fit },
	{ "filepick_cmp", t_filepick_cmp },
	{ "pick_jump", t_pick_jump },
	{ "filepick_load", t_filepick_load },
	{ "filepick_start_dir", t_filepick_start_dir },
	{ "cfg_parse", t_cfg_parse },
	{ "cfg_resolve", t_cfg_resolve },
	{ "cfg_color", t_cfg_color },
	{ "cfg_theme", t_cfg_theme },
	{ "jsf_charset", t_jsf_charset },
	{ "jsf_highlight", t_jsf_highlight },
	{ "jsf_linecomment", t_jsf_linecomment },
	{ "jsf_recolormark", t_jsf_recolormark },
	{ "jsf_include", t_jsf_include },
	{ "replace", t_replace },
	{ "regex_engine", t_regex_engine },
	{ "base64", t_base64 },
	{ "eol", t_eol },
	{ "indent", t_indent },
	{ "retab", t_retab },
	{ "tags", t_tags },
	{ "ex_abbrev", t_ex_abbrev },
	{ "ex_subst", t_ex_subst },
	{ "include_target", t_include_target },
	{ "cc_split", t_cc_split },
	{ "cc_db", t_cc_db },
	{ "cc_cache", t_cc_cache },
#ifndef VEDIT_NO_TOOLS
	{ "tool_sev", t_tool_sev },
	{ "tool_nav", t_tool_nav },
	{ "tool_expand", t_tool_expand },
	{ "tool_parse", t_tool_parse },
	{ "tool_run", t_tool_run },
#endif
	{ "bufpick", t_bufpick },
	{ "sym_classify", t_sym_classify },
	{ "symscan", t_symscan },
	{ "isearch", t_isearch },
	{ NULL, NULL },
};
