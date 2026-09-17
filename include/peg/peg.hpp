/*
 * peg.hpp - the C++ wrapper over the stable C ABI.
 *
 * A thin, header-only skin over the public headers: no new runtime, no
 * separate library to link, no exceptions.  Include it, link against
 * libpeg, done.
 *
 * The C API's ownership rules are prose -- peg_pattern.h explains that a
 * pat* owns its children, peg_memo.h that the table owns its captures,
 * peg_term.h that next and sub each own a reference while prev is weak.
 * The whole point of the wrapper is to make those rules the compiler's
 * problem instead of the reader's: every owning handle below is
 * move-only, and every C function that transfers ownership is reached
 * through a call that transfers it in the type system too.  A pat* can
 * no more appear at two sites here than a std::unique_ptr can.
 *
 *	libpeg::pattern p = my_grammar();
 *	libpeg::parser g(p, 512);	// compiles p; p may die here
 *	libpeg::result r = g.parse(text);
 *	g.edit(start, end, replacement);
 *	libpeg::result r2 = g.reparse();
 *
 * Two things are deliberately absent:
 *
 *   - Operator sugar for composing patterns (*p, p | q, ...).  Every
 *     pat_* constructor consumes its arguments, and a shorthand that hid
 *     the call would hide the consumption with it.  The named calls are
 *     the documentation.
 *   - Exceptions, or any other error channel of the wrapper's own.  The
 *     library aborts on allocation failure and reports a failed parse in
 *     the result; a wrapper that threw would invent a failure mode the
 *     library does not have (see CLAUDE.md on the error model).
 *
 * The namespace is libpeg, not peg: "typedef struct peg peg;" is part of
 * the C ABI, and C++ will not let a namespace share a name with a type
 * declared in the same scope.
 */
#ifndef PEG_PEG_HPP
#define PEG_PEG_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "peg/peg.h"
#include "peg/peg_interval.h"
#include "peg/peg_memo.h"
#include "peg/peg_pattern.h"
#include "peg/peg_term.h"
#include "peg/peg_vm.h"

namespace libpeg {

/*
 * owner<T, Free> - the move-only base every owning handle derives from.
 *
 * One pointer, one deleter.  Copying is deleted rather than deep-copied:
 * the C objects are single-owner by construction (a pat* owns its
 * children outright) and none of them can be cloned, so a copy would
 * have to be a second call to the constructor, which the caller can
 * write.  Moving is the ownership transfer every pat_* constructor
 * already implements.
 *
 * Free must be a function of exactly T*: pat_free, vm_code_free,
 * vm_prog_free, memo_table_free, memo_capture_free, vm_input_free,
 * peg_node_unref and peg_free all qualify.
 */
template <typename T, void (*Free)(T *)>
class owner {
public:
	owner() = default;

	explicit owner(T *p) : p_(p)
	{
	}

	owner(owner &&o) noexcept : p_(o.p_)
	{
		o.p_ = nullptr;
	}

	owner &operator=(owner &&o) noexcept
	{
		if (this != &o) {
			reset();
			p_ = o.p_;
			o.p_ = nullptr;
		}
		return *this;
	}

	owner(const owner &) = delete;
	owner &operator=(const owner &) = delete;

	~owner()
	{
		reset();
	}

	/* The raw handle, for the C calls this wrapper does not mirror. */
	T *get() const
	{
		return p_;
	}

	explicit operator bool() const
	{
		return p_ != nullptr;
	}

	/* Give up the handle without freeing it. */
	T *release()
	{
		T *p = p_;

		p_ = nullptr;
		return p;
	}

	/* Free what is held and take `p` instead. */
	void reset(T *p = nullptr)
	{
		if (p_ != nullptr)
			Free(p_);
		p_ = p;
	}

protected:
	T *p_ = nullptr;
};

/*
 * charset - a value wrapper over the VM's 256-bit byte set.
 *
 * vm_charset is a 32-byte POD with no ownership, so this is convenience
 * rather than resource management.  pattern::set() takes one.
 */
class charset {
public:
	charset()
	{
		clear();
	}

	/* Every byte in [lo, hi]. */
	static charset range(uint8_t lo, uint8_t hi)
	{
		charset c;

		vm_charset_range(&c.set_, lo, hi);
		return c;
	}

