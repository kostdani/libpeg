/*
 * test_term.c - tests for the term type.
 *
 * Four areas:
 *
 *  1. Spans and tags: the memo key's predicates and the interned tag
 *     registry, including the id stability the built-ins promise.
 *  2. Lists and sub-lists: construction, every mutation, and the
 *     prev/next symmetry that has to survive each one -- peg_term_check
 *     is re-run after every mutation, so a single broken back-pointer
 *     fails the suite at the point that broke it rather than later.
 *  3. The printer: exact expected s-expressions, including the escaping
 *     and the subject clamping, plus the round-trip shape of a known
 *     tree.
 *  4. Reference counting and teardown: a shared node surviving the chain
 *     that held it, and a 100 000 node chain and a 100 000 deep nest
 *     being released iteratively -- a recursive unref dies on the second
 *     one, and a recursive printer on the third.
 *
 * Finally, corrupt structures are offered to peg_term_check, which must
 * reject each of them (and accept them again once repaired).
 *
 * Build: ctest --test-dir build-term  (see src/term/CMakeLists.txt)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peg/peg_term.h"

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

static peg_tag TAG_ADD;
static peg_tag TAG_NUM;
static peg_tag TAG_X;

static peg_span sp(size_t lo, size_t hi)
{
	return peg_span_make(lo, hi);
}

static peg_node *mk(peg_tag tag, size_t lo, size_t hi)
{
	return peg_node_new(tag, sp(lo, hi));
}

/*
 * Build a chain of n nodes, each spanning [i, i+1), and store the nodes in
 * out[].  The returned head owns the chain; out[] borrows.
 */
static peg_node *mk_chain(peg_node **out, size_t n)
{
	peg_node *head = NULL;

	for (size_t i = 0; i < n; i++) {
		out[i] = mk(TAG_NUM, i, i + 1);
		head = peg_list_append(head, out[i]);
	}
	return head;
}

static void check_str(const char *got, const char *want, const char *what)
{
	CHECK(strcmp(got, want) == 0, "%s: printed %s, want %s", what, got,
	      want);
}

/* Print through the heap-string entry point and compare. */
static void check_print(const peg_node *n, const char *subject,
                        const char *want)
{
	char *got = peg_term_to_string(n, subject);

	check_str(got, want, "string print");
	peg_term_string_free(got);
}

/* ---------------------------------------------------------------------- */
/* Spans                                                                  */
/* ---------------------------------------------------------------------- */

static void test_spans(void)
{
	CHECK(peg_span_eq(sp(0, 5), sp(0, 5)), "identical spans are equal");
	CHECK(!peg_span_eq(sp(0, 5), sp(0, 6)), "different spans are not");
	CHECK(!peg_span_eq(sp(0, 5), sp(1, 5)), "a shifted span is not");

	CHECK(peg_span_len(sp(2, 7)) == 5, "length is hi - lo");
	CHECK(peg_span_len(sp(4, 4)) == 0, "an empty span has length 0");
	CHECK(peg_span_empty(sp(4, 4)) && !peg_span_empty(sp(4, 5)),
	      "emptiness");

	CHECK(peg_span_overlap(sp(0, 5), sp(4, 9)), "overlapping spans");
	CHECK(peg_span_overlap(sp(0, 5), sp(0, 5)), "a span overlaps itself");
	CHECK(!peg_span_overlap(sp(0, 5), sp(5, 9)), "touching spans are disjoint");
	CHECK(!peg_span_overlap(sp(5, 9), sp(0, 5)), "and disjoint either way");
	CHECK(!peg_span_overlap(sp(3, 3), sp(3, 3)), "an empty span overlaps nothing");
	CHECK(!peg_span_overlap(sp(3, 3), sp(0, 10)), "not even a span containing it");

	CHECK(peg_span_contains(sp(0, 5), sp(1, 3)), "containment");
	CHECK(peg_span_contains(sp(0, 5), sp(0, 5)), "a span contains itself");
	CHECK(!peg_span_contains(sp(0, 5), sp(4, 6)), "overhang is not containment");
	CHECK(!peg_span_contains(sp(0, 5), sp(0, 6)), "and neither is a longer span");
	CHECK(peg_span_contains(sp(3, 3), sp(3, 3)), "empty contains empty");

	CHECK(peg_span_contains_pos(sp(0, 5), 0), "lo is inside");
	CHECK(peg_span_contains_pos(sp(0, 5), 4), "hi - 1 is inside");
	CHECK(!peg_span_contains_pos(sp(0, 5), 5), "hi is not inside");
	CHECK(!peg_span_contains_pos(sp(3, 3), 3), "an empty span holds no position");
}

