/*
 * interval_array.h - naive reference implementation of the interval map.
 *
 * A plain unsorted array of (id, interval, value) slots implementing the
 * same abstract interface as interval_tree.h.  Every operation is linear
 * in the number of stored intervals, which makes this implementation far
 * too slow for the parser itself, but it is a deliberately simple oracle:
 * the randomized differential test cross-checks the AVL interval tree
 * against it op by op.
 *
 * The semantics match the interval tree's exactly: half-open overlap,
 * and shifts apply to intervals whose start is >= the shift index.
 */
#ifndef PEG_INTERVAL_ARRAY_H
#define PEG_INTERVAL_ARRAY_H

#include <stddef.h>

/* Opaque array. */
typedef struct iarray iarray;

/*
 * A stable handle to one stored interval (the counterpart of itree_loc
 * for the reference implementation).  `slot->low` is the current start
 * position and is kept up to date by shifts.
 */
typedef struct iarray_slot {
	int id;
	int low;
	int high;
	void *value;
} iarray_slot;

/* Create an empty interval array. */
iarray *iarray_new(void);

/* Free the array (but not the user values).  NULL is allowed. */
void iarray_free(iarray *a);

/* Insert the interval [low, high) with identifier `id`. */
iarray_slot *iarray_add(iarray *a, int id, int low, int high, void *value);

/*
 * Return the value of the largest interval stored at exactly (id, pos),
 * or NULL if there is none.  Ties break toward the earliest inserted,
 * matching itree_find_largest.
 */
void *iarray_find_largest(iarray *a, int id, int pos);

/*
 * Remove every interval overlapping [low, high), then shift every
 * remaining interval whose start is >= low by amt.  Semantically
 * identical to itree_remove_and_shift.
 */
void iarray_remove_and_shift(iarray *a, int low, int high, int amt);

/* The number of stored intervals. */
size_t iarray_size(iarray *a);

#endif /* PEG_INTERVAL_ARRAY_H */