	/* Exactly the bytes of `bytes`. */
	static charset of(std::string_view bytes)
	{
		charset c;

		for (size_t i = 0; i < bytes.size(); i++)
			vm_charset_set(&c.set_, (uint8_t)bytes[i], true);
		return c;
	}

	static charset complement_of(const charset &s)
	{
		charset c;

		vm_charset_negate(&s.set_, &c.set_);
		return c;
	}

	charset &clear()
	{
		for (size_t i = 0; i < 4; i++)
			set_.bits[i] = 0;
		return *this;
	}

	charset &add(uint8_t c)
	{
		vm_charset_set(&set_, c, true);
		return *this;
	}

	charset &remove(uint8_t c)
	{
		vm_charset_set(&set_, c, false);
		return *this;
	}

	bool has(uint8_t c) const
	{
		return vm_charset_has(&set_, c);
	}

	size_t count() const
	{
		return vm_charset_count(&set_);
	}

	/* The underlying set, for the C API. */
	const vm_charset *get() const
	{
		return &set_;
	}

private:
	vm_charset set_;
};

/* ---------------------------------------------------------------------- */
/* Memoization                                                            */
/* ---------------------------------------------------------------------- */

/*
 * capture - one node of a parse tree, holding a reference of its own.
 *
 * The C type is reference-counted because a capture is shared between
 * the memo table, the parser stack, and the result tree.  Owning one
 * here means holding exactly one of those references; the destructor
 * drops it, and the capture dies when the last holder lets go.
 *
 * A borrowed capture -- the return of child(), or result::captures() --
 * carries no reference of its own; share() is how one is taken.
 */
class capture : public owner<memo_capture, memo_capture_free> {
public:
	using owner::owner;

	/* 0 for dummies (see below). */
	int id() const
	{
		return memo_capture_id(p_);
	}

	/*
	 * Dummies are the transparent groups tree memoization introduces
	 * when it merges several matches; they carry no id of their own and
	 * the traversal accessors here step over them.
	 */
	bool dummy() const
	{
		return memo_capture_dummy_p(p_);
	}

	int start() const
	{
		return memo_capture_start(p_);
	}

	int len() const
	{
		return memo_capture_len(p_);
	}

	int end() const
	{
		return memo_capture_end(p_);
	}

	int num_children() const
	{
		return memo_capture_num_children(p_);
	}

	/* The n-th non-dummy child, borrowed.  O(n). */
	const memo_capture *child(int n) const
	{
		return memo_capture_child(p_, n);
	}

	/* A second reference, owned by the caller. */
	capture share() const
	{
		memo_capture_ref(p_);
		return capture(p_);
	}

	/* Attach a child: ownership of `c` moves into this capture. */
	void add(capture c)
	{
		memo_capture_add(p_, c.release());
	}
};

/*
 * memo - the memoization table.
 *
 * Entries from get() are borrowed: the table owns them, and they die
 * with it or with the edit that evicts them.
 */
class memo : public owner<memo_table, memo_table_free> {
public:
	using owner::owner;

	/*
	 * threshold: the minimum examined length worth storing (<= 0 stores
	 * everything).  128-512 is the paper's measured sweet spot.
	 */
	explicit memo(int threshold = 0) : owner(memo_table_new(threshold))
	{
	}

	/*
	 * The entry for (id, pos), or NULL.  Named lookup() rather than
	 * get(): get() is the inherited handle accessor on every wrapper,
	 * and a two-argument overload would hide it for this one type.
	 */
	memo_entry *lookup(int id, int pos)
	{
		return memo_table_get(p_, id, pos);
	}

	/* Ownership of `captures` and its elements passes to the table. */
	void put(int id, int start, int length, int examined, int count,
	         memo_capture **captures, size_t ncap)
	{
		memo_table_put(p_, id, start, length, examined, count, captures,
		               ncap);
	}

	void apply_edit(memo_edit e)
	{
		memo_table_apply_edit(p_, e);
	}

	void apply_edit(int start, int end, int len)
	{
		memo_edit e;

		e.start = start;
		e.end = end;
		e.len = len;
		apply_edit(e);
	}