/* ---------------------------------------------------------------------- */
/* Tags                                                                   */
/* ---------------------------------------------------------------------- */

static void test_tags(void)
{
	peg_tag a, b;

	CHECK(peg_tag_count() >= PEG_TAG_BUILTIN_COUNT,
	      "the built-ins are registered, got %zu", peg_tag_count());
	CHECK(peg_tag_name(PEG_TAG_NONE) != NULL &&
	      strcmp(peg_tag_name(PEG_TAG_NONE), "_") == 0,
	      "PEG_TAG_NONE is named _");
	CHECK(strcmp(peg_tag_name(PEG_TAG_ROOT), "root") == 0, "root");
	CHECK(strcmp(peg_tag_name(PEG_TAG_TERM), "term") == 0, "term");
	CHECK(strcmp(peg_tag_name(PEG_TAG_TEXT), "text") == 0, "text");
	CHECK(strcmp(peg_tag_name(PEG_TAG_ERROR), "error") == 0, "error");

	CHECK(peg_tag_lookup("_") == PEG_TAG_NONE, "the anonymous tag is findable");
	CHECK(peg_tag_lookup("root") == PEG_TAG_ROOT, "built-ins keep their ids");
	CHECK(peg_tag_lookup("error") == PEG_TAG_ERROR, "all of them");

	a = peg_tag_intern("Expr");
	b = peg_tag_intern("Expr");
	CHECK(a == b, "interning the same name twice gives one tag");
	CHECK(a >= PEG_TAG_BUILTIN_COUNT, "user tags start after the built-ins, got %u", a);
	CHECK(strcmp(peg_tag_name(a), "Expr") == 0, "the name comes back");
	CHECK(peg_tag_lookup("Expr") == a, "and the lookup agrees");

	CHECK(peg_tag_lookup("never-interned-name") == PEG_TAG_NONE,
	      "an unknown name looks up as PEG_TAG_NONE");
	CHECK(peg_tag_intern(NULL) == PEG_TAG_NONE, "NULL interns as none");
	CHECK(peg_tag_intern("") == PEG_TAG_NONE, "so does the empty name");
	CHECK(peg_tag_name((peg_tag)0x7fffffffu) == NULL,
	      "an id that was never handed out has no name");
	CHECK(peg_tag_count() > (size_t)a, "the registry grew");
}

/* ---------------------------------------------------------------------- */
/* Nodes and lists                                                        */
/* ---------------------------------------------------------------------- */

static void test_node_basic(void)
{
	peg_node *n = mk(TAG_NUM, 3, 7);

	CHECK(n->prev == NULL && n->next == NULL && n->sub == NULL,
	      "a fresh node is unattached");
	CHECK(n->tag == TAG_NUM, "the tag is stored");
	CHECK(peg_span_eq(n->span, sp(3, 7)), "the span is stored");
	CHECK(peg_node_refs(n) == 1, "a fresh node holds one reference, got %u",
	      peg_node_refs(n));
	CHECK(peg_term_check(n), "a lone node is a valid term");
	CHECK(peg_term_flat_p(n), "and it is flat");

	CHECK(peg_node_refs(NULL) == 0, "NULL holds no reference");
	peg_node_ref(NULL);	/* both are no-ops on NULL */
	peg_node_unref(NULL);
	CHECK(peg_term_check(NULL), "the empty term is valid");
	CHECK(peg_term_flat_p(NULL), "and flat");

	peg_node_ref(n);
	CHECK(peg_node_refs(n) == 2, "ref adds one, got %u", peg_node_refs(n));
	peg_node_unref(n);
	CHECK(peg_node_refs(n) == 1, "unref drops one, got %u", peg_node_refs(n));
	peg_node_unref(n);	/* the last reference: n is freed here */
}

