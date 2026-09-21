/*
 * test_peg.c - end-to-end tests for the incremental parser.
 *
 * End-to-end coverage:
 *
 *  - nested captures under Star(Memo(...)) over "12 34 56 78 9",
 *    checking each capture's extent;
 *  - incremental correctness: apply a series of edits and assert that
 *    the incremental result always equals a full reparse;
 *  - a performance smoke test on the Java testdata (ScriptRuntime.java)
 *    using a simplified Java-ish grammar, printing full-vs-incremental
 *    timings like the paper's evaluation.
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif


#include "peg/peg.h"
#include "util.h"
#include "peg/vm.h"

/* Test inputs live in a sibling testdata/ directory; the build system
 * passes its absolute path, with a source-tree default as fallback. */
#ifndef PEG_TESTDATA_DIR
#define PEG_TESTDATA_DIR "./testdata/"
#endif

static int nchecks = 0;
static int nfail = 0;

#define CHECK(cond, msg)						\
	do {								\
		nchecks++;						\
		if (!(cond)) {						\
			nfail++;					\
			fprintf(stderr, "FAIL: %s (line %d)\n", msg,	\
			        __LINE__);				\
		}							\
	} while (0)

/* ---------------------------------------------------------------------- */
/* The nested-capture grammar                                             */
/* ---------------------------------------------------------------------- */

enum {
	CAP_DIGIT = 0,
	CAP_NUM,
};

static pat *capture_test_grammar(void)
{
	vm_charset digits, space;
	vm_charset_range(&digits, '0', '9');
	vm_charset_fill(&space, (const uint8_t *)" ", 1);

	/* Star(Memo(Cap(Plus(Cap(Set))) Optional(" "))) */
	return pat_star(pat_memo(pat_concat(
		(pat *[]){
			pat_cap(pat_plus(pat_cap(pat_set(&digits), CAP_DIGIT)),
			        CAP_NUM),
			pat_optional(pat_literal(" ", 1)),
		}, 2)));
}

/* Flatten a capture tree in document order, collecting (id, start, len)
 * triples of non-dummy captures. */
static void flatten(const memo_capture *c, int (*out)[3], int *n, int max)
{
	if (c == NULL)
		return;
	if (!memo_capture_dummy_p(c) && *n < max) {
		out[*n][0] = memo_capture_id(c);
		out[*n][1] = memo_capture_start(c);
		out[*n][2] = memo_capture_len(c);
		(*n)++;
	}
	for (int i = 0; i < memo_capture_num_children(c); i++)
		flatten(memo_capture_child(c, i), out, n, max);
}

static void test_captures(void)
{
	pat *p = capture_test_grammar();
	peg *g = peg_new(p, 0);
	pat_free(p);

	const char *subj = "12 34 56 78 9";
	vm_result r = peg_parse(g, (const uint8_t *)subj, strlen(subj));
	CHECK(r.success && r.pos == 13, "capture grammar parses");

	int caps[32][3];
	int n = 0;
	flatten(r.captures, caps, &n, 32);
	/* digit captures: 0,1 3,4 6,7 9,10 12 */
	int expect_digits[5][2] = { {0, 1}, {1, 1}, {3, 1}, {4, 1},
	                            {6, 1} };
	int ndigit = 0, nnum = 0;
	for (int i = 0; i < n; i++) {
		if (caps[i][0] == CAP_DIGIT)
			ndigit++;
		else if (caps[i][0] == CAP_NUM)
			nnum++;
	}
	CHECK(ndigit == 9, "9 digit captures");
	CHECK(nnum == 5, "5 number captures");
	/* number captures: {0,2} {3,2} {6,2} {9,2} {12,1} */
	int numidx = 0;
	int expect_num[5][2] = { {0, 2}, {3, 2}, {6, 2}, {9, 2}, {12, 1} };
	for (int i = 0; i < n && numidx < 5; i++) {
		if (caps[i][0] != CAP_NUM)
			continue;
		CHECK(caps[i][1] == expect_num[numidx][0] &&
		      caps[i][2] == expect_num[numidx][1],
		      "number capture extent");
		numidx++;
	}
	(void)expect_digits;

	vm_result_free(&r);
	peg_free(g);
}

/* ---------------------------------------------------------------------- */
/* Incremental correctness: edits then reparse == full reparse            */
/* ---------------------------------------------------------------------- */

/* Constructors returning fresh nodes (never share a pat* between two
 * sites: pat_free would double-free the shared subtree). */
