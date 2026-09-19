/*
 * interval_tree.c - interval tree with lazy shifting, backed by an AVL tree.
 *
 * See interval_tree.h for the public contract and the design
 * rationale; the comments below focus on the internals.
 *
 * Structure
 * ---------
 * Each AVL node holds one key (position, id) and a group of intervals
 * sharing that key (they differ only in `high` and `value`).  The node is
 * augmented with `max`, an upper bound on the `high` of every interval in
 * its subtree, which lets overlap searches skip subtrees entirely.
 *
 * Lazy shifts
 * -----------
 * Shifts are recorded in a per-tree, append-only log with strictly
 * increasing timestamps.  Every node stores the timestamp of the newest
 * shift it has already absorbed; a node visited with pending shifts
 * replays only those newer than its timestamp.  Because a skipped shift
 * must also be skippable for the whole subtree, the per-node `max` is the
 * discriminator: a shift at index `idx` cannot affect a subtree whose
 * maximum is below `idx`.
 *
 * One subtlety inherited from the original: when a node absorbs a shift,
 * its children may not have absorbed it yet (each node is brought up to
 * date independently, in visit order).  A child that has not yet absorbed
 * a negative shift still holds pre-shift maxima, which over-estimate the
 * post-shift values.  update_max() therefore deliberately never decreases
 * a node's `max`, so that mixing frames can only over-estimate, never
 * under-estimate.  Over-estimation is harmless: it only weakens subtree
 * pruning, never correctness.
 */
#include "peg/interval_tree.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

/* Tree key: orders nodes by position, then by id. */
typedef struct {
	int pos;
	int id;
} iv_key;

static int iv_key_cmp(iv_key a, iv_key b)
{
	if (a.pos < b.pos)
		return -1;
	if (a.pos > b.pos)
		return 1;
	if (a.id < b.id)
		return -1;
	if (a.id > b.id)
		return 1;
	return 0;
}

/* One stored interval [low, high) and its user value. */
typedef struct {
	int low;
	int high;
	void *value;
} iv_ins;

/*
 * The group of intervals stored under one key.  This is the object an
 * itree_loc handle points to; it survives node deletions because remove()
 * swaps these structures between nodes rather than copying them, keeping
 * caller-held handles attached to the surviving data.
 */
struct itree_ivals {
	iv_ins *ins;		/* dynamic array of intervals */
	size_t n;
	size_t cap;
	struct itree_node *node;	/* node currently owning this group */
};

/* A recorded shift: intervals starting at or after `idx` move by `amt`. */
typedef struct {
	int idx;
	int amt;
	uint64_t tstamp;
} iv_shift;

typedef struct itree_node {
	iv_key key;
	int max;			/* upper bound on subtree highs */
	struct itree_ivals *iv;		/* interval group for this key */
	uint64_t tstamp;		/* newest shift absorbed by this node */
	struct itree *tree;		/* owning tree (for the shift log) */

	int height;			/* AVL height; leaf == 1 */
	struct itree_node *left;
	struct itree_node *right;
} iv_node;

struct itree {
	iv_node *root;
	iv_shift *shifts;		/* append-only log of shifts */
	size_t nshifts;
	size_t shiftcap;
	uint64_t tstamp;		/* timestamp of the newest shift */
};

/* ---------------------------------------------------------------------- */
/* Small helpers                                                          */
/* ---------------------------------------------------------------------- */

static int iv_max(int a, int b)
{
	return a > b ? a : b;
}

/* Highest endpoint among a group's intervals (0 if the group is empty). */
static int iv_group_high(const struct itree_ivals *iv)
{
	int m = 0;
	for (size_t i = 0; i < iv->n; i++)
		m = iv_max(m, iv->ins[i].high);
	return m;
}

/*
 * Half-open overlap test against [low, high).  Intervals that merely touch
 * do not overlap: memo entries are stored with their examined extent (one
 * past the furthest byte read), so an entry ending exactly at `low` never
 * read the edited bytes and stays valid, and one starting exactly at
 * `high` is simply shifted.
 */
