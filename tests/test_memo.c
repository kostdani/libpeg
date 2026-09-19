/*
 * test_memo.c - tests for the memoization table.
 *
 * Covers: threshold filtering, find-largest, edit invalidation via
 * examined-extent overlap, insertion-point invalidation, lazy shifts of
 * entry/capture positions (relocatable captures), and capture tree
 * traversal including dummy flattening.
 *
 * Build: make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peg/memo.h"

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
/* Table basics                                                           */
/* ---------------------------------------------------------------------- */

static void test_put_get(void)
{
	memo_table *t = memo_table_new(0);

	/* No threshold: everything is stored. */
	memo_table_put(t, 7, 0, 5, 5, 1, NULL, 0);
	memo_table_put(t, 7, 0, 12, 12, 1, NULL, 0);
	memo_table_put(t, 8, 0, 3, 3, 1, NULL, 0);

	CHECK(memo_table_size(t) == 3, "three entries");
	memo_entry *e = memo_table_get(t, 7, 0);
	CHECK(e != NULL, "entry found");
	CHECK(memo_entry_length(e) == 12,
	      "largest entry wins, got length %d", memo_entry_length(e));
	CHECK(memo_entry_pos(e) == 0, "entry position");
	CHECK(memo_table_get(t, 8, 0) != NULL, "ids distinguished");
	CHECK(memo_table_get(t, 7, 1) == NULL, "wrong position misses");

	memo_table_free(t);
}

static void test_threshold(void)
{
	memo_table *t = memo_table_new(128);

	/* Below threshold: dropped, and its captures freed.  Note that
	 * memo_table_put takes ownership of both the capture array and its
	 * elements, so the array must be heap-allocated. */
	memo_capture **caps = malloc(sizeof(memo_capture *));
	caps[0] = memo_capture_node(1, 0, 10);
	memo_table_put(t, 1, 0, 10, 10, 1, caps, 1);
	CHECK(memo_table_size(t) == 0, "below-threshold entry dropped");
	CHECK(memo_table_get(t, 1, 0) == NULL, "dropped entry not found");

	/* Examined matters, not matched: a 10-byte match that examined 200
	 * bytes is worth memoizing. */
	memo_table_put(t, 1, 0, 10, 200, 1, NULL, 0);
	CHECK(memo_table_size(t) == 1, "large examined stored");

	/* Zero-length matches are never stored. */
	memo_table_put(t, 2, 0, 0, 500, 1, NULL, 0);
	CHECK(memo_table_size(t) == 1, "zero-length match dropped");

	/* Failed matches (length -1) are memoized if they examined enough:
	 * a failure at a position is just as reusable as a success. */
	memo_table_put(t, 3, 0, -1, 300, 1, NULL, 0);
	memo_entry *e = memo_table_get(t, 3, 0);
	CHECK(e != NULL && memo_entry_length(e) == -1,
	      "failed match memoized");
	CHECK(memo_entry_examined(e) == 300, "failed match examined");

	memo_table_free(t);
}

/* ---------------------------------------------------------------------- */
/* Edit invalidation                                                      */
/* ---------------------------------------------------------------------- */

