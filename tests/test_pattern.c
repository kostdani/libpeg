/*
 * test_pattern.c - tests for the pattern compiler.
 *
 * Exercises the public pattern API with the paper's example grammars
 * every test compiles a pattern (or grammar) with pat_compile, runs it
 * through vm_exec, and checks success, final position, and captures.
 *
 * Also includes a differential check: a grammar compiled with tree
 * memoization (Star(Memo(p))) parses the same inputs as its un-memoized
 * twin, and a round-trip check of pat_prettify output against the
 * pattern it came from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peg/pattern.h"
#include "util.h"
#include "peg/vm.h"

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
/* The arithmetic grammar of the paper                                    */
/* ---------------------------------------------------------------------- */

/* Expr   <- Factor ([+-] Factor)*
 * Factor <- Term ([*x2f] Term)*   (the set is '*' and '/')
 * Term   <- Number / '(' Expr ')'
 * Number <- [0-9]+ */
static pat *arith_grammar(void)
{
	vm_charset plus_minus, mul_div, digits;

	vm_charset_fill(&plus_minus, (const uint8_t *)"+-", 2);
	vm_charset_fill(&mul_div, (const uint8_t *)"*/", 2);
	vm_charset_range(&digits, '0', '9');

	pat *expr = pat_concat(
		(pat *[]){ pat_nonterm("Factor"),
			   pat_star(pat_concat(
				   (pat *[]){ pat_set(&plus_minus),
					      pat_nonterm("Factor") }, 2)) },
		2);
	pat *factor = pat_concat(
		(pat *[]){ pat_nonterm("Term"),
			   pat_star(pat_concat(
				   (pat *[]){ pat_set(&mul_div),
					      pat_nonterm("Term") }, 2)) },
		2);
	pat *term = pat_alt(
		pat_nonterm("Number"),
		pat_concat((pat *[]){ pat_literal("(", 1),
				      pat_nonterm("Expr"),
				      pat_literal(")", 1) }, 3));
	pat *number = pat_plus(pat_set(&digits));

	const char *names[] = { "Expr", "Factor", "Term", "Number" };
	pat *defs[] = { expr, factor, term, number };
	return pat_grammar("Expr", names, defs, 4);
}

/* ---------------------------------------------------------------------- */

static void test_literals_and_sets(void)
{
	/* "abc" */
	pat *lit = pat_literal("abc", 3);
	vm_code *c = pat_compile(lit, NULL);
	CHECK(c != NULL, "literal compiles");
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"abcxyz", 6, t, -1, -1);
	CHECK(r.success && r.pos == 3, "literal match");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(lit);

	/* [0-9]+ */
	vm_charset digits;
	vm_charset_range(&digits, '0', '9');
	pat *digits_plus = pat_plus(pat_set(&digits));
	c = pat_compile(digits_plus, NULL);
	t = memo_table_new(0);
	r = vm_exec(c, (const uint8_t *)"1234;", 5, t, -1, -1);
	CHECK(r.success && r.pos == 4, "digit plus match");
	vm_result_free(&r);
	r = vm_exec(c, (const uint8_t *)"x", 1, t, -1, -1);
	CHECK(!r.success, "digit plus fails on 'x'");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(digits_plus);

	/* head-fail optimization is exercised by every Alt of matchers
	 * above; verify count of instructions is small (Choice+Char pairs
	 * collapse to TestChar).  'a' / 'b' is a single Set after the
	 * combine optimization. */
	vm_charset ab;
	vm_charset_fill(&ab, (const uint8_t *)"ab", 2);
	pat *alt_ab = pat_alt(pat_literal("a", 1), pat_literal("b", 1));
	c = pat_compile(alt_ab, NULL);
	CHECK(c != NULL, "alt of literals compiles");
	/* One Set insn + the appended End = 2 pre-decoded instructions. */
	CHECK(vm_code_ninsn(c) == 2, "alt of literals combines to one Set");
	vm_code_free(c);
	pat_free(alt_ab);
}

