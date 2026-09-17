/*
 * memo.c - memoization table for incremental packrat parsing.
 * See peg_memo.h for the public contract.
 */
#include "peg/peg_memo.h"

#include <stdlib.h>

#include "peg/peg_util.h"

/* ---------------------------------------------------------------------- */
/* Capture                                                                */
/* ---------------------------------------------------------------------- */

enum { CAP_NODE, CAP_DUMMY };

struct memo_capture {
	int id;			/* grammar id (unused for dummies) */
	int type;		/* CAP_NODE or CAP_DUMMY */

	/*
	 * If ment is NULL: off is an absolute position (the capture was
	 * just built and has not been attached to an entry yet, or its
	 * entry was destroyed and the position re-absolutized).
	 * If ment is set: off is relative to the entry's (shifted) start,
	 * so the capture relocates automatically with the entry.
	 */
	int off;
	int length;
	struct memo_entry *ment;	/* weak reference */

	uint32_t refs;		/* reference count (see peg_memo.h) */

	memo_capture **children;
	size_t nchild;
	size_t cap;
};

memo_capture *memo_capture_node(int id, int start, int length)
{
	memo_capture *c = xmalloc(sizeof(*c));
	c->id = id;
	c->type = CAP_NODE;
	c->off = start;
	c->length = length;
	c->ment = NULL;
	c->refs = 1;
	c->children = NULL;
	c->nchild = c->cap = 0;
	return c;
}

memo_capture *memo_capture_dummy(int start, int length)
{
	memo_capture *c = xmalloc(sizeof(*c));
	c->id = 0;
	c->type = CAP_DUMMY;
	c->off = start;
	c->length = length;
	c->ment = NULL;
	c->refs = 1;
	c->children = NULL;
	c->nchild = c->cap = 0;
	return c;
}

void memo_capture_ref(memo_capture *c)
{
	if (c != NULL)
		c->refs++;
}

/*
 * Drop a reference; destroy the capture when the count reaches zero.
 * Destruction releases the children array's references recursively.
 */
void memo_capture_free(memo_capture *c)
{
	if (c == NULL || --c->refs != 0)
		return;
	for (size_t i = 0; i < c->nchild; i++)
		memo_capture_free(c->children[i]);
	free(c->children);
	free(c);
}

void memo_capture_add(memo_capture *c, memo_capture *child)
{
	if (c->nchild == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 4;
		c->children = xrealloc(c->children,
		                       c->cap * sizeof(c->children[0]));
	}
	/* The children array holds one reference per stored pointer. */
	c->children[c->nchild++] = child;
	memo_capture_ref(child);
}

int memo_capture_id(const memo_capture *c)
{
	return c->id;
}

bool memo_capture_dummy_p(const memo_capture *c)
{
	return c->type == CAP_DUMMY;
}

int memo_capture_num_children(const memo_capture *c)
{
	int n = 0;
	for (size_t i = 0; i < c->nchild; i++) {
		if (memo_capture_dummy_p(c->children[i]))
			n += memo_capture_num_children(c->children[i]);
		else
			n++;
	}
	return n;
}

int memo_capture_start(const memo_capture *c)
{
	if (c->ment != NULL)
		return memo_entry_pos(c->ment) + c->off;
	return c->off;
}

int memo_capture_len(const memo_capture *c)
{
	return c->length;
}

int memo_capture_end(const memo_capture *c)
{
	return memo_capture_start(c) + c->length;
}

static void capture_each(const memo_capture *c,
                         void (*fn)(const memo_capture *c, void *ud),
                         void *ud)
{
	for (size_t i = 0; i < c->nchild; i++) {
		const memo_capture *ch = c->children[i];
		if (memo_capture_dummy_p(ch)) {
			capture_each(ch, fn, ud);
		} else {
			fn(ch, ud);
		}
	}
}

void memo_capture_each_child(const memo_capture *c,
                             void (*fn)(const memo_capture *c, void *ud),
                             void *ud)
{
	capture_each(c, fn, ud);
}

