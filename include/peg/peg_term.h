/*
 * peg_term.h - the term type: the value a rule produces.
 *
 * A rule's arrow chooses what it produces: nothing at all (<=), the
 * matched text (<-), or a structured term (<--).  All three are served by
 * one node type -- a tagged doubly linked list with optional sub-lists:
 *
 *	struct peg_node {
 *		peg_tag          tag;    the "tagged" part
 *		struct peg_node *prev;   doubly linked ...
 *		struct peg_node *next;   ... sibling chain
 *		struct peg_node *sub;    nested list (children), or NULL
 *		peg_span         span;   byte range -- the memo key
 *	};
 *
 * A *list* is a sibling chain anchored on its head: the head's prev is
 * NULL.  A <- value is a flat list (every node's sub is NULL); a <--
 * value is a list whose nodes may each carry one sub-list.  A <= value
 * produces no node at all, only a span, so this module is never reached
 * on the memoization fast path.
 *
 * The prev link is load-bearing, not decoration: it is what makes a
 * splice O(1) and the term an *editable* result rather than a write-once
 * AST.  Nothing here invalidates a node's identity when its span is left
 * alone, which is what pegvm's in-place tree edits need.
 *
 * Ownership and reference counting
 * --------------------------------
 * Nodes are shared -- a memo entry, a parent node's sub list and a live
 * value on the parser's stack may all point at the same node -- so each
 * node carries a reference count.  The count covers exactly two kinds of
 * holder:
 *
 *  - External holders: a memo entry, a caller's variable, a stack slot.
 *    peg_node_ref takes one such reference, peg_node_unref drops one.
 *  - The structure itself: a node's ->next owns the node it points at,
 *    and a node's ->sub owns the head of the sub-list.  Whoever owns a
 *    list owns exactly one reference, on its head.
 *
 * The ->prev link is a *weak* back-pointer: it never counts.  It is the
 * only link the reference count does not cover, and that asymmetry is
 * deliberate -- counting both directions would make every pair of
 * siblings own each other, and a two-node cycle can never reach zero.
 *
 * The rules that follow from this, stated once here so no caller has to
 * re-derive them:
 *
 *  - A caller holds a list by holding one reference to its head.  There
 *    is no separate list object and nothing else to free.
 *  - peg_node_unref destroys a node only when its ->prev is NULL.  A node
 *    with a predecessor is held by that predecessor, so an external unref
 *    can never take it to zero; it is destroyed when the cascade from the
 *    left reaches it.  Freeing a head therefore frees the tail of the
 *    chain with it.
 *  - Nodes shared with a still-live holder survive that cascade.  A
 *    survivor is left as a well-formed chain head (its ->prev is set to
 *    NULL as its predecessor dies), so no link ever dangles.
 *  - Teardown is iterative in every direction: a 100 000 node chain, or a
 *    100 000 deep nest of sub-lists, is released with a flat stack.  On
 *    the Cortex-M target (>= 64 KB of RAM, so a few KB of stack) a
 *    recursive free is the first thing to die.
 *  - The list operations *transfer* ownership; not one of them calls
 *    peg_node_ref or peg_node_unref.  Each takes the reference its
 *    argument carries and hands one back through its return value, which
 *    is exactly why append to a known tail and splice in place are O(1),
 *    and why "the caller's reference moves here" is the contract on every
 *    mutator below.
 *
 * Spans
 * -----
 * A span is half-open -- [lo, hi), lo inclusive, hi exclusive -- matching
 * the interval tree (see peg_interval.h) that keys the memo table.  Two
 * non-empty spans overlap iff a.lo < b.hi && b.lo < a.hi, so spans that
 * merely touch do not overlap: a memo entry ending exactly where an edit
 * starts did not read the edited bytes.  An empty span (lo == hi) holds
 * no byte, contains only itself and overlaps nothing at all.
 *
 * Printed form
 * ------------
 * The s-expression printer is both the debug printout and the wire format
 * for pegvm replies:
 *
 *	(Add (Num "1") (Add (Num "2") (Num "3")))
 *
 * Concretely, with `tag` the node's tag name and `text` its matched text:
 *
 *	form(n)    = "(" tag ")"
 *	           | "(" tag " " <quoted text> ")"     n->sub == NULL
 *	           | "(" tag " " form(c1) " " ... ")"  n->sub == [c1 ...]
 *	list(head) = form(n1) " " form(n2) ...        no enclosing parentheses
 *	empty list = "()"
 *
 * A list is printed as its nodes' forms separated by single spaces, so a
 * one-node list -- the shape a term has in practice -- is exactly one
 * s-expression, while a flat <- list of several nodes reads as a
 * sequence.  No trailing newline is written; the caller adds it.
 *
 * Matched text is quoted and escaped: `"` -> \", `\` -> \\, newline ->
 * \n, carriage return -> \r, tab -> \t, and every other byte outside
 * 0x20..0x7E -> \xHH with two lowercase hex digits.  High bytes are
 * escaped rather than passed through, because a byte-oriented parser
 * cannot tell UTF-8 from binary and the wire protocol is text.
 */
