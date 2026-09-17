/*
 * peg_interval.h - interval tree with lazy shifting.
 *
 * A key-value map from intervals to user values, implemented as an AVL tree
 * augmented with subtree-maximum information (a classic interval tree).  It
 * is the data structure behind the memoization table of the incremental
 * packrat parser described in "Fast Incremental PEG Parsing" (Yedidia &
 * Chong, SLE '21), Section 4.1/4.2.
 *
 * Features
 * --------
 *  - Insert an interval with an integer id:                 O(log n)
 *  - Look up the largest interval at (id, position):         O(log n)
 *  - Delete every interval overlapping [low, high):          O(m + log n)
 *  - Shift all intervals at/after an index by an amount:     amortized O(1)
 *
 * The last operation is what makes incremental parsing cheap: applying a
 * text edit must move every memo entry that starts after the edit, which
 * would be linear if done eagerly.  Instead the shift is appended to a
 * global log of shifts and applied lazily: each tree node carries a
 * timestamp of the newest shift it has absorbed, and any code that observes
 * a node first brings it up to date by replaying shifts newer than its
 * timestamp.
 *
 * Keys and values
 * ---------------
 * The tree is keyed by (position, id) pairs; the id distinguishes multiple
 * intervals that start at the same position (in the memo table, the id is
 * the grammar rule being memoized).  All intervals starting at the same
 * position with the same id share one tree node and are stored together in
 * a small array; itree_find_largest returns the value of the longest of
 * them, which lets a reparse skip as much input as possible.
 *
 * Semantics
 * ---------
 *  - Intervals are half-open: [low, high).  Two intervals overlap iff
 *    a.low < b.high && a.high > b.low; intervals that merely touch do not
 *    overlap.  Memo entries are stored with their *examined* extent, so an
 *    entry ending exactly at the start of an edit never read the edited
 *    bytes and correctly stays valid.
 *  - itree_remove_and_shift first removes every interval overlapping
 *    [low, high), then records a shift of `amt` starting at `low`.  The
 *    ordering guarantees no interval crosses the shift index when it is
 *    applied.  Shifts move exactly the intervals whose start is >= the
 *    shift index; intervals entirely to the left are untouched.
 *  - A negative shift may move intervals onto positions occupied by
 *    intervals to the left; callers (the memo table) must ensure this
 *    cannot happen, i.e. that the caller's coordinate space stays
 *    consistent.
 *
 * Value ownership
 * ---------------
 * Values are opaque pointers owned by the caller.  The tree never
 * dereferences or frees them; itree_free releases only the tree's own
 * memory.
 *
 * Location handles
 * ----------------
 * itree_add returns an itree_loc handle that can be queried for the
 * interval's current start position with itree_loc_pos even after lazy
 * shifts have moved it.  This is how the memo table makes parse captures
 * "relocatable" (paper Section 4.2, "Relocatable Parse Results").  A handle
 * stays valid as long as the interval it was created for has not been
 * removed from the tree (removal happens only through
 * itree_remove_and_shift when the interval overlaps the edited range).
 */
#ifndef PEG_INTERVAL_TREE_H
#define PEG_INTERVAL_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * C linkage for C++ callers (peg.hpp).  The definitions are compiled as
 * C, so without this every symbol here would be mangled on the way in.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Opaque tree. */
typedef struct itree itree;

/*
 * Opaque handle to the group of intervals stored under one (position, id)
 * key, as returned by itree_add.  Query its (shifted) start position with
 * itree_loc_pos.  Valid until the interval is removed from the tree.
 */
typedef struct itree_ivals *itree_loc;

/* Create an empty interval tree.  Never returns NULL (aborts on OOM). */
itree *itree_new(void);

/*
 * Free the tree and all nodes and shift bookkeeping.  User values are not
 * freed.  `t` may be NULL, in which case this is a no-op.
 */
void itree_free(itree *t);

/*
 * Insert the interval [low, high) with identifier `id` and associated
 * `value`.  If the key (low, id) already exists the interval joins the
 * existing group (itree_find_largest will consider it).  Requires
 * low <= high.  Returns a location handle for the inserted interval.
 */
itree_loc itree_add(itree *t, int id, int low, int high, void *value);

/*
 * Return the value of the largest interval stored at exactly (id, pos),
 * or NULL if there is none.  Ties are broken by insertion order (the
 * earliest inserted of the equally-long intervals wins).  Applies any
 * pending lazy shifts along the search path.
 */
void *itree_find_largest(itree *t, int id, int pos);

/*
 * Remove every interval that overlaps [low, high), then record a lazy
 * shift by `amt` affecting every remaining interval whose start is >= low.
 * `amt` may be zero (pure removal) or negative (deletion).  After this
 * call no interval in the tree crosses or contains any point of
 * [low, high).
 */
void itree_remove_and_shift(itree *t, int low, int high, int amt);

/*
 * Like itree_remove_and_shift, but every evicted value is passed to
 * `on_evict(value, low, high, ud)` before being unlinked from the tree,
 * letting the owner (the memo table) release entries the interval tree
 * cannot free itself.  `on_evict` may be NULL, in which case values are
 * dropped silently (the tree never dereferences them).
 */
void itree_remove_and_shift_cb(itree *t, int low, int high, int amt,
                               void (*on_evict)(void *value, int low,
                                                int high, void *ud),
                               void *ud);

/*
 * Return the number of intervals (values) currently stored in the tree.
 * A key holding several intervals counts once per interval.
 */
size_t itree_size(itree *t);

/*
 * Call `fn(value, low, high, ud)` for every interval in the tree, in
 * ascending key order.  Pending lazy shifts are applied, so the reported
 * positions are current.
 */
void itree_each(itree *t,
                void (*fn)(void *value, int low, int high, void *ud),
                void *ud);

/*
 * Return the current start position of the interval group `loc` refers to,
 * applying any pending lazy shifts first.
 */
int itree_loc_pos(itree_loc loc);

/*
 * Immediately apply every pending shift to every node and clear the shift
 * log.  Purely an optimization/memory measure: lazy application produces
 * the same positions; this just does it all at once.
 */
void itree_apply_all_shifts(itree *t);

/*
 * Structural self-check for tests and debugging.  Applies all shifts, then
 * verifies: AVL balance and height correctness, key ordering, every node's
 * interval group non-empty with all lows equal to the key position, and
 * every subtree maximum an upper bound on the true maximum (the stored
 * maximum may over-estimate but never under-estimate).  Returns true if
 * all invariants hold; otherwise prints diagnostics to stderr.
 */
bool itree_verify(itree *t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PEG_INTERVAL_TREE_H */
