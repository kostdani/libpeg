/*
 * test_vm.c - tests for the parsing machine.
 *
 * Programs are hand-built with the vm_prog builder (mirroring the
 * construction style used throughout: programs are built via
 * the pattern package — ported in a later module).  Covers: character
 * and set matching, control flow (choice/commit/back-commit/partial
 * commit/fail-twice), captures, the memo instructions, tree
 * memoization, the window optimization, error recording, and the
 * encoding invariants (padding, mixed-endian labels, terminating End).
 *
 * Build: make test
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "peg/peg_vm.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		checks++;                                                  \
		if (!(cond)) {                                             \
			failures++;                                        \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__,          \
			        __LINE__);                                 \
			fprintf(stderr, __VA_ARGS__);                      \
			fprintf(stderr, "\n");                             \
		}                                                          \
	} while (0)

/* ---------------------------------------------------------------------- */
/* Encoding                                                                */
/* ---------------------------------------------------------------------- */

static void test_encoding(void)
{
	/* Char 'a': [op, 'a'] — 1 arg, odd, no pad. */
	vm_prog *p = vm_prog_new();
	size_t off = vm_emit_char(p, 'a');
	CHECK(off == 0, "first instruction at offset 0");
	vm_code *c = vm_prog_finish(p);
	CHECK(vm_code_size(c) == 4,
	      "char + End = 2 insns = 4 bytes, got %zu", vm_code_size(c));
	const uint8_t *insns = vm_code_insns(c);
	CHECK(insns[0] == VM_CHAR, "opcode byte");
	CHECK(insns[1] == 'a', "argument byte");
	CHECK(insns[2] == VM_END, "terminating End appended");
	CHECK(insns[3] == 0, "End success flag");
	CHECK(vm_code_ninsn(c) == 2, "two pre-decoded instructions, got %zu",
	      vm_code_ninsn(c));
	vm_code_free(c);
	vm_prog_free(p);

	/* A label operand must encode mixed-endian:
	 * jump to a label 10 instructions forward. */
	p = vm_prog_new();
	int l = vm_prog_label(p);	/* forward: marked below */
	vm_emit_jump(p, l);
	for (int i = 0; i < 10; i++)
		vm_emit_char(p, 'x');
	vm_prog_mark(p, l);		/* offset 4 + 20 = 24 */
	c = vm_prog_finish(p);
	/* insns[0] = Jump opcode; 3 args, odd, no pad: [24>>16, 24&0xff,
	 * (24>>8)&0xff] = [0, 24, 0]. */
	insns = vm_code_insns(c);
	CHECK(insns[1] == 0 && insns[2] == 24 && insns[3] == 0,
	      "jump target 24 mixed-endian: %d %d %d", insns[1],
	      insns[2], insns[3]);
	CHECK(vm_code_ninsn(c) == 12,
	      "program is 12 insns (jump + 10 chars + End), got %zu",
	      vm_code_ninsn(c));
	vm_code_free(c);
	vm_prog_free(p);
}

/* ---------------------------------------------------------------------- */
/* Basic matching                                                          */
/* ---------------------------------------------------------------------- */

static void test_char_set_any(void)
{
	/* 'a' set{b,c} . . End(fail=0) */
	vm_prog *p = vm_prog_new();
	vm_emit_char(p, 'a');
	vm_charset s;
	vm_charset_fill(&s, (const uint8_t[]){ 'b', 'c' }, 2);
	vm_emit_set(p, &s);
	vm_emit_any(p, 2);
	vm_emit_any(p, 1);
	vm_code *c = vm_prog_finish(p);

	vm_result r = vm_exec(c, (const uint8_t *)"abcxyz", 6, NULL, -1, 0);
	CHECK(r.success, "literal+set+any match");
	CHECK(r.pos == 5, "matched 5 bytes (1+1+2+1), got %d", r.pos);
	CHECK(r.nerrors == 0, "no errors");
	vm_result_free(&r);

	/* Set mismatch at 'a' after consuming nothing... second insn. */
	r = vm_exec(c, (const uint8_t *)"axx", 3, NULL, -1, 0);
	CHECK(!r.success, "set mismatch fails overall (no backtrack)");
	vm_result_free(&r);

	/* Any(n) at end of input fails. */
	vm_prog *p2 = vm_prog_new();
	vm_emit_any(p2, 5);
	vm_code *c2 = vm_prog_finish(p2);
	r = vm_exec(c2, (const uint8_t *)"abc", 3, NULL, -1, 0);
	CHECK(!r.success, "Any(5) on 3 bytes fails");
	vm_result_free(&r);
	vm_code_free(c2);
	vm_prog_free(p2);

	vm_code_free(c);
	vm_prog_free(p);
}