static void test_edit_invalidation(void)
{
	memo_table *t = memo_table_new(0);

	/*
	 * Entry A: [0,10) matched, examined 10.  Entry B: [10,20) matched,
	 * examined 20 (lookahead past its match, extent [10,30)).  Entry C:
	 * [40,50) matched, examined 45 (extent [40,85)).
	 */
	memo_table_put(t, 1, 0, 10, 10, 1, NULL, 0);		/* A */
	memo_table_put(t, 1, 10, 10, 20, 1, NULL, 0);		/* B */
	memo_table_put(t, 1, 40, 10, 45, 1, NULL, 0);		/* C */

	/*
	 * Insert one byte at 5: A examined it and dies.  B and C start at
	 * or after 10 and never read byte 5, so they survive and shift by
	 * one.
	 */
	memo_table_apply_edit(t, (memo_edit){ .start = 5, .end = 5, .len = 1 });
	CHECK(memo_table_size(t) == 2, "only A evicted, got %zu",
	      memo_table_size(t));
	memo_entry *b = memo_table_get(t, 1, 11);
	memo_entry *c = memo_table_get(t, 1, 41);
	CHECK(b != NULL && memo_entry_pos(b) == 11, "B shifted to 11");
	CHECK(c != NULL && memo_entry_pos(c) == 41, "C shifted to 41");

	/*
	 * Examined-extent eviction, not match overlap: replace [51, 52)
	 * with one byte.  C's match is [41, 51) so the edit is one past
	 * its end — but C's examined extent is [41, 86), and it read byte
	 * 51, so C must die.  B (extent [11, 31)) is untouched.
	 */
	memo_table_apply_edit(t, (memo_edit){ .start = 51, .end = 52, .len = 1 });
	CHECK(memo_table_size(t) == 1, "C evicted via examined extent, got %zu",
	      memo_table_size(t));
	CHECK(memo_table_get(t, 1, 11) != NULL, "B still present");

	memo_table_free(t);
}

static void test_edit_boundaries(void)
{
	memo_table *t = memo_table_new(0);

	/* Entry examining [10, 20). */
	memo_table_put(t, 1, 10, 5, 10, 1, NULL, 0);

	/* Edit [5, 10): touches bytes 5..9 — entry stays. */
	memo_table_apply_edit(t, (memo_edit){ .start = 5, .end = 10, .len = 5 });
	CHECK(memo_table_size(t) == 1, "edit ending at entry start is safe");

	/* Edit [20, 25): entry ends at 20, stays. */
	memo_table_apply_edit(t, (memo_edit){ .start = 20, .end = 25, .len = 5 });
	CHECK(memo_table_size(t) == 1, "edit starting at entry end is safe");

	/*
	 * Pure insertion at 20 (one past the extent end): the entry never
	 * read byte 20, so it survives.  Its start (10) is left of the
	 * edit, so it does not shift either.
	 */
	memo_table_apply_edit(t, (memo_edit){ .start = 20, .end = 20, .len = 3 });
	CHECK(memo_table_size(t) == 1, "insertion at extent end survives");
	memo_entry *e = memo_table_get(t, 1, 10);
	CHECK(e != NULL, "entry still at 10 after right insertion");
	CHECK(e != NULL && memo_entry_pos(e) == 10, "no shift for left entry");

	/*
	 * Pure insertion at 15 (inside the extent [10,20)): the byte at 15
	 * changed from the entry's point of view, so it must be evicted.
	 */
	memo_table_apply_edit(t, (memo_edit){ .start = 15, .end = 15, .len = 3 });
	CHECK(memo_table_size(t) == 0, "mid-extent insertion evicts");

	memo_table_free(t);
}

/* ---------------------------------------------------------------------- */
/* Relocatable captures                                                   */
/* ---------------------------------------------------------------------- */

/*
 * Build a capture tree:
 *   node id=1 "outer"  [start, start+50)
 *     node id=2 "a"    [start+5, +10)
 *     dummy            [start+20, +20)
 *       node id=3 "b"  [start+25, +5)
 *
 * Returns the outer node holding the caller's single reference; the
 * children are owned by their parent (memo_capture_add takes its own
 * reference).
 */
static memo_capture *build_tree(int start)
{
	memo_capture *outer = memo_capture_node(1, start, 50);
	memo_capture *a = memo_capture_node(2, start + 5, 10);
	memo_capture *b = memo_capture_node(3, start + 25, 5);
	memo_capture *d = memo_capture_dummy(start + 20, 20);

	memo_capture_add(d, b);
	memo_capture_add(outer, a);
	memo_capture_add(outer, d);

	/* Release the creator references now owned by the parents. */
	memo_capture_free(b);
	memo_capture_free(a);
	memo_capture_free(d);
	return outer;
}

