/*
 * test_interval_tree.c - tests for the lazy interval tree.
 *
 * Two layers:
 *
 *  1. Targeted unit tests for the behaviors that are easy to get wrong:
 *     half-open overlap semantics, find-largest tie-breaking, group
 *     intervals under one key, lazy shift application, and loc handles
 *     surviving deletions/rebalancing.
 *
 *  2. A randomized differential test: the AVL tree is compared
 *     op-by-op against the naive array implementation, which is simple
 *     enough to trust as an oracle.  Structural invariants (AVL balance,
 *     key order, max augmentation, shift log) are re-checked on a sample
 *     of iterations via itree_verify.
 *
 * Build: make test  (see Makefile)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interval_array.h"
#include "peg/peg_interval.h"

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
/* Unit tests                                                             */
/* ---------------------------------------------------------------------- */

static void test_basic(void)
{
	itree *t = itree_new();

	CHECK(itree_size(t) == 0, "empty tree has size 0");
	CHECK(itree_find_largest(t, 1, 0) == NULL, "empty tree finds nothing");
	CHECK(itree_verify(t), "empty tree verifies");

	itree_free(t);
}

static void test_add_find(void)
{
	itree *t = itree_new();

	itree_add(t, 1, 0, 10, "a");
	itree_add(t, 1, 0, 25, "b");	/* same key, larger interval */
	itree_add(t, 1, 0, 25, "c");	/* same length as "b": first wins */
	itree_add(t, 2, 0, 5, "d");	/* same pos, different id */
	itree_add(t, 1, 10, 12, "e");

	CHECK(itree_size(t) == 5, "size 5, got %zu", itree_size(t));
	CHECK(itree_find_largest(t, 1, 0) == (void *)"b",
	      "find largest prefers longer, first on tie");
	CHECK(itree_find_largest(t, 2, 0) == (void *)"d",
	      "ids are distinguished");
	CHECK(itree_find_largest(t, 1, 10) == (void *)"e", "second key");
	CHECK(itree_find_largest(t, 1, 5) == NULL, "miss at unstored pos");
	CHECK(itree_verify(t), "invariants hold");

	itree_free(t);
}

static void test_remove_overlaps(void)
{
	itree *t = itree_new();

	itree_loc a = itree_add(t, 1, 0, 10, "a");
	itree_add(t, 1, 10, 20, "b");
	itree_loc c = itree_add(t, 1, 20, 30, "c");

	/* Remove [12, 18): only "b" overlaps. */
	itree_remove_and_shift(t, 12, 18, 0);
	CHECK(itree_size(t) == 2, "one interval removed, got %zu",
	      itree_size(t));
	CHECK(itree_loc_pos(a) == 0, "a unaffected");
	CHECK(itree_loc_pos(c) == 20, "c unaffected");
	CHECK(itree_verify(t), "invariants hold");

	/*
	 * Half-open boundaries: an edit at [10, 12) touches neither the
	 * interval ending at 10 ("a") nor the one starting at 12 (none).
	 */
	itree_remove_and_shift(t, 10, 12, 0);
	CHECK(itree_size(t) == 2, "touching intervals survive");
	CHECK(itree_verify(t), "invariants hold");

	itree_free(t);
}

static void test_lazy_shift(void)
{
	itree *t = itree_new();

	itree_loc a = itree_add(t, 1, 0, 10, "a");
	itree_loc b = itree_add(t, 1, 10, 20, "b");
	itree_loc c = itree_add(t, 1, 30, 40, "c");

	/* Insert 5 bytes at position 25: everything >= 25 shifts right. */
	itree_remove_and_shift(t, 25, 25, 5);
	CHECK(itree_loc_pos(c) == 35, "c shifted 30 -> 35, got %d",
	      itree_loc_pos(c));
	CHECK(itree_loc_pos(a) == 0, "a untouched");
	CHECK(itree_loc_pos(b) == 10, "b untouched");

	/* Find must account for the shift. */
	CHECK(itree_find_largest(t, 1, 35) == (void *)"c",
	      "find sees shifted position");
	CHECK(itree_find_largest(t, 1, 30) == NULL,
	      "old position no longer present");

	/* Delete 5 bytes at position 25: c returns to 30. */
	itree_remove_and_shift(t, 25, 25, -5);
	CHECK(itree_loc_pos(c) == 30, "c shifted back");
	CHECK(itree_verify(t), "invariants hold after shifts");

	/*
	 * Multiple pending shifts: positions observed through loc handles
	 * and find must agree with eager application.
	 */
	itree_remove_and_shift(t, 0, 0, 100);
	itree_remove_and_shift(t, 0, 0, -3);
	CHECK(itree_loc_pos(a) == 97, "a at 97, got %d", itree_loc_pos(a));
	CHECK(itree_find_largest(t, 1, 97) == (void *)"a",
	      "find after stacked shifts");
	CHECK(itree_verify(t), "verify drains shift log");
	CHECK(itree_loc_pos(a) == 97, "positions stable after verify");

	itree_free(t);
}