	size_t size() const
	{
		return memo_table_size(p_);
	}
};

/* ---------------------------------------------------------------------- */
/* Results                                                                */
/* ---------------------------------------------------------------------- */

/* One parse error, as recorded by the Error instruction. */
struct error {
	int pos;
	std::string_view message;
};

/*
 * result - an owning vm_result.
 *
 * vm_exec returns a value whose captures and error strings are the
 * caller's to free, which makes a bare vm_result the one C struct in the
 * API that leaks if you forget it.  This is that value with the free
 * attached.  release() hands the raw struct back to a C caller.
 */
class result {
public:
	result() = default;

	explicit result(vm_result r) : r_(r)
	{
	}

	result(result &&o) noexcept : r_(o.r_)
	{
		o.r_.captures = nullptr;
		o.r_.errors = nullptr;
		o.r_.nerrors = 0;
	}

	result &operator=(result &&o) noexcept
	{
		if (this != &o) {
			vm_result_free(&r_);
			r_ = o.r_;
			o.r_.captures = nullptr;
			o.r_.errors = nullptr;
			o.r_.nerrors = 0;
		}
		return *this;
	}

	result(const result &) = delete;
	result &operator=(const result &) = delete;

	~result()
	{
		vm_result_free(&r_);
	}

	bool success() const
	{
		return r_.success;
	}

	/* The final subject position: on success, where the parse stopped. */
	int pos() const
	{
		return r_.pos;
	}

	/* The result tree's dummy root, borrowed: the result owns it. */
	const memo_capture *captures() const
	{
		return r_.captures;
	}

	/* The result tree with a reference of its own. */
	capture root() const
	{
		memo_capture_ref(r_.captures);
		return capture(r_.captures);
	}

	size_t nerrors() const
	{
		return r_.nerrors;
	}

	error error_at(size_t i) const
	{
		error e;

		e.pos = r_.errors[i].pos;
		e.message = r_.errors[i].message;
		return e;
	}

	/* Hand the raw result to C, and forget it here. */
	vm_result release()
	{
		vm_result r = r_;

		r_.captures = nullptr;
		r_.errors = nullptr;
		r_.nerrors = 0;
		return r;
	}

private:
	/* Zeroed, so a default-constructed result is a plain failure. */
	vm_result r_{};
};

/* ---------------------------------------------------------------------- */
/* Interning                                                              */
/* ---------------------------------------------------------------------- */

/*
 * Tags are interned per registry, not per term, so a tag is a cheap
 * uint32_t to compare and the name comes back out of the registry.  The
 * C API wants NUL-terminated names, so the views below are copied into a
 * temporary before the call -- hence the std::string, not string_view, on
 * the way in.
 */
inline peg_tag tag_intern(std::string_view name)
{
	std::string owned(name);

	return peg_tag_intern(owned.c_str());
}

inline peg_tag tag_lookup(std::string_view name)
{
	std::string owned(name);

	return peg_tag_lookup(owned.c_str());
}

inline std::string_view tag_name(peg_tag tag)
{
	const char *n = peg_tag_name(tag);

	return n != nullptr ? std::string_view(n) : std::string_view();
}

inline size_t tag_count()
{
	return peg_tag_count();
}

/* Drop every interned tag.  Only safe when no term is alive. */
inline void tag_registry_reset()
{
	peg_tag_registry_reset();
}

/* ---------------------------------------------------------------------- */
/* Programs                                                               */
/* ---------------------------------------------------------------------- */

class pattern;
class parser;
class program;

/*
 * builder - the streaming bytecode builder.
 *
 * The compiler drives this; hand-built programs use it directly.  Only
 * the ownership-bearing operations are mirrored here, because the emit
 * functions are numerous and their arguments are already typed: reach
 * them through get() (vm_emit_* in peg_vm.h).
 */
class builder : public owner<vm_prog, vm_prog_free> {
public:
	explicit builder() : owner(vm_prog_new())
	{
	}

	/* A new label; it starts positioned at the current offset. */
	int label()
	{
		return vm_prog_label(p_);
	}

	/* Move a label's target to the current offset. */
	void mark(int label)
	{
		vm_prog_mark(p_, label);
	}

	size_t here() const
	{
		return vm_prog_here(p_);
	}

	/* Resolve labels and pre-decode.  The builder is spent afterwards. */
	program finish();
};

/*
 * program - a compiled pattern, ready to execute.
 *
 * With a memo table and no window, exec() is a full parse that fills the
 * table; the parser class below is the assembled incremental protocol.
 */
class program : public owner<vm_code, vm_code_free> {
public:
	using owner::owner;