static bool iv_overlaps(const iv_ins *in, int low, int high)
{
	return in->low < high && in->high > low;
}

static void iv_group_push(struct itree_ivals *iv, int low, int high,
                          void *value)
{
	if (iv->n == iv->cap) {
		iv->cap = iv->cap ? iv->cap * 2 : 4;
		iv->ins = xrealloc(iv->ins, iv->cap * sizeof(iv->ins[0]));
	}
	iv->ins[iv->n].low = low;
	iv->ins[iv->n].high = high;
	iv->ins[iv->n].value = value;
	iv->n++;
}

/* Move every interval in a group by `amt`. */
static void iv_group_shift(struct itree_ivals *iv, int amt)
{
	for (size_t i = 0; i < iv->n; i++) {
		iv->ins[i].low += amt;
		iv->ins[i].high += amt;
	}
}

/*
 * Recompute n->max from own intervals and children.  Deliberately never
 * shrinks the stored value: children may lag behind on lazy shifts and
 * still hold pre-shift maxima, so only over-estimation is safe (see the
 * file header).  The over-estimate merely weakens pruning.
 */
static void iv_update_max(iv_node *n)
{
	int m = iv_group_high(n->iv);
	if (n->left)
		m = iv_max(m, n->left->max);
	if (n->right)
		m = iv_max(m, n->right->max);
	if (m > n->max)
		n->max = m;
}

static void iv_update_height(iv_node *n)
{
	int lh = n->left ? n->left->height : 0;
	int rh = n->right ? n->right->height : 0;
	n->height = 1 + iv_max(lh, rh);
}

static int iv_balance(const iv_node *n)
{
	int lh = n->left ? n->left->height : 0;
	int rh = n->right ? n->right->height : 0;
	return lh - rh;
}

static void iv_free_node(iv_node *n)
{
	free(n->iv->ins);
	free(n->iv);
	free(n);
}

/* ---------------------------------------------------------------------- */
/* Lazy shift application                                                 */
/* ---------------------------------------------------------------------- */

/*
 * Absorb one shift into this node.  The timestamp is advanced even when
 * the shift cannot affect the subtree (max < idx), so the shift is never
 * reconsidered.
 */
static void iv_apply_shift(iv_node *n, const iv_shift *s)
{
	if (n->tstamp >= s->tstamp)
		return;			/* already applied */

	n->tstamp = s->tstamp;
	if (n->max >= s->idx) {
		n->max += s->amt;
		if (n->key.pos >= s->idx) {
			n->key.pos += s->amt;
			iv_group_shift(n->iv, s->amt);
		}
	}
	/*
	 * Note: no update_max here.  A node that skipped the shift (max <
	 * idx) must not take the max against children that may have applied
	 * it (and grown): that happens in iv_apply_shifts below, once, after
	 * the node is fully current.
	 */
}

/* Bring node n up to date with the tree's shift log. */
static void iv_apply_shifts(iv_node *n)
{
	struct itree *t = n->tree;

	if (t->nshifts == 0)
		return;

	if (n->tstamp < t->shifts[t->nshifts - 1].tstamp) {
		/*
		 * Shifts have strictly increasing timestamps, so the
		 * pending ones are a suffix of the log.  Find the first
		 * one newer than our timestamp by walking back from the
		 * end; iv_apply_shift re-checks each shift's timestamp
		 * anyway, so an off-by-one start index only costs a
		 * redundant (and self-skipping) application.
		 */
		size_t j = t->nshifts;
		while (j > 0 && t->shifts[j - 1].tstamp > n->tstamp)
			j--;
		for (; j < t->nshifts; j++)
			iv_apply_shift(n, &t->shifts[j]);
	}

	/*
	 * Re-establish the subtree-max bound against the children's current
	 * stored maxima.  This must run even when this node had nothing to
	 * absorb: a child visited by an earlier traversal may have applied
	 * shifts this node already carried and grown past this node's
	 * (stale) max, and the max is what overlap searches prune on — an
	 * under-estimate could skip a subtree containing an overlapping
	 * interval.  update_max only grows the stored value, so lagging
	 * children (whose stored maxima are pre-shift, possibly stale-high)
	 * merely over-estimate, which is safe.
	 */
	iv_update_max(n);
}