static void test_span(void)
{
	/* span{digits} End */
	vm_prog *p = vm_prog_new();
	vm_charset d;
	vm_charset_range(&d, '0', '9');
	vm_emit_span(p, &d);
	vm_code *c = vm_prog_finish(p);

	vm_result r = vm_exec(c, (const uint8_t *)"123abc", 6, NULL, -1, 0);
	CHECK(r.success && r.pos == 3, "span consumes the digit run, pos %d",
	      r.pos);
	vm_result_free(&r);

	r = vm_exec(c, (const uint8_t *)"abc", 3, NULL, -1, 0);
	CHECK(r.success && r.pos == 0, "span matches empty");
	vm_result_free(&r);

	vm_code_free(c);
	vm_prog_free(p);
}

/* ---------------------------------------------------------------------- */
/* Control flow                                                            */
/* ---------------------------------------------------------------------- */

static void test_choice_backtrack(void)
{
	/*
	 *   choice L1; 'a'; 'b'; commit L2; L1: 'x'; L2:
	 * which parses "xb" by backtracking from the 'a'/'b' failure.
	 */
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	int l2 = vm_prog_label(p);
	vm_emit_choice(p, l1);
	vm_emit_char(p, 'a');
	vm_emit_char(p, 'b');
	vm_emit_commit(p, l2);
	vm_prog_mark(p, l1);
	vm_emit_char(p, 'x');
	vm_prog_mark(p, l2);
	vm_code *c = vm_prog_finish(p);

	vm_result r = vm_exec(c, (const uint8_t *)"xb", 2, NULL, -1, 0);
	CHECK(r.success && r.pos == 1, "backtrack to alternative, pos %d",
	      r.pos);
	vm_result_free(&r);

	r = vm_exec(c, (const uint8_t *)"ab", 2, NULL, -1, 0);
	CHECK(r.success && r.pos == 2, "first alternative matches");
	vm_result_free(&r);

	r = vm_exec(c, (const uint8_t *)"q", 1, NULL, -1, 0);
	CHECK(!r.success, "all alternatives exhausted");
	vm_result_free(&r);

	vm_code_free(c);
	vm_prog_free(p);
}

static void test_call_return(void)
{
	/* entry: call R; jump Done.  R: 'a'; return.  Done: End.
	 * (The jump keeps the return address from landing back inside
	 * the rule body.) */
	vm_prog *p = vm_prog_new();
	int body = vm_prog_label(p);
	int done = vm_prog_label(p);
	vm_emit_call(p, body);
	vm_emit_jump(p, done);
	vm_prog_mark(p, body);
	vm_emit_char(p, 'a');
	vm_emit_return(p);
	vm_prog_mark(p, done);
	vm_code *c = vm_prog_finish(p);

	vm_result res = vm_exec(c, (const uint8_t *)"a", 1, NULL, -1, 0);
	CHECK(res.success && res.pos == 1, "call/return matches 'a', pos %d",
	      res.pos);
	vm_result_free(&res);

	vm_code_free(c);
	vm_prog_free(p);
}

static void test_partial_commit_star(void)
{
	/* star('a'): choice L2; L1: 'a'; partial_commit L1; L2:  — the
	 * standard loop compiled for Star nodes. */
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	int l2 = vm_prog_label(p);
	vm_emit_choice(p, l2);
	vm_emit_jump(p, l1);
	vm_prog_mark(p, l1);
	vm_emit_char(p, 'a');
	vm_emit_partial_commit(p, l1);
	vm_prog_mark(p, l2);
	vm_code *c = vm_prog_finish(p);

	vm_result r = vm_exec(c, (const uint8_t *)"aaab", 4, NULL, -1, 0);
	CHECK(r.success && r.pos == 3, "star consumes 3 a's, pos %d", r.pos);
	vm_result_free(&r);

	r = vm_exec(c, (const uint8_t *)"xyz", 3, NULL, -1, 0);
	CHECK(r.success && r.pos == 0, "star matches empty");
	vm_result_free(&r);

	vm_code_free(c);
	vm_prog_free(p);
}