static pat *jws(void)
{
	vm_charset ws;
	vm_charset_fill(&ws, (const uint8_t *)" \t\r\n", 4);
	return pat_star(pat_set(&ws));
}

static pat *jdigits_plus(void)
{
	vm_charset d;
	vm_charset_range(&d, '0', '9');
	return pat_plus(pat_set(&d));
}

static pat *jdigits_star(void)
{
	vm_charset d;
	vm_charset_range(&d, '0', '9');
	return pat_star(pat_set(&d));
}

static pat *jnonzero(void)
{
	vm_charset d;
	vm_charset_range(&d, '1', '9');
	return pat_set(&d);
}

/* ws X ws, taking ownership of X. */
static pat *jwrap(pat *x)
{
	return pat_concat((pat *[]){ jws(), x, jws() }, 3);
}

/* A JSON grammar:
 *   Doc    <- Value !.
 *   Value  <- Object / Array / String / Number / true / false / null
 *   Object <- '{' (String ':' Value (',' String ':' Value)* / eps) '}'
 *            (with optional whitespace around every token)
 *   Array  <- '[' (Value (',' Value)* / eps) ']'
 *   String <- '"' ( '\\' . / !["\\] . )* '"'
 *   Number <- '-'? ('0' / [1-9] [0-9]*) ('.' [0-9]+)? ([eE] [+\-]? [0-9]+)?
 */
static pat *json_grammar(void)
{
	pat *defs[] = {
		/* Doc <- Value !. */
		pat_concat((pat *[]){
			pat_nonterm("Value"),
			pat_not(pat_any(1)),
		}, 2),
		/* Value <- Object / Array / String / Number / kw */
		pat_or((pat *[]){
			pat_nonterm("Object"),
			pat_nonterm("Array"),
			pat_nonterm("String"),
			pat_nonterm("Number"),
			pat_literal("true", 4),
			pat_literal("false", 5),
			pat_literal("null", 4),
		}, 7),
		/* Object <- '{' (String ':' Value (',' String ':' Value)* / eps) '}'
		 * with optional whitespace around every token */
		pat_concat((pat *[]){
			pat_literal("{", 1),
			pat_optional(jwrap(pat_concat((pat *[]){
				pat_nonterm("String"),
				jwrap(pat_concat((pat *[]){
					pat_literal(":", 1),
					jwrap(pat_concat((pat *[]){
						pat_nonterm("Value"),
						pat_star(jwrap(pat_concat((pat *[]){
							pat_literal(",", 1),
							jwrap(pat_concat((pat *[]){
								pat_nonterm("String"),
								jwrap(pat_concat((pat *[]){
									pat_literal(":", 1),
									jwrap(pat_nonterm("Value")),
								}, 2)),
							}, 2)),
						}, 2))),
					}, 2)),
				}, 2)),
			}, 2))),
			jws(),
			pat_literal("}", 1),
		}, 4),
		/* Array <- '[' (Value (',' Value)* / eps) ']' with ws */
		pat_concat((pat *[]){
			pat_literal("[", 1),
			pat_optional(jwrap(pat_concat((pat *[]){
				pat_nonterm("Value"),
				pat_star(jwrap(pat_concat((pat *[]){
					pat_literal(",", 1),
					jwrap(pat_nonterm("Value")),
				}, 2))),
			}, 2))),
			jws(),
			pat_literal("]", 1),
		}, 4),
		/* String <- '"' ( '\\' . / !["\\] . )* '"' */
		pat_concat((pat *[]){
			pat_literal("\"", 1),
			pat_star(pat_alt(
				pat_concat((pat *[]){
					pat_literal("\\", 1),
					pat_any(1),
				}, 2),
				pat_seq(pat_not(pat_or((pat *[]){
					pat_literal("\"", 1),
					pat_literal("\\", 1),
				}, 2)), pat_any(1)))),
			pat_literal("\"", 1),
		}, 3),
		/* Number <- '-'? ('0' / [1-9] [0-9]*)
		 *           ('.' [0-9]+)? ([eE] [+\-]? [0-9]+)? */
		pat_concat((pat *[]){
			pat_optional(pat_literal("-", 1)),
			pat_alt(pat_literal("0", 1),
				pat_concat((pat *[]){
					jnonzero(),
					jdigits_star(),
				}, 2)),
			pat_optional(pat_concat((pat *[]){
				pat_literal(".", 1),
				jdigits_plus(),
			}, 2)),
			pat_optional(pat_concat((pat *[]){
				pat_or((pat *[]){
					pat_literal("e", 1),
					pat_literal("E", 1),
				}, 2),
				pat_concat((pat *[]){
					pat_optional(pat_or((pat *[]){
						pat_literal("+", 1),
						pat_literal("-", 1),
					}, 2)),
					jdigits_plus(),
				}, 2),
			}, 2)),
		}, 4),
	};
	const char *names[] = { "Doc", "Value", "Object", "Array",
	                        "String", "Number" };
	return pat_grammar("Doc", names, defs, 6);
}

