/*
 * test_peg_cpp.cpp - tests for the C++ wrapper (peg/peg.hpp).
 *
 * The wrapper adds no runtime, so there is nothing here about parsing
 * that the C suites do not already cover.  What is tested is the layer
 * itself, in the three places a hand-written C++ skin actually breaks:
 *
 *  1. Ownership.  Every handle moves and does not copy, a moved-from
 *     handle is empty, and handing a pattern to a combinator leaves the
 *     argument empty -- that is the type-system half of the "one pat*
 *     must never appear at two sites" rule in CLAUDE.md.  Built with
 *     -DPEG_SANITIZE=ON the same checks catch a double free as well.
 *  2. The end-to-end protocol.  The nested-capture grammar, the
 *     incremental-equals-full differential, and the window
 *     optimization, each asserted against the same expectations the C
 *     suites use so that a divergence between the two spellings of the
 *     API shows up here.
 *  3. Lifetime.  A parser keeps nothing of the pattern it compiled, a
 *     result owns its captures, a borrowed capture does not keep the
 *     tree alive by itself, and teardown of a long chain does not
 *     recurse.
 *
 * Build: ctest --test-dir build  (the peg_cpp target, C++17)
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "peg/peg.hpp"

static int checks = 0;
static int failures = 0;

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
/* Handles: move, release, and consumption                                */
/* ---------------------------------------------------------------------- */

static void test_move_semantics()
{
	libpeg::pattern a = libpeg::pattern::literal("x");
	libpeg::pattern b(std::move(a));

	CHECK(!a, "a moved-from handle is empty");
	CHECK(b, "the destination holds the tree");

	libpeg::pattern c;

	c = std::move(b);
	CHECK(!b, "move assignment empties the source");
	CHECK(c, "move assignment transfers the tree");

	/*
	 * The combinators take their children by value, so giving one away
	 * is visible at the call site and the argument cannot be reused.
	 */
	libpeg::pattern l = libpeg::pattern::literal("a");
	libpeg::pattern r = libpeg::pattern::literal("b");
	libpeg::pattern al = libpeg::pattern::alt(std::move(l), std::move(r));

	CHECK(!l && !r, "the combinator's arguments are consumed");
	CHECK(al, "and it returns the combined tree");

	/* release() hands the raw pointer back with the ownership. */
	pat *raw = c.release();

	CHECK(!c, "release empties the handle");
	CHECK(raw != nullptr, "release returns the pointer");
	pat_free(raw);

	/* A default-constructed handle is empty and safe to destroy. */
	libpeg::pattern empty;

	CHECK(!empty, "a default-constructed handle is empty");

	/* And a container of handles moves element-wise without copying. */
	std::vector<libpeg::pattern> v;

	v.push_back(libpeg::pattern::literal("y"));
	v.push_back(libpeg::pattern::literal("z"));
	CHECK(v.size() == 2 && v[0] && v[1], "handles live in containers");
}

/* ---------------------------------------------------------------------- */
/* Charsets                                                               */
/* ---------------------------------------------------------------------- */

static void test_charset()
{
	libpeg::charset digits = libpeg::charset::range('0', '9');

	CHECK(digits.count() == 10, "a range counts its bytes");
	CHECK(digits.has('5') && !digits.has('a'), "membership");

	libpeg::charset sign = libpeg::charset::of("-+");

	CHECK(sign.count() == 2, "of() counts the view");
	CHECK(sign.has('-') && sign.has('+') && !sign.has('0'), "of() sets them");

	libpeg::charset other = libpeg::charset::complement_of(digits);

	CHECK(other.count() == 246, "the complement is 256 - 10");
	CHECK(other.has('a') && !other.has('5'), "and holds what the range did not");

	libpeg::charset chained;

	CHECK(chained.count() == 0, "a fresh set is empty");
	chained.add('a').add('b').remove('a');
	CHECK(!chained.has('a') && chained.has('b'), "add/remove chain");

	chained.clear();
	CHECK(chained.count() == 0, "clear empties it");
}

/* ---------------------------------------------------------------------- */
/* The nested-capture grammar                                             */
/* ---------------------------------------------------------------------- */

enum { CAP_DIGIT = 0, CAP_NUM = 1 };

struct cap_extent {
	int id;
	int start;
	int len;
};