static void test_list_build(void)
{
	peg_node *a = mk(TAG_NUM, 0, 1);
	peg_node *b = mk(TAG_NUM, 1, 2);
	peg_node *c = mk(TAG_NUM, 2, 3);
	peg_node *z = mk(TAG_NUM, 9, 10);
	peg_node *h = NULL;

	CHECK(peg_list_length(NULL) == 0, "the empty list has length 0");
	CHECK(peg_list_first(NULL) == NULL, "and no first node");
	CHECK(peg_list_last(NULL) == NULL, "and no last node");

	h = peg_list_append(h, a);
	CHECK(h == a, "appending to the empty list returns the node");
	CHECK(peg_list_length(h) == 1, "length one");
	CHECK(peg_term_check(h), "invariants after an append to empty");

	h = peg_list_append(h, b);
	h = peg_list_append(h, c);
	CHECK(peg_list_length(h) == 3, "length %zu, want 3", peg_list_length(h));
	CHECK(peg_list_first(h) == a, "the first node is the head");
	CHECK(peg_list_last(h) == c, "the last node is the tail");
	CHECK(a->next == b && b->next == c && c->next == NULL,
	      "the forward links are in order");
	CHECK(a->prev == NULL && b->prev == a && c->prev == b,
	      "the back links mirror them");
	CHECK(peg_term_check(h), "invariants after three appends");

	h = peg_list_prepend(h, z);
	CHECK(h == z, "prepending returns the new head");
	CHECK(peg_list_length(h) == 4, "length four");
	CHECK(z->prev == NULL && z->next == a && a->prev == z,
	      "the prepended node is linked in front");
	CHECK(peg_term_check(h), "invariants after a prepend");

	CHECK(peg_list_append(h, NULL) == h, "appending nothing is a no-op");
	CHECK(peg_list_prepend(h, NULL) == h, "so is prepending nothing");

	peg_node_unref(h);	/* frees the whole chain */
}

static void test_list_insert(void)
{
	peg_node *c[3];
	peg_node *x = mk(TAG_NUM, 3, 4);
	peg_node *y = mk(TAG_NUM, 4, 5);
	peg_node *h = mk_chain(c, 3);

	peg_list_insert_after(c[0], x);
	CHECK(c[0]->next == x && x->prev == c[0], "insert_after links forward");
	CHECK(x->next == c[1] && c[1]->prev == x, "and closes the gap");
	CHECK(h == c[0], "the head did not move");
	CHECK(peg_list_length(h) == 4, "length four, got %zu", peg_list_length(h));
	CHECK(peg_term_check(h), "invariants after insert_after");

	CHECK(peg_list_insert_before(h, c[2], y) == h,
	      "inserting before a non-head leaves the head alone");
	CHECK(c[1]->next == y && y->prev == c[1], "insert_before links forward");
	CHECK(y->next == c[2] && c[2]->prev == y, "and closes the gap");
	CHECK(peg_list_length(h) == 5, "length five");
	CHECK(peg_term_check(h), "invariants after insert_before");

	/* Inserting before the head does move it. */
	{
		peg_node *w = mk(TAG_NUM, 5, 6);
		peg_node *nh = peg_list_insert_before(h, c[0], w);

		CHECK(nh == w, "the new node is the new head");
		CHECK(w->prev == NULL && w->next == c[0] && c[0]->prev == w,
		      "and it is linked in front");
		CHECK(peg_list_length(nh) == 6, "length six");
		CHECK(peg_term_check(nh), "invariants after a head insert");
		h = nh;
	}

	/* Inserting after the tail. */
	{
		peg_node *v = mk(TAG_NUM, 6, 7);

		peg_list_insert_after(c[2], v);
		CHECK(peg_list_last(h) == v, "insert_after the tail extends it");
		CHECK(v->prev == c[2] && v->next == NULL, "linked in at the end");
		CHECK(peg_term_check(h), "invariants after a tail insert");
	}

	/* A NULL position inserts nothing, and keeps the caller's reference. */
	{
		peg_node *stray = mk(TAG_NUM, 7, 8);

		peg_list_insert_after(NULL, stray);
		CHECK(stray->prev == NULL && stray->next == NULL,
		      "inserting after nothing changes nothing");
		CHECK(peg_list_insert_before(h, NULL, stray) == h,
		      "inserting before nothing changes nothing");
		CHECK(stray->prev == NULL && stray->next == NULL, "and leaves it lone");
		CHECK(peg_node_refs(stray) == 1, "the caller still owns it, got %u",
		      peg_node_refs(stray));
		peg_node_unref(stray);
	}

	peg_node_unref(h);
}

