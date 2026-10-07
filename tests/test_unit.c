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
	uint16_t out[32];
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
	uint16_t out[32];
	uint32_t st;
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
	uint16_t out[32];
	uint32_t st;
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
t_syntax_md(Test *t)
{
	const Syntax *md = syn_for_ext("md");
	uint16_t out[64];
	uint32_t st;
	int head, bold, ital, code, cb, quote, link, url, lm, txt;

	TAP_ASSERT(t, md != NULL && md->fsm != NULL);
	head = fsm_class(md->fsm, "heading");
	bold = fsm_class(md->fsm, "bold");
	ital = fsm_class(md->fsm, "italic");
	code = fsm_class(md->fsm, "code");
	cb = fsm_class(md->fsm, "codeblock");
	quote = fsm_class(md->fsm, "quote");
	link = fsm_class(md->fsm, "link");
	url = fsm_class(md->fsm, "url");
	lm = fsm_class(md->fsm, "listmark");
	txt = fsm_class(md->fsm, "text");
	TAP_ASSERT(t, head > 0 && bold > 0 && code > 0 && link > 0);

	/* a heading colors the whole line, '#' included */
	syn_line(md, md->start, "# Title", 7, out);
	TAP_CHECKF(t, out[0] == head && out[6] == head, "heading [%d %d]",
	    out[0], out[6]);

	/* blockquote colors the whole line */
	syn_line(md, md->start, "> quoted", 8, out);
	TAP_CHECKF(t, out[0] == quote && out[7] == quote, "quote [%d %d]",
	    out[0], out[7]);

	/* **bold** paints the markers and the span */
	syn_line(md, md->start, "a **b** c", 9, out);
	TAP_CHECKF(t, out[0] == txt, "pre-bold text %d", out[0]);
	TAP_CHECKF(t, out[2] == bold && out[3] == bold && out[5] == bold &&
	    out[6] == bold, "bold [%d %d %d %d]", out[2], out[3], out[5],
	    out[6]);
	TAP_CHECKF(t, out[8] == txt, "post-bold text %d", out[8]);

	/* *italic* with single stars */
	syn_line(md, md->start, "a *b* c", 7, out);
	TAP_CHECKF(t, out[2] == ital && out[3] == ital && out[4] == ital,
	    "italic [%d %d %d]", out[2], out[3], out[4]);

	/* `code` span, including both backticks */
	syn_line(md, md->start, "x `y` z", 7, out);
	TAP_CHECKF(t, out[2] == code && out[3] == code && out[4] == code,
	    "code span [%d %d %d]", out[2], out[3], out[4]);

	/* a backslash escapes the next byte so '*' stays plain text */
	syn_line(md, md->start, "a \\*b\\* c", 9, out);
	TAP_CHECKF(t, out[3] == txt && out[4] == txt, "escaped star [%d %d]",
	    out[3], out[4]);

	/* [text](url) link */
	syn_line(md, md->start, "[t](u)", 6, out);
	TAP_CHECKF(t, out[0] == link && out[1] == link && out[2] == link,
	    "link text [%d %d %d]", out[0], out[1], out[2]);
	TAP_CHECKF(t, out[4] == url, "link url %d", out[4]);

	/* a dash bullet colors the marker; the rest is inline */
	syn_line(md, md->start, "- item", 6, out);
	TAP_CHECKF(t, out[0] == lm && out[1] == lm, "bullet [%d %d]",
	    out[0], out[1]);

	/* an ordered-list marker is repainted once the dot and space confirm it */
	syn_line(md, md->start, "12. item", 8, out);
	TAP_CHECKF(t, out[0] == lm && out[1] == lm && out[2] == lm,
	    "ordered [%d %d %d]", out[0], out[1], out[2]);

	/* a plain '-' that is not a bullet is not mis-colored */
	syn_line(md, md->start, "-x", 2, out);
	TAP_CHECKF(t, out[0] == txt, "lone dash %d", out[0]);

	/* a fenced code block carries across lines until the closing fence (a
	 * language the editor knows is styled by its grammar: t_syntax_md_embed) */
	st = syn_line(md, md->start, "```nosuch", 9, out);
	TAP_CHECKF(t, out[0] == code && out[3] == code, "fence open [%d %d]",
	    out[0], out[3]);
	TAP_CHECKF(t, st != md->start, "fence carries state %u", st);
	st = syn_line(md, st, "int x;", 6, out);
	TAP_CHECKF(t, out[0] == cb && out[5] == cb, "code body [%d %d]",
	    out[0], out[5]);
	st = syn_line(md, st, "```", 3, out);
	TAP_CHECKF(t, out[0] == cb, "fence close %d", out[0]);
	TAP_CHECK(t, st == md->start);		/* block closed on this line */
}

/* A fenced block whose info string names a known language is styled by that
 * language's grammar, tagged with its id, and the carry state packs both
 * grammars until the closing fence returns the line to Markdown. An unknown
 * language keeps the block in Markdown's own codeblock color. */
static void
t_syntax_md_embed(Test *t)
{
	const Syntax *md = syn_for_ext("md");
	const Syntax *c = syn_for_ext("c");
	uint16_t out[32];
	uint32_t st;
	int cb, type, kw, gid;

	TAP_ASSERT(t, md && md->fsm && c && c->fsm);
	cb = fsm_class(md->fsm, "codeblock");
	type = fsm_class(c->fsm, "type");
	kw = fsm_class(c->fsm, "keyword");
	gid = syn_gid(c);
	TAP_ASSERT(t, cb > 0 && type > 0 && kw > 0 && gid > 0);

	st = syn_line(md, md->start, "```c", 4, out);
	TAP_CHECKF(t, SYN_GID(st) == gid && SYN_INNER(st) == c->start,
	    "after the fence: gid %d inner %u", SYN_GID(st), SYN_INNER(st));
	st = syn_line(md, st, "int x; return", 13, out);
	TAP_CHECKF(t, out[0] == ((gid << 8) | type), "int -> %#x", out[0]);
	TAP_CHECKF(t, out[7] == ((gid << 8) | kw), "return -> %#x", out[7]);
	TAP_CHECK(t, SYN_GID(st) == gid);
	/* a C block comment left open carries the inner state across lines */
	st = syn_line(md, st, "/* open", 7, out);
	TAP_CHECK(t, SYN_GID(st) == gid && SYN_INNER(st) != c->start);
	st = syn_line(md, st, "still", 5, out);
	TAP_CHECKF(t, (out[0] >> 8) == gid && (out[0] & 0xff) ==
	    fsm_class(c->fsm, "comment"), "comment carry -> %#x", out[0]);
	/* the closing fence, even indented, ends the region in Markdown's color */
	st = syn_line(md, st, "  ```", 5, out);
	TAP_CHECKF(t, out[2] == cb, "fence close -> %#x", out[2]);
	TAP_CHECKF(t, st == SYN_PACK(md->start, 0, 0), "closed: %#x", st);

	/* the info string's first word picks the language; extras are ignored */
	st = syn_line(md, md->start, "```sh title=x", 13, out);
	TAP_CHECKF(t, SYN_GID(st) == syn_gid(syn_for_ext("sh")),
	    "sh fence gid %d", SYN_GID(st));
	st = syn_line(md, st, "```", 3, out);
	TAP_CHECK(t, st == SYN_PACK(md->start, 0, 0));

	/* an unknown language: plain codeblock, no inner grammar */
	st = syn_line(md, md->start, "```nosuch", 9, out);
	TAP_CHECKF(t, SYN_GID(st) == 0, "unknown gid %d", SYN_GID(st));
	st = syn_line(md, st, "int x;", 6, out);
	TAP_CHECKF(t, out[0] == cb && out[5] == cb, "plain body [%#x %#x]",
	    out[0], out[5]);
	st = syn_line(md, st, "```", 3, out);
	TAP_CHECK(t, st == SYN_PACK(md->start, 0, 0));

	/* a bare fence too */
	st = syn_line(md, md->start, "```", 3, out);
	st = syn_line(md, st, "x", 1, out);
	TAP_CHECK(t, out[0] == cb && SYN_GID(st) == 0);
}