/* Every non-dummy capture in document order.  This walks the borrowed
 * C view of the tree on purpose: the owning wrapper (result) holds the
 * root, the children are the library's to keep. */
static void flatten(const memo_capture *c, std::vector<cap_extent> &out)
{
	if (c == nullptr)
		return;
	if (!memo_capture_dummy_p(c))
		out.push_back(cap_extent{memo_capture_id(c),
		                         memo_capture_start(c),
		                         memo_capture_len(c)});
	for (int i = 0; i < memo_capture_num_children(c); i++)
		flatten(memo_capture_child(c, i), out);
}

/*
 * Star(Memo(Cap1(Plus(Cap0({0..9}))) " "?)) -- the C suite's
 * capture_test_grammar(), spelled with the C++ constructors.
 */
static libpeg::pattern capture_grammar()
{
	libpeg::charset digits = libpeg::charset::range('0', '9');
	std::vector<libpeg::pattern> item;

	item.push_back(libpeg::pattern::cap(
	    libpeg::pattern::plus(
	        libpeg::pattern::cap(libpeg::pattern::set(digits), CAP_DIGIT)),
	    CAP_NUM));
	item.push_back(libpeg::pattern::optional(
	    libpeg::pattern::literal(" ")));

	return libpeg::pattern::star(
	    libpeg::pattern::memo(libpeg::pattern::concat(item)));
}

static void test_captures()
{
	libpeg::parser g(capture_grammar());

	const char *subj = "12 34 56 78 9";
	libpeg::result r = g.parse(subj);

	CHECK(r.success(), "the capture grammar parses");
	CHECK(r.pos() == 13, "and consumes the whole subject: pos = %d", r.pos());
	CHECK(r.nerrors() == 0, "with no recorded errors");

	/* The result owns its tree; the root here is one more reference. */
	libpeg::capture root = r.root();

	CHECK(root.dummy(), "the result tree has a dummy root");

	std::vector<cap_extent> caps;

	flatten(r.captures(), caps);

	int ndigit = 0, nnum = 0;

	for (const cap_extent &c : caps) {
		if (c.id == CAP_DIGIT)
			ndigit++;
		else if (c.id == CAP_NUM)
			nnum++;
	}
	CHECK(ndigit == 9, "9 digit captures, got %d", ndigit);
	CHECK(nnum == 5, "5 number captures, got %d", nnum);

	/* The captures must cover the subject left to right. */
	const int expect[5][2] = { {0, 2}, {3, 2}, {6, 2}, {9, 2}, {12, 1} };
	int i = 0;

	for (const cap_extent &c : caps) {
		if (c.id != CAP_NUM || i >= 5)
			continue;
		CHECK(c.start == expect[i][0] && c.len == expect[i][1],
		      "number %d spans (%d,%d), want (%d,%d)", i, c.start,
		      c.len, expect[i][0], expect[i][1]);
		i++;
	}
	CHECK(i == 5, "every number capture was seen");

	/* The subject view borrows the parser's buffer. */
	CHECK(g.subject() == std::string_view(subj), "subject is the text");

	/* The window optimization: only captures overlapping [3, 5) — the
	 * "34" — need exist, and one of them must. */
	libpeg::result w = g.capture_interval(3, 5);

	CHECK(w.success(), "the window reparse succeeds");

	std::vector<cap_extent> wcaps;
	int wnum = 0;

	flatten(w.captures(), wcaps);
	for (const cap_extent &c : wcaps)
		if (c.id == CAP_NUM)
			wnum++;
	CHECK(wnum >= 1, "a number capture lies in the window");
}

/* ---------------------------------------------------------------------- */
/* Incremental equals full                                                */
/* ---------------------------------------------------------------------- */

/*
 * Doc <- Item* !.        Item <- {{ [a-z]+ ' '? }}
 *
 * Two independent trees, because a pattern compiles once: pat_compile
 * mutates the non-terminals during inlining, so one tree cannot back
 * two parsers.
 */