static void test_list_detach(void)
{
	peg_node *c[3];
	peg_node *h, *nh;

	/* A middle node. */
	h = mk_chain(c, 3);
	nh = peg_list_detach(h, c[1]);
	CHECK(nh == c[0], "detaching a middle node leaves the head");
	CHECK(c[0]->next == c[2] && c[2]->prev == c[0], "the gap is closed");
	CHECK(c[1]->prev == NULL && c[1]->next == NULL,
	      "the detached node stands alone");
	CHECK(peg_node_refs(c[1]) == 1,
	      "the chain's reference moved to the caller, got %u",
	      peg_node_refs(c[1]));
	CHECK(peg_term_check(nh), "invariants after a middle detach");
	CHECK(peg_term_check(c[1]), "the detached node is a term of its own");
	CHECK(peg_list_length(nh) == 2, "two nodes left");
	peg_node_unref(c[1]);
	peg_node_unref(nh);

	/* The head. */
	h = mk_chain(c, 3);
	nh = peg_list_detach(h, c[0]);
	CHECK(nh == c[1], "detaching the head promotes its successor");
	CHECK(c[1]->prev == NULL, "the new head has no predecessor");
	CHECK(peg_node_refs(c[1]) == 1, "and it carries one reference, got %u",
	      peg_node_refs(c[1]));
	CHECK(peg_term_check(nh), "invariants after a head detach");
	CHECK(peg_term_check(c[0]), "the detached node is a term of its own");
	peg_node_unref(c[0]);
	peg_node_unref(nh);

	/* The tail. */
	h = mk_chain(c, 3);
	nh = peg_list_detach(h, c[2]);
	CHECK(nh == c[0], "detaching the tail leaves the head");
	CHECK(c[1]->next == NULL, "the new tail ends the chain");
	CHECK(c[2]->prev == NULL && c[2]->next == NULL, "the detached tail is lone");
	CHECK(peg_term_check(nh) && peg_term_check(c[2]), "invariants");
	peg_node_unref(c[2]);
	peg_node_unref(nh);

	/* The only node. */
	h = mk_chain(c, 1);
	nh = peg_list_detach(h, c[0]);
	CHECK(nh == NULL, "detaching the only node empties the list");
	CHECK(c[0]->prev == NULL && c[0]->next == NULL, "the node stands alone");
	CHECK(peg_term_check(c[0]), "and is still a valid term");
	peg_node_unref(c[0]);

	CHECK(peg_list_detach(NULL, NULL) == NULL,
	      "detaching nothing from nothing is nothing");
}

static void test_list_splice_concat(void)
{
	peg_node *d[3], *s[2], *t[2];
	peg_node *hd, *hs, *ht, *r;

	/* Splice into the middle. */
	hd = mk_chain(d, 3);
	hs = mk_chain(s, 2);
	r = peg_list_splice(hd, d[1], hs);
	CHECK(r == hd, "splicing after a node leaves the head");
	CHECK(d[1]->next == s[0] && s[0]->prev == d[1], "the source follows pos");
	CHECK(s[1]->next == d[2] && d[2]->prev == s[1], "and the rest follows it");
	CHECK(peg_list_length(r) == 5, "length five, got %zu", peg_list_length(r));
	CHECK(peg_term_check(r), "invariants after a middle splice");
	peg_node_unref(r);

	/* Splice at the front: the source becomes the head. */
	hd = mk_chain(d, 3);
	hs = mk_chain(s, 2);
	r = peg_list_splice(hd, NULL, hs);
	CHECK(r == s[0], "a front splice returns the source's head");
	CHECK(s[1]->next == d[0] && d[0]->prev == s[1], "the old head follows");
	CHECK(peg_term_check(r), "invariants after a front splice");
	peg_node_unref(r);

	/* Splice into the empty list. */
	hs = mk_chain(s, 2);
	CHECK(peg_list_splice(NULL, NULL, hs) == s[0],
	      "splicing into nothing gives the source back");
	CHECK(peg_term_check(s[0]), "invariants");
	peg_node_unref(s[0]);

	/* Splicing nothing is a no-op, and takes no reference. */
	hd = mk_chain(d, 3);
	CHECK(peg_list_splice(hd, NULL, NULL) == hd, "splicing nothing");
	CHECK(peg_list_splice(hd, d[0], NULL) == hd, "from the middle either");
	CHECK(peg_term_check(hd), "invariants");

	/* Concatenate onto it. */
	hs = mk_chain(s, 2);
	r = peg_list_concat(hd, hs);
	CHECK(r == hd, "concatenation keeps the first head");
	CHECK(d[2]->next == s[0] && s[0]->prev == d[2], "the second list follows");
	CHECK(peg_list_length(r) == 5, "length five, got %zu", peg_list_length(r));
	CHECK(peg_term_check(r), "invariants after a concatenation");
	peg_node_unref(r);

	/* Both empty-side cases. */
	ht = mk_chain(t, 2);
	CHECK(peg_list_concat(NULL, ht) == t[0], "concatenating onto nothing");
	CHECK(peg_list_concat(ht, NULL) == t[0], "concatenating nothing");
	CHECK(peg_term_check(t[0]), "invariants");
	peg_node_unref(t[0]);
}