static void test_back_commit(void)
{
	/* back-commit restores sp: choice L1; 'a'; back_commit L2; L1:
	 * 'b'.. build 'a' optional-not style: parse "b" via backtrack
	 * where the backtrack point's sp must be restored. */
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	int l2 = vm_prog_label(p);
	vm_emit_choice(p, l1);
	vm_emit_char(p, 'a');
	vm_emit_back_commit(p, l2);
	vm_prog_mark(p, l1);
	vm_emit_char(p, 'b');
	vm_prog_mark(p, l2);
	vm_emit_char(p, 'c');
	vm_code *c = vm_prog_finish(p);

	/* "ac": choice pushes (l1, 0); 'a' matches; back_commit pops and
	 * seeks back to 0, jumps l2, 'c' fails at 'a'... So "ac" fails.
	 * Actually back_commit pops the choice and RESTORES sp=0, then
	 * jumps to l2 where 'c' vs input[0]='a' fails.  Whole parse
	 * fails (nothing left to backtrack to). */
	vm_result r = vm_exec(c, (const uint8_t *)"ac", 2, NULL, -1, 0);
	CHECK(!r.success, "back_commit restores sp (so 'c' seen at 0)");
	vm_result_free(&r);

	vm_result r2 = vm_exec(c, (const uint8_t *)"abc", 3, NULL, -1, 0);
	/* Wait: same trace — after back_commit sp=0, l2: 'c' vs 'a'
	 * fails immediately.  "abc" should fail too.  The expected
	 * semantics... it does. */
	CHECK(!r2.success, "same trace fails");
	vm_result_free(&r2);

	vm_code_free(c);
	vm_prog_free(p);
}

static void test_fail_twice(void)
{
	/* fail_twice pops an entry then fails: choice L2; choice L1;
	 * 'a'; commit L1...  Use: choice L1; 'x'; commit L2; L1:
	 * fail_twice; L2: — failing twice consumes the outer choice. */
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	int l2 = vm_prog_label(p);
	vm_emit_choice(p, l1);		/* outer */
	vm_emit_char(p, 'x');
	vm_emit_commit(p, l2);
	vm_prog_mark(p, l1);
	vm_emit_fail_twice(p);		/* pops the choice... then fails */
	vm_prog_mark(p, l2);
	vm_code *c = vm_prog_finish(p);

	vm_result r = vm_exec(c, (const uint8_t *)"y", 1, NULL, -1, 0);
	CHECK(!r.success, "fail_twice kills the outer choice too");
	vm_result_free(&r);

	vm_code_free(c);
	vm_prog_free(p);
}

/* ---------------------------------------------------------------------- */
/* Captures                                                                */
/* ---------------------------------------------------------------------- */

static void test_captures(void)
{
	/* capt-begin(1); 'a'; capt-end; capt-begin(2); 'b'; capt-end */
	vm_prog *p = vm_prog_new();
	vm_emit_capture_begin(p, 1);
	vm_emit_char(p, 'a');
	vm_emit_capture_end(p);
	vm_emit_capture_begin(p, 2);
	vm_emit_char(p, 'b');
	vm_emit_capture_end(p);
	vm_code *c = vm_prog_finish(p);

	vm_result r = vm_exec(c, (const uint8_t *)"ab", 2, NULL, -1, 0);
	CHECK(r.success && r.pos == 2, "captured parse matches");
	CHECK(r.captures != NULL, "result tree exists");
	CHECK(memo_capture_num_children(r.captures) == 2,
	      "two top-level captures, got %d",
	      memo_capture_num_children(r.captures));
	const memo_capture *c1 = memo_capture_child(r.captures, 0);
	const memo_capture *c2 = memo_capture_child(r.captures, 1);
	CHECK(c1 != NULL && memo_capture_id(c1) == 1 &&
	      memo_capture_start(c1) == 0 && memo_capture_len(c1) == 1,
	      "capture 1: id=%d start=%d len=%d",
	      c1 ? memo_capture_id(c1) : -1,
	      c1 ? memo_capture_start(c1) : -1,
	      c1 ? memo_capture_len(c1) : -1);
	CHECK(c2 != NULL && memo_capture_id(c2) == 2 &&
	      memo_capture_start(c2) == 1 && memo_capture_len(c2) == 1,
	      "capture 2 at [1,2)");
	vm_result_free(&r);

	/* Nested captures: outer(1) { a(2) } */
	vm_prog *p2 = vm_prog_new();
	vm_emit_capture_begin(p2, 1);
	vm_emit_capture_begin(p2, 2);
	vm_emit_char(p2, 'a');
	vm_emit_capture_end(p2);
	vm_emit_capture_end(p2);
	vm_code *cprog2 = vm_prog_finish(p2);
	vm_result r2 = vm_exec(cprog2, (const uint8_t *)"a", 1, NULL, -1, 0);
	const memo_capture *outer = memo_capture_child(r2.captures, 0);
	CHECK(outer != NULL && memo_capture_id(outer) == 1 &&
	      memo_capture_num_children(outer) == 1,
	      "outer capture with one child");
	const memo_capture *inner = memo_capture_child(outer, 0);
	CHECK(inner != NULL && memo_capture_id(inner) == 2 &&
	      memo_capture_start(inner) == 0 && memo_capture_len(inner) == 1,
	      "inner capture id=%d start=%d len=%d",
	      inner ? memo_capture_id(inner) : -1,
	      inner ? memo_capture_start(inner) : -1,
	      inner ? memo_capture_len(inner) : -1);
	vm_result_free(&r2);
	vm_code_free(cprog2);
	vm_prog_free(p2);

	vm_code_free(c);
	vm_prog_free(p);
}