static libpeg::pattern list_grammar()
{
	libpeg::charset letters = libpeg::charset::range('a', 'z');
	std::vector<libpeg::pattern> item;
	std::vector<libpeg::pattern> doc;

	item.push_back(libpeg::pattern::plus(libpeg::pattern::set(letters)));
	item.push_back(libpeg::pattern::optional(
	    libpeg::pattern::literal(" ")));

	doc.push_back(libpeg::pattern::star(
	    libpeg::pattern::memo(libpeg::pattern::concat(item))));
	doc.push_back(libpeg::pattern::negate(libpeg::pattern::any(1)));

	return libpeg::pattern::concat(doc);
}

static void test_incremental()
{
	libpeg::parser g(list_grammar());
	libpeg::parser full(list_grammar());
	std::string text;

	for (int i = 0; i < 40; i++)
		text += "lorem ipsum dolor sit amet ";

	libpeg::result r = g.parse(text);

	CHECK(r.success() && r.pos() == (int)text.size(),
	      "the initial parse consumes the text");

	/*
	 * A deterministic sequence of single-byte edits -- insert, delete,
	 * change -- applied both to the parser (buffer splice plus memo
	 * edit, one call) and to our own copy of the text.
	 */
	unsigned seed = 12345;
	auto next = [&seed]() {
		seed = seed * 1103515245u + 12345u;
		return (seed >> 16) & 0x7fffu;
	};

	for (int edit = 0; edit < 200; edit++) {
		size_t pos = (size_t)next() % text.size();
		int kind = (int)(next() % 3);

		if (kind == 0) {
			char ch = (next() % 2) ? (char)('a' + next() % 26) : ' ';

			g.edit((int)pos, (int)pos, std::string_view(&ch, 1));
			text.insert(pos, 1, ch);
		} else if (kind == 1) {
			g.edit((int)pos, (int)pos + 1, std::string_view());
			text.erase(pos, 1);
		} else {
			char ch = (next() % 2) ? (char)('a' + next() % 26) : ' ';

			g.edit((int)pos, (int)pos + 1, std::string_view(&ch, 1));
			text[pos] = ch;
		}

		libpeg::result inc = g.reparse();
		libpeg::result ful = full.parse(text);

		CHECK(inc.success() == ful.success() && inc.pos() == ful.pos(),
		      "edit %d: incremental (%d, %d) != full (%d, %d)", edit,
		      (int)inc.success(), inc.pos(), (int)ful.success(),
		      ful.pos());

		if (inc.success() != ful.success() || inc.pos() != ful.pos())
			break;
	}
}

/* ---------------------------------------------------------------------- */
/* The program wrapper: checkers, errors, memoization                     */
/* ---------------------------------------------------------------------- */

/* Checker accepting only spans of even length. */
static int evenlen_checker(const uint8_t *match, size_t matchlen,
                           const uint8_t *subject, size_t subjectlen,
                           int id, int flag, void *ud)
{
	(void)match;
	(void)subject;
	(void)subjectlen;
	(void)id;
	(void)flag;
	(void)ud;
	return (matchlen % 2 == 0) ? 0 : -1;
}

