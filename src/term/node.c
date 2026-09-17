/*
 * node.c - the term node: construction, reference counting, list and
 * sub-list surgery, the tag registry, and the structural checker.
 *
 * peg_term.h documents the ownership model; everything below implements
 * exactly that and nothing more.  In particular the list operations only
 * ever move pointers: none of them takes or drops a reference, which is
 * the property that makes a splice O(1) and the reason each of them can
 * document "the caller's reference moves here".
 */
#include "peg/peg_term.h"

#include "term_internal.h"

/* ---------------------------------------------------------------------- */
/* Tags                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * The registry is a flat array; the tag *is* the index.  Interning is
 * linear in the number of tags, which is fine because it happens once per
 * rule at grammar-construction time, never per parse.
 */
static char **tag_names;
static size_t tag_count;
static size_t tag_cap;

/* Seeded in this order so the built-in ids stay put across a reset. */
static const char *const builtin_names[PEG_TAG_BUILTIN_COUNT] = {
	"_", "root", "term", "text", "error"
};

static bool tags_ready;

/* Seed the built-ins on first use. */
static void tags_init(void)
{
	/*
	 * Set the flag first: peg_tag_intern calls back into here, and the
	 * recursion would otherwise re-seed on every level.
	 */
	if (tags_ready)
		return;
	tags_ready = true;

	for (size_t i = 0; i < PEG_TAG_BUILTIN_COUNT; i++)
		(void)peg_tag_intern(builtin_names[i]);
}

peg_tag peg_tag_intern(const char *name)
{
	tags_init();

	if (name == NULL || name[0] == '\0')
		return PEG_TAG_NONE;

	for (size_t i = 0; i < tag_count; i++) {
		if (strcmp(tag_names[i], name) == 0)
			return (peg_tag)i;
	}

	if (tag_count == tag_cap) {
		tag_cap = tag_cap ? tag_cap * 2 : PEG_TAG_BUILTIN_COUNT;
		tag_names = peg_term_xrealloc(tag_names,
		                              tag_cap * sizeof(tag_names[0]));
	}
	tag_names[tag_count] = peg_term_xstrdup(name);
	return (peg_tag)tag_count++;
}

peg_tag peg_tag_lookup(const char *name)
{
	tags_init();

	if (name == NULL || name[0] == '\0')
		return PEG_TAG_NONE;

	for (size_t i = 0; i < tag_count; i++) {
		if (strcmp(tag_names[i], name) == 0)
			return (peg_tag)i;
	}
	return PEG_TAG_NONE;
}

const char *peg_tag_name(peg_tag tag)
{
	tags_init();

	if ((size_t)tag < tag_count)
		return tag_names[tag];
	return NULL;
}

size_t peg_tag_count(void)
{
	tags_init();
	return tag_count;
}

void peg_tag_registry_reset(void)
{
	for (size_t i = 0; i < tag_count; i++)
		peg_term_xfree(tag_names[i]);
	peg_term_xfree(tag_names);
	tag_names = NULL;
	tag_count = tag_cap = 0;
	tags_ready = false;
}

/* ---------------------------------------------------------------------- */
/* Node lifetime                                                          */
/* ---------------------------------------------------------------------- */

peg_node *peg_node_new(peg_tag tag, peg_span span)
{
	peg_node *n = peg_term_xmalloc(sizeof(*n));

	n->tag = tag;
	n->refs = 1;
	n->prev = NULL;
	n->next = NULL;
	n->sub = NULL;
	n->span = span;
	return n;
}

void peg_node_ref(peg_node *n)
{
	if (n != NULL)
		n->refs++;
}

/*
 * Release one reference, destroying whatever that reference kept alive.
 *
 * The traversal is a single loop over two nested structures that would
 * naturally be handled by recursion -- the sibling chain and, one level
 * down, each node's sub-list.  A 10 000 deep term is an ordinary parse
 * result for a nested document, and recursion here would consume roughly
 * a frame per level on a target whose entire stack is a few kilobytes, so
 * instead dead nodes are threaded onto a worklist through their own
 * ->prev field: a node on that list has already been unlinked from its
 * chain (its successor no longer points back at it), so it is unreachable
 * and its prev field is free to reuse.  No allocation is needed, however
 * deep the term turns out to be.
 */