static void test_capture_full_late(void)
{
	/* CaptureFull: 'a'; capture_full(back=1, id=5) — captures [0,1)
	 * after the fact.  CaptureLate: 'a' 'b'; capture_late(back=2, id=6)
	 * pushes a capt entry at pos-b and a following capture_end closes
	 * it. */
	vm_prog *p = vm_prog_new();
	vm_emit_char(p, 'a');
	vm_emit_capture_full(p, 1, 5);
	vm_code *c = vm_prog_finish(p);
	vm_result r = vm_exec(c, (const uint8_t *)"a", 1, NULL, -1, 0);
	const memo_capture *m = memo_capture_child(r.captures, 0);
	CHECK(m != NULL && memo_capture_id(m) == 5 &&
	      memo_capture_start(m) == 0 && memo_capture_len(m) == 1,
	      "capture_full records [0,1) with id 5");
	vm_result_free(&r);
	vm_code_free(c);
	vm_prog_free(p);

	/* CaptureLate + CaptureEnd. */
	p = vm_prog_new();
	vm_emit_char(p, 'a');
	vm_emit_char(p, 'b');
	vm_emit_capture_late(p, 2, 6);
	vm_emit_char(p, 'c');
	vm_emit_capture_end(p);
	c = vm_prog_finish(p);
	r = vm_exec(c, (const uint8_t *)"abc", 3, NULL, -1, 0);
	m = memo_capture_child(r.captures, 0);
	CHECK(m != NULL && memo_capture_id(m) == 6 &&
	      memo_capture_start(m) == 0 && memo_capture_len(m) == 3,
	      "capture_late(+end) records [0,3) with id 6");
	vm_result_free(&r);
	vm_code_free(c);
	vm_prog_free(p);
}

/* ---------------------------------------------------------------------- */
/* Memoization                                                             */
/* ---------------------------------------------------------------------- */

/* A memoized 'a'+ rule: memo_open(id=1, fail->Lfail); 'a'+ ; memo_close.
 * Structure (as compiled for Memo nodes):
 *   memo_open Lfail, 1
 *   <body>
 *   memo_close
 *   jump Lend
 *   Lfail: fail
 *   Lend:
 */
/*
 * A memoized rule (the shape compiled for Memo nodes):
 *
 *   memo_open L1, id      ; on memo hit: add captures, advance, jump L1
 *   <body>
 *   memo_close            ; memoize the result
 *   L1:
 *
 * A memoized *failure* (entry length -1) fails through the normal
 * backtracking path, not through the label.
 */
static vm_code *build_memo_aaplus(int memo_id, int16_t cap_id)
{
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	vm_emit_memo_open(p, l1, memo_id);
	/* body: 'a' with a capture */
	vm_emit_capture_begin(p, cap_id);
	vm_emit_char(p, 'a');
	vm_emit_capture_end(p);
	vm_emit_memo_close(p);
	vm_prog_mark(p, l1);
	vm_code *c = vm_prog_finish(p);
	vm_prog_free(p);
	return c;
}