static void test_relocatable_captures(void)
{
	memo_table *t = memo_table_new(0);

	int start = 100;
	memo_capture *outer = build_tree(start);
	/* memo_table_put takes ownership of the array; heap-allocate it. */
	memo_capture **caps = malloc(sizeof(memo_capture *));
	caps[0] = outer;
	memo_table_put(t, 1, start, 50, 50, 1, caps, 1);

	/* Before any edit, positions are absolute and unchanged. */
	CHECK(memo_capture_start(outer) == start, "outer start");
	CHECK(memo_capture_end(outer) == start + 50, "outer end");

	/* Insert 10 bytes at position 10 (left of the entry). */
	memo_table_apply_edit(t, (memo_edit){ .start = 10, .end = 10, .len = 10 });

	CHECK(memo_capture_start(outer) == start + 10,
	      "outer shifted to %d, want %d", memo_capture_start(outer),
	      start + 10);

	/* Children relocate through the entry (entry-relative offsets). */
	const memo_capture *a = memo_capture_child(outer, 0);
	const memo_capture *b = memo_capture_child(outer, 1);
	CHECK(a != NULL && b != NULL, "children found through dummy");
	CHECK(a && memo_capture_start(a) == start + 15,
	      "child a shifted");
	CHECK(b && memo_capture_start(b) == start + 35,
	      "child b shifted through dummy");
	CHECK(a && b && memo_capture_len(a) == 10 && memo_capture_len(b) == 5,
	      "lengths unaffected by shifts");

	/* Delete 10 bytes at the same place: back to original positions. */
	memo_table_apply_edit(t, (memo_edit){ .start = 10, .end = 10, .len = -10 });
	CHECK(memo_capture_start(outer) == start, "outer back at %d",
	      memo_capture_start(outer));
	CHECK(memo_capture_start(memo_capture_child(outer, 0)) == start + 5,
	      "child a back in place");

	/* Entry lookup follows the shift too. */
	memo_entry *e = memo_table_get(t, 1, start);
	CHECK(e != NULL, "entry findable at shifted position");
	CHECK(memo_table_get(t, 1, start + 10) == NULL,
	      "old position gone after shift back");

	memo_table_free(t);
}

/* Helper for traversal tests: collect capture ids. */
struct idcoll { int ids[8]; int n; };
static void collect_ids(const memo_capture *c, void *ud)
{
	struct idcoll *ic = ud;
	if (ic->n < 8)
		ic->ids[ic->n++] = memo_capture_id(c);
}

static void test_capture_traversal(void)
{
	/* Unattached captures use absolute positions (ment == NULL). */
	int start = 7;
	memo_capture *outer = build_tree(start);

	CHECK(memo_capture_num_children(outer) == 2,
	      "dummy children flattened in count");
	CHECK(memo_capture_child(outer, 0) != NULL &&
	      memo_capture_id(memo_capture_child(outer, 0)) == 2,
	      "child 0 is node a");
	CHECK(memo_capture_child(outer, 1) != NULL &&
	      memo_capture_id(memo_capture_child(outer, 1)) == 3,
	      "child 1 is node b, through the dummy");
	CHECK(memo_capture_child(outer, 2) == NULL, "no third child");
	CHECK(memo_capture_start(memo_capture_child(outer, 1)) == start + 25,
	      "unattached child keeps absolute position");

	/* Each-child iteration matches the indexed view. */
	struct idcoll ic = { 0 };
	memo_capture_each_child(outer, collect_ids, &ic);
	CHECK(ic.n == 2 && ic.ids[0] == 2 && ic.ids[1] == 3,
	      "each_child flattens dummies in order, got n=%d", ic.n);

	memo_capture_free(outer);
}

static void test_capture_each(void)
{
	int start = 0;
	memo_capture *outer = build_tree(start);

	struct idcoll ic = { 0 };
	memo_capture_each_child(outer, collect_ids, &ic);
	CHECK(ic.n == 2 && ic.ids[0] == 2 && ic.ids[1] == 3,
	      "each_child flattens dummies in order, got n=%d", ic.n);

	memo_capture_free(outer);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	test_put_get();
	test_threshold();
	test_edit_invalidation();
	test_edit_boundaries();
	test_relocatable_captures();
	test_capture_traversal();
	test_capture_each();

	printf("memo table: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