/* Record a shift in the log; actual application happens lazily. */
static void iv_record_shift(struct itree *t, int idx, int amt)
{
	if (amt == 0)
		return;

	t->tstamp++;
	if (t->nshifts == t->shiftcap) {
		t->shiftcap = t->shiftcap ? t->shiftcap * 2 : 16;
		t->shifts = xrealloc(t->shifts, t->shiftcap * sizeof(t->shifts[0]));
	}
	t->shifts[t->nshifts].idx = idx;
	t->shifts[t->nshifts].amt = amt;
	t->shifts[t->nshifts].tstamp = t->tstamp;
	t->nshifts++;
}

/* ---------------------------------------------------------------------- */
/* AVL rebalancing                                                        */
/* ---------------------------------------------------------------------- */

static iv_node *iv_rotate_left(iv_node *n)
{
	iv_node *newroot = n->right;

	/*
	 * Both nodes are about to change subtree contents, so both must be
	 * up to date on shifts before their maxima are recomputed.
	 */
	iv_apply_shifts(n);
	iv_apply_shifts(newroot);

	n->right = newroot->left;
	newroot->left = n;

	iv_update_height(n);
	iv_update_max(n);
	iv_update_height(newroot);
	iv_update_max(newroot);
	return newroot;
}

static iv_node *iv_rotate_right(iv_node *n)
{
	iv_node *newroot = n->left;

	iv_apply_shifts(n);
	iv_apply_shifts(newroot);

	n->left = newroot->right;
	newroot->right = n;

	iv_update_height(n);
	iv_update_max(n);
	iv_update_height(newroot);
	iv_update_max(newroot);
	return newroot;
}

/*
 * Rebalance the subtree rooted at n and return its new root.
 *
 * Unlike textbook AVL code, this handles arbitrary imbalance, not just
 * |bf| == 2.  An overlap-removal traversal can delete an entire subtree
 * in one recursive call (every interval in it overlapping the edit), so a
 * node's balance factor can jump by more than 1 — e.g. from +1 to +3 when
 * both descendants on one side are removed.  A single rotation leaves such
 * a node unbalanced, so after rotating we recursively re-rebalance both
 * the new root and the demoted node.  Both recursions operate on strictly
 * smaller subtrees (the demoted node's new height is below its old one),
 * so this terminates; when called with |bf| <= 1 it is the usual
 * single/double rotation and costs nothing extra.
 *
 * The pre-rotation of the tall child (the "left-right"/"right-left" case)
 * keeps the child's imbalance aligned with the parent's so the following
 * rotation makes progress.
 */
static iv_node *iv_rebalance(iv_node *n)
{
	if (n == NULL)
		return NULL;

	iv_update_height(n);
	iv_update_max(n);

	int bf = iv_balance(n);
	if (bf > 1) {
		/* left-heavy; if the child leans right, rotate it first */
		if (iv_balance(n->left) < 0)
			n->left = iv_rotate_left(n->left);
		n = iv_rotate_right(n);
		n->right = iv_rebalance(n->right);	/* demoted node */
		return iv_rebalance(n);			/* new root */
	}
	if (bf < -1) {
		/* right-heavy; if the child leans left, rotate it first */
		if (iv_balance(n->right) > 0)
			n->right = iv_rotate_right(n->right);
		n = iv_rotate_left(n);
		n->left = iv_rebalance(n->left);	/* demoted node */
		return iv_rebalance(n);			/* new root */
	}
	return n;
}

/* ---------------------------------------------------------------------- */
/* Insertion                                                              */
/* ---------------------------------------------------------------------- */