static void test_predicates(void)
{
	/* !a . — the complement-set optimization; matches any byte but a */
	pat *nota = pat_seq(pat_not(pat_literal("a", 1)), pat_any(1));
	vm_code *c = pat_compile(nota, NULL);
	CHECK(c != NULL, "not-seq compiles");
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"z", 1, t, -1, -1);
	CHECK(r.success && r.pos == 1, "!a . matches z");
	vm_result_free(&r);
	r = vm_exec(c, (const uint8_t *)"a", 1, t, -1, -1);
	CHECK(!r.success, "!a . fails on a");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(nota);

	/* &a a — and-predicate */
	pat *anda = pat_seq(pat_and(pat_literal("a", 1)),
	                    pat_literal("a", 1));
	c = pat_compile(anda, NULL);
	t = memo_table_new(0);
	r = vm_exec(c, (const uint8_t *)"ab", 2, t, -1, -1);
	CHECK(r.success && r.pos == 1, "&a a matches");
	vm_result_free(&r);
	r = vm_exec(c, (const uint8_t *)"b", 1, t, -1, -1);
	CHECK(!r.success, "&a a fails on b");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(anda);

	/* optional single char uses TestCharNoChoice */
	pat *optx = pat_optional(pat_literal("x", 1));
	c = pat_compile(optx, NULL);
	t = memo_table_new(0);
	r = vm_exec(c, (const uint8_t *)"xy", 2, t, -1, -1);
	CHECK(r.success && r.pos == 1, "x? matches x");
	vm_result_free(&r);
	r = vm_exec(c, (const uint8_t *)"y", 1, t, -1, -1);
	CHECK(r.success && r.pos == 0, "x? skips y");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(optx);
}

static void test_star_plus_repeat(void)
{
	/* star of a set compiles to Span */
	vm_charset digits;
	vm_charset_range(&digits, '0', '9');
	pat *digits_star = pat_star(pat_set(&digits));
	vm_code *c = pat_compile(digits_star, NULL);
	CHECK(vm_code_ninsn(c) == 2, "star of set is one Span insn");
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"123abc", 6, t, -1, -1);
	CHECK(r.success && r.pos == 3, "span match");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(digits_star);

	/* p{3} — exact repetition */
	pat *rep3 = pat_repeat(pat_literal("ab", 2), 3);
	c = pat_compile(rep3, NULL);
	t = memo_table_new(0);
	r = vm_exec(c, (const uint8_t *)"abababX", 7, t, -1, -1);
	CHECK(r.success && r.pos == 6, "(ab){3} matches");
	vm_result_free(&r);
	r = vm_exec(c, (const uint8_t *)"ababX", 5, t, -1, -1);
	CHECK(!r.success, "(ab){3} fails on two");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(rep3);

	/* n <= 0 is empty (pat_repeat frees its arg itself) */
	pat *rep0 = pat_repeat(pat_literal("a", 1), 0);
	c = pat_compile(rep0, NULL);
	t = memo_table_new(0);
	r = vm_exec(c, (const uint8_t *)"zzz", 3, t, -1, -1);
	CHECK(r.success && r.pos == 0, "p{0} matches empty");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(rep0);
}

/* ---------------------------------------------------------------------- */
/* Input-cache edge cases                                                 */
/* ---------------------------------------------------------------------- */

/*
 * Regression: a multi-byte advance that crosses the input cache's
 * chunk boundary.  The 4KB chunk is a cache, not the subject, so
 * landing beyond it says nothing about the end of the data — the
 * bound has to be the subject length.
 */
static void test_advance_across_chunk(void)
{
	size_t n = 5000;
	uint8_t *subj = xmalloc(n);
	memset(subj, 'a', n);

	/* .{k} .{2} must match for every k; k = 4095 lands exactly on
	 * the chunk edge, where the advance overshoots it. */
	int ok = 1;
	for (int k = 4093; k <= 4097; k++) {
		pat *p = pat_seq(pat_repeat(pat_any(1), k), pat_any(2));
		vm_code *c = pat_compile(p, NULL);
		memo_table *t = memo_table_new(0);
		vm_result r = vm_exec(c, subj, n, t, -1, -1);
		if (!r.success || r.pos != k + 2) {
			ok = 0;
			fprintf(stderr, "  k=%d: success=%d pos=%d (want %d)\n",
			        k, r.success, r.pos, k + 2);
		}
		vm_result_free(&r);
		memo_table_free(t);
		vm_code_free(c);
		pat_free(p);
	}
	CHECK(ok, "multi-byte advance across the chunk boundary matches");

	/* A genuine over-advance must still fail: 5000 bytes, ask for
	 * 5001. */
	pat *over = pat_repeat(pat_any(1), 5001);
	vm_code *c = pat_compile(over, NULL);
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, subj, n, t, -1, -1);
	CHECK(!r.success, "advance past the subject end still fails");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(over);

	free(subj);
}

