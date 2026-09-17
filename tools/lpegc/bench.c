/*
 * bench.c - Bible search benchmarks plus an incremental-parsing
 * benchmark.
 *
 * Each Bible benchmark parses testdata/bible.txt (4.4 MB) with a
 * specific pattern and no memoization.  Iterations and reporting
 * follow standard benchmark conventions: enough iterations to reach a
 * stable median-of-batch timing, reported as ns/op.
 *
 * Also runs the incremental benchmark (the paper's headline use case):
 * full parse of ScriptRuntime.java, then incremental reparses after
 * single-character edits, compared against full reparses of the same
 * edited text.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "peg/peg.h"
#include "peg/peg_util.h"
#include "peg/peg_vm.h"

/* Benchmark inputs live in a sibling testdata/ directory; the build
 * system passes its absolute path, with a source-tree default. */
#ifndef PEG_TESTDATA_DIR
#define PEG_TESTDATA_DIR "../testdata/"
#endif

static uint8_t *slurp(const char *path, size_t *outlen)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = xmalloc((size_t)sz);
	if (fread(data, 1, (size_t)sz, f) != (size_t)sz)
		abort_msg("short read");
	fclose(f);
	*outlen = (size_t)sz;
	return data;
}

static double now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* Run one pattern over the subject, `iters` times, return ns/op of the
 * median iteration (a mean is skewed by scheduler noise, but the
 * numbers are dominated by the parse itself so median is a fair
 * stand-in). */