static void test_loc_survives_deletion(void)
{
	itree *t = itree_new();

	itree_loc a = itree_add(t, 1, 0, 10, "a");
	itree_add(t, 1, 10, 20, "b");
	itree_loc c = itree_add(t, 1, 20, 30, "c");

	/* Remove b; the tree rebalances, but a and c's handles stay live. */
	itree_remove_and_shift(t, 12, 18, 0);
	CHECK(itree_size(t) == 2, "b removed");
	CHECK(itree_loc_pos(a) == 0 && itree_loc_pos(c) == 20,
	      "handles still resolve after node deletion");
	CHECK(itree_verify(t), "invariants hold");

	/* Shift after deletion: handles must track. */
	itree_remove_and_shift(t, 5, 5, 100);
	CHECK(itree_loc_pos(c) == 120, "c at 120, got %d", itree_loc_pos(c));
	CHECK(itree_find_largest(t, 1, 120) == (void *)"c",
	      "find matches loc after shift");

	itree_free(t);
}

static void test_stress_shifts_and_verify(void)
{
	/*
	 * Deterministic xorshift PRNG so failures are reproducible; run
	 * several seeds to widen coverage.
	 */
	for (int round = 0; round < 8; round++) {
		uint32_t seed = 0x12345678 + round * 0x9e3779b9;
#define RAND()                                                           \
		(seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5, \
		 (uint32_t)(seed >> 1))

		itree *t = itree_new();

		for (int i = 0; i < 20000; i++) {
			switch (RAND() % 4) {
			case 0: {		/* add */
				int low = (int)(RAND() % 1000);
				int len = (int)(RAND() % 100) + 1;
				itree_add(t, i & 0xff, low, low + len, NULL);
				break;
			}
			case 1: {		/* remove overlaps */
				int low = (int)(RAND() % 1000);
				int len = (int)(RAND() % 50) + 1;
				itree_remove_and_shift(t, low, low + len, 0);
				break;
			}
			case 2: {		/* remove + shift */
				int low = (int)(RAND() % 1000);
				int len = (int)(RAND() % 50) + 1;
				/*
				 * amt must be >= -len: an edit replaces len
				 * bytes with max(len + amt, 0) bytes, so it
				 * can never shrink by more than len.
				 */
				int amt = (int)(RAND() % (50 + len)) - len;
				itree_remove_and_shift(t, low, low + len, amt);
				break;
			}
			case 3:		/* verify */
				CHECK(itree_verify(t), "round %d: invariants at iter %d",
				      round, i);
				break;
			}
		}

		CHECK(itree_verify(t), "round %d: final verify", round);
		itree_free(t);
#undef RAND
	}
}

/*
 * Regression test: minimal case (delta-debuged from a randomized failure)
 * where a single remove_and_shift deletes a node's entire right subtree —
 * both descendants — dropping its height by 2 at once.  Textbook AVL
 * rebalancing (single rotation for |bf| == 2) leaves the node unbalanced
 * at +2; iv_rebalance must handle |bf| > 2.
 */
static void test_regression_whole_subtree_removal(void)
{
	itree *t = itree_new();

	itree_add(t, 144, 992, 1029, NULL);
	itree_add(t, 157, 727, 729, NULL);
	itree_add(t, 170, 711, 803, NULL);
	itree_add(t, 223, 291, 327, NULL);
	itree_add(t, 228, 986, 1059, NULL);
	itree_add(t, 251, 502, 509, NULL);
	itree_add(t, 10, 597, 670, NULL);
	itree_add(t, 39, 416, 505, NULL);

	itree_remove_and_shift(t, 966, 998, 0);

	CHECK(itree_verify(t), "balanced after whole-subtree removal");
	itree_free(t);
}

/* ---------------------------------------------------------------------- */
/* Differential test vs the naive array                                   */
/* ---------------------------------------------------------------------- */