static void test_memo_basic(void)
{
	vm_code *c = build_memo_aaplus(1, 9);
	memo_table *t = memo_table_new(0);

	vm_result r = vm_exec(c, (const uint8_t *)"a", 1, t, -1, 0);
	CHECK(r.success && r.pos == 1, "memoized rule matches");
	CHECK(memo_table_size(t) == 1, "one entry stored, got %zu",
	      memo_table_size(t));
	memo_entry *e = memo_table_get(t, 1, 0);
	CHECK(e != NULL, "entry (1, 0) present");
	CHECK(memo_entry_length(e) == 1, "entry length 1");
	size_t ncap;
	memo_capture **caps = memo_entry_captures(e, &ncap);
	CHECK(ncap == 1 && caps[0] != NULL &&
	      memo_capture_id(caps[0]) == 9,
	      "entry holds the body's capture");
	/* The entry's capture start is entry-relative; resolve it. */
	CHECK(memo_capture_start(caps[0]) == 0,
	      "entry capture resolves to start 0");
	vm_result_free(&r);

	/* Re-run: the entry is hit, the same captures are re-added, pos
	 * advances. */
	vm_result r2 = vm_exec(c, (const uint8_t *)"a", 1, t, -1, 0);
	CHECK(r2.success && r2.pos == 1, "second run matches");
	const memo_capture *m = memo_capture_child(r2.captures, 0);
	CHECK(m != NULL && memo_capture_id(m) == 9,
	      "second run recovers capture from memo");
	vm_result_free(&r2);

	memo_table_free(t);
	vm_code_free(c);
}

/* Memo failure: a memoized rule that fails is memoized as length -1 and
 * fails again instantly on re-run. */
static void test_memo_failure(void)
{
	/* memo_open L1, 7; 'z'; memo_close; L1: — on "a", the body fails,
	 * the fail path memoizes (7, 0, -1), and the parse fails. */
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	vm_emit_memo_open(p, l1, 7);
	vm_emit_char(p, 'z');
	vm_emit_memo_close(p);
	vm_prog_mark(p, l1);
	vm_code *c = vm_prog_finish(p);
	vm_prog_free(p);

	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"a", 1, t, -1, 0);
	CHECK(!r.success, "rule fails (backtracks to the memo fail label)");
	CHECK(memo_table_size(t) == 1, "failure memoized");
	memo_entry *e = memo_table_get(t, 7, 0);
	CHECK(e != NULL && memo_entry_length(e) == -1,
	      "failure entry has length -1");
	vm_result_free(&r);
	memo_table_free(t);
	vm_code_free(c);
}

/* ---------------------------------------------------------------------- */
/* Tree memoization                                                        */
/* ---------------------------------------------------------------------- */

/*
 * A tree-memoized star of a memoized 'a': the shape compiled for
 * Star(Memo(p)):
 *
 *   L1: memo_tree_open L3, id
 *       choice L2
 *       <body: 'a'>
 *       commit NoJump
 *   NoJump:
 *       memo_tree_insert
 *   L3: memo_tree
 *       jump L1
 *   L2: memo_tree_close id
 */
static vm_code *build_memo_tree_aastar(int16_t tree_id)
{
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	int l2 = vm_prog_label(p);
	int l3 = vm_prog_label(p);
	int nojump = vm_prog_label(p);
	vm_prog_mark(p, l1);
	vm_emit_memo_tree_open(p, l3, tree_id);
	vm_emit_choice(p, l2);
	vm_emit_char(p, 'a');
	vm_emit_commit(p, nojump);
	vm_prog_mark(p, nojump);
	vm_emit_memo_tree_insert(p);
	vm_prog_mark(p, l3);
	vm_emit_memo_tree(p);
	vm_emit_jump(p, l1);
	vm_prog_mark(p, l2);
	vm_code *c = vm_prog_finish(p);
	vm_prog_free(p);
	return c;
}