static double bench_one(const char *name, pat *p, const uint8_t *data,
                        size_t len, int iters, bool expect_match)
{
	vm_code *code = pat_compile(p, NULL);
	if (code == NULL)
		abort_msg("benchmark pattern failed to compile");

	/* warmup */
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(code, data, len, t, -1, -1);
	bool ok = r.success == expect_match;
	vm_result_free(&r);
	memo_table_free(t);

	double *times = xmalloc(sizeof(double) * (size_t)iters);
	for (int i = 0; i < iters; i++) {
		t = memo_table_new(0);
		double t0 = now_ns();
		r = vm_exec(code, data, len, t, -1, -1);
		times[i] = now_ns() - t0;
		ok = ok && r.success == expect_match;
		vm_result_free(&r);
		memo_table_free(t);
	}

	/* median */
	for (int i = 1; i < iters; i++) {
		double key = times[i];
		int j = i - 1;
		while (j >= 0 && times[j] > key) {
			times[j + 1] = times[j];
			j--;
		}
		times[j + 1] = key;
	}
	double med = times[iters / 2];

	printf("%-32s %12.0f ns/op  match=%s\n", name, med,
	       ok ? "ok" : "MISMATCH");
	if (!ok)
		fprintf(stderr, "  WARNING: match result mismatch!\n");

	free(times);
	vm_code_free(code);
	return med;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double median_sorted(double *times, int n)
{
	qsort(times, (size_t)n, sizeof(double), cmp_double);
	return times[n / 2];
}

/* ---------------------------------------------------------------------- */
/* The Bible search patterns                                               */
/* ---------------------------------------------------------------------- */

static pat *p_abram_word(void)
{
	vm_charset letters;
	vm_charset_range(&letters, 'a', 'z');
	for (int c = 'A'; c <= 'Z'; c++)
		vm_charset_set(&letters, (uint8_t)c, true);
	return pat_concat((pat *[]){
		pat_plus(pat_set(&letters)),
		pat_literal(" Abram", 6),
	}, 2);
}

/* Word for the java benchmark grammar: [A-Za-z_][A-Za-z0-9_]* */
static pat *jword(void)
{
	vm_charset head, tail;
	vm_charset_range(&head, 'A', 'Z');
	for (int c = 'a'; c <= 'z'; c++)
		vm_charset_set(&head, (uint8_t)c, true);
	vm_charset_set(&head, '_', true);
	tail = head;
	for (int c = '0'; c <= '9'; c++)
		vm_charset_set(&tail, (uint8_t)c, true);
	return pat_concat((pat *[]){
		pat_set(&head),
		pat_star(pat_set(&tail)),
	}, 2);
}

static void bench_bible(const uint8_t *data, size_t len)
{
	printf("\n=== Bible search benchmarks ===\n");
	printf("subject: bible.txt (%zu bytes), no memoization\n\n", len);

	/* BenchmarkBibleSearchFirstEartt: Search("eartt") — "eartt" does
	 * not occur in this bible.txt, so the search runs to EOF and
	 * fails. */
	{
		pat *p = pat_search(pat_literal("eartt", 5));
		bench_one("SearchFirstEartt", p, data, len, 5, false);
		pat_free(p);
	}
	/* BenchmarkBibleSearchFirstAbram: Search([a-zA-Z]+ " Abram") */
	{
		pat *p = pat_search(p_abram_word());
		bench_one("SearchFirstAbram", p, data, len, 30, true);
		pat_free(p);
    }
	/* BenchmarkBibleSearchLastAbram: Star(Search(abram)) */
	{
		pat *abram = p_abram_word();
		pat *p = pat_star(pat_search(abram));
		/* note: abram is a child of p; pat_free(p) frees it */
		bench_one("SearchLastAbram", p, data, len, 5, true);
		pat_free(p);
	}
	/* BenchmarkBibleSearchLastTubalcain: Star(Search("Tubalcain")) */
	{
		pat *p = pat_star(pat_search(pat_literal("Tubalcain", 9)));
		bench_one("SearchLastTubalcain", p, data, len, 20, true);
		pat_free(p);
	}
	/* BenchmarkBibleOmegaPattern: (!"Omega" .)* "Omega" */
	{
		pat *p = pat_concat((pat *[]){
			pat_star(pat_seq(pat_not(pat_literal("Omega", 5)),
			                 pat_any(1))),
			pat_literal("Omega", 5),
		}, 2);
		bench_one("OmegaPattern", p, data, len, 5, true);
		pat_free(p);
	}
	/* BenchmarkBibleOmegaGrammar: grammar S <- (!P .)* P; P <- "Omega" */
	{
		pat *defs[] = {
			/* S <- (!P .)* P */
			pat_concat((pat *[]){
				pat_star(pat_seq(pat_not(pat_nonterm("P")),
				                 pat_any(1))),
				pat_nonterm("P"),
			}, 2),
			/* P <- "Omega" */
			pat_literal("Omega", 5),
		};
		const char *names[] = { "S", "P" };
		pat *p = pat_grammar("S", names, defs, 2);
		bench_one("OmegaGrammar", p, data, len, 5, true);
		pat_free(p);
	}
}

/* xorshift PRNG so edit positions are deterministic */
static uint64_t prng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t rngless(uint64_t bound)
{
	prng_state ^= prng_state << 13;
	prng_state ^= prng_state >> 7;
	prng_state ^= prng_state << 17;
	return prng_state % bound;
}

/* ---------------------------------------------------------------------- */
/* The incremental benchmark (paper's core scenario)                       */
/* ---------------------------------------------------------------------- */

/* The Java benchmark grammar. */
enum {
	CAP_LINECOMMENT = 0,
	CAP_FUNCNAME,
	CAP_FUNCQUAL,
	CAP_STRING,
	CAP_NEWLINE,
};

static pat *block_patt(const char *start, const char *end, pat *escape)
{
	pat *notend = pat_seq(pat_not(pat_literal(end, strlen(end))),
	                      pat_any(1));
	if (escape != NULL) {
		return pat_concat((pat *[]){
			pat_literal(start, strlen(start)),
			pat_star(pat_alt(escape, notend)),
			pat_literal(end, strlen(end)),
		}, 3);
	}
	return pat_concat((pat *[]){
		pat_literal(start, strlen(start)),
		pat_star(notend),
		pat_literal(end, strlen(end)),
	}, 3);
}

/* Word-match checker for FuncQual ("public"/"protected"/"private"). */
static int wordmatch_checker(const uint8_t *match, size_t matchlen,
                             const uint8_t *subject, size_t subjectlen,
                             int id, int flag, void *ud)
{
	static const char *words[] = { "public", "protected", "private" };
	(void)subject; (void)subjectlen; (void)id; (void)flag; (void)ud;
	for (size_t i = 0; i < 3; i++) {
		size_t wl = strlen(words[i]);
		if (matchlen == wl && memcmp(match, words[i], wl) == 0)
			return 0;
	}
	return -1;
}

static pat *java_grammar(void)
{
	/* Escape's charset: '"', '\'', 't', 'n', 'b', 'f', 'r', '\\' */
	vm_charset escape_set;
	vm_charset_fill(&escape_set, (const uint8_t *)"\"'tnbfr\\", 8);

	/* S <- {{ (Token / (.( !Token .)*)) }}*      (tree-memoized)
	 * Token <- Comment / FuncQual / FuncName / String / Newline
	 */
	pat *defs[] = {
		/* S */
		pat_star(pat_memo(pat_alt(
			pat_nonterm("Token"),
			pat_concat((pat *[]){
				pat_any(1),
				pat_star(pat_seq(pat_not(pat_nonterm("Token")),
				                 pat_any(1))),
			}, 2)))),
		/* Token */
		pat_or((pat *[]){
			pat_nonterm("Comment"),
			pat_nonterm("FuncQual"),
			pat_nonterm("FuncName"),
			pat_nonterm("String"),
			pat_nonterm("Newline"),
		}, 5),
		/* Comment <- LineComment / LongComment */
		pat_alt(pat_nonterm("LineComment"),
		        pat_nonterm("LongComment")),
		/* LineComment <- Cap("//" (!"\n" .)* "\n") */
		pat_cap(block_patt("//", "\n", NULL), CAP_LINECOMMENT),
		/* LongComment: block comment, no escape */
		block_patt("/*", "*/", NULL),
		/* FuncQual <- Cap(Check(word, {public,protected,private})) */
		pat_cap(pat_check_flags(jword(), wordmatch_checker, NULL, 0, 0),
		        CAP_FUNCQUAL),
		/* FuncName <- Cap(Identifier) "(" */
		pat_concat((pat *[]){
			pat_cap(pat_nonterm("Identifier"), CAP_FUNCNAME),
			pat_literal("(", 1),
		}, 2),
		/* Identifier <- word */
		jword(),
		/* String <- Cap(BlockPatt("\"", "\"", Escape)) */
		pat_cap(block_patt("\"", "\"", pat_nonterm("Escape")),
		        CAP_STRING),
		/* Escape <- "\\" ['"tnbfr\\] */
		pat_concat((pat *[]){
			pat_literal("\\", 1),
			pat_set(&escape_set),
		}, 2),
		/* Newline <- Cap("\n") */
		pat_cap(pat_literal("\n", 1), CAP_NEWLINE),
	};
	const char *names[] = { "S", "Token", "Comment", "LineComment",
	                        "LongComment", "FuncQual", "FuncName",
	                        "Identifier", "String", "Escape",
	                        "Newline" };
	return pat_grammar("S", names, defs, 11);
}

static void bench_incremental(const uint8_t *jdata, size_t jlen)
{
	printf("\n=== Incremental benchmark (Java grammar) ===\n");
	printf("subject: ScriptRuntime.java (%zu bytes), tree table 512\n\n",
	       jlen);

	/* 1. full parse */
	pat *p = java_grammar();
	peg *g = peg_new(p, 512);
	pat_free(p);

	double times[5];
	for (int i = 0; i < 5; i++) {
		double t0 = now_ns();
		vm_result r = peg_parse(g, jdata, jlen);
		times[i] = now_ns() - t0;
		if (!r.success)
			fprintf(stderr, "  WARNING: full parse failed\n");
		vm_result_free(&r);
	}
	double full_med = median_sorted(times, 5);
	printf("%-32s %12.0f ns/op  (%.2f ms)\n", "full parse (median)",
	       full_med, full_med / 1e6);

	/* 2. incremental reparse after single-char edits at line starts */
	/* collect line-start offsets */
	size_t *lines = xmalloc(sizeof(size_t) * (jlen / 10 + 8));
	size_t nlines = 0;
	lines[nlines++] = 0;
	for (size_t i = 0; i < jlen; i++)
		if (jdata[i] == '\n')
			lines[nlines++] = i + 1;

	int nedits = 100;
	double *inc = xmalloc(sizeof(double) * (size_t)nedits);
	for (int i = 0; i < nedits; i++) {
		/* edit: insert a newline at a random line start */
		size_t line = (size_t)(rngless(nlines));
		size_t off = lines[line];
		uint8_t nl = '\n';
		peg_edit(g, (int)off, (int)off, &nl, 1);
		double t0 = now_ns();
		vm_result r = peg_reparse(g);
		inc[i] = now_ns() - t0;
		vm_result_free(&r);
		/* undo */
		peg_edit(g, (int)off, (int)off + 1, NULL, 0);
		vm_result r2 = peg_reparse(g);
		vm_result_free(&r2);
	}
	double inc_med = median_sorted(inc, nedits);
	printf("%-32s %12.0f ns/op  (%.3f ms)\n",
	       "incremental reparse (median)", inc_med, inc_med / 1e6);

	/* 3. full reparse of an edited text, for comparison: insert one
	 * newline at the middle line start, then time full parses. */
	{
		size_t off = lines[nlines / 2];
		uint8_t *copy = xmalloc(jlen + 1);
		memcpy(copy, jdata, off);
		copy[off] = '\n';
		memcpy(copy + off + 1, jdata + off, jlen - off);

		pat *p2 = java_grammar();
		peg *g2 = peg_new(p2, 512);
		pat_free(p2);
		double ft[5];
		for (int i = 0; i < 5; i++) {
			double t0 = now_ns();
			vm_result r = peg_parse(g2, copy, jlen + 1);
			ft[i] = now_ns() - t0;
			if (!r.success)
				fprintf(stderr, "  WARNING: edited full parse failed\n");
			vm_result_free(&r);
		}
		printf("%-32s %12.0f ns/op  (%.2f ms)\n",
		       "full parse of edited text (median)",
		       median_sorted(ft, 5), median_sorted(ft, 5) / 1e6);
		peg_free(g2);
		free(copy);
	}

	peg_free(g);
	free(lines);
	free(inc);
}

int main(void)
{
	size_t blen, jlen;
	uint8_t *bible = slurp(PEG_TESTDATA_DIR "bible.txt", &blen);
	uint8_t *java = slurp(PEG_TESTDATA_DIR "ScriptRuntime.java", &jlen);
	if (bible == NULL || java == NULL)
		abort_msg("cannot open testdata (enable PEG_BUILD_BENCH with testdata present)");

	bench_bible(bible, blen);
	bench_incremental(java, jlen);

	free(bible);
	free(java);
	return 0;
}