void peg_node_unref(peg_node *n)
{
	peg_node *work = NULL;	/* LIFO of dead nodes whose sub-list is due */

	while (n != NULL || work != NULL) {
		peg_node *next;

		if (n == NULL) {
			/*
			 * Pop a node that was unlinked and freed above.  Its
			 * reference on its sub-list is what we consume now,
			 * by walking that chain next.
			 */
			peg_node *dead = work;

			work = dead->prev;
			n = dead->sub;
			peg_term_xfree(dead);
			continue;
		}

		if (--n->refs > 0) {
			/*
			 * Someone else still holds this node.  So does the
			 * rest of its chain, through the reference n holds
			 * on its next sibling, so the cascade stops here.
			 * The link it was reached by is gone, though: it is
			 * a chain head now.
			 */
			n->prev = NULL;
			n = NULL;
			continue;
		}

		/*
		 * n is dead.  Unlink it before anything else: a surviving
		 * successor must not keep a back-pointer into freed memory.
		 */
		next = n->next;
		if (next != NULL)
			next->prev = NULL;

		n->prev = work;
		work = n;
		n = next;
	}
}

uint32_t peg_node_refs(const peg_node *n)
{
	return n != NULL ? n->refs : 0;
}

/* ---------------------------------------------------------------------- */
/* Lists                                                                  */
/* ---------------------------------------------------------------------- */

size_t peg_list_length(const peg_node *head)
{
	size_t n = 0;

	for (; head != NULL; head = head->next)
		n++;
	return n;
}

const peg_node *peg_list_first(const peg_node *head)
{
	return head;
}

const peg_node *peg_list_last(const peg_node *head)
{
	if (head == NULL)
		return NULL;
	while (head->next != NULL)
		head = head->next;
	return head;
}

peg_node *peg_list_append(peg_node *head, peg_node *n)
{
	peg_node *tail;

	if (n == NULL)
		return head;
	if (head == NULL)
		return n;	/* the caller's reference becomes the list's */

	tail = head;
	while (tail->next != NULL)
		tail = tail->next;

	/* The old tail takes over the caller's reference on n. */
	tail->next = n;
	n->prev = tail;
	return head;
}

peg_node *peg_list_prepend(peg_node *head, peg_node *n)
{
	if (n == NULL)
		return head;

	/* n takes over the caller's reference on head. */
	n->prev = NULL;
	n->next = head;
	if (head != NULL)
		head->prev = n;
	return n;
}

void peg_list_insert_after(peg_node *pos, peg_node *n)
{
	peg_node *old;

	if (pos == NULL || n == NULL)
		return;

	old = pos->next;
	/* n takes over pos's reference on its old successor, ... */
	n->prev = pos;
	n->next = old;
	if (old != NULL)
		old->prev = n;
	/* ... and pos takes over the caller's reference on n. */
	pos->next = n;
}

peg_node *peg_list_insert_before(peg_node *head, peg_node *pos, peg_node *n)
{
	peg_node *prev;

	if (pos == NULL || n == NULL)
		return head;

	prev = pos->prev;
	if (prev == NULL) {
		/*
		 * pos is the head: n becomes the new head and takes over
		 * the caller's reference on pos.  Nothing walks back from
		 * pos to find the head -- the caller passed it, because
		 * this is the one insert that can move it.
		 */
		n->prev = NULL;
		n->next = pos;
		pos->prev = n;
		return n;
	}

	n->prev = prev;
	n->next = pos;		/* n takes over prev's reference on pos */
	prev->next = n;		/* prev takes over the caller's reference */
	pos->prev = n;
	return head;
}

peg_node *peg_list_detach(peg_node *head, peg_node *n)
{
	peg_node *prev, *next;

	if (n == NULL)
		return head;

	prev = n->prev;
	next = n->next;

	if (prev != NULL) {
		/* The predecessor inherits n's reference on next. */
		prev->next = next;
	} else {
		/*
		 * n was the head.  The caller's reference on it stays with
		 * the detached node, and next inherits the reference n held
		 * on it, so the caller's list reference simply moves along.
		 */
		head = next;
	}
	if (next != NULL)
		next->prev = prev;

	n->prev = NULL;
	n->next = NULL;
	return head;
}