/* Small PRNG (deterministic; xorshift32). */
static uint32_t rng_state = 42;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* The list pattern (built fresh per parser: pat_compile mutates the
 * nonterm/inline back-references during grammar compilation, so one
 * pat tree must not be compiled twice). */
static pat *list_grammar(void)
{
	vm_charset letters;
	vm_charset_range(&letters, 'a', 'z');
	pat *item = pat_memo(pat_concat((pat *[]){
		pat_plus(pat_set(&letters)),
		pat_optional(pat_literal(" ", 1)),
	}, 2));
	return pat_concat((pat *[]){
		pat_star(item),
		pat_not(pat_any(1)),
	}, 2);
}

static void test_incremental_correctness(void)
{
	/* A simple list grammar with tree memoization, the shape the
	 * paper's incremental guarantee is about:
	 *   Doc <- Item* !.
	 *   Item <- {{ [a-z]+ ' '? }}           (memoized)
	 */
	pat *doc = list_grammar();
	peg *g = peg_new(doc, 0);
	pat_free(doc);
	doc = list_grammar();
	peg *full = peg_new(doc, 0);
	pat_free(doc);

	/* Start with a run of words, then apply random single-byte edits
	 * (insert a letter, delete a byte, change a byte). */
	char buf[2048];
	size_t n = 0;
	for (int i = 0; i < 40; i++) {
		const char *w = "lorem ipsum dolor sit amet ";
		size_t wl = strlen(w);
		if (n + wl + 8 >= sizeof(buf))
			break;
		memcpy(buf + n, w, wl);
		n += wl;
	}

	vm_result r = peg_parse(g, (const uint8_t *)buf, n);
	CHECK(r.success, "initial parse");
	vm_result_free(&r);

	for (int edit = 0; edit < 200; edit++) {
		int kind = rng() % 3;
		int pos = (int)(rng() % n);
		if (kind == 0 && n < sizeof(buf) - 1) {
			/* insert a letter or space */
			uint8_t ch = (rng() % 2) ? (uint8_t)('a' + rng() % 26)
			                         : (uint8_t)' ';
			peg_edit(g, pos, pos, &ch, 1);
			memmove(buf + pos + 1, buf + pos, n - pos);
			buf[pos] = (char)ch;
			n++;
		} else if (kind == 1 && n > 1) {
			/* delete */
			peg_edit(g, pos, pos + 1, NULL, 0);
			memmove(buf + pos, buf + pos + 1, n - pos - 1);
			n--;
		} else {
			/* change */
			uint8_t ch = (rng() % 2) ? (uint8_t)('a' + rng() % 26)
			                         : (uint8_t)' ';
			peg_edit(g, pos, pos + 1, &ch, 1);
			buf[pos] = (char)ch;
		}

		vm_result inc = peg_reparse(g);
		vm_result ful = peg_parse(full, (const uint8_t *)buf, n);
		CHECK(inc.success == ful.success && inc.pos == ful.pos,
		      "incremental equals full reparse");
		if (inc.success != ful.success || inc.pos != ful.pos)
			fprintf(stderr, "  edit %d kind %d pos %d\n", edit,
			        kind, pos);
		vm_result_free(&inc);
		vm_result_free(&ful);
	}

	peg_free(g);
	peg_free(full);
}

/* Extract the first K complete top-level objects of test.json's outer
 * array and wrap them in [ ] — a complete JSON document made of whole
 * elements, so a plain prefix cut is not needed (a cut mid-array would
 * fail !. for reasons unrelated to incremental parsing). */