static void test_sublist(void)
{
	peg_node *parent = mk(PEG_TAG_TERM, 0, 10);
	/* Two lone nodes: add_child takes a node, not an existing chain. */
	peg_node *k0 = mk(TAG_NUM, 0, 1);
	peg_node *k1 = mk(TAG_NUM, 1, 2);
	peg_node *taken;
	peg_node *other = mk(TAG_NUM, 1, 2);

	CHECK(peg_node_sub(parent) == NULL, "a fresh node has no children");
	CHECK(peg_term_flat_p(parent), "and is flat");

	peg_node_add_child(parent, k0);
	CHECK(peg_node_sub(parent) == k0, "the first child becomes the sub-list");
	CHECK(k0->prev == NULL, "the sub-list has no predecessor");
	peg_node_add_child(parent, k1);
	CHECK(peg_list_length(peg_node_sub(parent)) == 2, "two children");
	CHECK(peg_node_sub(parent) == k0, "the sub-list kept its head");
	CHECK(k1->prev == k0 && k0->next == k1, "the children are chained in order");
	CHECK(!peg_term_flat_p(parent), "a node with children is not flat");
	CHECK(peg_term_check(parent), "invariants after add_child");

	taken = peg_node_take_sub(parent);
	CHECK(taken == k0, "take_sub returns the list");
	CHECK(peg_node_sub(parent) == NULL, "and detaches it");
	CHECK(peg_term_flat_p(parent), "the node is flat again");
	CHECK(peg_term_check(taken) && peg_term_check(parent), "invariants");
	peg_node_unref(taken);

	peg_node_set_sub(parent, other);
	CHECK(peg_node_sub(parent) == other, "set_sub attaches");
	CHECK(peg_term_check(parent), "invariants after set_sub");

	{
		peg_node *repl = mk(TAG_NUM, 3, 4);

		peg_node_set_sub(parent, repl);
		CHECK(peg_node_sub(parent) == repl, "set_sub replaces");
		CHECK(peg_term_check(parent), "invariants after a replacement");
		/* `other` was released and freed by the replacement. */
	}

	{
		peg_node *sub = peg_node_sub(parent);

		peg_node_set_sub(parent, sub);
		CHECK(peg_node_sub(parent) == sub,
		      "setting the same list is a no-op, not a release");
		CHECK(peg_term_check(parent), "invariants");
	}

	CHECK(peg_node_take_sub(NULL) == NULL, "no sub-list on NULL");
	CHECK(peg_node_sub(NULL) == NULL, "and nothing to borrow");
	peg_node_add_child(NULL, mk(TAG_NUM, 0, 1));	/* releases the child */

	peg_node_unref(parent);
}

static void test_flat_p(void)
{
	peg_node *c[3];
	peg_node *h = mk_chain(c, 3);

	CHECK(peg_term_flat_p(h), "a chain of leaves is flat");

	peg_node_set_sub(c[1], mk(PEG_TAG_TERM, 1, 2));
	CHECK(!peg_term_flat_p(h), "a sub-list anywhere makes it structured");
	CHECK(peg_term_check(h), "invariants hold regardless");

	peg_node_unref(h);
}