static iv_node *iv_add(iv_node *n, struct itree *t, iv_key key, int high,
                       void *value, struct itree_ivals **loc)
{
	if (n == NULL) {
		n = xmalloc(sizeof(*n));
		n->iv = xmalloc(sizeof(*n->iv));
		n->iv->ins = NULL;
		n->iv->n = n->iv->cap = 0;
		n->iv->node = n;
		iv_group_push(n->iv, key.pos, high, value);

		n->key = key;
		n->max = high;
		/*
		 * The caller's coordinates are already post-shift, so the
		 * node starts out current with the log.
		 */
		n->tstamp = t->tstamp;
		n->tree = t;
		n->height = 1;
		n->left = n->right = NULL;
		*loc = n->iv;
		return n;
	}

	iv_apply_shifts(n);

	int c = iv_key_cmp(key, n->key);
	if (c < 0) {
		n->left = iv_add(n->left, t, key, high, value, loc);
	} else if (c > 0) {
		n->right = iv_add(n->right, t, key, high, value, loc);
	} else {
		/* same key: the interval joins the existing group */
		iv_group_push(n->iv, key.pos, high, value);
		n->tstamp = t->tstamp;	/* new interval is in post-shift coords */
		*loc = n->iv;
	}
	return iv_rebalance(n);
}

/* ---------------------------------------------------------------------- */
/* Deletion                                                               */
/* ---------------------------------------------------------------------- */

/* Smallest (leftmost) node in the subtree rooted at n. */
static iv_node *iv_find_smallest(iv_node *n)
{
	while (n->left) {
		iv_apply_shifts(n->left);
		n = n->left;
	}
	return n;
}

/*
 * Remove the node with the given key from the subtree rooted at n and
 * rebalance.  Only ever called (from iv_remove_overlaps) on nodes whose
 * interval group has just been emptied, so the node that is actually
 * freed always carries an empty group; groups holding live intervals are
 * swapped up the tree instead, which keeps itree_loc handles valid.
 */
static iv_node *iv_remove(iv_node *n, struct itree *t, iv_key key)
{
	if (n == NULL)
		return NULL;

	iv_apply_shifts(n);

	int c = iv_key_cmp(key, n->key);
	if (c < 0) {
		n->left = iv_remove(n->left, t, key);
	} else if (c > 0) {
		n->right = iv_remove(n->right, t, key);
	} else {
		if (n->left && n->right) {
			/*
			 * Two children: adopt the key and interval group of
			 * the in-order successor, then delete the successor.
			 * The group is swapped (not copied) so handles stay
			 * attached to the surviving data, and the successor
			 * node ends up holding our emptied group, which is
			 * what gets freed when the recursion bottoms out.
			 */
			iv_apply_shifts(n->left);
			iv_apply_shifts(n->right);
			iv_node *succ = iv_find_smallest(n->right);
			iv_key succ_key = succ->key;

			n->key = succ->key;
			struct itree_ivals *tmp = n->iv;
			n->iv = succ->iv;
			succ->iv = tmp;
			n->iv->node = n;
			succ->iv->node = succ;
			n->tstamp = succ->tstamp;

			n->right = iv_remove(n->right, t, succ_key);
		} else if (n->left) {
			iv_apply_shifts(n->left);
			iv_node *child = n->left;
			iv_free_node(n);
			n = child;
		} else if (n->right) {
			iv_apply_shifts(n->right);
			iv_node *child = n->right;
			iv_free_node(n);
			n = child;
		} else {
			iv_free_node(n);
			return NULL;
		}
	}
	return iv_rebalance(n);
}

/*
 * Remove every interval overlapping [low, high) from the subtree rooted
 * at n.  Mirrors the overlap traversal of Algorithm 2 in the paper: the
 * left subtree is always explored, the right subtree only when the query
 * can reach past this node's key.
 *
 * Every unwind goes through iv_rebalance: node deletions deep in the
 * traversal shrink subtrees, and without rebalancing on the way back up
 * the tree slowly accumulates imbalance (the AVL bound is what makes
 * edits O(log n), so it is enforced for real).
 *
 * `on_evict` (if set) is called with each evicted value right before
 * the interval is unlinked, so the caller can release it.
 */