	/*
	 * Compile a pattern.  NULL (an empty program) means a non-terminal
	 * was not resolved: *err, when given, then names it -- borrowed from
	 * the pattern, so the name lives only as long as the pattern does.
	 *
	 * Defined below class pattern: it needs the complete type to reach
	 * pat_compile.
	 */
	static program compile(const pattern &p, const char **err = nullptr);

	const uint8_t *insns() const
	{
		return vm_code_insns(p_);
	}

	size_t size() const
	{
		return vm_code_size(p_);
	}

	size_t ninsn() const
	{
		return vm_code_ninsn(p_);
	}

	size_t add_checker(vm_checker_fn fn, void *ud = nullptr)
	{
		return vm_code_add_checker(p_, fn, ud);
	}

	result exec(const uint8_t *data, size_t len, memo *tbl = nullptr,
	            int window_low = -1, int window_high = -1) const
	{
		return result(vm_exec(p_, data, len,
		                      tbl != nullptr ? tbl->get() : nullptr,
		                      window_low, window_high));
	}

	result exec(std::string_view text, memo *tbl = nullptr) const
	{
		/*
		 * vm_exec takes a pointer and a length; an empty subject must
		 * still be a valid pointer, and string_view::data() is not
		 * required to be one for an empty view.
		 */
		static const uint8_t empty = 0;

		return exec(text.empty()
		                ? &empty
		                : reinterpret_cast<const uint8_t *>(text.data()),
		            text.size(), tbl);
	}
};

inline program builder::finish()
{
	vm_code *c = vm_prog_finish(p_);

	reset();
	return program(c);
}

/* ---------------------------------------------------------------------- */
/* Terms                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * node - the term type: a tagged doubly-linked list with optional
 * sub-lists, and the value a rule produces (PEG D9).
 *
 * Owning a node here means holding one reference to a *chain head*: a
 * node with a predecessor is held by that predecessor, so an owner of a
 * later sibling would never reach zero on its own.  Traversal therefore
 * hands back borrowed pegnode pointers -- the list owns its nodes -- and
 * share() is how a caller takes a reference of its own.
 *
 * The list mutators transfer ownership exactly as their C counterparts
 * do: the arguments move in, and the (possibly new) head moves out.
 */
class node : public owner<peg_node, peg_node_unref> {
public:
	using owner::owner;

	static node make(peg_tag tag, peg_span span)
	{
		return node(peg_node_new(tag, span));
	}

	static node make(std::string_view tag, peg_span span)
	{
		return make(tag_intern(tag), span);
	}

	peg_tag tag() const
	{
		return p_->tag;
	}

	peg_span span() const
	{
		return p_->span;
	}

	uint32_t refs() const
	{
		return peg_node_refs(p_);
	}

	/* The chain head: walk the weak prev links back to the start. */
	const peg_node *head() const
	{
		const peg_node *n = p_;

		while (n != nullptr && n->prev != nullptr)
			n = n->prev;
		return n;
	}

	/* Borrowed neighbours and sub-list; NULL when there is none. */
	const peg_node *next() const
	{
		return p_ != nullptr ? p_->next : nullptr;
	}

	const peg_node *sub() const
	{
		return p_ != nullptr ? p_->sub : nullptr;
	}

	/* A second reference on this node, owned by the caller. */
	node share() const
	{
		peg_node_ref(p_);
		return node(p_);
	}

	/* ---- list structure ---- */

	static size_t length(const node &head)
	{
		return peg_list_length(head.get());
	}

	/* Detach the sub-list and take it, leaving the node childless. */
	node take_sub()
	{
		return node(peg_node_take_sub(p_));
	}

	void set_sub(node s)
	{
		peg_node_set_sub(p_, s.release());
	}

	void add_child(node child)
	{
		peg_node_add_child(p_, child.release());
	}

	static node append(node head, node n)
	{
		return node(peg_list_append(head.release(), n.release()));
	}

	static node prepend(node head, node n)
	{
		return node(peg_list_prepend(head.release(), n.release()));
	}

	static node concat(node a, node b)
	{
		return node(peg_list_concat(a.release(), b.release()));
	}

	/*
	 * The position arguments of detach/insert/splice are borrowed
	 * pointers from traversal, which hands out const pegnode; the
	 * mutators need the non-const handle of a node they are about to
	 * relink.  The const_cast is the C API's own signature, not an
	 * oversight here.
	 */
	static node detach(node head, const peg_node *n)
	{
		return node(peg_list_detach(head.release(), const_cast<peg_node *>(n)));
	}