/* ---------------------------------------------------------------------- */
/* Printing                                                               */
/* ---------------------------------------------------------------------- */

/*
 * (Add (Num "1") (Add (Num "2") (Num "3"))) over "1+2+3" -- the worked
 * example from the design, built bottom-up so every span is contained in
 * its parent's.
 */
static peg_node *build_arith(void)
{
	peg_node *add1 = mk(TAG_ADD, 0, 5);
	peg_node *num1 = mk(TAG_NUM, 0, 1);
	peg_node *add2 = mk(TAG_ADD, 2, 5);
	peg_node *num2 = mk(TAG_NUM, 2, 3);
	peg_node *num3 = mk(TAG_NUM, 4, 5);
	peg_node *inner = NULL;
	peg_node *outer = NULL;

	inner = peg_list_append(inner, num2);
	inner = peg_list_append(inner, num3);
	peg_node_set_sub(add2, inner);

	outer = peg_list_append(outer, num1);
	outer = peg_list_append(outer, add2);
	peg_node_set_sub(add1, outer);

	return add1;
}

static void test_print_tree(void)
{
	const char *subject = "1+2+3";
	peg_node *root = build_arith();
	FILE *f;
	char buf[128];
	size_t got;

	CHECK(peg_term_check(root), "the worked example is well-formed");
	CHECK(!peg_term_flat_p(root), "and structured");

	check_print(root, subject,
	            "(Add (Num \"1\") (Add (Num \"2\") (Num \"3\")))");
	check_print(root, NULL, "(Add (Num) (Add (Num) (Num)))");

	{
		char *tagsonly = peg_term_to_tags_string(root);

		check_str(tagsonly, "(Add (Num) (Add (Num) (Num)))",
		          "to_tags_string");
		peg_term_string_free(tagsonly);
	}

	/* The FILE* printer must produce exactly the same bytes. */
	f = tmpfile();
	if (f == NULL) {
		CHECK(false, "tmpfile() failed");
	} else {
		peg_term_print(f, root, subject);
		peg_term_print_tags(f, root);
		rewind(f);
		got = fread(buf, 1, sizeof(buf) - 1, f);
		buf[got] = '\0';
		fclose(f);
		check_str(buf,
		          "(Add (Num \"1\") (Add (Num \"2\") (Num \"3\")))"
		          "(Add (Num) (Add (Num) (Num)))",
		          "file print");
	}

	peg_node_unref(root);
}

static void test_print_shapes(void)
{
	peg_node *c[3];
	peg_node *h;
	peg_node *lone = mk(TAG_NUM, 0, 1);

	check_print(NULL, NULL, "()");
	check_print(NULL, "anything", "()");
	CHECK(peg_list_length(NULL) == 0, "the empty term has no nodes");

	/* A lone leaf, tags only and with its text. */
	check_print(lone, NULL, "(Num)");
	check_print(lone, "x", "(Num \"x\")");
	/* An empty span prints as an empty string, not as nothing. */
	{
		peg_node *e = mk(TAG_NUM, 0, 0);

		check_print(e, "abc", "(Num \"\")");
		peg_node_unref(e);
	}
	peg_node_unref(lone);

	/* A flat list is a sequence of forms, one s-expression per node. */
	h = mk_chain(c, 3);
	check_print(h, NULL, "(Num) (Num) (Num)");
	check_print(h, "abc", "(Num \"a\") (Num \"b\") (Num \"c\")");
	peg_node_unref(h);

	/* A structured node prints a group; a group of groups nests. */
	{
		peg_tag A = peg_tag_intern("A");
		peg_tag B = peg_tag_intern("B");
		peg_tag C = peg_tag_intern("C");
		peg_node *outer = mk(A, 0, 3);
		peg_node *mid = mk(B, 0, 3);
		peg_node *leaf = mk(C, 1, 2);

		peg_node_set_sub(mid, leaf);
		peg_node_set_sub(outer, mid);
		CHECK(peg_term_check(outer), "the nested example is well-formed");
		check_print(outer, NULL, "(A (B (C)))");
		check_print(outer, "xyz", "(A (B (C \"y\")))");
		peg_node_unref(outer);
	}

	/*
	 * A tag that was never interned has no name; the shape is still
	 * printed, with the numeric id in place of the name.
	 */
	{
		peg_node *odd = peg_node_new((peg_tag)9999, sp(0, 1));

		check_print(odd, NULL, "(#9999)");
		check_print(odd, "x", "(#9999 \"x\")");
		peg_node_unref(odd);
	}
}