static void test_memo_tree(void)
{
	vm_code *c = build_memo_tree_aastar(1);
	memo_table *t = memo_table_new(0);

	/* First run: parses "aaaa" and memoizes merged entries. */
	vm_result r = vm_exec(c, (const uint8_t *)"aaaa", 4, t, -1, 0);
	CHECK(r.success && r.pos == 4, "tree-star consumes all a's, pos %d",
	      r.pos);
	CHECK(memo_table_size(t) > 0, "entries memoized, got %zu",
	      memo_table_size(t));

	/* The merged parent: the largest entry at pos 0 should have
	 * count > 1 (tree memoization folded the repetitions). */
	memo_entry *e = memo_table_get(t, 1, 0);
	CHECK(e != NULL, "merged entry at (1,0) exists");
	CHECK(memo_entry_length(e) == 4,
	      "merged entry covers all 4, length %d", memo_entry_length(e));
	CHECK(memo_entry_count(e) > 1,
	      "merged entry count > 1, got %d", memo_entry_count(e));
	vm_result_free(&r);

	/* Second run with the table: the parse should be O(1)-ish (the
	 * merged entry is hit at pos 0 and advances 4). */
	vm_result r2 = vm_exec(c, (const uint8_t *)"aaaa", 4, t, -1, 0);
	CHECK(r2.success && r2.pos == 4, "tree-memo reparse matches");
	vm_result_free(&r2);

	memo_table_free(t);
	vm_code_free(c);
}

/* ---------------------------------------------------------------------- */
/* Window (interval) parsing                                               */
/* ---------------------------------------------------------------------- */

static void test_window(void)
{
	/* Same tree-memo star, but parse with a window. */
	vm_code *c = build_memo_tree_aastar(1);
	memo_table *t = memo_table_new(0);

	/* Fill pass (no window) then a windowed reparse. */
	vm_result fill = vm_exec(c, (const uint8_t *)"aaaa", 4, t, -1, 0);
	CHECK(fill.success, "fill pass");
	vm_result_free(&fill);

	/* Window [2,4): the edit clears entries in the window, captures
	 * inside it are regenerated, outside skipped. */
	vm_result r = vm_exec(c, (const uint8_t *)"aaaa", 4, t, 2, 4);
	CHECK(r.success && r.pos == 4, "windowed parse matches");
	/* Windowed runs produce no tree (captures are skipped when
	 * memoized... but direct capture construction still happens for
	 * in-window captures).  Here the parse has no capture
	 * instructions, so nothing is constructed. */
	CHECK(memo_capture_num_children(r.captures) == 0,
	      "no captures with no capture instructions");
	vm_result_free(&r);

	memo_table_free(t);
	vm_code_free(c);
}

static void test_window_captures(void)
{
	/* capt(1){'a' 'a'} with memo: window [0,2) constructs the
	 * capture, window [3,5) on a 2-byte input does not. */
	vm_prog *p = vm_prog_new();
	int l1 = vm_prog_label(p);
	vm_emit_memo_open(p, l1, 1);
	vm_emit_capture_begin(p, 1);
	vm_emit_char(p, 'a');
	vm_emit_char(p, 'a');
	vm_emit_capture_end(p);
	vm_emit_memo_close(p);
	vm_prog_mark(p, l1);
	vm_code *c = vm_prog_finish(p);
	vm_prog_free(p);

	memo_table *t = memo_table_new(0);
	vm_result r = vm_exec(c, (const uint8_t *)"aa", 2, t, 0, 2);
	CHECK(r.success, "windowed parse with captures");
	CHECK(memo_capture_num_children(r.captures) == 1,
	      "in-window capture constructed");
	const memo_capture *m = memo_capture_child(r.captures, 0);
	CHECK(m != NULL && memo_capture_id(m) == 1 &&
	      memo_capture_start(m) == 0 && memo_capture_len(m) == 2,
	      "in-window capture [0,2)");
	vm_result_free(&r);

	/* Disjoint window: nothing constructed. */
	vm_result r2 = vm_exec(c, (const uint8_t *)"aa", 2, t, 5, 9);
	CHECK(r2.success, "disjoint window parse");
	CHECK(memo_capture_num_children(r2.captures) == 0,
	      "out-of-window capture not constructed");
	vm_result_free(&r2);

	memo_table_free(t);
	vm_code_free(c);
}

/* ---------------------------------------------------------------------- */
/* Errors and checkers                                                     */
/* ---------------------------------------------------------------------- */

static void test_errors(void)
{
	vm_prog *p = vm_prog_new();
	vm_emit_error(p, "expected 'a' here");
	vm_emit_char(p, 'a');
	vm_code *c = vm_prog_finish(p);
	vm_prog_free(p);

	vm_result r = vm_exec(c, (const uint8_t *)"a", 1, NULL, -1, 0);
	CHECK(r.success, "error does not fail the parse");
	CHECK(r.nerrors == 1, "one error recorded");
	CHECK(r.errors != NULL && r.errors[0].pos == 0 &&
	      strcmp(r.errors[0].message, "expected 'a' here") == 0,
	      "error pos and message");
	vm_result_free(&r);
	vm_code_free(c);
}