peg_node *peg_list_splice(peg_node *dst, peg_node *pos, peg_node *src)
{
	peg_node *rest, *tail;

	if (src == NULL)
		return dst;

	tail = src;
	while (tail->next != NULL)
		tail = tail->next;

	if (pos == NULL) {
		/*
		 * At the front: src's tail takes over the caller's
		 * reference on dst, and src keeps the one on itself.
		 */
		if (dst == NULL)
			return src;
		tail->next = dst;
		dst->prev = tail;
		return src;
	}

	rest = pos->next;
	tail->next = rest;	/* src's tail takes over pos's reference */
	if (rest != NULL)
		rest->prev = tail;
	pos->next = src;	/* pos takes over the caller's reference */
	src->prev = pos;
	return dst;
}

peg_node *peg_list_concat(peg_node *a, peg_node *b)
{
	peg_node *tail;

	if (a == NULL)
		return b;
	if (b == NULL)
		return a;

	tail = a;
	while (tail->next != NULL)
		tail = tail->next;

	tail->next = b;		/* a's tail takes over the caller's reference */
	b->prev = tail;
	return a;
}

/* ---------------------------------------------------------------------- */
/* Sub-lists                                                              */
/* ---------------------------------------------------------------------- */

void peg_node_set_sub(peg_node *n, peg_node *sub)
{
	if (n == NULL || n->sub == sub)
		return;

	peg_node_unref(n->sub);	/* release the list being replaced */
	n->sub = sub;		/* takes over the caller's reference */
}

peg_node *peg_node_take_sub(peg_node *n)
{
	peg_node *sub;

	if (n == NULL)
		return NULL;

	sub = n->sub;
	n->sub = NULL;
	return sub;
}

peg_node *peg_node_sub(const peg_node *n)
{
	return n != NULL ? n->sub : NULL;
}

void peg_node_add_child(peg_node *n, peg_node *child)
{
	if (n == NULL) {
		/* The call owns the child either way. */
		peg_node_unref(child);
		return;
	}
	n->sub = peg_list_append(n->sub, child);
}

/* ---------------------------------------------------------------------- */
/* Structure                                                              */
/* ---------------------------------------------------------------------- */

bool peg_term_flat_p(const peg_node *head)
{
	for (; head != NULL; head = head->next) {
		if (head->sub != NULL)
			return false;
	}
	return true;
}

/*
 * One open sibling chain.  The walk keeps the previously visited sibling
 * so it can check the back-pointer of the next one, and the parent's span
 * so it can check containment on the way in.
 */
struct check_frame {
	const peg_node *child;	/* next sibling to visit, NULL when done */
	const peg_node *prev;	/* the sibling last visited at this level */
	peg_span parent;	/* the enclosing node's span */
	bool has_parent;
};

bool peg_term_check(const peg_node *head)
{
	struct check_frame *stack;
	size_t depth = 1;
	size_t cap = 8;
	size_t budget = PEG_TERM_CHECK_BUDGET;
	bool ok = true;

	/* A term is always entered by its head: anything else is a fragment. */
	if (head != NULL && head->prev != NULL)
		return false;

	stack = peg_term_xmalloc(cap * sizeof(stack[0]));
	stack[0].child = head;
	stack[0].prev = NULL;
	stack[0].parent = PEG_SPAN_EMPTY;
	stack[0].has_parent = false;

	while (depth > 0) {
		struct check_frame *f = &stack[depth - 1];
		const peg_node *n = f->child;

		if (n == NULL) {
			depth--;	/* this chain is done */
			continue;
		}
		f->child = n->next;

		if (n->prev != f->prev || n->refs == 0 ||
		    (f->has_parent && !peg_span_contains(f->parent, n->span)) ||
		    budget-- == 0) {
			ok = false;
			break;
		}

		f->prev = n;

		if (n->sub != NULL) {
			if (depth == PEG_TERM_CHECK_MAX_DEPTH) {
				ok = false;
				break;
			}
			if (depth == cap) {
				cap *= 2;
				stack = peg_term_xrealloc(stack,
				                          cap * sizeof(stack[0]));
			}
			/*
			 * A fresh level: its first node must have no
			 * predecessor, which the prev check performs.
			 */
			stack[depth].child = n->sub;
			stack[depth].prev = NULL;
			stack[depth].parent = n->span;
			stack[depth].has_parent = true;
			depth++;
		}
	}

	peg_term_xfree(stack);
	return ok;
}