	static node splice(node dst, const peg_node *pos, node src)
	{
		return node(peg_list_splice(dst.release(),
		                            const_cast<peg_node *>(pos),
		                            src.release()));
	}

	static void insert_after(const peg_node *pos, node n)
	{
		peg_list_insert_after(const_cast<peg_node *>(pos), n.release());
	}

	static node insert_before(node head, const peg_node *pos, node n)
	{
		return node(peg_list_insert_before(head.release(),
		                                   const_cast<peg_node *>(pos),
		                                   n.release()));
	}

	/* ---- queries and printing ---- */

	/* True when every node's sub-list is empty (a flat <- value). */
	bool flat() const
	{
		return peg_term_flat_p(p_);
	}

	/* The structural invariants (refcounts, links, no cycles). */
	bool check() const
	{
		return peg_term_check(p_);
	}

	/*
	 * The s-expression form, which is the wire format rather than a
	 * debugging aid.  The printer treats `subject` as a C string --
	 * it measures it with strlen -- so a view that is not
	 * NUL-terminated is copied first.
	 */
	std::string to_string(std::string_view subject) const
	{
		std::string owned(subject);
		char *s;
		std::string out;

		if (p_ == nullptr)
			return out;
		s = peg_term_to_string(p_, owned.c_str());
		out = s;
		peg_term_string_free(s);
		return out;
	}

	/* The same shape with tags only: no subject needed. */
	std::string tags_string() const
	{
		char *s;
		std::string out;

		if (p_ == nullptr)
			return out;
		s = peg_term_to_tags_string(p_);
		out = s;
		peg_term_string_free(s);
		return out;
	}
};

/* ---------------------------------------------------------------------- */
/* Patterns                                                               */
/* ---------------------------------------------------------------------- */

/*
 * pattern - a PEG expression tree.
 *
 * The combinators take their children *by value*, so passing a pattern
 * to one is visibly giving it away: the argument is emptied and the
 * returned tree owns it.  That is the defence against the failure
 * CLAUDE.md calls out -- one pat* installed at two sites, double-freed.
 *
 * A pattern compiles once (pat_compile mutates non-terminals during
 * inlining), so compile() is the end of its useful life.  parser()
 * compiles what it is given and keeps nothing.
 */
class pattern : public owner<pat, pat_free> {
public:
	using owner::owner;

	/* ---- leaves ---- */

	static pattern literal(std::string_view s)
	{
		return pattern(pat_literal(s.data(), s.size()));
	}

	static pattern set(const charset &cs)
	{
		return pattern(pat_set(cs.get()));
	}

	/* Any n bytes; n = 1 is a single byte. */
	static pattern any(uint8_t n = 1)
	{
		return pattern(pat_any(n));
	}

	/*
	 * A zero-width assertion.  op is a mask of the empty-op bits in
	 * peg_pattern.h: begin/end of line, begin/end of text, word
	 * boundary and its negation.
	 */
	static pattern empty(uint8_t op)
	{
		return pattern(pat_emptyop(op));
	}

	/*
	 * An unresolved non-terminal, to be defined by a grammar.  The
	 * compiler's inlining mutates these, so build a fresh one per use
	 * site rather than sharing one.
	 */
	static pattern nonterm(std::string_view name)
	{
		std::string owned(name);

		return pattern(pat_nonterm(owned.c_str()));
	}

	static pattern repeat(pattern p, int n)
	{
		return pattern(pat_repeat(p.release(), n));
	}

	/* ---- n-ary concatenation and choice ---- */

	/*
	 * All of the parts in sequence.  The parts are consumed and the
	 * vector is left empty, so the caller cannot accidentally build a
	 * second tree out of the same children.
	 */
	static pattern concat(std::vector<pattern> &parts)
	{
		std::vector<pat *> raw = release_all(parts.data(), parts.size());
		pattern out(pat_concat(raw.data(), raw.size()));

		parts.clear();
		return out;
	}

	/* Ordered choice across the parts, preferring the earliest. */
	static pattern choice(std::vector<pattern> &parts)
	{
		std::vector<pat *> raw = release_all(parts.data(), parts.size());
		pattern out(pat_or(raw.data(), raw.size()));

		parts.clear();
		return out;
	}