static int check_even_len(const uint8_t *match, size_t matchlen,
                          const uint8_t *subject, size_t subjectlen,
                          int id, int flag, void *ud)
{
	(void)subject; (void)subjectlen; (void)id; (void)flag; (void)ud;
	(void)match;
	return matchlen % 2 == 0 ? 0 : -1;
}

static void test_checkers(void)
{
	/* check_begin(0, 0); choice L2; L1: 'a'; partial_commit L1; L2:
	 * check_end — the checker validates the 'a'+ span. */
	vm_prog *p = vm_prog_new();
	vm_emit_check_begin(p, 0, 0);
	int l1 = vm_prog_label(p);
	int l2 = vm_prog_label(p);
	vm_emit_choice(p, l2);
	vm_prog_mark(p, l1);
	vm_emit_char(p, 'a');
	vm_emit_partial_commit(p, l1);
	vm_prog_mark(p, l2);
	vm_emit_check_end(p);
	vm_code *c = vm_prog_finish(p);
	size_t idx = vm_code_add_checker(c, check_even_len, NULL);
	CHECK(idx == 0, "checker registered at index 0");

	/* "aaa": the greedy 'a'+ matched 3, checker rejects (odd), and
	 * with no backtrack point left...  The choice inside the loop
	 * was consumed by partial_commit's reuse; the parse fails. */
	vm_result r = vm_exec(c, (const uint8_t *)"aaa", 3, NULL, -1, 0);
	CHECK(!r.success, "odd-length match rejected by checker");
	vm_result_free(&r);

	/* "aa": even, accepted. */
	vm_result r2 = vm_exec(c, (const uint8_t *)"aab", 3, NULL, -1, 0);
	CHECK(r2.success && r2.pos == 2, "even-length match accepted, pos %d",
	      r2.pos);
	vm_result_free(&r2);

	vm_code_free(c);
	vm_prog_free(p);
}

/* The checker contract only promises ">= 0 on success", so the machine
 * must not trust an advance that runs past the end of the subject. */
static int check_overadvance(const uint8_t *match, size_t matchlen,
                             const uint8_t *subject, size_t subjectlen,
                             int id, int flag, void *ud)
{
	(void)match; (void)matchlen; (void)subject; (void)subjectlen;
	(void)id; (void)flag; (void)ud;
	return 1000;		/* far past the end of the subject below */
}

static void test_checker_overadvance(void)
{
	vm_prog *p = vm_prog_new();
	vm_emit_check_begin(p, 0, 0);
	int l1 = vm_prog_label(p);
	int l2 = vm_prog_label(p);
	vm_emit_choice(p, l2);
	vm_prog_mark(p, l1);
	vm_emit_char(p, 'a');
	vm_emit_partial_commit(p, l1);
	vm_prog_mark(p, l2);
	vm_emit_check_end(p);
	vm_code *c = vm_prog_finish(p);
	vm_code_add_checker(c, check_overadvance, NULL);

	/* An over-advance must fail the match.  Left unchecked it moves
	 * the position off the end and the result reports success at an
	 * impossible position (and later instructions index the buffer). */
	vm_result r = vm_exec(c, (const uint8_t *)"aaa", 3, NULL, -1, 0);
	CHECK(!r.success, "over-advancing checker fails the match");
	vm_result_free(&r);

	vm_code_free(c);
	vm_prog_free(p);
}

/*
 * A builder abandoned before vm_prog_finish still owns its pending label
 * operands.  Freeing it has to release them: enough jumps to force the
 * fixup array to grow past its initial capacity, then no finish.  The
 * assertion is really the sanitizer's -- a leak is reported at exit.
 */
static void test_builder_abandoned(void)
{
	vm_prog *p = vm_prog_new();
	int l = vm_prog_label(p);
	for (int i = 0; i < 64; i++)
		vm_emit_jump(p, l);
	vm_emit_char(p, 'a');
	vm_prog_free(p);
	CHECK(true, "abandoned builder frees cleanly");
}

/* ---------------------------------------------------------------------- */
/* Jump-target width                                                       */
/* ---------------------------------------------------------------------- */