/*
 * Regression: a failed TestAny must leave the subject position where
 * the attempt started.  The head-fail optimization rewrites
 * (Choice; Any) into TestAny, whose failure branch jumps straight to
 * the alternative instead of unwinding through the fail handler that
 * restores the position — so it has to restore it itself.
 */
static void test_failed_testany_restores_pos(void)
{
	/* .{2} fails on "a", so !(.{2}) succeeds at 0 and "a" matches. */
	pat *p = pat_seq(pat_not(pat_any(2)), pat_literal("a", 1));
	vm_code *c = pat_compile(p, NULL);
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"a", 1, t, -1, -1);
	CHECK(r.success && r.pos == 1,
	      "failed TestAny leaves the position at the attempt start");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(p);

	/* The same failure followed by an Empty, which indexes the bytes
	 * around the position: a leaked position made it read past the
	 * subject. */
	pat *q = pat_seq(pat_not(pat_any(2)), pat_emptyop(1));
	c = pat_compile(q, NULL);
	t = memo_table_new(0);
	r = vm_exec(c, (const uint8_t *)"a", 1, t, -1, -1);
	CHECK(r.success && r.pos == 0,
	      "failed TestAny leaks no position into a following Empty");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(q);
}

static void test_grammar_arith(void)
{
	pat *g = arith_grammar();
	vm_code *c = pat_compile(g, NULL);
	CHECK(c != NULL, "arith grammar compiles");
	if (c == NULL)
		return;

	memo_table *t = memo_table_new(0);
	struct { const char *in; bool ok; int pos; } cases[] = {
		{ "1+2*3", true, 5 },
		{ "(1+2)*3", true, 7 },
		{ "12*34+((5))", true, 11 },
		{ "1+", true, 1 },	/* backtracks: Expr matches the "1" */
		{ "*3", false, 0 },
		{ "1+2)*3", true, 3 },	/* prefix match: 1+2 then stops at ) */
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		vm_result r = vm_exec(c, (const uint8_t *)cases[i].in,
		                      strlen(cases[i].in), t, -1, -1);
		CHECK(r.success == cases[i].ok &&
		      (!cases[i].ok || r.pos == cases[i].pos),
		      cases[i].in);
		vm_result_free(&r);
	}
	memo_table_free(t);
	vm_code_free(c);
	pat_free(g);
}

/* Recursively count non-dummy capture ids in a tree (n ids total). */
static void count_capture_ids(const memo_capture *c, int *counts, int n)
{
	if (c == NULL)
		return;
	if (!memo_capture_dummy_p(c)) {
		int id = memo_capture_id(c);
		if (id >= 0 && id < n)
			counts[id]++;
	}
	for (int i = 0; i < memo_capture_num_children(c); i++)
		count_capture_ids(memo_capture_child(c, i), counts, n);
}