#ifndef PEG_TERM_H
#define PEG_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The FILE* printer is the only stdio dependency in the module.  A device
 * build that wants the term type without stdio defines PEG_TERM_NO_STDIO
 * and keeps everything except the two FILE* entry points (the string form
 * is built with the module's own formatter).
 */
#ifndef PEG_TERM_NO_STDIO
#include <stdio.h>
#endif

/*
 * C linkage for C++ callers (peg.hpp).  The definitions are compiled as
 * C, so without this every symbol here would be mangled on the way in.
 * The inline helpers below are unaffected -- an inline function has
 * internal linkage either way.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* Span                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * A byte range, [lo, hi).  See the "Spans" section above for the
 * half-open and overlap rules.  size_t, not int: positions in this layer
 * are unsigned and wide, whatever the upper layers end up using.
 */
typedef struct {
	size_t lo;	/* first byte, inclusive */
	size_t hi;	/* one past the last byte, exclusive */
} peg_span;

/*
 * The empty span, at the start of the subject.  C++ has no compound
 * literals, so it spells the same value as a braced temporary.
 */
#ifdef __cplusplus
#define PEG_SPAN_EMPTY peg_span{ 0, 0 }
#else
#define PEG_SPAN_EMPTY ((peg_span){ 0, 0 })
#endif

static inline peg_span peg_span_make(size_t lo, size_t hi)
{
	peg_span s = { lo, hi };
	return s;
}

/* Number of bytes covered; an inverted span (hi < lo) counts as empty. */
static inline size_t peg_span_len(peg_span s)
{
	return s.hi > s.lo ? s.hi - s.lo : 0;
}

static inline bool peg_span_empty(peg_span s)
{
	return s.hi <= s.lo;
}

static inline bool peg_span_eq(peg_span a, peg_span b)
{
	return a.lo == b.lo && a.hi == b.hi;
}

/*
 * Half-open overlap: [0,5) and [5,9) are disjoint, so a memo entry ending
 * exactly where an edit starts did not read the edited bytes.  The two
 * emptiness tests are not redundant -- without them an empty span would
 * "overlap" any span strictly containing its position, and an empty range
 * cannot overlap anything, having no bytes to invalidate.
 */
static inline bool peg_span_overlap(peg_span a, peg_span b)
{
	return !peg_span_empty(a) && !peg_span_empty(b) &&
	       a.lo < b.hi && b.lo < a.hi;
}

/* outer covers inner entirely (equal spans contain each other). */
static inline bool peg_span_contains(peg_span outer, peg_span inner)
{
	return outer.lo <= inner.lo && inner.hi <= outer.hi;
}

static inline bool peg_span_contains_pos(peg_span s, size_t pos)
{
	return pos >= s.lo && pos < s.hi;
}

/* ---------------------------------------------------------------------- */
/* Tags                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * A tag names what a node is -- in practice, the grammar rule that
 * produced it.  Tags are small integers into a process-wide interned
 * registry, so they copy and compare as cheaply as an int and a node
 * never owns its name.  Intern before parsing: the registry is global
 * mutable state (not thread-safe, and it allocates), and interning a rule
 * name per parse would be both.
 */
typedef uint32_t peg_tag;

/*
 * Built-in tags, registered by the module itself.  Their ids are stable
 * (they are seeded first, in this order, and a registry reset re-seeds
 * them identically), so they can be used as compile-time constants.
 */
enum {
	PEG_TAG_NONE = 0,	/* no tag; prints as "_" */
	PEG_TAG_ROOT,		/* the root of a whole term */
	PEG_TAG_TERM,		/* a structured term built by a <-- rule */
	PEG_TAG_TEXT,		/* a leaf carrying matched text (a <- value) */
	PEG_TAG_ERROR,		/* a node standing in for a failed match */
	PEG_TAG_BUILTIN_COUNT
};

/*
 * Intern a name, creating the tag on first use.  The name is copied.
 * NULL or "" are not errors: they mean "no tag", i.e. PEG_TAG_NONE.
 */
peg_tag peg_tag_intern(const char *name);

/*
 * Look up an already-interned name without creating a tag.  Returns
 * PEG_TAG_NONE when the name is unknown -- which is also the answer for
 * the name "_", deliberately: both cases print the same, and a caller
 * comparing tags wants the miss to behave like the anonymous tag.
 */
peg_tag peg_tag_lookup(const char *name);

/*
 * The interned name of a tag, or NULL if the id was never handed out.
 * PEG_TAG_NONE has the name "_".
 */
const char *peg_tag_name(peg_tag tag);

/* Number of interned tags, built-ins included. */
size_t peg_tag_count(void);

/*
 * Drop every interned name.  For tests and teardown only: it invalidates
 * every tag id handed out so far, which is safe only when no live node
 * holds one.  The built-ins are re-seeded on the next intern, with the
 * same ids as before.
 */
void peg_tag_registry_reset(void);

/* ---------------------------------------------------------------------- */
/* Node                                                                   */
/* ---------------------------------------------------------------------- */

typedef struct peg_node peg_node;

struct peg_node {
	peg_tag   tag;		/* the "tagged" part */
	/*
	 * The reference count sits beside the tag rather than at the end:
	 * the two 4-byte fields then fill one 8-byte slot on LP64, which
	 * is 8 bytes per node saved on a target where the whole runtime
	 * gets 64 KB (D8).  Read it through peg_node_refs; never assign.
	 */
	uint32_t  refs;
	struct peg_node *prev;	/* weak back-pointer; never counted */
	struct peg_node *next;	/* next sibling; owns one reference */
	struct peg_node *sub;	/* nested list (children), or NULL; owns one */
	peg_span  span;		/* byte range -- the memo key */
};

/*
 * A fresh, unattached node: no siblings, no sub-list, one reference held
 * by the caller.
 */
peg_node *peg_node_new(peg_tag tag, peg_span span);

/* Take another reference on `n` (NULL is a no-op). */
void peg_node_ref(peg_node *n);

/*
 * Drop a reference.  When the count reaches zero the node is freed, its
 * sub-list is released, and the reference it held on its next sibling is
 * dropped -- which frees the rest of the chain unless someone else holds
 * it.  All of that is iterative (see the header comment).  NULL is a
 * no-op.
 */
void peg_node_unref(peg_node *n);

/* Current reference count, 0 for NULL.  For tests and debugging. */
uint32_t peg_node_refs(const peg_node *n);

/* ---------------------------------------------------------------------- */
/* Lists                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * Every function below takes the head of a chain, or NULL for the empty
 * list.  Ownership follows the module's single rule (see the header
 * comment): each of them takes over the reference the caller holds on the
 * node or list it is given, and hands one back with its return value.
 * Call peg_node_ref first if you need to keep your own handle on it.
 *
 * The node being inserted (or appended, or prepended) must be a lone
 * node: prev and next NULL.  Passing a chain would silently drop the rest
 * of it, since only one predecessor link is written.
 */

/* Number of nodes in the chain.  O(n). */
size_t peg_list_length(const peg_node *head);

/* The first node of a chain is its head; this is sugar for symmetry. */
const peg_node *peg_list_first(const peg_node *head);

/* The last node of the chain, or NULL when empty.  O(n). */
const peg_node *peg_list_last(const peg_node *head);

/* Append `n` at the end, prepend it at the front.  Return the new head. */
peg_node *peg_list_append(peg_node *head, peg_node *n);
peg_node *peg_list_prepend(peg_node *head, peg_node *n);

/*
 * Insert `n` next to `pos`, which must be a node of the chain.
 *
 * Inserting after `pos` cannot move the head, so that form needs neither
 * the head nor a return value.  Inserting before it can -- `pos` may be
 * the head -- so that form is handed the head and gives the (possibly
 * new) one back; it never walks back through prev to find the head, which
 * would cost O(pos) for a fact the caller already has.
 *
 * A NULL `pos` makes either of them a no-op, and the caller keeps its
 * reference on `n`.
 */
void peg_list_insert_after(peg_node *pos, peg_node *n);
peg_node *peg_list_insert_before(peg_node *head, peg_node *pos, peg_node *n);

/*
 * Cut `n`, which must be a node of the chain, out of it, and return the
 * new head (NULL if the chain became empty).  The detached node is handed
 * back to the caller as a one-node list, carrying the reference the chain
 * held on it, so its count is unchanged by the detach: if the caller was
 * already holding a reference to `n` of its own, it now holds two.  The
 * caller owns both the returned head and `n` afterwards.  A NULL `n` is
 * a no-op.
 */
peg_node *peg_list_detach(peg_node *head, peg_node *n);

/*
 * Splice the whole `src` list into `dst` immediately after `pos`, or at
 * the front when `pos` is NULL.  Returns the new head.  O(|src|), which
 * is what finding src's tail costs; the splice itself is O(1).
 */
peg_node *peg_list_splice(peg_node *dst, peg_node *pos, peg_node *src);

/* Concatenate two lists into one.  Returns the new head.  O(|a|). */
peg_node *peg_list_concat(peg_node *a, peg_node *b);

/* ---------------------------------------------------------------------- */
/* Sub-lists                                                              */
/* ---------------------------------------------------------------------- */

/*
 * Attach `sub` as n's children, releasing the list n already had.  The
 * caller's reference moves into the node; call peg_node_ref first to keep
 * one.  `sub` must not be inside the list being replaced.
 */
void peg_node_set_sub(peg_node *n, peg_node *sub);

/* Detach and return n's sub-list, transferring ownership to the caller. */
peg_node *peg_node_take_sub(peg_node *n);

/* Borrowed read of n's sub-list; the caller gets no reference. */
peg_node *peg_node_sub(const peg_node *n);

/*
 * Append one child to n's sub-list, creating it if needed.  `child` must
 * be a lone node.  With n == NULL the child is released, since the call
 * takes ownership either way.
 */
void peg_node_add_child(peg_node *n, peg_node *child);

/* ---------------------------------------------------------------------- */
/* Structure                                                              */
/* ---------------------------------------------------------------------- */

/*
 * True when the term is flat -- no node in the chain carries a sub-list,
 * i.e. it is a valid <- value.  The empty term is flat.  Assumes a
 * well-formed chain (use peg_term_check on anything you do not trust).
 */
bool peg_term_flat_p(const peg_node *head);

/*
 * Verify the structure rooted at `head`, which must be a chain head (or
 * NULL for the empty term).  Checks, iteratively and without recursing:
 *
 *  - head->prev == NULL, and a->next->prev == a for every link, so no
 *    chain has a broken back-pointer or a predecessor from elsewhere;
 *  - no cycles in a sibling chain (a node in a cycle would have to name
 *    two different predecessors, so the prev check alone catches it);
 *  - every refcount is positive;
 *  - every sub-list is itself a well-formed chain;
 *  - a child's span is contained in its parent's -- spans come from a
 *    parse, so a child never reaches outside the rule that produced it.
 *
 * That last pair of checks walks sub-lists with an explicit stack, so
 * deep nesting costs heap, not stack.  Two ceilings keep a *corrupt*
 * term -- one whose sub pointers form a cycle, which no local check can
 * see -- from looping forever: PEG_TERM_CHECK_BUDGET nodes visited and
 * PEG_TERM_CHECK_MAX_DEPTH levels of nesting.  Both are far above any
 * real term; exceeding either returns false.
 *
 * Returns false on the first violation found; it does not print.  It
 * validates structure, not pointers: every non-NULL link must address a
 * live node of this term.
 */
bool peg_term_check(const peg_node *head);

#ifndef PEG_TERM_CHECK_BUDGET
#define PEG_TERM_CHECK_BUDGET ((size_t)1 << 24)
#endif
#ifndef PEG_TERM_CHECK_MAX_DEPTH
#define PEG_TERM_CHECK_MAX_DEPTH ((size_t)1 << 18)
#endif

/* ---------------------------------------------------------------------- */
/* Printing                                                               */
/* ---------------------------------------------------------------------- */

/*
 * Write `head`'s s-expression (see the header comment).  `subject` is the
 * buffer the spans index into, or NULL to print tags only; its length is
 * taken as strlen(subject), and spans past its end are clamped, so a
 * stale span prints the text it still has rather than reading past the
 * buffer.  A subject containing a NUL therefore prints only up to it.
 */
#ifndef PEG_TERM_NO_STDIO
void peg_term_print(FILE *out, const peg_node *head, const char *subject);
void peg_term_print_tags(FILE *out, const peg_node *head);
#endif

/*
 * The same text as a freshly allocated NUL-terminated string, which the
 * caller releases with peg_term_string_free.  (Not free(): allocation
 * stays behind this module's one allocator shim, which becomes the
 * peg_alloc vtable.)
 */
char *peg_term_to_string(const peg_node *head, const char *subject);
char *peg_term_to_tags_string(const peg_node *head);

void peg_term_string_free(char *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PEG_TERM_H */