/*
 * Jump targets are 24-bit byte offsets.  A program that grows past that
 * has to be rejected: truncating the offset silently encodes a jump to
 * an unrelated instruction, and the result still decodes and runs.
 * Building such a program costs ~16MB and ends in abort(), so the check
 * runs in a child process.
 */
static void test_jump_target_overflow(void)
{
#if defined(__unix__) || defined(__APPLE__)
	pid_t pid = fork();
	if (pid == 0) {
		/* Child: build a program whose code passes the 24-bit
		 * limit, then reference a label past it.  The abort
		 * message is expected, so keep it out of the report. */
		if (freopen("/dev/null", "w", stderr) == NULL)
			_exit(2);	/* report failure to the parent */
		vm_prog *p = vm_prog_new();
		for (size_t i = 0; i < 0x1000000 / 2 + 8; i++)
			vm_emit_fail(p);
		int far = vm_prog_label(p);
		vm_emit_jump(p, far);
		vm_code *c = vm_prog_finish(p);
		/* Only reached if the offset was truncated. */
		vm_code_free(c);
		vm_prog_free(p);
		_exit(0);
	}
	int status = 0;
	CHECK(pid > 0, "forked overflow probe");
	if (pid > 0 && waitpid(pid, &status, 0) == pid)
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
		      "jump target past 24 bits aborts rather than truncating");
#else
	(void)0;
#endif
}

/* ---------------------------------------------------------------------- */
/* Empty (zero-width assertions)                                           */
/* ---------------------------------------------------------------------- */

static void test_empty(void)
{
	/* BeginText: at pos 0 it holds, at pos 1 it fails. */
	vm_prog *p = vm_prog_new();
	vm_emit_empty(p, 4 /* EmptyBeginText */);
	vm_code *c = vm_prog_finish(p);
	vm_prog_free(p);

	vm_result r = vm_exec(c, (const uint8_t *)"ab", 2, NULL, -1, 0);
	CHECK(r.success && r.pos == 0, "BeginText at 0");
	vm_result_free(&r);

	vm_prog *p2 = vm_prog_new();
	vm_emit_char(p2, 'a');
	vm_emit_empty(p2, 4);
	vm_code *c2 = vm_prog_finish(p2);
	vm_prog_free(p2);
	vm_result r2 = vm_exec(c2, (const uint8_t *)"ab", 2, NULL, -1, 0);
	CHECK(!r2.success, "BeginText at pos 1 fails");
	vm_result_free(&r2);
	vm_code_free(c2);

	vm_code_free(c);
}

/* ---------------------------------------------------------------------- */
/* No-memo table fuzz-ish differential against a no-memo run             */
/* ---------------------------------------------------------------------- */

/* Parse 'a'* with tree memoization on random-ish inputs; compare with
 * a plain no-memo exec of the same program: results must agree. */
static void test_tree_agrees_with_nomemo(void)
{
	vm_code *c = build_memo_tree_aastar(1);

	/* Deterministic pseudo-random inputs. */
	uint32_t seed = 12345;
	for (int iter = 0; iter < 200; iter++) {
		uint8_t buf[64];
		size_t n = 0;
		seed = seed * 1103515245 + 12345;
		size_t target = (seed >> 16) % 40;
		for (size_t i = 0; i < target; i++) {
			seed = seed * 1103515245 + 12345;
			buf[n++] = ((seed >> 16) & 1) ? 'a' : 'b';
		}

		memo_table *t = memo_table_new(0);
		vm_result with = vm_exec(c, buf, n, t, -1, 0);
		vm_result without = vm_exec(c, buf, n, NULL, -1, 0);
		CHECK(with.success == without.success &&
		      with.pos == without.pos,
		      "iter %d: memo pos %d vs no-memo pos %d (success %d/%d)",
		      iter, with.pos, without.pos, with.success,
		      without.success);
		vm_result_free(&with);
		vm_result_free(&without);
		memo_table_free(t);
	}

	vm_code_free(c);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	test_encoding();
	test_char_set_any();
	test_span();
	test_choice_backtrack();
	test_call_return();
	test_partial_commit_star();
	test_back_commit();
	test_fail_twice();
	test_captures();
	test_capture_full_late();
	test_memo_basic();
	test_memo_failure();
	test_memo_tree();
	test_window();
	test_window_captures();
	test_errors();
	test_checkers();
	test_checker_overadvance();
	test_builder_abandoned();
	test_jump_target_overflow();
	test_empty();
	test_tree_agrees_with_nomemo();

	printf("vm: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