	/* ---- combinators ---- */

	/* Ordered choice; prefers `l`. */
	static pattern alt(pattern l, pattern r)
	{
		return pattern(pat_alt(l.release(), r.release()));
	}

	static pattern seq(pattern l, pattern r)
	{
		return pattern(pat_seq(l.release(), r.release()));
	}

	static pattern star(pattern p)
	{
		return pattern(pat_star(p.release()));
	}

	static pattern plus(pattern p)
	{
		return pattern(pat_plus(p.release()));
	}

	static pattern optional(pattern p)
	{
		return pattern(pat_optional(p.release()));
	}

	/* Negative lookahead (!p): pat_not, named rather than spelled not_
	 * because `not` is a C++ keyword. */
	static pattern negate(pattern p)
	{
		return pattern(pat_not(p.release()));
	}

	/* Lookahead (&p): pat_and, spelled out for the same reason. */
	static pattern peek(pattern p)
	{
		return pattern(pat_and(p.release()));
	}

	static pattern search(pattern p)
	{
		return pattern(pat_search(p.release()));
	}

	/* Capture the span p matched under the given id. */
	static pattern cap(pattern p, int id)
	{
		return pattern(pat_cap(p.release(), id));
	}

	/* Memoize with a fresh id, or with an explicit one. */
	static pattern memo(pattern p)
	{
		return pattern(pat_memo(p.release()));
	}

	static pattern memo(pattern p, int id)
	{
		return pattern(pat_memo_id(p.release(), id));
	}

	static pattern check(pattern p, vm_checker_fn fn, void *ud = nullptr)
	{
		return pattern(pat_check(p.release(), fn, ud));
	}

	static pattern check_flags(pattern p, vm_checker_fn fn, void *ud,
	                           int id, int flag)
	{
		return pattern(pat_check_flags(p.release(), fn, ud, id, flag));
	}

	/*
	 * Fail with a message.  With `recover` the parse continues after
	 * recording the error; without it, it fails.
	 */
	static pattern error(std::string_view msg, pattern recover = pattern())
	{
		std::string owned(msg);

		return pattern(pat_error(owned.c_str(), recover.release()));
	}

	/*
	 * A grammar: `start` names the entry definition, and the defs are
	 * consumed.  The C API copies the names, so views are safe.
	 */
	static pattern grammar(std::string_view start, const char **names,
	                       pattern *defs, size_t ndefs)
	{
		std::string owned(start);
		std::vector<pat *> raw = release_all(defs, ndefs);

		return pattern(pat_grammar(owned.c_str(), names, raw.data(),
		                           ndefs));
	}

	static pattern grammar(std::string_view start,
	                       const std::vector<std::string> &names,
	                       std::vector<pattern> &defs)
	{
		std::vector<const char *> cnames = c_names(names);

		return grammar(start, cnames.data(), defs.data(), defs.size());
	}

	/*
	 * A grammar in which every definition is captured: ids[i] receives
	 * the capture id assigned to names[i].  The defs are consumed.
	 */
	static pattern cap_grammar(std::string_view start, const char **names,
	                           pattern *defs, size_t ndefs, int *ids)
	{
		std::string owned(start);
		std::vector<pat *> raw = release_all(defs, ndefs);

		return pattern(pat_cap_grammar(owned.c_str(), names, raw.data(),
		                               ndefs, ids));
	}

	static pattern cap_grammar(std::string_view start,
	                           const std::vector<std::string> &names,
	                           std::vector<pattern> &defs,
	                           std::vector<int> &ids)
	{
		std::vector<const char *> cnames = c_names(names);

		ids.assign(names.size(), 0);
		return cap_grammar(start, cnames.data(), defs.data(), defs.size(),
		                   ids.data());
	}

	/* A PEG-ish rendering, for debugging. */
	std::string prettify() const
	{
		char *s;
		std::string out;

		if (p_ == nullptr)
			return out;
		/*
		 * pat_prettify allocates through peg_util.h, not through the
		 * term module's own allocator, so it is freed with free() --
		 * not peg_term_string_free, which only looks like the right
		 * match.
		 */
		s = pat_prettify(p_);
		out = s;
		std::free(s);
		return out;
	}

private:
	static std::vector<const char *> c_names(
	    const std::vector<std::string> &names)
	{
		std::vector<const char *> out;

		out.reserve(names.size());
		for (size_t i = 0; i < names.size(); i++)
			out.push_back(names[i].c_str());
		return out;
	}