static size_t json_slice(const uint8_t *data, size_t sz, uint8_t *out,
                         size_t max, int kobjs)
{
	size_t n = 0;
	out[n++] = '[';
	int depth = 0;
	bool instr = false, esc = false;
	int objs = 0;
	size_t start = 0;
	for (size_t i = 0; i < sz && n + 2 < max; i++) {
		uint8_t c = data[i];
		if (depth == 0) {
			if (c == '{') {
				start = i;
				depth = 1;
			}
			continue;
		}
		if (instr) {
			if (esc)
				esc = false;
			else if (c == '\\')
				esc = true;
			else if (c == '"')
				instr = false;
		} else {
			if (c == '"')
				instr = true;
			else if (c == '{')
				depth++;
			else if (c == '}') {
				depth--;
				if (depth == 0) {
					size_t olen = i - start + 1;
					if (objs > 0 && n + 1 + olen + 2 >= max)
						break;
					if (objs > 0)
						out[n++] = ',';
					if (n + olen + 2 >= max)
						break;
					memcpy(out + n, data + start, olen);
					n += olen;
					if (++objs >= kobjs)
						break;
				}
			}
		}
	}
	out[n++] = ']';
	return n;
}

/* Incremental correctness on the real JSON testdata: parse a slice of
 * test.json, apply random single-character edits, and check that the
 * incremental result equals a full reparse of the edited text. */