/*
 * One mirrored insertion: the same (id, interval, value) stored in both
 * implementations, plus a liveness flag.  A handle is dead once its
 * interval has been removed by an overlapping edit; after that the
 * underlying memory is freed in both implementations and must not be
 * touched (the paper's "haspt" flag).
 */
struct mirror {
	itree_loc tloc;
	iarray_slot *aslot;
	bool dead;
};

static void test_differential(void)
{
	uint32_t seed = 0xdeadbeef;
#define RAND()                                                           \
	(seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5,       \
	 (uint32_t)(seed >> 1))

	enum { OP_ADD, OP_FIND, OP_REMOVE_SHIFT, OP_POS, NOPS };
	enum { MAXID = 10, MAXPOS = 20, MAXLEN = 30, MAXSHIFT = 25 };

	itree *t = itree_new();
	iarray *a = iarray_new();

	struct mirror m[512];
	int nmirror = 0;

	for (int iter = 0; iter < 300000; iter++) {
		switch (RAND() % NOPS) {
		case OP_ADD: {
			if (nmirror == 512)
				break;
			int id = (int)(RAND() % MAXID);
			int low = (int)(RAND() % MAXPOS);
			int len = (int)(RAND() % MAXLEN) + 1;
			long value = nmirror + 1000;	/* unique value */

			m[nmirror].tloc = itree_add(t, id, low, low + len,
			                            (void *)value);
			m[nmirror].aslot = iarray_add(a, id, low, low + len,
			                              (void *)value);
			m[nmirror].dead = false;
			nmirror++;
			break;
		}
		case OP_FIND: {
			int id = (int)(RAND() % MAXID);
			int pos = (int)(RAND() % (MAXPOS + MAXSHIFT * 8));

			void *vt = itree_find_largest(t, id, pos);
			void *va = iarray_find_largest(a, id, pos);
			CHECK(vt == va,
			      "iter %d: find(%d, %d): tree %ld != array %ld",
			      iter, id, pos, (long)vt, (long)va);
			break;
		}
		case OP_REMOVE_SHIFT: {
			int low = (int)(RAND() % MAXPOS);
			int len = (int)(RAND() % 10) + 1;
			/*
			 * Valid edit: replaces len bytes with (len+amt)
			 * bytes, so amt >= -len.  Weaker shifts could break
			 * the tree's key ordering (see the stress test).
			 */
			int amt = (int)(RAND() % (MAXSHIFT + len)) - len;

			/*
			 * Retire mirrors whose interval overlaps the edit:
			 * both implementations free them.  The array's
			 * eager state is the oracle for the overlap test.
			 */
			for (int i = 0; i < nmirror; i++) {
				if (m[i].dead)
					continue;
				if (m[i].aslot->low < low + len &&
				    m[i].aslot->high > low)
					m[i].dead = true;
			}

			itree_remove_and_shift(t, low, low + len, amt);
			iarray_remove_and_shift(a, low, low + len, amt);
			break;
		}
		case OP_POS: {
			for (int i = 0; i < nmirror; i++) {
				if (m[i].dead || RAND() % 8 != 0)
					continue;	/* spot check */
				int tp = itree_loc_pos(m[i].tloc);
				int ap = m[i].aslot->low;
				CHECK(tp == ap,
				      "iter %d: loc[%d] pos %d != array %d",
				      iter, i, tp, ap);
			}
			break;
		}
		}

		/* Sizes must always agree. */
		if (itree_size(t) != iarray_size(a))
			CHECK(itree_size(t) == iarray_size(a),
			      "iter %d: size mismatch tree %zu != array %zu",
			      iter, itree_size(t), iarray_size(a));

		/*
		 * Structural check on a sample of iterations (verify is
		 * O(n) and applies all shifts, which also exercises
		 * draining the shift log mid-stream).
		 */
		if (iter % 997 == 0)
			CHECK(itree_verify(t), "invariants at iter %d", iter);
	}

	/* Final sweep: every still-live handle must agree. */
	for (int i = 0; i < nmirror; i++) {
		if (m[i].dead)
			continue;
		CHECK(itree_loc_pos(m[i].tloc) == m[i].aslot->low,
		      "final loc[%d] %d != %d", i, itree_loc_pos(m[i].tloc),
		      m[i].aslot->low);
	}

	itree_free(t);
	iarray_free(a);
#undef RAND
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	test_basic();
	test_add_find();
	test_remove_overlaps();
	test_lazy_shift();
	test_loc_survives_deletion();
	test_regression_whole_subtree_removal();
	test_stress_shifts_and_verify();
	test_differential();

	printf("interval tree: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