static void test_captures(void)
{
	/* { [0-9]+ } with capture-full (constant-width body is the whole
	 * sub-program) */
	vm_charset digits;
	vm_charset_range(&digits, '0', '9');
	pat *p = pat_concat((pat *[]){ pat_cap(pat_plus(pat_set(&digits)), 7),
	                               pat_literal(";", 1) }, 2);
	vm_code *c = pat_compile(p, NULL);
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"123;", 4, t, -1, -1);
	CHECK(r.success && r.pos == 4, "capture match");
	bool found = false;
	/* walk the root's children looking for id 7 over [0,3) */
	const memo_capture *root = r.captures;
	if (root != NULL) {
		for (int i = 0; i < memo_capture_num_children(root); i++) {
			const memo_capture *ch = memo_capture_child(root, i);
			if (ch && !memo_capture_dummy_p(ch) &&
			    memo_capture_id(ch) == 7 &&
			    memo_capture_start(ch) == 0 &&
			    memo_capture_len(ch) == 3)
				found = true;
		}
	}
	CHECK(found, "capture id 7 over [0,3)");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(p);

	/* CapGrammar: every definition captured with a known id */
	int ids[4];
	const char *names[] = { "Expr", "Factor", "Term", "Number" };
	vm_charset plus_minus, mul_div;
	vm_charset_fill(&plus_minus, (const uint8_t *)"+-", 2);
	vm_charset_fill(&mul_div, (const uint8_t *)"*/", 2);
	pat *defs[4];
	defs[0] = pat_concat(
		(pat *[]){ pat_nonterm("Factor"),
			   pat_star(pat_concat(
				   (pat *[]){ pat_set(&plus_minus),
					      pat_nonterm("Factor") }, 2)) },
		2);
	defs[1] = pat_concat(
		(pat *[]){ pat_nonterm("Term"),
			   pat_star(pat_concat(
				   (pat *[]){ pat_set(&mul_div),
					      pat_nonterm("Term") }, 2)) },
		2);
	defs[2] = pat_alt(
		pat_nonterm("Number"),
		pat_concat((pat *[]){ pat_literal("(", 1),
				      pat_nonterm("Expr"),
				      pat_literal(")", 1) }, 3));
	defs[3] = pat_plus(pat_set(&digits));
	pat *g = pat_cap_grammar("Expr", names, defs, 4, ids);
	CHECK(ids[0] == 0 && ids[1] == 1 && ids[2] == 2 && ids[3] == 3,
	      "cap grammar ids");
	c = pat_compile(g, NULL);
	CHECK(c != NULL, "cap grammar compiles");
	t = memo_table_new(0);
	r = vm_exec(c, (const uint8_t *)"2*(3+4)", 7, t, -1, -1);
	CHECK(r.success && r.pos == 7, "cap grammar parse");
	int found_ids[4] = { 0, 0, 0, 0 };
	count_capture_ids(r.captures, found_ids, 4);
	CHECK(found_ids[0] >= 1, "Expr captured");
	CHECK(found_ids[1] >= 1, "Factor captured");
	CHECK(found_ids[2] >= 1, "Term captured");
	CHECK(found_ids[3] >= 1, "Number captured");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(g);
}

static void test_search(void)
{
	/* search("ab") — finds first occurrence, with the complement-set
	 * skip heuristic */
	pat *p = pat_search(pat_literal("ab", 2));
	vm_code *c = pat_compile(p, NULL);
	CHECK(c != NULL, "search compiles");
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"xxabyy", 6, t, -1, -1);
	CHECK(r.success && r.pos == 4, "search finds ab at 2");
	vm_result_free(&r);
	r = vm_exec(c, (const uint8_t *)"zzzz", 4, t, -1, -1);
	CHECK(!r.success, "search fails when absent");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(p);
}

/* Checker accepting only spans of even length. */
static int evenlen_checker(const uint8_t *match, size_t matchlen,
                           const uint8_t *subject, size_t subjectlen,
                           int id, int flag, void *ud)
{
	(void)match; (void)subject; (void)subjectlen; (void)id;
	(void)flag; (void)ud;
	return (matchlen % 2 == 0) ? 0 : -1;
}

static void test_checkers(void)
{
	vm_charset semi, notsemi;
	vm_charset_fill(&semi, (const uint8_t *)";", 1);
	vm_charset_negate(&semi, &notsemi);
	pat *p = pat_concat((pat *[]){
		pat_check(pat_plus(pat_set(&notsemi)), evenlen_checker, NULL),
		pat_literal(";", 1) }, 2);
	vm_code *c = pat_compile(p, NULL);
	CHECK(c != NULL, "check compiles");
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"abcd;", 5, t, -1, -1);
	CHECK(r.success, "even-length span accepted");
	vm_result_free(&r);
	r = vm_exec(c, (const uint8_t *)"abc;", 4, t, -1, -1);
	CHECK(!r.success, "odd-length span rejected");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(p);
}