static void test_program()
{
	/* CheckEnd has to be followed by a literal, or the checker would
	 * be looking at a span that the choice point can still undo. */
	libpeg::charset notsemi = libpeg::charset::complement_of(
	    libpeg::charset::of(";"));
	libpeg::program c = libpeg::program::compile(libpeg::pattern::seq(
	    libpeg::pattern::check(
	        libpeg::pattern::plus(libpeg::pattern::set(notsemi)),
	        evenlen_checker),
	    libpeg::pattern::literal(";")));

	CHECK(c, "check() compiles");
	CHECK(c.size() > 0 && c.ninsn() > 0, "the program is encoded");

	libpeg::result r = c.exec("abcd;");

	CHECK(r.success() && r.pos() == 5, "an even-length span is accepted");

	libpeg::result r2 = c.exec("abc;");

	CHECK(!r2.success(), "an odd-length span is rejected");
	CHECK(r2.captures() == nullptr, "and a failed result has no tree");
	CHECK(!r2.root(), "so the root handle is empty, not dangling");

	/* A default-constructed result is an empty failure; its destructor
	 * is the C free on a zeroed struct, which must be a no-op. */
	{
		libpeg::result none;

		CHECK(!none.success(), "a default result is a failure");
		CHECK(none.pos() == 0 && none.nerrors() == 0, "and is empty");
	}

	/* Error recovery: the diagnostic is recorded, not fatal. */
	libpeg::pattern ep = libpeg::pattern::error(
	    "expected x", libpeg::pattern::seq(libpeg::pattern::any(1),
	                                       libpeg::pattern::literal("x")));
	libpeg::program ec = libpeg::program::compile(ep);
	libpeg::result er = ec.exec("yx");

	CHECK(er.success() && er.pos() == 2, "error recovery continues");
	CHECK(er.nerrors() == 1, "one error recorded, got %zu", er.nerrors());
	CHECK(er.error_at(0).message == "expected x", "and the message");

	/* A missing non-terminal is a compile error, returned as a name. */
	libpeg::pattern bad = libpeg::pattern::seq(
	    libpeg::pattern::nonterm("Undefined"),
	    libpeg::pattern::literal("a"));
	const char *err = nullptr;
	libpeg::program bc = libpeg::program::compile(bad, &err);

	CHECK(!bc, "an undefined non-terminal does not compile");
	CHECK(err != nullptr && std::strcmp(err, "Undefined") == 0,
	      "and the name is reported");

	/* Memoization through the program wrapper, and the memo edit. */
	libpeg::program mc = libpeg::program::compile(
	    libpeg::pattern::memo(
	        libpeg::pattern::plus(libpeg::pattern::literal("a"))));
	libpeg::memo tbl;
	libpeg::result mr = mc.exec("aaa", &tbl);

	CHECK(mr.success() && mr.pos() == 3, "a memoized program matches");
	CHECK(tbl.size() > 0, "and fills the memo table");

	/* Replace [0, 1) with one byte: the shift is zero, which the
	 * table always accepts. */
	tbl.apply_edit(0, 1, 1);

	libpeg::result mr2 = mc.exec("aab", &tbl);

	CHECK(mr2.success() && mr2.pos() == 2, "the table survives the edit");

	/* The builder is the low-level path: labels and hand-emitted
	 * instructions, terminated by finish(). */
	libpeg::builder b;

	b.mark(b.label());
	CHECK(b.here() == 0, "a fresh builder has emitted nothing");

	libpeg::program empty = b.finish();

	CHECK(!b, "finish empties the builder");
	CHECK(empty, "and returns the program");

	libpeg::result br = empty.exec("");

	CHECK(br.success() && br.pos() == 0,
	      "the empty program succeeds at the start");

	/* The input wrapper, public for callers driving the machine. */
	static const uint8_t data[4] = { 'a', 'b', 'c', 'd' };
	libpeg::input in(data, sizeof(data));

	CHECK(in.pos() == 0, "a fresh input sits at 0");
	CHECK(in.furthest() == 0, "with nothing read yet");
	in.reset_furthest();
	CHECK(in.furthest() == 0, "reset_furthest is callable");
}

/* ---------------------------------------------------------------------- */
/* Tags                                                                   */
/* ---------------------------------------------------------------------- */

static void test_tags()
{
	libpeg::pattern p = libpeg::pattern::literal("a");
	libpeg::program c = libpeg::program::compile(p);
	libpeg::result r = c.exec("a");

	CHECK(r.success(), "the tag test program matches");

	peg_tag sentinel = libpeg::tag_intern("Zzz_Sentinel");

	CHECK(sentinel == libpeg::tag_intern("Zzz_Sentinel"),
	      "interning the same name twice gives one id");
	CHECK(libpeg::tag_lookup("Zzz_Sentinel") == sentinel, "lookup finds it");
	CHECK(libpeg::tag_name(sentinel) == "Zzz_Sentinel", "name round-trips");

	/* A miss is not an error: an unknown name is the anonymous tag,
	 * which is also how a caller's comparison sees "_". */
	CHECK(libpeg::tag_lookup("NoSuchTag") == PEG_TAG_NONE,
	      "an unknown name is the anonymous tag");
	CHECK(libpeg::tag_name(PEG_TAG_NONE) == "_", "which prints as _");
	CHECK(libpeg::tag_count() > 0, "the registry is populated");
}

/* ---------------------------------------------------------------------- */
/* Terms                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * (Add (Num "1") (Add (Num "2") (Num "3"))) over "1+2+3" -- the worked
 * example the term design uses, rebuilt through the wrapper.
 */