/* The JavaScript grammar: keywords, builtins, a line comment, and a template
 * literal that carries its string color across lines. */
static void
t_syntax_js(Test *t)
{
	const Syntax *js = syn_for_ext("js");
	uint16_t out[64];
	uint32_t st;
	int kw, type, str, com, txt;

	TAP_ASSERT(t, js && js->fsm && syn_for_ext("mjs") == js);
	kw = fsm_class(js->fsm, "keyword");
	type = fsm_class(js->fsm, "type");
	str = fsm_class(js->fsm, "string");
	com = fsm_class(js->fsm, "comment");
	txt = fsm_class(js->fsm, "text");
	TAP_ASSERT(t, kw > 0 && type > 0 && str > 0 && com > 0);

	/* "const $x = null; // c" */
	syn_line(js, js->start, "const $x = null; // c", 21, out);
	TAP_CHECKF(t, out[0] == kw && out[4] == kw, "const [%d %d]", out[0], out[4]);
	TAP_CHECKF(t, out[6] == txt && out[7] == txt, "$x [%d %d]", out[6], out[7]);
	TAP_CHECKF(t, out[11] == type && out[14] == type, "null [%d %d]",
	    out[11], out[14]);
	TAP_CHECKF(t, out[17] == com && out[20] == com, "comment [%d %d]",
	    out[17], out[20]);

	/* a template literal spans lines; a double-quoted string does not */
	st = syn_line(js, js->start, "f(`a ${b}", 9, out);
	TAP_CHECKF(t, out[2] == str && out[8] == str, "template [%d %d]",
	    out[2], out[8]);
	st = syn_line(js, st, "c` + 1", 6, out);
	TAP_CHECKF(t, out[0] == str && out[1] == str && out[3] == txt,
	    "template carry [%d %d %d]", out[0], out[1], out[3]);
	TAP_CHECK(t, st == SYN_PACK(js->start, 0, 0));
	st = syn_line(js, js->start, "\"open", 5, out);
	TAP_CHECK(t, st == SYN_PACK(js->start, 0, 0));
}

/* The HTML grammar: tags, attributes, quoted values, entities, comments, and
 * a <script> body colored by the JavaScript grammar until </script>. */
static void
t_syntax_html(Test *t)
{
	const Syntax *html = syn_for_ext("html");
	const Syntax *js = syn_for_ext("js");
	uint16_t out[64];
	uint32_t st;
	int tag, attr, str, com, ent, txt, gid, kw;

	TAP_ASSERT(t, html && html->fsm && js && js->fsm);
	TAP_CHECK(t, syn_for_ext("htm") == html);
	tag = fsm_class(html->fsm, "tag");
	attr = fsm_class(html->fsm, "attr");
	str = fsm_class(html->fsm, "string");
	com = fsm_class(html->fsm, "comment");
	ent = fsm_class(html->fsm, "entity");
	txt = fsm_class(html->fsm, "text");
	gid = syn_gid(js);
	kw = fsm_class(js->fsm, "keyword");
	TAP_ASSERT(t, tag > 0 && attr > 0 && str > 0 && com > 0 && ent > 0 && gid > 0);

	/* <a href="x">&amp;</a> */
	syn_line(html, html->start, "<a href=\"x\">&amp;</a>", 21, out);
	TAP_CHECKF(t, out[0] == tag && out[1] == tag, "open tag [%d %d]",
	    out[0], out[1]);
	TAP_CHECKF(t, out[3] == attr && out[6] == attr, "attr [%d %d]", out[3], out[6]);
	TAP_CHECKF(t, out[8] == str && out[10] == str, "value [%d %d]", out[8], out[10]);
	TAP_CHECKF(t, out[11] == tag, "close > %d", out[11]);
	TAP_CHECKF(t, out[12] == ent && out[16] == ent, "entity [%d %d]",
	    out[12], out[16]);
	TAP_CHECKF(t, out[17] == tag && out[20] == tag, "end tag [%d %d]",
	    out[17], out[20]);

	/* a comment carries across lines; a doctype does not */
	st = syn_line(html, html->start, "x<!-- c", 7, out);
	TAP_CHECKF(t, out[0] == txt && out[1] == com && out[6] == com,
	    "comment [%d %d %d]", out[0], out[1], out[6]);
	st = syn_line(html, st, "--> y", 5, out);
	TAP_CHECKF(t, out[0] == com && out[4] == txt, "comment end [%d %d]",
	    out[0], out[4]);
	TAP_CHECK(t, st == SYN_PACK(html->start, 0, 0));

	/* <script>var x</script>: the body is JavaScript, the tags are HTML */
	syn_line(html, html->start, "<script>var x</script>!", 23, out);
	TAP_CHECKF(t, out[8] == ((gid << 8) | kw) && out[10] == out[8],
	    "script var [%#x %#x]", out[8], out[10]);
	TAP_CHECKF(t, out[13] == tag && out[21] == tag && out[22] == txt,
	    "script close [%d %d %d]", out[13], out[21], out[22]);

	/* with attributes and in upper case, carrying to a later line */
	st = syn_line(html, html->start, "<SCRIPT type=\"module\">let a", 27, out);
	TAP_CHECKF(t, out[8] == attr && out[14] == str, "script attrs [%d %d]",
	    out[8], out[14]);
	TAP_CHECKF(t, out[22] == ((gid << 8) | kw), "let -> %#x", out[22]);
	TAP_CHECKF(t, SYN_GID(st) == gid, "carry gid %d", SYN_GID(st));
	st = syn_line(html, st, "b</SCRIPT><p>", 13, out);
	TAP_CHECKF(t, (out[0] >> 8) == gid && out[1] == tag && out[10] == tag,
	    "after script [%#x %d %d]", out[0], out[1], out[10]);
	TAP_CHECK(t, st == SYN_PACK(html->start, 0, 0));

	/* "<scripts>" is an ordinary tag, not a script */
	st = syn_line(html, html->start, "<scripts>if", 11, out);
	TAP_CHECKF(t, out[9] == txt && SYN_GID(st) == 0, "scripts tag %d gid %d",
	    out[9], SYN_GID(st));

	/* a Markdown fence names JavaScript by extension */
	st = syn_line(syn_for_ext("md"), syn_for_ext("md")->start, "```js", 5, out);
	TAP_CHECKF(t, SYN_GID(st) == gid, "md js fence gid %d", SYN_GID(st));
}

/* The built-in INI grammar: section headers, keys, values, strings, and
 * comments, each line standing alone. */
static void
t_syntax_ini(Test *t)
{
	const Syntax *ini = syn_for_ext("ini");
	uint16_t out[64];
	int sec, key, val, str, com, txt;

	TAP_ASSERT(t, ini != NULL && ini->fsm != NULL);
	sec = fsm_class(ini->fsm, "section");
	key = fsm_class(ini->fsm, "key");
	val = fsm_class(ini->fsm, "value");
	str = fsm_class(ini->fsm, "string");
	com = fsm_class(ini->fsm, "comment");
	txt = fsm_class(ini->fsm, "text");
	TAP_ASSERT(t, sec > 0 && key > 0 && val > 0 && str > 0 && com > 0);

	syn_line(ini, ini->start, "[ui \"sub\"]", 10, out);
	TAP_CHECKF(t, out[0] == sec && out[9] == sec, "section [%d %d]",
	    out[0], out[9]);

	/* key = value: key, separator as text, value */
	syn_line(ini, ini->start, "wrap = on", 9, out);
	TAP_CHECKF(t, out[0] == key && out[3] == key, "key [%d %d]",
	    out[0], out[3]);
	TAP_CHECKF(t, out[5] == txt, "separator %d", out[5]);
	TAP_CHECKF(t, out[7] == val && out[8] == val, "value [%d %d]",
	    out[7], out[8]);

	/* both comment leaders, also after leading blanks */
	syn_line(ini, ini->start, "; note", 6, out);
	TAP_CHECKF(t, out[0] == com && out[5] == com, "; comment [%d %d]",
	    out[0], out[5]);
	syn_line(ini, ini->start, "  # note", 8, out);
	TAP_CHECKF(t, out[2] == com && out[7] == com, "# comment [%d %d]",
	    out[2], out[7]);

	/* a quoted value and a trailing comment */
	syn_line(ini, ini->start, "k = \"a;b\" # c", 13, out);
	TAP_CHECKF(t, out[4] == str && out[6] == str && out[8] == str,
	    "string [%d %d %d]", out[4], out[6], out[8]);
	TAP_CHECKF(t, out[10] == com && out[12] == com, "trailing [%d %d]",
	    out[10], out[12]);

	/* a key alone (a gitconfig boolean) stays a key */
	syn_line(ini, ini->start, "bare", 4, out);
	TAP_CHECKF(t, out[0] == key && out[3] == key, "bare [%d %d]",
	    out[0], out[3]);

	/* the extension and basename mappings */
	TAP_CHECK(t, syn_for_ext("cfg") == ini);
	TAP_CHECK(t, syn_for_ext("gitconfig") == ini);
	TAP_CHECK(t, syn_for_path("/home/u/.veditrc") == ini);
	TAP_CHECK(t, syn_for_path("/home/u/.config/vedit/config") == ini);
	TAP_CHECK(t, syn_for_path(".git/config") == ini);
	TAP_CHECK(t, syn_for_path("config") == ini);
	TAP_CHECK(t, syn_for_path("/x/Makefile") == NULL);
	TAP_CHECK(t, syn_for_path("/x/main.c") == syn_for_ext("c"));
}