const memo_capture *memo_capture_child(const memo_capture *c, int n)
{
	int i = 0;
	for (size_t j = 0; j < c->nchild; j++) {
		const memo_capture *ch = c->children[j];
		if (memo_capture_dummy_p(ch)) {
			const memo_capture *sub = memo_capture_child(ch, n - i);
			if (sub != NULL)
				return sub;
			i += memo_capture_num_children(ch);
		} else if (i == n) {
			return ch;
		} else {
			i++;
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* Entry                                                                  */
/* ---------------------------------------------------------------------- */

struct memo_entry {
	int length;		/* -1 = failed to match */
	int examined;
	int count;		/* repetitions folded in (tree memoization) */
	memo_capture **captures;
	size_t ncap;
	itree_loc pos;		/* interval-tree location handle */
};

int memo_entry_pos(const memo_entry *e)
{
	return itree_loc_pos(e->pos);
}

/*
 * Forget `entry` as the owner of this capture and of any descendant
 * attached to the same entry, converting offsets to absolute positions.
 * Descends unconditionally (not only through nodes owned by this entry):
 * shared subtrees can mix ownership boundaries, and nodes owned by other
 * entries are simply skipped by the ment test.
 */
void memo_capture_absolutize(memo_capture *c, struct memo_entry *entry)
{
	if (c == NULL)
		return;
	if (c->ment == entry) {
		c->off += memo_entry_pos(entry);
		c->ment = NULL;
	}
	for (size_t i = 0; i < c->nchild; i++)
		memo_capture_absolutize(c->children[i], entry);
}

static void capture_set_ment(memo_capture *c, memo_entry *e);

/*
 * Attach an entry's captures to it, converting their absolute offsets
 * into entry-relative ones.  Idempotent per capture:
 * capture_set_ment returns early if the capture already belongs to an
 * entry, which also keeps shared capture subtrees from double-shifting.
 */
static void entry_set_pos(memo_entry *e)
{
	for (size_t i = 0; i < e->ncap; i++)
		capture_set_ment(e->captures[i], e);
}

static void capture_set_ment(memo_capture *c, memo_entry *e)
{
	if (c->ment != NULL)
		return;

	c->off -= memo_entry_pos(e);
	c->ment = e;

	for (size_t i = 0; i < c->nchild; i++)
		capture_set_ment(c->children[i], e);
}

int memo_entry_length(const memo_entry *e)
{
	return e->length;
}

int memo_entry_examined(const memo_entry *e)
{
	return e->examined;
}

int memo_entry_count(const memo_entry *e)
{
	return e->count;
}

memo_capture **memo_entry_captures(const memo_entry *e, size_t *n)
{
	*n = e->ncap;
	return e->captures;
}

/* ---------------------------------------------------------------------- */
/* Table                                                                  */
/* ---------------------------------------------------------------------- */

struct memo_table {
	itree *tree;
	int threshold;
};

memo_table *memo_table_new(int threshold)
{
	memo_table *t = xmalloc(sizeof(*t));
	t->tree = itree_new();
	t->threshold = threshold;
	return t;
}

/*
 * Freeing the table frees every entry; each entry frees its captures.
 * The interval tree itself frees its nodes but not the entry values.
 * Before releasing a capture, re-absolutize any still-attached capture
 * (an outer result tree may hold a reference to it), so no dangling
 * ment pointer survives the entry.
 */
static void table_free_entry(void *value, int low, int high, void *ud)
{
	memo_entry *e = value;
	(void)low;
	(void)high;
	(void)ud;
	for (size_t i = 0; i < e->ncap; i++)
		memo_capture_absolutize(e->captures[i], e);
	for (size_t i = 0; i < e->ncap; i++)
		memo_capture_free(e->captures[i]);
	free(e->captures);
	free(e);
}

void memo_table_free(memo_table *t)
{
	if (t == NULL)
		return;
	itree_each(t->tree, table_free_entry, NULL);
	itree_free(t->tree);
	free(t);
}

memo_entry *memo_table_get(memo_table *t, int id, int pos)
{
	return itree_find_largest(t->tree, id, pos);
}

void memo_table_put(memo_table *t, int id, int start, int length,
                    int examined, int count, memo_capture **captures,
                    size_t ncap)
{
	/*
	 * Below-threshold entries are not worth their space: tree
	 * memoization guarantees repeated small matches are covered by
	 * larger parent entries (paper Section 4.4).  Zero-length
	 * matches are likewise useless to memoize.  The captures are
	 * dropped with the entry.
	 */
	if ((t->threshold > 0 && examined < t->threshold) || length == 0) {
		for (size_t i = 0; i < ncap; i++)
			memo_capture_free(captures[i]);
		free(captures);
		return;
	}

	if (examined < length)
		examined = length;

	memo_entry *e = xmalloc(sizeof(*e));
	e->length = length;
	e->examined = examined;
	e->count = count;
	e->captures = captures;
	e->ncap = ncap;

	e->pos = itree_add(t->tree, id, start, start + examined, e);
	entry_set_pos(e);
}

void memo_table_apply_edit(memo_table *t, memo_edit edit)
{
	int low = edit.start;
	int high = edit.end;
	/*
	 * A pure insertion (low == high) must still evict entries: the
	 * inserted bytes land at `low`, so anything whose examined extent
	 * reaches low has read a byte that has now changed.  Widen the
	 * query by one to make the half-open overlap test cover it.
	 */
	if (low == high)
		high = low + 1;

	int amt = edit.len - (edit.end - edit.start);
	/*
	 * The interval tree cannot free evicted entries itself, so every
	 * invalidated entry is handed to the same destructor used at
	 * table teardown.
	 */
	itree_remove_and_shift_cb(t->tree, low, high, amt, table_free_entry,
	                          NULL);
}

size_t memo_table_size(memo_table *t)
{
	return itree_size(t->tree);
}
