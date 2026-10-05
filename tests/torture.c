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

/* ---- editing: a differential model with full undo/redo replay ---- */

/* A flat reference document: the buffer content as one byte string, lines
 * joined by '\n'. It is the oracle the Text buffer is checked against. */
struct doc {
	char	*b;
	size_t	 len;
	size_t	 cap;
};

static int
doc_reserve(struct doc *d, size_t need)
{
	size_t nc;
	char *nb;

	if (need <= d->cap)
		return 0;
	nc = d->cap ? d->cap : 32;
	while (nc < need)
		nc *= 2;
	nb = realloc(d->b, nc);
	if (!nb)
		return -1;
	d->b = nb;
	d->cap = nc;
	return 0;
}

/* Count lines: one more than the number of newlines. */
static size_t
doc_nlines(const struct doc *d)
{
	size_t i, n = 1;

	for (i = 0; i < d->len; i++)
		if (d->b[i] == '\n')
			n++;
	return n;
}

/* Byte offset where line begins and its length (to the next '\n' or the end). */
static void
doc_lineinfo(const struct doc *d, size_t line, size_t *start, size_t *llen)
{
	size_t i, ln = 0, s = 0;

	for (i = 0; i < d->len; i++) {
		if (d->b[i] == '\n') {
			if (ln == line) {
				*start = s;
				*llen = i - s;
				return;
			}
			ln++;
			s = i + 1;
		}
	}
	*start = s;				/* the last line */
	*llen = d->len - s;
}

/* Remove del bytes at off and insert nins bytes there, mirroring an edit. */
static int
doc_splice(struct doc *d, size_t off, size_t del, const char *ins, size_t nins)
{
	if (doc_reserve(d, d->len - del + nins + 1) != 0)
		return -1;
	memmove(d->b + off + nins, d->b + off + del, d->len - off - del);
	if (nins)
		memcpy(d->b + off, ins, nins);
	d->len = d->len - del + nins;
	return 0;
}

/* Serialize the Text buffer the way doc stores it; caller frees. */
static char *
text_serialize(Text *t, size_t *lenout)
{
	size_t n = text_lines(t), i, len = 0, cap = 64;
	char *s = malloc(cap);

	if (!s)
		return NULL;
	for (i = 0; i < n; i++) {
		size_t ll = 0;
		const char *l = text_line(t, i, &ll);

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
	*lenout = len;
	return s;
}

/* Abort if the Text buffer and the model have diverged. */
static void
edit_check(Text *t, const struct doc *d, const char *where)
{
	size_t tl = 0;
	char *ts = text_serialize(t, &tl);

	if (!ts)
		return;				/* out of memory: skip the check */
	if (tl != d->len || (tl && memcmp(ts, d->b, tl) != 0)) {
		fprintf(stderr, "edit: %s: buffer diverged from model "
		    "(buffer %zu bytes, model %zu)\n", where, tl, d->len);
		abort();
	}
	free(ts);
}

/* Drive a random run of primitive edits against both the Text buffer and the
 * flat model, checking they agree after every edit, then undo the whole run and
 * redo it, checking the buffer against a snapshot of every intermediate state.
 * This is the core data-integrity fuzzer: an editor must never drop or corrupt
 * a byte, and every edit must be exactly reversible. */
static void
fuzz_edit(void)
{
	enum { MAXSNAP = 64 };
	Text *t = text_new();
	struct doc d = { NULL, 0, 0 };
	char *snap[MAXSNAP];
	size_t snaplen[MAXSNAP];
	int nsnap = 0, j;
	size_t nops = 1 + rnd_below(40), i;

	if (!t)
		return;
	snap[nsnap] = malloc(1);		/* the initial empty state */
	if (!snap[nsnap]) {
		text_free(t);
		return;
	}
	snaplen[nsnap++] = 0;

	for (i = 0; i < nops && nsnap < MAXSNAP; i++) {
		size_t nlines = doc_nlines(&d);
		size_t line = rnd_below(nlines);
		size_t start = 0, llen = 0, col;
		int did = 0;

		doc_lineinfo(&d, line, &start, &llen);
		col = rnd_below(llen + 1);

		switch (rnd_below(4)) {
		case 0: {				/* insert */
			char ins[8];
			size_t k, n = 1 + rnd_below(sizeof(ins));

			for (k = 0; k < n; k++) {
				int c;

				do {
					c = (int)(rnd() & 0x7f);
				} while (c == '\n' || c == '\0');
				ins[k] = (char)c;
			}
			if (text_insert(t, line, col, ins, n) == OK)
				did = doc_splice(&d, start + col, 0, ins, n) == 0;
			break;
		}
		case 1:					/* delete within the line */
			if (llen - col >= 1) {
				size_t avail = llen - col;
				size_t n = 1 + rnd_below(avail + 2); /* may over-ask */
				size_t eff = n > avail ? avail : n;  /* clamp like the op */

				if (text_delete(t, line, col, n) == OK)
					did = doc_splice(&d, start + col, eff,
					    NULL, 0) == 0;
			}
			break;
		case 2:					/* split the line */
			if (text_split(t, line, col) == OK)
				did = doc_splice(&d, start + col, 0, "\n", 1) == 0;
			break;
		case 3:					/* join with the next line */
			if (line + 1 < nlines && text_join(t, line) == OK)
				did = doc_splice(&d, start + llen, 1, NULL, 0) == 0;
			break;
		}

		if (!did)
			continue;
		edit_check(t, &d, "forward");
		text_undo_boundary(t);			/* keep undo steps 1:1 */
		snap[nsnap] = malloc(d.len ? d.len : 1);
		if (!snap[nsnap])
			break;
		if (d.len)
			memcpy(snap[nsnap], d.b, d.len);
		snaplen[nsnap++] = d.len;
	}

	/* Undo every recorded edit, checking the buffer against each prior state. */
	for (j = nsnap - 1; j > 0; j--) {
		size_t ln = 0, cl = 0, tl = 0;
		char *ts;

		if (text_undo(t, &ln, &cl) != OK) {
			fprintf(stderr, "edit: undo exhausted early at step %d\n", j);
			abort();
		}
		ts = text_serialize(t, &tl);
		if (ts && (tl != snaplen[j - 1] ||
		    (tl && memcmp(ts, snap[j - 1], tl) != 0))) {
			fprintf(stderr, "edit: undo to state %d mismatch\n", j - 1);
			abort();
		}
		free(ts);
	}

	/* Redo it all, checking the buffer against each forward state again. */
	for (j = 1; j < nsnap; j++) {
		size_t ln = 0, cl = 0, tl = 0;
		char *ts;

		if (text_redo(t, &ln, &cl) != OK) {
			fprintf(stderr, "edit: redo exhausted early at step %d\n", j);
			abort();
		}
		ts = text_serialize(t, &tl);
		if (ts && (tl != snaplen[j] ||
		    (tl && memcmp(ts, snap[j], tl) != 0))) {
			fprintf(stderr, "edit: redo to state %d mismatch\n", j);
			abort();
		}
		free(ts);
	}

	for (j = 0; j < nsnap; j++)
		free(snap[j]);
	free(d.b);
	text_free(t);
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
		fuzz_edit();
	}

	printf("torture: %ld rounds, seed %llu, ok\n", rounds, seed);
	return 0;
}