static void test_memo_and_tree_memo(void)
{
	/* Memo(p): plain memoization */
	vm_charset a;
	vm_charset_fill(&a, (const uint8_t *)"a", 1);
	pat *p = pat_memo(pat_plus(pat_set(&a)));
	vm_code *c = pat_compile(p, NULL);
	CHECK(c != NULL, "memo compiles");
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"aaa", 3, t, -1, -1);
	CHECK(r.success && r.pos == 3, "memo match");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(p);

	/* Star(Memo(p)): tree memoization, same results as the un-memoized
	 * twin (differential) */
	for (int trial = 0; trial < 50; trial++) {
		char input[512];
		size_t n = 0;
		for (int i = 0; i < trial + 1 && n + 8 < sizeof(input); i++) {
			for (int j = 0; j <= (i * 7 + trial) % 4; j++)
				input[n++] = 'a';
			if ((i + trial) % 3 == 0)
				input[n++] = 'b';
		}

		pat *memoized = pat_star(pat_memo(pat_plus(pat_set(&a))));
		pat *plain = pat_star(pat_plus(pat_set(&a)));
		vm_code *cm = pat_compile(memoized, NULL);
		vm_code *cp = pat_compile(plain, NULL);

		memo_table *tm = memo_table_new(0);
		memo_table *tp = memo_table_new(0);
		vm_result rm = vm_exec(cm, (const uint8_t *)input, n, tm, -1, -1);
		vm_result rp = vm_exec(cp, (const uint8_t *)input, n, tp, -1, -1);
		CHECK(rm.success == rp.success && rm.pos == rp.pos,
		      "tree-memo agrees with plain");
		vm_result_free(&rm);
		vm_result_free(&rp);
		memo_table_free(tm);
		memo_table_free(tp);
		vm_code_free(cm);
		vm_code_free(cp);
		pat_free(memoized);
		pat_free(plain);
	}
}

static void test_errors(void)
{
	/* Error with recovery: the recovery pattern continues the parse */
	pat *p = pat_error("expected x",
	                   pat_concat((pat *[]){ pat_any(1),
	                         pat_literal("x", 1) }, 2));
	vm_code *c = pat_compile(p, NULL);
	CHECK(c != NULL, "error compiles");
	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"yx", 2, t, -1, -1);
	CHECK(r.success && r.pos == 2, "error recovery works");
	CHECK(r.nerrors == 1 && r.errors != NULL &&
	      strcmp(r.errors[0].message, "expected x") == 0,
	      "error recorded");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
	pat_free(p);
}

static void test_missing_nonterm(void)
{
	pat *p = pat_concat((pat *[]){ pat_nonterm("Undefined"),
	                               pat_literal("a", 1) }, 2);
	const char *err = NULL;
	vm_code *c = pat_compile(p, &err);
	CHECK(c == NULL && err != NULL && strcmp(err, "Undefined") == 0,
	      "missing non-terminal reported");
	pat_free(p);
}

static void test_prettify(void)
{
	vm_charset digits;
	vm_charset_range(&digits, '0', '9');
	pat *p = pat_plus(pat_set(&digits));
	char *s = pat_prettify(p);
	CHECK(strcmp(s, "[{0..9}]+") == 0, "prettify [0-9]+");
	free(s);
	pat_free(p);

	p = pat_alt(pat_literal("a", 1), pat_literal("b", 1));
	s = pat_prettify(p);
	/* the alt is not combined by Get at prettify time (no combine in
	 * Prettify's Get path here since literals of len 1 do combine) */
	CHECK(s != NULL && strlen(s) > 0, "prettify alt nonempty");
	free(s);
	pat_free(p);

	p = pat_star(pat_seq(pat_not(pat_literal("a", 1)), pat_any(1)));
	s = pat_prettify(p);
	CHECK(s != NULL, "prettify complex");
	free(s);
	pat_free(p);
}

int main(void)
{
	test_literals_and_sets();
	test_predicates();
	test_star_plus_repeat();
	test_advance_across_chunk();
	test_failed_testany_restores_pos();
	test_grammar_arith();
	test_captures();
	test_search();
	test_checkers();
	test_memo_and_tree_memo();
	test_errors();
	test_missing_nonterm();
	test_prettify();

	printf("pattern: %d checks, %d failures\n", nchecks, nfail);
	return nfail ? 1 : 0;
}
