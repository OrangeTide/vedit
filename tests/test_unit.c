/*
 * Unit tests for vedit's internal helpers. The whole editor is included as a
 * single translation unit (with main renamed out of the way), so the static
 * functions are reachable directly.
 */
#include "test.h"

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

static void
t_syntax_c(Test *t)
{
	const Syntax *sy = syn_for_ext("c");
	uint8_t out[32];

	TAP_ASSERT(t, sy != NULL);

	syn_line(sy, 0, "int", 3, out);
	TAP_CHECK(t, out[0] == SYN_TYPE);

	syn_line(sy, 0, "if", 2, out);
	TAP_CHECK(t, out[0] == SYN_KEYWORD);

	syn_line(sy, 0, "// hi", 5, out);
	TAP_CHECK(t, out[0] == SYN_COMMENT);

	syn_line(sy, 0, "#include", 8, out);
	TAP_CHECK(t, out[0] == SYN_PREPROC);

	/* an unknown extension is not highlighted */
	TAP_CHECK(t, syn_for_ext("xyz") == NULL);
}

static void
t_syntax_block_comment_carry(Test *t)
{
	const Syntax *sy = syn_for_ext("c");
	uint8_t out[32];
	uint16_t st;

	/* an unterminated block comment carries in-comment state to next line */
	st = syn_line(sy, 0, "/* open", 7, out);
	TAP_CHECK(t, out[0] == SYN_COMMENT);
	TAP_CHECK(t, st == SYN_INCOMMENT);
	st = syn_line(sy, st, "still */ x", 10, out);
	TAP_CHECK(t, out[0] == SYN_COMMENT);	/* continuation still comment */
	TAP_CHECK(t, st == 0);			/* closed on this line */
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
	    "ui.scheme = dos\n"		/* shorthand, overwritten below */
	    "edit.mode = vi\n"
	    "[ui]\n"
	    "  scheme = black ; trailing comment\n"
	    "  wrap = on\n"
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

	for (k = 0; k < g_jsf[lang].nclasses; k++)
		if (strcmp(g_jsf[lang].classname[k], name) == 0)
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
	syntax_load_cfg(c);
	lang = jsf_find("mini");
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
	syntax_load_cfg(NULL);			/* clear registry for other tests */
	vedit_cfg_free(c);
	unlink(path);
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
	{ NULL, NULL },
};