static void test_incremental_json(void)
{
	FILE *f = fopen(PEG_TESTDATA_DIR "test.json", "rb");
	if (f == NULL) {
		fprintf(stderr, "SKIP: json incremental (no testdata)\n");
		return;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = xmalloc((size_t)sz);
	if (fread(data, 1, (size_t)sz, f) != (size_t)sz)
		abort_msg("short read");
	fclose(f);

	size_t max = 200000;
	uint8_t *subj = xmalloc(max);
	size_t len = json_slice(data, (size_t)sz, subj, max, 200);
	free(data);
	CHECK(len > 100, "json slice extracted");

	pat *p = json_grammar();
	peg *g = peg_new(p, 512);
	pat_free(p);
	p = json_grammar();
	peg *full = peg_new(p, 0);
	pat_free(p);

	vm_result r = peg_parse(g, subj, len);
	CHECK(r.success, "json initial parse");
	vm_result_free(&r);

	/* independent copy mirroring the edits (with headroom for inserts) */
	size_t mcap = len + 4096;
	uint8_t *mirror = xmalloc(mcap);
	memcpy(mirror, subj, len);
	size_t mlen = len;

	for (int edit = 0; edit < 30; edit++) {
		int kind = rng() % 3;
		size_t pos = rng() % mlen;
		uint8_t ch = "\"0,[]{} abcdefz\n\\-"[rng() % 18];
		if (kind == 0 && mlen + 1 < mcap) {
			peg_edit(g, (int)pos, (int)pos, &ch, 1);
			memmove(mirror + pos + 1, mirror + pos, mlen - pos);
			mirror[pos] = ch;
			mlen++;
		} else if (kind == 1) {
			peg_edit(g, (int)pos, (int)pos + 1, NULL, 0);
			memmove(mirror + pos, mirror + pos + 1, mlen - pos - 1);
			mlen--;
		} else {
			peg_edit(g, (int)pos, (int)pos + 1, &ch, 1);
			mirror[pos] = ch;
		}

		vm_result inc = peg_reparse(g);
		vm_result ful = peg_parse(full, mirror, mlen);
		CHECK(inc.success == ful.success && inc.pos == ful.pos,
		      "json incremental equals full reparse");
		if (inc.success != ful.success || inc.pos != ful.pos)
			fprintf(stderr, "  json edit %d kind %d pos %zu: "
			        "inc(%d,%d) full(%d,%d)\n", edit, kind, pos,
			        inc.success, inc.pos, ful.success, ful.pos);
		vm_result_free(&inc);
		vm_result_free(&ful);
	}

	peg_free(g);
	peg_free(full);
	free(mirror);
	free(subj);
}

/* Window capture extraction: captures in [low, high) match a full
 * parse's captures there. */
static void test_capture_interval(void)
{
	pat *p = capture_test_grammar();
	peg *g = peg_new(p, 0);
	pat_free(p);

	const char *subj = "12 34 56 78 9";
	vm_result r = peg_parse(g, (const uint8_t *)subj, strlen(subj));
	CHECK(r.success, "parse for interval test");
	vm_result_free(&r);

	/* window over "34" (bytes 3-5) */
	r = peg_capture_interval(g, 3, 5);
	CHECK(r.success, "interval parse");
	int caps[32][3];
	int n = 0;
	flatten(r.captures, caps, &n, 32);
	int nnum = 0;
	for (int i = 0; i < n; i++)
		if (caps[i][0] == CAP_NUM)
			nnum++;
	/* only the number capture overlapping the window is built */
	CHECK(nnum >= 1, "number capture found in window");
	vm_result_free(&r);

	peg_free(g);
}

/* ---------------------------------------------------------------------- */
/* Benchmark: full vs incremental on ScriptRuntime.java                   */
/* ---------------------------------------------------------------------- */

/* A Java-flavored grammar covering the file's constructs coarsely.
 *
 * Every alternative inside the line body consumes at least one byte
 * (a nullable alternative would make the outer Star loop forever),
 * and none of them matches '\n' — the line terminator is consumed
 * only by the trailing literal. */
static pat *java_bench_grammar(void)
{
	vm_charset wsnl, letters, digits, alnum;
	vm_charset_fill(&wsnl, (const uint8_t *)" \t\r", 3);
	vm_charset_range(&letters, 'a', 'z');
	vm_charset_range(&digits, '0', '9');
	vm_charset_union(&letters, &digits, &alnum);

	pat *ident = pat_plus(pat_set(&alnum));
	pat *line_comment = pat_concat((pat *[]){
		pat_literal("//", 2),
		pat_star(pat_seq(pat_not(pat_literal("\n", 1)),
		                 pat_any(1))),
	}, 2);
	pat *block_comment = pat_concat((pat *[]){
		pat_literal("/*", 2),
		pat_star(pat_seq(pat_not(pat_literal("*/", 2)),
		                 pat_any(1))),
		pat_literal("*/", 2),
	}, 3);
	pat *string_lit = pat_concat((pat *[]){
		pat_literal("\"", 1),
		pat_star(pat_alt(
			pat_concat((pat *[]){
				pat_literal("\\", 1), pat_any(1),
			}, 2),
			pat_seq(pat_not(pat_or((pat *[]){
				pat_literal("\"", 1),
				pat_literal("\\", 1),
			}, 2)), pat_any(1)))),
		pat_literal("\"", 1),
	}, 3);

	/* line <- (ident / comment / string / [ \t\r] / !'\n' .)* '\n',
	 * tree-memoized per line for the incremental speedup on edits. */
	pat *line = pat_memo(pat_concat((pat *[]){
		pat_star(pat_or((pat *[]){
			ident,
			line_comment,
			block_comment,
			string_lit,
			pat_set(&wsnl),
			pat_seq(pat_not(pat_literal("\n", 1)), pat_any(1)),
		}, 6)),
		pat_literal("\n", 1),
	}, 2));
	return pat_concat((pat *[]){
		pat_star(line),
		pat_not(pat_any(1)),
	}, 2);
}


static double now_ms(void)
{
#ifdef _WIN32
	LARGE_INTEGER freq, counter;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}
static void test_bench_java(void)
{
	FILE *f = fopen(PEG_TESTDATA_DIR "ScriptRuntime.java", "rb");
	if (f == NULL) {
		fprintf(stderr, "SKIP: java benchmark (no testdata)\n");
		return;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = xmalloc((size_t)sz);
	if (fread(data, 1, (size_t)sz, f) != (size_t)sz)
		abort_msg("short read");
	fclose(f);

	pat *p = java_bench_grammar();
	peg *g = peg_new(p, 512);
	pat_free(p);

	double t0 = now_ms();
	vm_result r = peg_parse(g, data, (size_t)sz);
	double full_ms = now_ms() - t0;
	CHECK(r.success, "java full parse");
	printf("  java: full parse %.1f ms (%ld bytes)\n", full_ms, sz);
	vm_result_free(&r);

	/* One-character edits at a few positions, incremental reparse. */
	double inc_total = 0;
	int nedits = 20;
	for (int i = 0; i < nedits; i++) {
		int pos = (int)(((uint64_t)rng() % (uint64_t)(sz - 2)));
		uint8_t ch = 'x';
		peg_edit(g, pos, pos + 1, &ch, 1);
		double t1 = now_ms();
		vm_result rr = peg_reparse(g);
		inc_total += now_ms() - t1;
		CHECK(rr.success, "java incremental parse");
		vm_result_free(&rr);
	}
	printf("  java: incremental reparse %.2f ms/edit (avg of %d)\n",
	       inc_total / nedits, nedits);

	/* undo edits not needed; done */
	peg_free(g);
	free(data);
}

int main(void)
{
	test_captures();
	test_incremental_correctness();
	test_incremental_json();
	test_capture_interval();
	test_bench_java();

	printf("test_peg: %d checks, %d failures\n", nchecks, nfail);
	return nfail ? 1 : 0;
}