static void test_print_escaping(void)
{
	/* quote, backslash, newline, tab, a control byte, and a high byte */
	static const char esc[] = "q\"\\\n\t\x01" "z";
	static const char high[] = "\xe9";
	peg_node *n;
	peg_node *e;

	n = mk(TAG_X, 0, 7);
	check_print(n, esc, "(X \"q\\\"\\\\\\n\\t\\x01z\")");
	check_print(n, NULL, "(X)");
	peg_node_unref(n);

	n = mk(TAG_X, 0, 1);
	check_print(n, high, "(X \"\\xe9\")");
	/* Printable ASCII passes through untouched, including spaces. */
	check_print(n, " ", "(X \" \")");
	check_print(n, "\"", "(X \"\\\"\")");
	check_print(n, "\\", "(X \"\\\\\")");
	peg_node_unref(n);

	/* A span reaching past the buffer prints what is left of it. */
	e = mk(TAG_X, 1, 99);
	check_print(e, "abc", "(X \"bc\")");
	/* A span starting past the end prints nothing at all. */
	e->span = sp(9, 12);
	check_print(e, "abc", "(X \"\")");
	/* A subject with an embedded NUL is a C string, so it ends there. */
	e->span = sp(0, 5);
	check_print(e, "ab\0cd", "(X \"ab\")");
	peg_node_unref(e);
}

/* ---------------------------------------------------------------------- */
/* Reference counting and teardown                                        */
/* ---------------------------------------------------------------------- */

static void test_refcount_shared(void)
{
	peg_node *n = mk(TAG_NUM, 0, 1);
	peg_node *c[3];
	peg_node *h;

	/* A second holder keeps the node alive past its chain. */
	peg_node_ref(n);
	CHECK(peg_node_refs(n) == 2, "two references, got %u", peg_node_refs(n));
	peg_node_unref(n);
	CHECK(peg_node_refs(n) == 1, "the chain's reference went first, got %u",
	      peg_node_refs(n));
	CHECK(peg_term_check(n), "the survivor is still a whole term");
	peg_node_unref(n);

	/*
	 * A shared middle node survives the head's death as a chain head
	 * of its own: the back-pointer into the freed node is repaired.
	 */
	h = mk_chain(c, 3);
	peg_node_ref(c[1]);
	CHECK(peg_node_refs(c[1]) == 2, "the middle node is shared, got %u",
	      peg_node_refs(c[1]));

	peg_node_unref(h);	/* frees c[0], then stops at c[1] */
	CHECK(peg_node_refs(c[1]) == 1, "c[1] survived, got %u",
	      peg_node_refs(c[1]));
	CHECK(c[1]->prev == NULL, "and became a chain head");
	CHECK(c[1]->next == c[2] && c[2]->prev == c[1],
	      "with its own chain intact");
	CHECK(peg_term_check(c[1]), "the surviving tail is a valid term");
	CHECK(peg_list_length(c[1]) == 2, "two nodes left");

	peg_node_unref(c[1]);	/* frees c[1] and, with it, c[2] */
}

static void test_deep_chain(void)
{
	enum { N = 100000 };
	peg_node *h = NULL;

	/*
	 * Prepending is O(1) where appending would be O(n), and the chain
	 * it builds is the same shape.  Each node carries the one reference
	 * the previous link (or, for the head, the caller) holds on it.
	 */
	for (int i = 0; i < N; i++)
		h = peg_list_prepend(h, mk(PEG_TAG_TEXT, (size_t)i, (size_t)i + 1));

	CHECK(peg_list_length(h) == N, "built %zu nodes, want %d",
	      peg_list_length(h), N);
	CHECK(peg_list_last(h) != NULL, "the tail is reachable");
	CHECK(peg_term_check(h), "a 100000 node chain checks out");
	CHECK(peg_term_flat_p(h), "and is flat");

	/*
	 * If this recursed, it would need ~N frames and take the process
	 * down; a clean run is the test.
	 */
	peg_node_unref(h);
}