static iv_node *iv_remove_overlaps(iv_node *n, int low, int high,
                                   void (*on_evict)(void *, int, int,
                                                    void *),
                                   void *ud)
{
	if (n == NULL)
		return NULL;

	iv_apply_shifts(n);

	if (low > n->max)
		return n;			/* whole subtree is left of the query */

	n->left = iv_remove_overlaps(n->left, low, high, on_evict, ud);

	/* Drop overlapping intervals from this node's group (swap with last). */
	size_t i = 0;
	while (i < n->iv->n) {
		if (iv_overlaps(&n->iv->ins[i], low, high)) {
			if (on_evict)
				on_evict(n->iv->ins[i].value,
				         n->iv->ins[i].low, n->iv->ins[i].high,
				         ud);
			n->iv->ins[i] = n->iv->ins[--n->iv->n];
		} else {
			i++;
		}
	}

	if (n->iv->n == 0) {
		/*
		 * The group is exhausted; delete the node.  If the query
		 * reaches this node's key the replacement (in-order
		 * successor, which comes from the right subtree) still has
		 * to be checked, so restart the traversal there.
		 */
		bool doright = high >= n->key.pos;
		n = iv_remove(n, n->tree, n->key);
		if (doright)
			n = iv_remove_overlaps(n, low, high, on_evict, ud);
		return iv_rebalance(n);
	}

	if (high < n->key.pos)
		return iv_rebalance(n);		/* query ends left of this key */

	n->right = iv_remove_overlaps(n->right, low, high, on_evict, ud);
	return iv_rebalance(n);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

itree *itree_new(void)
{
	return xcalloc(1, sizeof(itree));
}

void itree_free(itree *t)
{
	if (t == NULL)
		return;

	/* Iterative destruction to avoid deep recursion on large trees. */
	for (;;) {
		iv_node *n = t->root;
		if (n == NULL)
			break;
		/* Rotate the minimum to the root and pull it off. */
		while (n->left) {
			iv_apply_shifts(n->left);
			n = iv_rotate_right(n);
			t->root = n;
		}
		t->root = n->right;
		iv_free_node(n);
	}
	free(t->shifts);
	free(t);
}

itree_loc itree_add(itree *t, int id, int low, int high, void *value)
{
	struct itree_ivals *loc;
	t->root = iv_add(t->root, t, (iv_key){ .pos = low, .id = id },
	                 high, value, &loc);
	return loc;
}

void *itree_find_largest(itree *t, int id, int pos)
{
	iv_node *n = t->root;
	while (n) {
		iv_apply_shifts(n);
		int c = iv_key_cmp((iv_key){ .pos = pos, .id = id }, n->key);
		if (c < 0)
			n = n->left;
		else if (c > 0)
			n = n->right;
		else
			break;
	}
	if (n == NULL || n->iv->n == 0)
		return NULL;

	/* The group shares its low end, so the longest is the highest. */
	size_t best = 0;
	for (size_t i = 1; i < n->iv->n; i++) {
		int l = n->iv->ins[i].high - n->iv->ins[i].low;
		int b = n->iv->ins[best].high - n->iv->ins[best].low;
		if (l > b)
			best = i;
	}
	return n->iv->ins[best].value;
}

void itree_remove_and_shift(itree *t, int low, int high, int amt)
{
	itree_remove_and_shift_cb(t, low, high, amt, NULL, NULL);
}

void itree_remove_and_shift_cb(itree *t, int low, int high, int amt,
                               void (*on_evict)(void *, int, int, void *),
                               void *ud)
{
	t->root = iv_remove_overlaps(t->root, low, high, on_evict, ud);
	iv_record_shift(t, low, amt);
}

static size_t iv_size(const iv_node *n)
{
	if (n == NULL)
		return 0;
	return iv_size(n->left) + iv_size(n->right) + n->iv->n;
}

size_t itree_size(itree *t)
{
	return iv_size(t->root);
}

static void iv_each(iv_node *n,
                    void (*fn)(void *value, int low, int high, void *ud),
                    void *ud)
{
	if (n == NULL)
		return;
	iv_each(n->left, fn, ud);
	iv_apply_shifts(n);
	for (size_t i = 0; i < n->iv->n; i++)
		fn(n->iv->ins[i].value, n->iv->ins[i].low, n->iv->ins[i].high,
		   ud);
	iv_each(n->right, fn, ud);
}

void itree_each(itree *t,
                void (*fn)(void *value, int low, int high, void *ud),
                void *ud)
{
	iv_each(t->root, fn, ud);
}

int itree_loc_pos(itree_loc loc)
{
	iv_apply_shifts(loc->node);
	return loc->node->key.pos;
}

static void iv_apply_all(iv_node *n)
{
	if (n == NULL)
		return;
	iv_apply_all(n->left);
	iv_apply_all(n->right);
	iv_apply_shifts(n);
}

void itree_apply_all_shifts(itree *t)
{
	iv_apply_all(t->root);
	/*
	 * Every node now carries the newest timestamp, so the log can be
	 * dropped; future shifts get fresh, larger timestamps.
	 */
	t->nshifts = 0;
}

/* ---------------------------------------------------------------------- */
/* Verification (tests/debugging)                                         */
/* ---------------------------------------------------------------------- */

static bool iv_verify_node(const iv_node *n, const iv_key *lo,
                           const iv_key *hi, int *height)
{
	if (n == NULL) {
		*height = 0;
		return true;
	}

	/* BST ordering (strict: equal keys share a node). */
	if (lo && iv_key_cmp(n->key, *lo) <= 0) {
		fprintf(stderr, "itree_verify: key order violated\n");
		return false;
	}
	if (hi && iv_key_cmp(n->key, *hi) >= 0) {
		fprintf(stderr, "itree_verify: key order violated\n");
		return false;
	}

	if (n->iv->n == 0) {
		fprintf(stderr, "itree_verify: empty interval group\n");
		return false;
	}
	for (size_t i = 0; i < n->iv->n; i++) {
		if (n->iv->ins[i].low != n->key.pos) {
			fprintf(stderr,
			        "itree_verify: interval low != key position\n");
			return false;
		}
		if (n->iv->ins[i].high < n->iv->ins[i].low) {
			fprintf(stderr, "itree_verify: inverted interval\n");
			return false;
		}
	}

	int lh, rh;
	if (!iv_verify_node(n->left, lo, &n->key, &lh))
		return false;
	if (!iv_verify_node(n->right, &n->key, hi, &rh))
		return false;

	if (n->height != 1 + iv_max(lh, rh)) {
		fprintf(stderr, "itree_verify: stale height\n");
		return false;
	}
	if (lh - rh > 1 || rh - lh > 1) {
		fprintf(stderr, "itree_verify: AVL balance violated\n");
		return false;
	}

	/* The stored max may over-estimate but never under-estimate. */
	int exact = iv_group_high(n->iv);
	if (n->left)
		exact = iv_max(exact, n->left->max);
	if (n->right)
		exact = iv_max(exact, n->right->max);
	if (n->max < exact) {
		fprintf(stderr,
		        "itree_verify: subtree max under-estimates (%d < %d)\n",
		        n->max, exact);
		return false;
	}

	*height = n->height;
	return true;
}

bool itree_verify(itree *t)
{
	itree_apply_all_shifts(t);

	if (t->nshifts != 0) {
		fprintf(stderr, "itree_verify: shift log not drained\n");
		return false;
	}

	int height;
	return iv_verify_node(t->root, NULL, NULL, &height);
}