	/*
	 * Empty an array of patterns into a raw pat** for a C constructor,
	 * which copies the pointer array but takes the elements.  The
	 * temporary buffer dies here; the trees it pointed at do not.
	 */
	static std::vector<pat *> release_all(pattern *defs, size_t ndefs)
	{
		std::vector<pat *> raw(ndefs);

		for (size_t i = 0; i < ndefs; i++)
			raw[i] = defs[i].release();
		return raw;
	}
};

inline program program::compile(const pattern &p, const char **err)
{
	return program(pat_compile(p.get(), err));
}

/* ---------------------------------------------------------------------- */
/* The end-to-end parser                                                  */
/* ---------------------------------------------------------------------- */

/*
 * parser - the incremental parser: one object owning the compiled
 * grammar, the memo table, and the subject buffer (peg.h).
 *
 *	pattern p = my_grammar();
 *	parser g(p, 512);
 *	result r = g.parse(text);	// full parse, fills the table
 *	g.edit(start, end, replacement);	// splice + update the table
 *	result r2 = g.reparse();	// incremental: reuses entries
 *
 * Bundling the protocol into one object is the C API's idea, and it is
 * what makes the invariant structural: the buffer splice and the memo
 * edit cannot drift apart because there is one call for both.
 */
class parser : public owner<peg, peg_free> {
public:
	/*
	 * Compile `p` into a parser.  The pattern is not kept: it may die as
	 * soon as this returns.  memo_threshold is the minimum examined
	 * length for a memo entry to be stored; 0 stores everything.
	 */
	explicit parser(const pattern &p, int memo_threshold = 0)
	    : owner(peg_new(p.get(), memo_threshold))
	{
	}

	/* Full parse of a new subject; the buffer is copied. */
	result parse(const uint8_t *data, size_t len)
	{
		return result(peg_parse(p_, data, len));
	}

	result parse(std::string_view text)
	{
		/*
		 * An empty subject must still be a valid pointer:
		 * string_view::data() is not required to be one for an empty
		 * view, and peg_parse copies len bytes out of it.
		 */
		static const uint8_t empty = 0;

		return parse(text.empty()
		                 ? &empty
		                 : reinterpret_cast<const uint8_t *>(text.data()),
		             text.size());
	}

	/*
	 * Replace [start, end) of the subject with `text`.  This is the
	 * whole incremental protocol: the buffer is spliced and the memo
	 * table updated together.
	 */
	void edit(int start, int end, const uint8_t *text, size_t len)
	{
		peg_edit(p_, start, end, text, len);
	}

	void edit(int start, int end, std::string_view text)
	{
		peg_edit(p_, start, end,
		         reinterpret_cast<const uint8_t *>(text.data()),
		         text.size());
	}

	/* Incremental reparse: equal to parse() on the same text, faster. */
	result reparse()
	{
		return result(peg_reparse(p_));
	}

	/*
	 * Rebuild only the captures overlapping [low, high) (the window
	 * optimization, paper Section 4.4).  Needs a prior parse/reparse of
	 * the current text.
	 */
	result capture_interval(int low, int high)
	{
		return result(peg_capture_interval(p_, low, high));
	}

	/*
	 * The current subject.  The view borrows the parser's buffer, so it
	 * is invalidated by the next parse() or edit().
	 */
	std::string_view subject() const
	{
		return std::string_view(
		    reinterpret_cast<const char *>(peg_subject(p_)),
		    peg_subject_len(p_));
	}
};

/*
 * input - the VM's chunk-caching input wrapper.
 *
 * The interpreter builds one of these itself; it is public for callers
 * driving the machine directly.
 */
class input : public owner<vm_input, vm_input_free> {
public:
	using owner::owner;

	/* The buffer is not copied, and must outlive the input. */
	input(const uint8_t *data, size_t len) : owner(vm_input_new(data, len))
	{
	}

	int pos() const
	{
		return vm_input_pos(p_);
	}

	/* The furthest position ever read; drives memo examined extents. */
	int furthest() const
	{
		return vm_input_furthest(p_);
	}

	void reset_furthest()
	{
		vm_input_reset_furthest(p_);
	}
};

} /* namespace libpeg */

#endif /* PEG_PEG_HPP */
