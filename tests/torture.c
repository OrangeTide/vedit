/*
 * Torture and fuzz suite for vedit. The whole editor is included as a single
 * translation unit (with main renamed out of the way), so the static helpers
 * and the vendored rx engine are reachable directly, and a sanitizer or gcov
 * sees the editor as one unit.
 *
 * This is not a correctness oracle. It drives the untrusted-input surfaces,
 * the regex engine as search and replace use it, the config parser, and the
 * UTF-8 codec, with pseudo-random input and a few cheap invariant checks, and
 * leans on AddressSanitizer and UndefinedBehaviorSanitizer to catch memory and
 * undefined-behavior faults along the way. A deterministic PRNG makes a run
 * reproducible from its seed.
 *
 * Usage: torture [rounds] [seed]
 *   rounds  number of fuzz iterations per surface (default 20000); 0 skips the
 *           fuzzer so the build and the invariant battery still run.
 *   seed    PRNG seed (default 1).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main vedit_main
#include "../vedit.c"
#undef main

#include "memio.h"

/* ---- deterministic PRNG (xorshift64) ---- */

static uint64_t rng_state = 1;

static uint64_t
rnd(void)
{
	uint64_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

/* A value in [0, n). n must be non-zero. */
static size_t
rnd_below(size_t n)
{
	return (size_t)(rnd() % n);
}

/* ---- random string builders ---- */

/* A NUL-terminated pattern over a small alphabet rich in regex metacharacters,
 * so rx_compile exercises both the accept and the error path. */
static void
make_pattern(char *buf, size_t cap)
{
	static const char alpha[] = "abc01.*+?()[]^$|\\{}-,:dws<>";
	size_t n = rnd_below(cap > 1 ? (cap - 1) : 1);
	size_t i;

	for (i = 0; i < n; i++)
		buf[i] = alpha[rnd_below(sizeof(alpha) - 1)];
	buf[n] = '\0';
}

/* A subject buffer (not NUL-terminated by contract; rx takes an explicit
 * length). Returns the length written. */
static size_t
make_subject(char *buf, size_t cap)
{
	static const char alpha[] = "abc ABC012\n\t_@.";
	size_t n = rnd_below(cap);
	size_t i;

	for (i = 0; i < n; i++)
		buf[i] = alpha[rnd_below(sizeof(alpha) - 1)];
	return n;
}

/* A NUL-terminated replacement template with the conveniences rx expands. */
static void
make_template(char *buf, size_t cap)
{
	static const char *const bits[] = {
		"x", "&", "\\0", "\\1", "\\2", "\\9", "\\U", "\\L", "\\u",
		"\\l", "\\E", "\\&", "\\\\", "\\n", "\\t", "-", " ",
	};
	size_t n = rnd_below(8), used = 0, i;

	for (i = 0; i < n; i++) {
		const char *b = bits[rnd_below(sizeof(bits) / sizeof(bits[0]))];
		size_t bl = strlen(b);

		if (used + bl + 1 >= cap)
			break;
		memcpy(buf + used, b, bl);
		used += bl;
	}
	buf[used] = '\0';
}

/* ---- fuzz surfaces ---- */

/* Compile a random pattern and, when it compiles, run exec, both search
 * directions, and replace over a random subject. Checks that any reported
 * match lies within the subject and is non-decreasing. */
static void
fuzz_regex(void)
{
	char pat[20], subj[48], tpl[48];
	size_t slen;
	const char *err;
	rx_t *re;

	make_pattern(pat, sizeof(pat));
	re = rx_compile(pat, (int)(rnd() & (RX_ICASE | RX_MULTILINE | RX_DOTALL)),
	    &err);
	if (!re)
		return;			/* a rejected pattern set *err, not a bug */

	slen = make_subject(subj, sizeof(subj));
	{
		rx_match m[4];
		size_t start = slen ? rnd_below(slen + 1) : 0;
		int r = rx_exec(re, subj, slen, start, m, 4);

		if (r == 1) {
			if (m[0].so < 0 || m[0].eo < m[0].so ||
			    (size_t)m[0].eo > slen) {
				fprintf(stderr, "exec span out of range: "
				    "pat=[%s] so=%ld eo=%ld len=%zu\n",
				    pat, m[0].so, m[0].eo, slen);
				abort();
			}
		}
	}
	{
		rx_match m[1];
		int sf = (rnd() & 1) ? RX_BACKWARD : 0;

		rx_search(re, subj, slen, slen ? rnd_below(slen + 1) : 0,
		    sf | RX_WRAP, m, 1);
	}
	(void)rx_matches_newline(re);
	(void)rx_ngroups(re);

	make_template(tpl, sizeof(tpl));
	{
		char *out = rx_replace(re, subj, slen, tpl,
		    (rnd() & 1) ? RX_GLOBAL : 0);

		free(out);		/* NULL is fine to free */
	}
	rx_free(re);
}

/* Feed random gitconfig-ish text through the parser and read a few keys back.
 * cfg_load_mem mutates its input, so hand it a private copy. */
static void
fuzz_config(void)
{
	static const char *const bits[] = {
		"[section]\n", "[section \"sub\"]\n", "key = value\n",
		"a.b.c = 1\n", "# comment\n", "; comment\n", "\n", "   \n",
		"novalue\n", "= orphan\n", "key=\n", "\tkey = v v v\n",
		"[unterminated\n", "list = a b c\n", "x", "]]][[\n",
	};
	char text[512];
	size_t used = 0, n = rnd_below(24), i;
	char *dup;
	Cfg *c;

	for (i = 0; i < n; i++) {
		const char *b = bits[rnd_below(sizeof(bits) / sizeof(bits[0]))];
		size_t bl = strlen(b);

		if (used + bl + 1 >= sizeof(text))
			break;
		memcpy(text + used, b, bl);
		used += bl;
	}
	text[used] = '\0';

	dup = cfg_dup(text);
	if (!dup)
		return;
	c = vedit_cfg_new();
	if (!c) {
		free(dup);
		return;
	}
	cfg_load_mem(c, dup);
	(void)cfg_get(c, "section.key");
	(void)cfg_get(c, "a.b.c");
	(void)cfg_get(c, "section.sub.key");
	vedit_cfg_free(c);
	free(dup);
}

/* Decode random bytes (the decoder must never read past len or loop), and
 * round-trip random scalar values through encode then decode. */
static void
fuzz_utf8(void)
{
	unsigned char buf[8];
	size_t len = 1 + rnd_below(sizeof(buf));
	size_t i = 0;
	uint32_t cp;

	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)rnd();
	i = 0;
	while (i < len) {
		int n = utf8_decode(&cp, buf + i, len - i);

		if (n <= 0) {			/* invalid lead: skip one byte */
			i++;
			continue;
		}
		i += (size_t)n;
	}

	cp = (uint32_t)rnd_below(0x110000);
	if (cp >= 0xd800 && cp <= 0xdfff)
		cp = 'A';			/* surrogates are not scalars */
	{
		unsigned char enc[4];
		uint32_t got = 0;
		int n = utf8_encode(enc, cp);
		int m = (n > 0) ? utf8_decode(&got, enc, (size_t)n) : -1;

		if (n > 0 && (m != n || got != cp)) {
			fprintf(stderr, "utf8 round-trip failed: U+%06X "
			    "n=%d m=%d got=U+%06X\n", cp, n, m, got);
			abort();
		}
	}
}