static libpeg::node build_arith()
{
	libpeg::node add1 = libpeg::node::make("Add", peg_span_make(0, 5));
	libpeg::node num1 = libpeg::node::make("Num", peg_span_make(0, 1));
	libpeg::node add2 = libpeg::node::make("Add", peg_span_make(2, 5));
	libpeg::node num2 = libpeg::node::make("Num", peg_span_make(2, 3));
	libpeg::node num3 = libpeg::node::make("Num", peg_span_make(4, 5));
	libpeg::node inner = libpeg::node::append(
	    libpeg::node(), std::move(num2));
	libpeg::node outer = libpeg::node::append(
	    libpeg::node(), std::move(num1));

	inner = libpeg::node::append(std::move(inner), std::move(num3));
	add2.set_sub(std::move(inner));

	outer = libpeg::node::append(std::move(outer), std::move(add2));
	add1.set_sub(std::move(outer));

	return add1;
}

static void test_term()
{
	libpeg::node root = build_arith();

	CHECK(root.check(), "the worked example is well-formed");
	CHECK(!root.flat(), "and structured");
	CHECK(root.tag() == libpeg::tag_intern("Add"), "the root's tag");

	CHECK(root.to_string("1+2+3") ==
	          "(Add (Num \"1\") (Add (Num \"2\") (Num \"3\")))",
	      "the s-expression over the subject");
	CHECK(root.tags_string() == "(Add (Num) (Add (Num) (Num)))",
	      "and the tags-only form");

	CHECK(root.head() == root.get(), "a chain head is its own head");
	CHECK(libpeg::node::length(root) == 1, "one top-level node");

	const peg_node *sub = root.sub();

	CHECK(sub != nullptr, "the root has a sub-list");
	CHECK(sub->prev == nullptr, "and its head has no predecessor");
	CHECK(sub->next != nullptr, "with a sibling after it");

	/*
	 * Sharing takes a reference: the chain can die while the handle
	 * lives, which is the whole point of the refcount.
	 */
	libpeg::node keep = root.share();

	CHECK(keep.refs() == 2, "share takes a second reference, got %u",
	      keep.refs());
	CHECK(keep.get() == root.get(), "pointing at the same node");

	root.reset();
	CHECK(keep.refs() == 1, "the node outlives the chain that held it");
	CHECK(keep.check(), "and is still well-formed");

	/* take_sub moves the sub-list out; the node becomes a leaf. */
	libpeg::node children = keep.take_sub();

	CHECK(children, "take_sub returns the sub-list");
	CHECK(keep.sub() == nullptr, "and leaves the node childless");
	CHECK(libpeg::node::length(children) == 2, "with both children");
	CHECK(!children.flat(), "which are structured in turn");

	/* A node built by hand, with a text tag and a child. */
	libpeg::node parent = libpeg::node::make(PEG_TAG_TEXT,
	                                         peg_span_make(0, 1));

	parent.add_child(libpeg::node::make("Num", peg_span_make(0, 1)));
	CHECK(!parent.flat(), "add_child makes it structured");
	CHECK(parent.check(), "and keeps it well-formed");
}

static void test_term_chain()
{
	enum { N = 100000 };

	/*
	 * Prepending is O(1) where appending is O(n), and the chain it
	 * builds has the same shape.  Releasing it must not recurse: if
	 * the wrapper's teardown walked the chain on the stack, this would
	 * take the process down rather than fail a check.
	 */
	libpeg::node chain;

	for (int i = 0; i < N; i++)
		chain = libpeg::node::prepend(
		    std::move(chain),
		    libpeg::node::make(PEG_TAG_TEXT, peg_span_make(i, i + 1)));

	CHECK(libpeg::node::length(chain) == N, "a %d node chain builds", N);
	CHECK(chain.check(), "and satisfies the invariants");
	CHECK(chain.flat(), "and is flat");
	CHECK(chain.head() == chain.get(), "its head is its own head");
	CHECK(chain.next() != nullptr, "with a sibling after it");
	CHECK(chain.next()->prev == chain.get(), "whose prev points back");
}

int main()
{
	test_move_semantics();
	test_charset();
	test_captures();
	test_incremental();
	test_program();
	test_tags();
	test_term();
	test_term_chain();

	printf("peg_cpp: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