static void test_deep_nesting(void)
{
	enum { DEPTH = 100000 };
	peg_node *root = mk(PEG_TAG_NONE, 0, 1);
	char *s;
	size_t nodes = (size_t)DEPTH + 1;
	/*
	 * "(_ " + the child's form + ")" for every level but the innermost,
	 * which is the leaf "(_)": three characters plus one for the space
	 * the nesting adds.
	 */
	size_t want = 4 * (nodes - 1) + 3;

	for (int i = 0; i < DEPTH; i++) {
		peg_node *n = mk(PEG_TAG_NONE, 0, 1);

		n->sub = root;	/* takes over the caller's reference */
		root = n;
	}

	CHECK(peg_term_check(root), "a 100000 deep nest checks out");
	s = peg_term_to_tags_string(root);
	CHECK(strlen(s) == want, "printed %zu bytes, want %zu", strlen(s), want);
	CHECK(s[0] == '(' && s[want - 1] == ')', "and it is a nested form");
	peg_term_string_free(s);

	/* Iterative teardown, or a very long walk off a short stack. */
	peg_node_unref(root);
}

/* ---------------------------------------------------------------------- */
/* The checker against corrupt structures                                 */
/* ---------------------------------------------------------------------- */

static void test_check_rejects(void)
{
	peg_node *c[3];
	peg_node *h = mk_chain(c, 3);

	CHECK(peg_term_check(h), "a healthy chain is accepted");

	/* A link whose back-pointer names the wrong predecessor. */
	c[2]->prev = c[0];
	CHECK(!peg_term_check(h), "a wrong prev is rejected");
	c[2]->prev = c[1];
	CHECK(peg_term_check(h), "and repairing it is accepted again");

	/* A head that claims a predecessor. */
	c[0]->prev = c[2];
	CHECK(!peg_term_check(h), "a head with a prev is rejected");
	c[0]->prev = NULL;

	/* A cycle.  The walk must terminate to fail. */
	c[2]->next = c[0];
	CHECK(!peg_term_check(h), "a sibling cycle is rejected");
	c[2]->next = NULL;
	CHECK(peg_term_check(h), "repaired");

	/* A node the reference count has already released. */
	c[1]->refs = 0;
	CHECK(!peg_term_check(h), "a zero refcount is rejected");
	c[1]->refs = 1;
	CHECK(peg_term_check(h), "repaired");

	/* A fragment offered as a term: the middle of a chain is not a head. */
	CHECK(!peg_term_check(c[1]), "a mid-chain node is not a term");

	/* A child that reaches outside its parent. */
	{
		peg_node *p = mk(PEG_TAG_TERM, 0, 5);
		peg_node *k = mk(TAG_NUM, 6, 8);

		peg_node_set_sub(p, k);
		CHECK(!peg_term_check(p), "a child outside its parent is rejected");
		k->span = sp(1, 2);
		CHECK(peg_term_check(p), "a contained child is accepted");
		peg_node_unref(p);
	}

	/* A sub-list whose head keeps a back-pointer into nothing. */
	{
		peg_node *p = mk(PEG_TAG_TERM, 0, 5);
		peg_node *k = mk(TAG_NUM, 1, 2);
		peg_node *ghost = mk(TAG_NUM, 1, 2);

		peg_node_set_sub(p, k);
		k->prev = ghost;	/* a predecessor that is not in the list */
		CHECK(!peg_term_check(p), "a sub-list head with a prev is rejected");
		k->prev = NULL;
		CHECK(peg_term_check(p), "repaired");
		peg_node_unref(ghost);
		peg_node_unref(p);
	}

	peg_node_unref(h);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	TAG_ADD = peg_tag_intern("Add");
	TAG_NUM = peg_tag_intern("Num");
	TAG_X = peg_tag_intern("X");

	test_spans();
	test_tags();
	test_node_basic();
	test_list_build();
	test_list_insert();
	test_list_detach();
	test_list_splice_concat();
	test_sublist();
	test_flat_p();
	test_print_tree();
	test_print_shapes();
	test_print_escaping();
	test_refcount_shared();
	test_deep_chain();
	test_deep_nesting();
	test_check_rejects();

	printf("term: %d checks, %d failures\n", checks, failures);
	peg_tag_registry_reset();
	return failures ? 1 : 0;
}