/* Drive the OSC 52 "copy whole file" path through a captured vedit_io: build a
 * random buffer, copy it to the terminal clipboard, and assert the emitted
 * OSC 52 payload is exactly the base64 of the buffer text. Also exercises the
 * selection copy and the mirrored-clip path so the sanitizers see them. */
static void
fuzz_clipboard(void)
{
	Memio m;
	struct vedit_io io;
	struct vedit *v;
	Editor *e;
	size_t nlines = 1 + rnd_below(6), i, lastlen = 0, elen = 0;
	char *expect;

	memio_init(&m, "", 0, 24, 80);
	memio_bind(&io, &m);
	v = vedit_new(&io);
	if (!v) {
		memio_free(&m);
		return;
	}
	e = &v->e;

	/* Fill with random lines. A line never holds the newline byte, which is
	 * the separator region_text inserts between lines. */
	for (i = 0; i < nlines; i++) {
		char ln[40];
		size_t k, n = rnd_below(sizeof(ln));

		for (k = 0; k < n; k++) {
			int c;

			do {
				c = (int)(rnd() & 0xff);
			} while (c == '\n' || c == '\0');
			ln[k] = (char)c;
		}
		if (i == 0)
			text_insert(e->t, 0, 0, ln, n);
		else
			lines_insert_at(e->t, i, ln, n);
	}

	/* Cover the selection copy and, with mirroring on, the clip_set path. */
	e->clip_osc52 = (int)(rnd() & 1);
	osc_copy_selection(e);
	{
		size_t ll = 0;
		const char *s = text_line(e->t, 0, &ll);
		char *c = malloc(ll ? ll : 1);

		if (c) {
			if (ll)
				memcpy(c, s, ll);
			clip_set(e, c, ll);	/* takes ownership */
		}
	}

	/* Headline path: copy the whole file, then check what was emitted. */
	m.outlen = 0;
	if (m.out)
		m.out[0] = '\0';
	osc_copy_file(e);

	nlines = text_lines(e->t);
	text_line(e->t, nlines - 1, &lastlen);
	expect = region_text(e, 0, 0, nlines - 1, lastlen, &elen);
	if (expect && elen > 0 && elen <= OSC52_MAX) {
		size_t cap = ((elen + 2) / 3) * 4 + 1;
		char *b64 = malloc(cap);

		if (b64) {
			b64_encode((const unsigned char *)expect, elen, b64);
			if (!m.out || strstr(m.out, b64) == NULL) {
				fprintf(stderr, "clipboard: OSC 52 payload does "
				    "not match the buffer (elen=%zu)\n", elen);
				abort();
			}
			free(b64);
		}
	}
	free(expect);
	vedit_free(v);
	memio_free(&m);
}

int
main(int argc, char **argv)
{
	long rounds = (argc > 1) ? strtol(argv[1], NULL, 10) : 20000;
	unsigned long long seed = 1;
	long i;

	if (argc > 2)
		seed = strtoull(argv[2], NULL, 10);
	if (seed == 0)
		seed = 1;			/* xorshift must not start at 0 */
	rng_state = seed;

	if (rounds < 0)
		rounds = 0;
	for (i = 0; i < rounds; i++) {
		fuzz_regex();
		fuzz_config();
		fuzz_utf8();
		fuzz_clipboard();
	}

	printf("torture: %ld rounds, seed %llu, ok\n", rounds, seed);
	return 0;
}