/* mkdir_p creates nested directories, accepts existing ones, and refuses a
 * path through a regular file. */
static void
t_mkdir_p(Test *t)
{
	char dir[] = "/tmp/vedit_mkXXXXXX";
	char path[PATH_MAX], file[PATH_MAX], bad[PATH_MAX];
	struct stat st;
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/a/b/c", dir);
	TAP_CHECK(t, mkdir_p(path) == 0);
	TAP_CHECK(t, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
	TAP_CHECK(t, mkdir_p(path) == 0);		/* already there */

	snprintf(file, sizeof(file), "%s/a/file", dir);
	f = fopen(file, "w");
	TAP_ASSERT(t, f != NULL);
	fclose(f);
	snprintf(bad, sizeof(bad), "%s/a/file/sub", dir);
	TAP_CHECK(t, mkdir_p(bad) != 0);
	TAP_CHECK(t, mkdir_p(file) != 0 && errno == ENOTDIR);

	unlink(file);
	rmdir(path);
	snprintf(path, sizeof(path), "%s/a/b", dir);
	rmdir(path);
	snprintf(path, sizeof(path), "%s/a", dir);
	rmdir(path);
	rmdir(dir);
}

/* cli_config_path: an explicit path as given; else $XDG_CONFIG_HOME/vedit/
 * config, with ~/.config when the variable is unset, found or not. ~/.veditrc
 * is not consulted on an XDG build. */
static void
t_cli_config_path(Test *t)
{
	char home[] = "/tmp/vedit_cpXXXXXX";
	char buf[PATH_MAX], want[PATH_MAX], sub[PATH_MAX];
	FILE *f;
	const char *old_home = getenv("HOME"), *old_xdg = getenv("XDG_CONFIG_HOME");
	const char *old_cfg = getenv("VEDIT_CONFIG");
	char save_home[PATH_MAX] = "", save_xdg[PATH_MAX] = "", save_cfg[PATH_MAX] = "";

	if (old_home)
		snprintf(save_home, sizeof(save_home), "%s", old_home);
	if (old_xdg)
		snprintf(save_xdg, sizeof(save_xdg), "%s", old_xdg);
	if (old_cfg)
		snprintf(save_cfg, sizeof(save_cfg), "%s", old_cfg);

	TAP_ASSERT(t, mkdtemp(home) != NULL);
	setenv("HOME", home, 1);
	unsetenv("XDG_CONFIG_HOME");
	unsetenv("VEDIT_CONFIG");

	/* nothing exists: the default location, flagged as missing */
	snprintf(want, sizeof(want), "%s/.config/vedit/config", home);
	TAP_CHECK(t, cli_config_path(NULL, buf, sizeof(buf)) == 0 &&
	    strcmp(buf, want) == 0);

	/* a ~/.veditrc is ignored on an XDG build */
	snprintf(sub, sizeof(sub), "%s/.veditrc", home);
	f = fopen(sub, "w");
	TAP_ASSERT(t, f != NULL);
	fclose(f);
	TAP_CHECK(t, cli_config_path(NULL, buf, sizeof(buf)) == 0 &&
	    strcmp(buf, want) == 0);
	unlink(sub);

	/* ~/.config/vedit/config exists: found and read, XDG unset */
	snprintf(sub, sizeof(sub), "%s/.config/vedit", home);
	mkdir_p(sub);
	f = fopen(want, "w");
	TAP_ASSERT(t, f != NULL);
	fclose(f);
	TAP_CHECKF(t, cli_config_path(NULL, buf, sizeof(buf)) == 1 &&
	    strcmp(buf, want) == 0, "got %s", buf);

	/* an explicit path wins as given, existing or not */
	TAP_CHECK(t, cli_config_path("/nowhere/x", buf, sizeof(buf)) == 1 &&
	    strcmp(buf, "/nowhere/x") == 0);
	setenv("VEDIT_CONFIG", "/nowhere/y", 1);
	TAP_CHECK(t, cli_config_path(NULL, buf, sizeof(buf)) == 1 &&
	    strcmp(buf, "/nowhere/y") == 0);
	unsetenv("VEDIT_CONFIG");

	/* XDG_CONFIG_HOME set: that location, whether the file exists or not */
	setenv("XDG_CONFIG_HOME", "/nonexistent-xdg", 1);
	TAP_CHECK(t, cli_config_path(NULL, buf, sizeof(buf)) == 0 &&
	    strcmp(buf, "/nonexistent-xdg/vedit/config") == 0);
	unsetenv("XDG_CONFIG_HOME");

	/* no HOME at all: no path */
	unsetenv("HOME");
	TAP_CHECK(t, cli_config_path(NULL, buf, sizeof(buf)) == -1);

	unlink(want);
	rmdir(sub);
	snprintf(sub, sizeof(sub), "%s/.config", home);
	rmdir(sub);
	rmdir(home);

	/* leave the process environment as it was for the other tests */
	if (old_home)
		setenv("HOME", save_home, 1);
	else
		unsetenv("HOME");
	if (old_xdg)
		setenv("XDG_CONFIG_HOME", save_xdg, 1);
	else
		unsetenv("XDG_CONFIG_HOME");
	if (old_cfg)
		setenv("VEDIT_CONFIG", save_cfg, 1);
	else
		unsetenv("VEDIT_CONFIG");
}

/* Serialize the whole buffer the way the file on disk would read: each line's
 * bytes in order, joined by '\n', with no trailing newline. Caller frees. */
static char *
text_dump(Text *tx, size_t *lenout)
{
	size_t n = text_lines(tx), i, len = 0, cap = 64;
	char *s = malloc(cap);

	if (!s)
		return NULL;
	for (i = 0; i < n; i++) {
		size_t ll = 0;
		const char *l = text_line(tx, i, &ll);

		while (len + ll + 2 > cap) {
			char *ns = realloc(s, cap *= 2);

			if (!ns) {
				free(s);
				return NULL;
			}
			s = ns;
		}
		if (ll)
			memcpy(s + len, l, ll);
		len += ll;
		if (i + 1 < n)
			s[len++] = '\n';
	}
	s[len] = '\0';
	if (lenout)
		*lenout = len;
	return s;
}

/* True when the buffer serializes to exactly want (a NUL-free string). */
static int
dump_is(Text *tx, const char *want)
{
	size_t len = 0;
	char *s = text_dump(tx, &len);
	int ok = s && len == strlen(want) && memcmp(s, want, len) == 0;

	free(s);
	return ok;
}

/* Fill a fresh (one empty line) Text with n whole lines. */
static void
tx_fill(Text *tx, const char *const *lines, int n)
{
	int i;

	if (n <= 0)
		return;
	text_insert(tx, 0, 0, lines[0], strlen(lines[0]));
	for (i = 1; i < n; i++)
		lines_insert_at(tx, (size_t)i, lines[i], strlen(lines[i]));
}

/* The picker entry field scrolls its tail into view in fixed jumps, keeping
 * the cursor visible without reflowing on every keystroke. */
static void
t_entry_scroll(Test *t)
{
	char s[64];
	int off, n;

	TAP_CHECK(t, entry_scroll_off("abc", 10) == 0);	/* fits: no scroll */

	/* avail 10, chunk 5: the offset is a multiple of the chunk and always
	 * leaves the end (the cursor) within avail columns. */
	for (n = 1; n <= 40; n++) {
		memset(s, 'x', (size_t)n);
		s[n] = '\0';
		off = entry_scroll_off(s, 10);
		TAP_CHECKF(t, off % 5 == 0, "len %d off %d not a jump", n, off);
		TAP_CHECKF(t, n - off <= 10, "len %d off %d hides cursor", n, off);
		TAP_CHECKF(t, n <= 10 ? off == 0 : off > 0,
		    "len %d off %d scroll wrong", n, off);
	}

	/* the offset holds steady within a chunk, then jumps by one chunk */
	memset(s, 'x', 15); s[15] = '\0';
	TAP_CHECKF(t, entry_scroll_off(s, 10) == 5, "len 15");
	memset(s, 'x', 16); s[16] = '\0';
	TAP_CHECKF(t, entry_scroll_off(s, 10) == 10, "len 16 should jump");
}

/* The FILE* serializer cores round-trip content and every line-ending style. */
static void
t_text_fp_roundtrip(Test *t)
{
	static const char *const L[] = { "alpha", "beta", "gamma" };
	int eols[] = { EOL_LF, EOL_CRLF, EOL_NUL };
	size_t k;

	for (k = 0; k < sizeof(eols) / sizeof(eols[0]); k++) {
		Text *a = text_new(), *b = text_new();
		FILE *fp = tmpfile();

		TAP_ASSERT(t, a && b && fp);
		tx_fill(a, L, 3);
		a->eol = eols[k];
		a->final_newline = (k != 1);	/* also exercise no-final-newline */
		TAP_CHECK(t, text_write_fp(a, fp) == OK);
		rewind(fp);
		TAP_CHECK(t, text_load_fp(b, fp) == OK);
		TAP_CHECKF(t, dump_is(b, "alpha\nbeta\ngamma"),
		    "eol %d body mismatch", eols[k]);
		TAP_CHECKF(t, b->eol == eols[k], "eol %d not detected: %d",
		    eols[k], b->eol);
		TAP_CHECKF(t, b->final_newline == (k != 1),
		    "final_newline %d wrong for eol %d", b->final_newline,
		    eols[k]);
		fclose(fp);
		text_free(a);
		text_free(b);
	}
}

/* Swap and backup path construction: beside the file, and in a shared dir. */
static void
t_swap_paths(Test *t)
{
	char dir[] = "/tmp/vedit_spXXXXXX";
	char out[PATH_MAX], want[PATH_MAX];

	TAP_ASSERT(t, swap_path_for("/a/b/foo.c", out, sizeof(out)));
	TAP_CHECKF(t, strcmp(out, "/a/b/.foo.c.swp") == 0, "beside swap: %s",
	    out);
	TAP_ASSERT(t, swap_path_for("foo.c", out, sizeof(out)));
	TAP_CHECKF(t, strcmp(out, ".foo.c.swp") == 0, "no-dir swap: %s", out);
	TAP_ASSERT(t, backup_path_for("/a/b/foo.c", out, sizeof(out)));
	TAP_CHECKF(t, strcmp(out, "/a/b/foo.c~") == 0, "beside backup: %s",
	    out);

	/* A configured swapdir mangles the full path into one flat name. */
	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	setenv("VEDIT_EDIT_SWAPDIR", dir, 1);
	TAP_ASSERT(t, swap_path_for("/a/b/foo.c", out, sizeof(out)));
	snprintf(want, sizeof(want), "%s/%%a%%b%%foo.c.swp", dir);
	TAP_CHECKF(t, strcmp(out, want) == 0, "swapdir name: %s", out);
	unsetenv("VEDIT_EDIT_SWAPDIR");
	rmdir(dir);
}

/* text_save writes atomically: the target gets the new bytes and no temp file
 * is left behind in the directory. */
static void
t_atomic_save(Test *t)
{
	char dir[] = "/tmp/vedit_asXXXXXX";
	char path[PATH_MAX];
	static const char *const L[] = { "one", "two" };
	Text *tx = text_new();
	DIR *d;
	struct dirent *de;
	int leftover = 0;

	TAP_ASSERT(t, tx && mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/out.txt", dir);
	tx_fill(tx, L, 2);
	tx->final_newline = 1;
	TAP_CHECK(t, text_save(tx, path) == OK);

	{
		Text *rd = text_new();

		TAP_ASSERT(t, rd && text_load(rd, path) == OK);
		TAP_CHECK(t, dump_is(rd, "one\ntwo"));
		text_free(rd);
	}
	d = opendir(dir);
	TAP_ASSERT(t, d != NULL);
	while ((de = readdir(d)))
		if (strncmp(de->d_name, ".vedit-save-", 12) == 0)
			leftover = 1;
	closedir(d);
	TAP_CHECK(t, !leftover);

	text_free(tx);
	unlink(path);
	rmdir(dir);
}

/* A writable file in a read-only directory still saves: the atomic temp cannot
 * be created there, so text_save falls back to a direct in-place write. */
static void
t_save_rodir_fallback(Test *t)
{
	char dir[] = "/tmp/vedit_roXXXXXX";
	char path[PATH_MAX];
	static const char *const L[] = { "updated" };
	Text *tx;
	FILE *f;

	if (geteuid() == 0) {		/* root ignores directory permissions */
		TAP_CHECK(t, 1);
		return;
	}
	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/f.txt", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("old\n", f);
	fclose(f);
	TAP_ASSERT(t, chmod(dir, 0500) == 0);	/* read + execute, no write */

	tx = text_new();
	TAP_ASSERT(t, tx != NULL);
	tx_fill(tx, L, 1);
	tx->final_newline = 1;
	TAP_CHECK(t, text_save(tx, path) == OK);	/* direct-write fallback */

	chmod(dir, 0700);			/* restore so we can read/clean up */
	{
		Text *rd = text_new();

		TAP_ASSERT(t, rd && text_load(rd, path) == OK);
		TAP_CHECK(t, dump_is(rd, "updated"));
		text_free(rd);
	}
	text_free(tx);
	unlink(path);
	rmdir(dir);
}

/* With backups on, a save copies the previous version to a "~" file first. */
static void
t_backup_save(Test *t)
{
	char dir[] = "/tmp/vedit_bkXXXXXX";
	char path[PATH_MAX], backup[PATH_MAX];
	static const char *const NEW[] = { "new" };
	Editor e;
	FILE *f;
	Text *rd;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/f.txt", dir);
	f = fopen(path, "wb");
	TAP_ASSERT(t, f != NULL);
	fputs("old\n", f);
	fclose(f);

	editor_init(&e);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	tx_fill(e.t, NEW, 1);
	e.t->final_newline = 1;
	snprintf(e.path, sizeof(e.path), "%s", path);
	e.has_name = 1;
	e.backup_enabled = 1;
	TAP_CHECK(t, ed_save_file(&e) == OK);

	rd = text_new();
	TAP_ASSERT(t, rd && text_load(rd, path) == OK);
	TAP_CHECK(t, dump_is(rd, "new"));		/* target updated */
	text_free(rd);

	TAP_ASSERT(t, backup_path_for(path, backup, sizeof(backup)));
	rd = text_new();
	TAP_ASSERT(t, rd && text_load(rd, backup) == OK);
	TAP_CHECK(t, dump_is(rd, "old"));		/* previous kept */
	text_free(rd);

	text_free(e.t);
	unlink(backup);
	unlink(path);
	rmdir(dir);
}

/* swap_write lays down a parseable snapshot; a save then clears it. */
static void
t_swap_write_clear(Test *t)
{
	char dir[] = "/tmp/vedit_swXXXXXX";
	char path[PATH_MAX], line[256];
	static const char *const L[] = { "swapme" };
	Editor e;
	FILE *fp;
	Text *rd;
	long body;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/s.txt", dir);
	editor_init(&e);
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	tx_fill(e.t, L, 1);
	e.t->final_newline = 1;
	snprintf(e.path, sizeof(e.path), "%s", path);
	e.has_name = 1;
	swap_write(&e);
	TAP_CHECK(t, e.swap_on && e.swap_path[0]);
	TAP_CHECK(t, access(e.swap_path, F_OK) == 0);

	/* parse the swap header, then load its body back */
	fp = fopen(e.swap_path, "rb");
	TAP_ASSERT(t, fp != NULL);
	TAP_ASSERT(t, fgets(line, sizeof(line), fp) &&
	    strncmp(line, SWAP_MAGIC, strlen(SWAP_MAGIC)) == 0);
	while (fgets(line, sizeof(line), fp) && line[0] != '\n')
		;
	body = ftell(fp);
	rd = text_new();
	TAP_ASSERT(t, rd && fseek(fp, body, SEEK_SET) == 0);
	TAP_CHECK(t, text_load_fp(rd, fp) == OK);
	TAP_CHECK(t, dump_is(rd, "swapme"));
	text_free(rd);
	fclose(fp);

	/* a successful save removes the now-redundant swap */
	TAP_CHECK(t, ed_save_file(&e) == OK);
	TAP_CHECK(t, !e.swap_on);
	TAP_CHECK(t, access(e.swap_path, F_OK) != 0);

	text_free(e.t);
	unlink(path);
	rmdir(dir);
}

/* The crash/OOM flush writes a swap for every dirty buffer: the active one from
 * the flat editor state and the parked ones from their slots (deriving a swap
 * path when the slot never had one). */
static void
t_swap_flush_all(Test *t)
{
	char dir[] = "/tmp/vedit_flushXXXXXX";
	char pa[PATH_MAX], pb[PATH_MAX], spa[PATH_MAX], spb[PATH_MAX], line[256];
	static const char *const A[] = { "active-dirty" };
	static const char *const B[] = { "parked-dirty" };
	Editor e;
	Buf bufs[2];
	FILE *fp;
	Text *rd;
	long body;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(pa, sizeof(pa), "%s/a.txt", dir);
	snprintf(pb, sizeof(pb), "%s/b.txt", dir);

	editor_init(&e);
	memset(bufs, 0, sizeof(bufs));
	e.bufs = bufs;
	e.nbuf = 2;
	e.cur = 0;

	/* buffer 0: active, named, dirty -- flushed from the flat state */
	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	tx_fill(e.t, A, 1);
	e.t->final_newline = 1;
	e.has_name = 1;
	snprintf(e.path, sizeof(e.path), "%s", pa);
	bufs[0].t = e.t;
	bufs[0].has_name = 1;
	snprintf(bufs[0].path, sizeof(bufs[0].path), "%s", pa);

	/* buffer 1: parked, named, dirty, no swap path yet -- derived on flush */
	bufs[1].t = text_new();
	TAP_ASSERT(t, bufs[1].t != NULL);
	tx_fill(bufs[1].t, B, 1);
	bufs[1].t->final_newline = 1;
	bufs[1].has_name = 1;
	snprintf(bufs[1].path, sizeof(bufs[1].path), "%s", pb);

	TAP_ASSERT(t, swap_path_for(pa, spa, sizeof(spa)));
	TAP_ASSERT(t, swap_path_for(pb, spb, sizeof(spb)));

	swap_flush_all(&e);

	TAP_CHECK(t, access(spa, F_OK) == 0);	/* active buffer flushed */
	TAP_CHECK(t, access(spb, F_OK) == 0);	/* parked buffer flushed */

	/* the parked snapshot carries that buffer's text, not the active one's */
	fp = fopen(spb, "rb");
	TAP_ASSERT(t, fp != NULL);
	TAP_ASSERT(t, fgets(line, sizeof(line), fp) &&
	    strncmp(line, SWAP_MAGIC, strlen(SWAP_MAGIC)) == 0);
	while (fgets(line, sizeof(line), fp) && line[0] != '\n')
		;
	body = ftell(fp);
	rd = text_new();
	TAP_ASSERT(t, rd && fseek(fp, body, SEEK_SET) == 0);
	TAP_CHECK(t, text_load_fp(rd, fp) == OK);
	TAP_CHECK(t, dump_is(rd, "parked-dirty"));
	text_free(rd);
	fclose(fp);

	text_free(bufs[0].t);
	text_free(bufs[1].t);
	unlink(spa);
	unlink(spb);
	unlink(pa);
	unlink(pb);
	rmdir(dir);
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

/* A long editing session exercising insert, delete, split, join, grouped undo,
 * a full undo/redo sweep, and a save/reload round-trip, checking byte-exact
 * content at every milestone. This is the data-integrity guard for the editor:
 * nothing it does should silently drop or corrupt a byte. */
static void
t_edit_roundtrip(Test *t)
{
	Text *tx = text_new(), *re = NULL;
	size_t line = 0, col = 0, len = 0;
	char path[64];
	char *final = NULL, *reloaded = NULL;

	TAP_ASSERT(t, tx != NULL);

	/* Build three lines from nothing with inserts and splits. */
	TAP_CHECK(t, text_insert(tx, 0, 0, "The quick", 9) == OK);
	TAP_CHECK(t, text_split(tx, 0, 9) == OK);	/* open line 1 */
	TAP_CHECK(t, text_insert(tx, 1, 0, "brown fox", 9) == OK);
	TAP_CHECK(t, text_split(tx, 1, 9) == OK);	/* open line 2 */
	TAP_CHECK(t, text_insert(tx, 2, 0, "jumps over", 10) == OK);
	TAP_CHECK(t, dump_is(tx, "The quick\nbrown fox\njumps over"));

	/* Insert inside a line, then delete part of another. */
	TAP_CHECK(t, text_insert(tx, 0, 3, " very", 5) == OK);	/* "The very quick" */
	TAP_CHECK(t, dump_is(tx, "The very quick\nbrown fox\njumps over"));
	TAP_CHECK(t, text_delete(tx, 2, 5, 5) == OK);		/* drop " over" */
	TAP_CHECK(t, dump_is(tx, "The very quick\nbrown fox\njumps"));

	/* Join line 1 onto line 0, splitting the three lines down to two. */
	TAP_CHECK(t, text_join(tx, 1) == OK);
	TAP_CHECK(t, dump_is(tx, "The very quick\nbrown foxjumps"));

	/* A grouped edit must undo and redo as a single unit. */
	text_undo_group_begin(tx);
	TAP_CHECK(t, text_insert(tx, 0, 0, "[", 1) == OK);
	TAP_CHECK(t, text_insert(tx, 0, 1, "]", 1) == OK);
	text_undo_group_end(tx);
	TAP_CHECK(t, dump_is(tx, "[]The very quick\nbrown foxjumps"));
	TAP_CHECK(t, text_undo(tx, &line, &col) == OK);		/* both inserts */
	TAP_CHECK(t, dump_is(tx, "The very quick\nbrown foxjumps"));
	TAP_CHECK(t, text_redo(tx, &line, &col) == OK);
	TAP_CHECK(t, dump_is(tx, "[]The very quick\nbrown foxjumps"));

	final = text_dump(tx, &len);
	TAP_ASSERT(t, final != NULL);

	/* Undo everything: the buffer must return to a single empty line. */
	while (text_undo(tx, &line, &col) == OK)
		;
	TAP_CHECK(t, text_lines(tx) == 1 && dump_is(tx, ""));

	/* Redo everything: byte-identical to the state before the undo sweep. */
	while (text_redo(tx, &line, &col) == OK)
		;
	TAP_CHECK(t, dump_is(tx, final));

	/* Save and reload: the bytes must survive a disk round-trip intact. */
	snprintf(path, sizeof(path), "/tmp/vedit_rtXXXXXX");
	{
		int fd = mkstemp(path);

		TAP_ASSERT(t, fd >= 0);
		close(fd);
	}
	TAP_ASSERT(t, text_save(tx, path) == OK);
	re = text_new();
	TAP_ASSERT(t, re != NULL && text_load(re, path) == OK);
	reloaded = text_dump(re, &len);
	TAP_ASSERT(t, reloaded != NULL);
	TAP_CHECKF(t, strcmp(final, reloaded) == 0,
	    "reload mismatch:\n  saved=[%s]\n  read =[%s]", final, reloaded);

	remove(path);
	free(final);
	free(reloaded);
	text_free(tx);
	text_free(re);
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

/* Per-project keys may be overridden by VEDIT_<KEY> environment variables:
 * env wins over the config, an empty var is treated as unset, and the key's dots
 * map to underscores with the whole name uppercased. */
static void
t_cfg_env(Test *t)
{
	static const char *text = "[tags]\nfile = from_config\n";
	char path[256];
	Cfg *c = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;

	TAP_ASSERT(t, c != NULL);
	g_cfg = c;
	unsetenv("VEDIT_TAGS_FILE");

	/* config value when no env var is set */
	TAP_CHECK(t, cfg_proj_get("tags.file") &&
	    strcmp(cfg_proj_get("tags.file"), "from_config") == 0);

	/* the env var wins over the config */
	setenv("VEDIT_TAGS_FILE", "from_env", 1);
	TAP_CHECK(t, strcmp(cfg_proj_get("tags.file"), "from_env") == 0);

	/* a dotted key maps '.' -> '_' and uppercases: command.c.build */
	setenv("VEDIT_COMMAND_C_BUILD", "make -j", 1);
	TAP_CHECK(t, cfg_proj_get("command.c.build") &&
	    strcmp(cfg_proj_get("command.c.build"), "make -j") == 0);
	unsetenv("VEDIT_COMMAND_C_BUILD");

	/* an empty variable is treated as unset: the config value shows again */
	setenv("VEDIT_TAGS_FILE", "", 1);
	TAP_CHECK(t, strcmp(cfg_proj_get("tags.file"), "from_config") == 0);
	unsetenv("VEDIT_TAGS_FILE");

	/* the override works even with no config loaded at all */
	g_cfg = NULL;
	setenv("VEDIT_CC_FILE", "/tmp/cc.json", 1);
	TAP_CHECK(t, cfg_proj_get("cc.file") &&
	    strcmp(cfg_proj_get("cc.file"), "/tmp/cc.json") == 0);
	unsetenv("VEDIT_CC_FILE");
	TAP_CHECK(t, cfg_proj_get("cc.file") == NULL);	/* neither set */

	g_cfg = old;
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
	uint16_t out[16];
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

/* A user grammar can embed one by name with a mid-line end string, matched
 * without regard to case; the outer grammar resumes on the end string with
 * its own rules, and the region carries across lines until it is seen. */
static void
t_jsf_embed(Test *t)
{
	static const char *text =
	    "[language \"mini\"]\n"
	    "[color \"mini\"]\n"
	    "  tag = red\n"
	    "[state \"mini.idle\"]\n"
	    "  color = text\n"
	    "  rule = \"<\" tag recolor\n"
	    "  rule = * idle\n"
	    "[state \"mini.tag\"]\n"
	    "  color = tag\n"
	    "  embed = c\n"
	    "  end = </c>\n"
	    "  endcase = off\n"
	    "  rule = \">\" idle\n"
	    "  rule = * tag\n";
	char path[256];
	Cfg *cfg = load_cfg_text(text, path, sizeof(path));
	const Cfg *old = g_cfg;
	const Syntax *sy, *c;
	uint16_t out[32];
	uint32_t st;
	int lang, tag, txt, gid, type;

	TAP_ASSERT(t, cfg != NULL);
	g_cfg = cfg;
	syntax_load_cfg(&g_user, cfg);
	lang = jsf_find(&g_user, "mini");
	TAP_ASSERT(t, lang >= 0);
	tag = jsf_class_of(lang, "tag");
	txt = jsf_class_of(lang, "text");
	sy = syn_for_ext("mini");
	c = syn_for_ext("c");
	TAP_ASSERT(t, sy && sy->fsm && c && c->fsm && tag > 0);
	gid = syn_gid(c);
	type = fsm_class(c->fsm, "type");

	/* "a<int</C>b": '<' enters tag, which embeds C from the next byte */
	st = syn_line(sy, sy->start, "a<int</C>b", 10, out);
	TAP_CHECKF(t, out[0] == txt && out[1] == tag, "lead [%#x %#x]",
	    out[0], out[1]);
	TAP_CHECKF(t, out[2] == ((gid << 8) | type) && out[4] == out[2],
	    "embedded int [%#x %#x]", out[2], out[4]);
	TAP_CHECKF(t, out[5] == tag && out[8] == tag, "end string [%#x %#x]",
	    out[5], out[8]);
	TAP_CHECKF(t, out[9] == txt, "after -> %#x", out[9]);
	TAP_CHECK(t, st == SYN_PACK(sy->start, 0, 0));

	/* no end on the line: the region carries, and ends on a later line */
	st = syn_line(sy, sy->start, "<int", 4, out);
	TAP_CHECKF(t, SYN_GID(st) == gid, "carry gid %d", SYN_GID(st));
	st = syn_line(sy, st, "char</c>z", 9, out);
	TAP_CHECKF(t, out[0] == ((gid << 8) | type), "line 2 char -> %#x", out[0]);
	TAP_CHECKF(t, out[4] == tag && out[8] == txt, "line 2 tail [%#x %#x]",
	    out[4], out[8]);
	TAP_CHECK(t, st == SYN_PACK(sy->start, 0, 0));

	g_cfg = old;
	syntax_load_cfg(&g_user, NULL);
	vedit_cfg_free(cfg);
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
	uint16_t out[32];
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
	uint16_t out[16];
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
	uint16_t out[16];
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
		{ "rel", EX_RELOAD }, { "reload", EX_RELOAD },
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

/* The lexical path normalizer: ".", "..", and duplicate slashes. */
static void
t_path_normalize(Test *t)
{
	char out[PATH_MAX];

	TAP_CHECK(t, path_normalize("inc/../bar.h", out, sizeof(out)) == 0 &&
	    strcmp(out, "bar.h") == 0);
	TAP_CHECK(t, path_normalize("a//b///c", out, sizeof(out)) == 0 &&
	    strcmp(out, "a/b/c") == 0);
	TAP_CHECK(t, path_normalize("./a/./b", out, sizeof(out)) == 0 &&
	    strcmp(out, "a/b") == 0);
	TAP_CHECK(t, path_normalize("a/b/../../c", out, sizeof(out)) == 0 &&
	    strcmp(out, "c") == 0);
	TAP_CHECK(t, path_normalize("/a/../b", out, sizeof(out)) == 0 &&
	    strcmp(out, "/b") == 0);
	TAP_CHECK(t, path_normalize("/a/../..", out, sizeof(out)) == 0 &&
	    strcmp(out, "/") == 0);	/* ".." cannot climb above root */
	TAP_CHECK(t, path_normalize("../x", out, sizeof(out)) == 0 &&
	    strcmp(out, "../x") == 0);	/* relative ".." is kept */
	TAP_CHECK(t, path_normalize("a/../../x", out, sizeof(out)) == 0 &&
	    strcmp(out, "../x") == 0);
	TAP_CHECK(t, path_normalize(".", out, sizeof(out)) == 0 &&
	    strcmp(out, ".") == 0);
	TAP_CHECK(t, path_normalize("a/b/", out, sizeof(out)) == 0 &&
	    strcmp(out, "a/b") == 0);	/* trailing slash dropped */
	TAP_CHECK(t, path_normalize("/", out, sizeof(out)) == 0 &&
	    strcmp(out, "/") == 0);
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
	    "C:\\src\\foo.c:12:7: error: oops\n"	/* a Windows drive path */
	    "make: *** [all] Error 1\n";

	editor_init(&e);
	sb_append(&e.tool_raw, &e.tool_rawlen, &e.tool_rawcap, out, strlen(out));
	tool_parse_output(&e);

	TAP_CHECKF(t, e.tool_nlines == 5, "lines %d", e.tool_nlines);
	TAP_CHECKF(t, e.tool_nerr == 3, "errors %d", e.tool_nerr);
	if (e.tool_nerr == 3) {
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
		TAP_CHECKF(t, strcmp(e.tool_errs[2].file, "C:\\src\\foo.c") == 0 &&
		    e.tool_errs[2].line == 12 && e.tool_errs[2].col == 7 &&
		    e.tool_errs[2].outline == 3, "err2 %s:%zu:%zu@%d",
		    e.tool_errs[2].file, e.tool_errs[2].line,
		    e.tool_errs[2].col, e.tool_errs[2].outline);
	}
	tool_free(&e);
}

/* User error.pattern regexes augment the built-ins and are tried first, so they
 * can parse formats gcc/clang do not. Group 1 = file, 2 = line, 3 = column. */
static void
t_tool_pattern(Test *t)
{
	static const char *cfgtext =
	    "[error]\n"
	    "pattern = ^([^(]+)\\(([0-9]+),([0-9]+)\\): \n"	/* MSVC/TS form */
	    "pattern = File \"([^\"]+)\", line ([0-9]+)\n";	/* no column */
	char cfgpath[256];
	Cfg *c = load_cfg_text(cfgtext, cfgpath, sizeof(cfgpath));
	const Cfg *old = g_cfg;
	Editor e;
	const char *out =
	    "main.c:10:5: error: boom\n"		/* still the built-in */
	    "widget.ts(12,5): error TS2322: bad\n"	/* config, with column */
	    "  File \"app.py\", line 42\n";		/* config, no column */

	TAP_ASSERT(t, c != NULL);
	g_cfg = c;
	editor_init(&e);
	sb_append(&e.tool_raw, &e.tool_rawlen, &e.tool_rawcap, out, strlen(out));
	tool_parse_output(&e);

	TAP_CHECKF(t, e.tool_nerr == 3, "errors %d", e.tool_nerr);
	if (e.tool_nerr == 3) {
		TAP_CHECK(t, strcmp(e.tool_errs[0].file, "main.c") == 0 &&
		    e.tool_errs[0].line == 10 && e.tool_errs[0].col == 5);
		TAP_CHECK(t, strcmp(e.tool_errs[1].file, "widget.ts") == 0 &&
		    e.tool_errs[1].line == 12 && e.tool_errs[1].col == 5);
		TAP_CHECK(t, strcmp(e.tool_errs[2].file, "app.py") == 0 &&
		    e.tool_errs[2].line == 42 && e.tool_errs[2].col == 0);
	}
	tool_free(&e);
	g_cfg = old;
	vedit_cfg_free(c);
	unlink(cfgpath);
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

/* tool_strip_ctl drops CSI and OSC sequences and carriage returns. */
static void
t_tool_strip(Test *t)
{
	static const char in[] =
	    "\033[1m\033[Kx.c:1:2:\033[m \033]0;title\007y\r\n\033]2;t\033\\z\033Mq";
	char out[sizeof(in)];
	size_t n = tool_strip_ctl(in, sizeof(in) - 1, out);

	out[n] = '\0';
	TAP_CHECKF(t, strcmp(out, "x.c:1:2: y\nzq") == 0, "got '%s'", out);
	/* a truncated sequence at the end is dropped, not read past */
	n = tool_strip_ctl("ab\033[3", 5, out);
	out[n] = '\0';
	TAP_CHECKF(t, strcmp(out, "ab") == 0, "got '%s'", out);
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

/* Buffer de-duplication identity: the same file reached by a different spelling,
 * a symlink, or a hard link is one file; distinct or not-yet-saved names are not. */
static void
t_buf_same_file(Test *t)
{
	char dir[] = "/tmp/vedit_bufXXXXXX";
	char a[PATH_MAX], b[PATH_MAX], dot[PATH_MAX], sym[PATH_MAX], hard[PATH_MAX];
	FILE *f;

	TAP_ASSERT(t, mkdtemp(dir) != NULL);
	snprintf(a, sizeof(a), "%s/a.c", dir);
	snprintf(b, sizeof(b), "%s/b.c", dir);
	snprintf(dot, sizeof(dot), "%s/./a.c", dir);
	snprintf(sym, sizeof(sym), "%s/link.c", dir);
	snprintf(hard, sizeof(hard), "%s/hard.c", dir);

	f = fopen(a, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("x\n", f);
	fclose(f);
	f = fopen(b, "w");
	TAP_ASSERT(t, f != NULL);
	fputs("y\n", f);
	fclose(f);
	TAP_ASSERT(t, symlink(a, sym) == 0);
	TAP_ASSERT(t, link(a, hard) == 0);

	/* identical strings match without needing the disk */
	TAP_CHECK(t, buf_same_file(a, a) == 1);
	TAP_CHECK(t, buf_same_file("/no/such/x", "/no/such/x") == 1);

	/* different spellings of one file match by device + inode */
	TAP_CHECK(t, buf_same_file(a, dot) == 1);	/* a "./" detour */
	TAP_CHECK(t, buf_same_file(a, sym) == 1);	/* a symlink */
	TAP_CHECK(t, buf_same_file(a, hard) == 1);	/* a hard link */

	/* distinct files, or names not on disk, do not match */
	TAP_CHECK(t, buf_same_file(a, b) == 0);
	TAP_CHECK(t, buf_same_file(a, "/no/such/y") == 0);
	TAP_CHECK(t, buf_same_file("/no/such/x", "/no/such/y") == 0);

	unlink(hard);
	unlink(sym);
	unlink(b);
	unlink(a);
	rmdir(dir);
}

/* Switching buffers clears the outgoing buffer's transient status message, so a
 * message like a terminal's "Ctrl-W q to close" does not linger after the move.
 * buf_load is the single mirror point, so this also covers closing a buffer. */
static void
t_buf_switch_clears_status(Test *t)
{
	Editor e;
	Buf bufs[2];

	editor_init(&e);
	memset(bufs, 0, sizeof(bufs));
	e.bufs = bufs;
	e.nbuf = 2;
	e.cur = 0;
	e.swap_enabled = 0;

	e.t = text_new();
	TAP_ASSERT(t, e.t != NULL);
	bufs[0].t = e.t;
	bufs[1].t = text_new();
	TAP_ASSERT(t, bufs[1].t != NULL);

	set_status(&e, "stale message");
	TAP_CHECK(t, e.status[0] != '\0');

	buf_switch(&e, 1);
	TAP_CHECKF(t, e.status[0] == '\0', "status not cleared: %s", e.status);

	text_free(bufs[0].t);
	text_free(bufs[1].t);
}

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

/* ---- the table view's parser ---- */

static void
t_tbl_fields(Test *t)
{
	static const char rec[] = "a,\"b,c\",\"d\"\"e\",,f";
	Tblfield f[8];
	char val[32];
	int n;

	n = tbl_fields(',', rec, sizeof(rec) - 1, f, 8);
	TAP_CHECKF(t, n == 5, "%d fields", n);
	TAP_CHECK(t, f[0].off == 0 && f[0].len == 1 && !f[0].quoted);
	TAP_CHECK(t, f[1].off == 2 && f[1].len == 5 && f[1].quoted);
	TAP_CHECK(t, f[3].len == 0 && f[4].off == 16 && f[4].len == 1);
	TAP_CHECK(t, tbl_unquote(&f[1], rec, val, sizeof(val)) == 3 &&
	    strcmp(val, "b,c") == 0);
	TAP_CHECK(t, tbl_unquote(&f[2], rec, val, sizeof(val)) == 3 &&
	    strcmp(val, "d\"e") == 0);
	TAP_CHECK(t, tbl_unquote(&f[3], rec, val, sizeof(val)) == 0 && val[0] == '\0');

	/* the count is right even when the array is too small */
	TAP_CHECK(t, tbl_fields(',', rec, sizeof(rec) - 1, f, 2) == 5);
	/* an empty line is one empty field */
	TAP_CHECK(t, tbl_fields(',', "", 0, f, 8) == 1 && f[0].len == 0);
	/* a tab file never quotes */
	n = tbl_fields('\t', "\"x\ty\"\tz", 7, f, 8);
	TAP_CHECK(t, n == 3 && !f[0].quoted && f[0].len == 2);
	/* an unterminated quote runs to the end of the line */
	n = tbl_fields(',', "a,\"open,x", 9, f, 8);
	TAP_CHECK(t, n == 2 && f[1].len == 7);
	TAP_CHECK(t, tbl_unquote(&f[1], "a,\"open,x", val, sizeof(val)) == 6 &&
	    strcmp(val, "open,x") == 0);
	/* a short output buffer still reports the full length */
	TAP_CHECK(t, tbl_unquote(&f[1], "a,\"open,x", val, 3) == 6 &&
	    strcmp(val, "op") == 0);
}

static void
t_tbl_label(Test *t)
{
	static const struct { int col; const char *s; } cases[] = {
		{ 0, "A" }, { 1, "B" }, { 25, "Z" }, { 26, "AA" }, { 27, "AB" },
		{ 51, "AZ" }, { 52, "BA" }, { 701, "ZZ" }, { 702, "AAA" },
		{ 18277, "ZZZ" }, { 18278, "AAAA" },
	};
	char buf[TBL_LABEL_MAX];
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		tbl_label(cases[i].col, buf, sizeof(buf));
		TAP_CHECKF(t, strcmp(buf, cases[i].s) == 0, "%d -> %s, want %s",
		    cases[i].col, buf, cases[i].s);
	}
	tbl_label(-1, buf, sizeof(buf));
	TAP_CHECK(t, buf[0] == '\0');
	tbl_label(27, buf, 2);			/* truncates, stays terminated */
	TAP_CHECK(t, strcmp(buf, "A") == 0);
}

static void
t_tbl_sniff(Test *t)
{
	static const char *const semi[] = { "a;b;c", "1;2;3", "x,y;z;w" };
	static const char *const tabs[] = { "a\tb", "1\t2", "", "3\t4" };
	static const char *const none[] = { "just text", "more" };
	static const char *const mixed[] = { "a,b;c", "1,2,3", "4,5" };
	Text *tx = text_new();

	TAP_ASSERT(t, tx != NULL);
	tx_fill(tx, semi, 3);
	TAP_CHECK(t, tbl_sniff(tx) == ';');
	text_free(tx);
	tx = text_new();
	tx_fill(tx, tabs, 4);
	TAP_CHECK(t, tbl_sniff(tx) == '\t');
	text_free(tx);
	tx = text_new();
	tx_fill(tx, none, 2);
	TAP_CHECK(t, tbl_sniff(tx) == ',');
	text_free(tx);
	tx = text_new();
	tx_fill(tx, mixed, 3);			/* commas: more, not consistent */
	TAP_CHECK(t, tbl_sniff(tx) == ',');
	text_free(tx);
}

static void
t_tbl_join(Test *t)
{
	static const char *const L[] = {
		"id,note", "1,\"first line", "second line\",x", "2,plain",
		"3,\"a \"\"quoted\"\" word\"", "4,a\"b,c" };
	Text *tx = text_new();
	size_t len;
	const char *s;
	int i;

	TAP_ASSERT(t, tx != NULL);
	tx_fill(tx, L, 6);
	text_set_eol(tx, EOL_CRLF);
	tx->dirty = 0;
	TAP_CHECK(t, tbl_join_records(tx, ',') == 0);
	TAP_CHECKF(t, text_lines(tx) == 5, "%zu lines after the join", text_lines(tx));
	s = text_line(tx, 1, &len);
	TAP_CHECKF(t, len == 29 && memcmp(s, "1,\"first line\r\nsecond line\",x", 29) == 0,
	    "joined [%.*s]", (int)len, s);
	TAP_CHECK(t, !text_dirty(tx));	/* a representation change, not an edit */
	/* a quote inside an unquoted field does not open one (line 4 stays) */
	s = text_line(tx, 4, &len);
	TAP_CHECK(t, len == 7 && memcmp(s, "4,a\"b,c", 7) == 0);
	/* joining again is a no-op */
	TAP_CHECK(t, tbl_join_records(tx, ',') == 0 && text_lines(tx) == 5);
	text_free(tx);

	/* a stray quote cannot swallow the file: the join stops at the cap */
	tx = text_new();
	TAP_ASSERT(t, tx != NULL);
	text_insert(tx, 0, 0, "a,\"stray", 8);
	for (i = 0; i < TBL_JOIN_MAX + 10; i++) {
		text_split(tx, (size_t)i, text_line_len(tx, (size_t)i));
		text_insert(tx, (size_t)i + 1, 0, "n,m", 3);
	}
	TAP_CHECK(t, tbl_join_records(tx, ',') == 1);
	TAP_CHECKF(t, text_lines(tx) == (size_t)(TBL_JOIN_MAX + 11) - (TBL_JOIN_MAX - 1),
	    "%zu lines left", text_lines(tx));
	text_free(tx);
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
	{ "syntax_md", t_syntax_md },
	{ "syntax_md_embed", t_syntax_md_embed },
	{ "syntax_js", t_syntax_js },
	{ "syntax_html", t_syntax_html },
	{ "syntax_ini", t_syntax_ini },
	{ "mkdir_p", t_mkdir_p },
	{ "cli_config_path", t_cli_config_path },
	{ "entry_scroll", t_entry_scroll },
	{ "text_fp_roundtrip", t_text_fp_roundtrip },
	{ "tbl_fields", t_tbl_fields },
	{ "tbl_label", t_tbl_label },
	{ "tbl_sniff", t_tbl_sniff },
	{ "tbl_join", t_tbl_join },
	{ "swap_paths", t_swap_paths },
	{ "atomic_save", t_atomic_save },
	{ "save_rodir_fallback", t_save_rodir_fallback },
	{ "backup_save", t_backup_save },
	{ "swap_write_clear", t_swap_write_clear },
	{ "swap_flush_all", t_swap_flush_all },
	{ "text_edit_undo", t_text_edit_undo },
	{ "edit_roundtrip", t_edit_roundtrip },
	{ "multiline_buffer", t_multiline_buffer },
	{ "pick_fit", t_pick_fit },
	{ "filepick_cmp", t_filepick_cmp },
	{ "pick_jump", t_pick_jump },
	{ "filepick_load", t_filepick_load },
	{ "filepick_start_dir", t_filepick_start_dir },
	{ "cfg_parse", t_cfg_parse },
	{ "cfg_resolve", t_cfg_resolve },
	{ "cfg_env", t_cfg_env },
	{ "cfg_color", t_cfg_color },
	{ "cfg_theme", t_cfg_theme },
	{ "jsf_charset", t_jsf_charset },
	{ "jsf_highlight", t_jsf_highlight },
	{ "jsf_embed", t_jsf_embed },
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
	{ "path_normalize", t_path_normalize },
	{ "include_target", t_include_target },
	{ "cc_split", t_cc_split },
	{ "cc_db", t_cc_db },
	{ "cc_cache", t_cc_cache },
#ifndef VEDIT_NO_TOOLS
	{ "tool_sev", t_tool_sev },
	{ "tool_nav", t_tool_nav },
	{ "tool_expand", t_tool_expand },
	{ "tool_parse", t_tool_parse },
	{ "tool_pattern", t_tool_pattern },
	{ "tool_run", t_tool_run },
	{ "tool_strip", t_tool_strip },
#endif
	{ "buf_same_file", t_buf_same_file },
	{ "buf_switch_clears_status", t_buf_switch_clears_status },
	{ "bufpick", t_bufpick },
	{ "sym_classify", t_sym_classify },
	{ "symscan", t_symscan },
	{ "isearch", t_isearch },
	{ NULL, NULL },
};
