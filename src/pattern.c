/*
 * pattern.c - pattern AST, grammar compiler, and optimization passes.
 *
 * See pattern.h for the pipeline overview.
 *
 * The instruction-list IR (struct pir, below) is a growable array of
 * typed instruction slots in which labels are entries.  Optimize runs
 * over the IR; encoding lowers it through the streaming vm_emit_*
 * builder, which backpatches labels at finish.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peg/pattern.h"
#include "util.h"

/* ---------------------------------------------------------------------- */
/* AST                                                                    */
/* ---------------------------------------------------------------------- */

enum pat_kind {
	PAT_ALT,
	PAT_SEQ,
	PAT_STAR,
	PAT_PLUS,
	PAT_OPTIONAL,
	PAT_NOT,
	PAT_AND,
	PAT_CAP,
	PAT_MEMO,
	PAT_CHECK,
	PAT_GRAMMAR,
	PAT_SEARCH,
	PAT_REPEAT,
	PAT_CLASS,		/* a set of bytes */
	PAT_LITERAL,
	PAT_NONTERM,
	PAT_DOT,
	PAT_ERROR,
	PAT_EMPTYOP,
	PAT_EMPTY,
};

struct pat {
	enum pat_kind kind;
	union {
		struct { pat *l, *r; } alt;			/* also seq, concat chain */
		struct { pat *p; } unary;			/* star/plus/opt/not/and/search */
		struct { pat *p; int id; } cap;		/* also memo */
		struct { pat *p; vm_checker_fn fn; void *ud; int id; int flag; } check;
		struct { char *start; char **names; pat **defs; size_t ndefs; } grammar;
		struct { pat *p; int n; } repeat;
		struct { vm_charset set; } class;
		struct { char *str; size_t len; } literal;
		struct { char *name; pat *inlined; } nonterm;
		struct { uint8_t n; } dot;
		struct { char *msg; pat *recover; } error;
		struct { uint8_t op; } emptyop;
	} u;
};

static pat *pat_new(enum pat_kind kind)
{
	pat *p = xmalloc(sizeof(*p));
	memset(p, 0, sizeof(*p));
	p->kind = kind;
	return p;
}

void pat_free(pat *p)
{
	if (p == NULL)
		return;
	switch (p->kind) {
	case PAT_ALT:
	case PAT_SEQ:
		pat_free(p->u.alt.l);
		pat_free(p->u.alt.r);
		break;
	case PAT_STAR:
	case PAT_PLUS:
	case PAT_OPTIONAL:
	case PAT_NOT:
	case PAT_AND:
	case PAT_SEARCH:
		pat_free(p->u.unary.p);
		break;
	case PAT_CAP:
	case PAT_MEMO:
		pat_free(p->u.cap.p);
		break;
	case PAT_CHECK:
		pat_free(p->u.check.p);
		break;
	case PAT_GRAMMAR:
		for (size_t i = 0; i < p->u.grammar.ndefs; i++) {
			free(p->u.grammar.names[i]);
			pat_free(p->u.grammar.defs[i]);
		}
		free(p->u.grammar.names);
		free(p->u.grammar.defs);
		free(p->u.grammar.start);
		break;
	case PAT_REPEAT:
		pat_free(p->u.repeat.p);
		break;
	case PAT_LITERAL:
		free(p->u.literal.str);
		break;
	case PAT_NONTERM:
		/* `inlined` is a back-reference owned by a grammar, not us. */
		free(p->u.nonterm.name);
		break;
	case PAT_ERROR:
		free(p->u.error.msg);
		pat_free(p->u.error.recover);
		break;
	default:
		break;
	}
	free(p);
}

/* ---------------------------------------------------------------------- */
/* Constructors                                                           */
/* ---------------------------------------------------------------------- */

pat *pat_alt(pat *l, pat *r)
{
	pat *p = pat_new(PAT_ALT);
	p->u.alt.l = l;
	p->u.alt.r = r;
	return p;
}

pat *pat_seq(pat *l, pat *r)
{
	pat *p = pat_new(PAT_SEQ);
	p->u.alt.l = l;
	p->u.alt.r = r;
	return p;
}

pat *pat_concat(pat **patts, size_t n)
{
	if (n == 0)
		return pat_new(PAT_EMPTY);
	pat *acc = patts[0];
	for (size_t i = 1; i < n; i++)
		acc = pat_seq(acc, patts[i]);
	return acc;
}

pat *pat_or(pat **patts, size_t n)
{
	if (n == 0)
		return pat_new(PAT_EMPTY);
	/* make the chain right-associative */
	pat *acc = patts[n - 1];
	for (size_t i = n - 1; i-- > 0; )
		acc = pat_alt(patts[i], acc);
	return acc;
}

pat *pat_star(pat *p)
{
	pat *n = pat_new(PAT_STAR);
	n->u.unary.p = p;
	return n;
}

pat *pat_plus(pat *p)
{
	pat *n = pat_new(PAT_PLUS);
	n->u.unary.p = p;
	return n;
}

pat *pat_optional(pat *p)
{
	pat *n = pat_new(PAT_OPTIONAL);
	n->u.unary.p = p;
	return n;
}

pat *pat_not(pat *p)
{
	pat *n = pat_new(PAT_NOT);
	n->u.unary.p = p;
	return n;
}

pat *pat_and(pat *p)
{
	pat *n = pat_new(PAT_AND);
	n->u.unary.p = p;
	return n;
}

pat *pat_cap(pat *p, int id)
{
	pat *n = pat_new(PAT_CAP);
	n->u.cap.p = p;
	n->u.cap.id = id;
	return n;
}

pat *pat_check(pat *p, vm_checker_fn fn, void *ud)
{
	return pat_check_flags(p, fn, ud, 0, 0);
}

pat *pat_check_flags(pat *p, vm_checker_fn fn, void *ud, int id, int flag)
{
	pat *n = pat_new(PAT_CHECK);
	n->u.check.p = p;
	n->u.check.fn = fn;
	n->u.check.ud = ud;
	n->u.check.id = id;
	n->u.check.flag = flag;
	return n;
}

/* Global memo id counter. */
static int pat_memo_counter = 0;

pat *pat_memo_id(pat *p, int id)
{
	pat *n = pat_new(PAT_MEMO);
	n->u.cap.p = p;
	n->u.cap.id = id;
	if (id > pat_memo_counter)
		pat_memo_counter = id + 1;
	return n;
}

pat *pat_memo(pat *p)
{
	return pat_memo_id(p, pat_memo_counter++);
}

pat *pat_literal(const char *s, size_t len)
{
	pat *p = pat_new(PAT_LITERAL);
	p->u.literal.str = xmalloc(len ? len : 1);
	if (len)
		memcpy(p->u.literal.str, s, len);
	p->u.literal.len = len;
	return p;
}

pat *pat_set(const vm_charset *set)
{
	pat *p = pat_new(PAT_CLASS);
	p->u.class.set = *set;
	return p;
}

pat *pat_any(uint8_t n)
{
	pat *p = pat_new(PAT_DOT);
	p->u.dot.n = n;
	return p;
}

pat *pat_repeat(pat *p, int n)
{
	if (n <= 0) {
		pat_free(p);
		return pat_new(PAT_EMPTY);
	}
	/* Kept as a node and expanded at compile time: sharing this
	 * sub-pattern n times would create pointers pat_free cannot
	 * release, and the AST shape is irrelevant anyway — Seq
	 * compilation is flat concatenation. */
	pat *r = pat_new(PAT_REPEAT);
	r->u.repeat.p = p;
	r->u.repeat.n = n;
	return r;
}

pat *pat_search(pat *p)
{
	pat *n = pat_new(PAT_SEARCH);
	n->u.unary.p = p;
	return n;
}

pat *pat_emptyop(uint8_t op)
{
	pat *p = pat_new(PAT_EMPTYOP);
	p->u.emptyop.op = op;
	return p;
}

pat *pat_nonterm(const char *name)
{
	pat *p = pat_new(PAT_NONTERM);
	p->u.nonterm.name = xstrdup(name);
	return p;
}

pat *pat_grammar(const char *start, const char **names, pat **defs,
                 size_t ndefs)
{
	pat *p = pat_new(PAT_GRAMMAR);
	p->u.grammar.start = xstrdup(start);
	p->u.grammar.names = xmalloc(ndefs * sizeof(char *));
	p->u.grammar.defs = xmalloc(ndefs * sizeof(pat *));
	for (size_t i = 0; i < ndefs; i++) {
		p->u.grammar.names[i] = xstrdup(names[i]);
		p->u.grammar.defs[i] = defs[i];
	}
	p->u.grammar.ndefs = ndefs;
	return p;
}

pat *pat_cap_grammar(const char *start, const char **names, pat **defs,
                     size_t ndefs, int *ids)
{
	pat **capped = xmalloc(ndefs * sizeof(pat *));
	for (size_t i = 0; i < ndefs; i++) {
		capped[i] = pat_cap(defs[i], (int)i);
		ids[i] = (int)i;
	}
	pat *g = pat_grammar(start, names, capped, ndefs);
	free(capped);
	return g;
}

pat *pat_error(const char *msg, pat *recovery)
{
	pat *p = pat_new(PAT_ERROR);
	p->u.error.msg = xstrdup(msg);
	p->u.error.recover = recovery;
	return p;
}

/* ---------------------------------------------------------------------- */
/* Get: pattern-level simplification                                      */
/* ---------------------------------------------------------------------- */

/*
 * Compilation context: holds transient nodes created by get_pat (which
 * are freed shallowly at the end — their children are always owned
 * elsewhere), and the first error (a non-terminal missing from its
 * grammar; the name is borrowed from the pattern tree).
 */
typedef struct {
	pat **transients;
	size_t ntrans, transcap;
	const char *err;
	size_t nlabels;		/* label ids handed out */
} pctx;

/* Register a node whose children are owned elsewhere; freed (shallowly)
 * when compilation ends. */
static pat *transient(pctx *ctx, pat *p)
{
	if (ctx->ntrans == ctx->transcap) {
		ctx->transcap = ctx->transcap ? ctx->transcap * 2 : 16;
		ctx->transients = xrealloc(ctx->transients,
		                           ctx->transcap * sizeof(pat *));
	}
	ctx->transients[ctx->ntrans++] = p;
	return p;
}

/*
 * If the bytes matched by p1 and p2 can be matched by a single charset,
 * that combined set is written to *out.
 */
static bool combine(pat *p1, pat *p2, vm_charset *out)
{
	vm_charset one;
	if (p1->kind == PAT_LITERAL && p1->u.literal.len == 1) {
		uint8_t b = (uint8_t)p1->u.literal.str[0];
		switch (p2->kind) {
		case PAT_CLASS:
			vm_charset_fill(&one, &b, 1);
			vm_charset_union(&p2->u.class.set, &one, out);
			return true;
		case PAT_LITERAL:
			if (p2->u.literal.len != 1)
				return false;
			vm_charset_fill(&one, &b, 1);
			vm_charset_set(&one, (uint8_t)p2->u.literal.str[0], true);
			*out = one;
			return true;
		default:
			break;
		}
	} else if (p1->kind == PAT_CLASS) {
		switch (p2->kind) {
		case PAT_CLASS:
			vm_charset_union(&p1->u.class.set, &p2->u.class.set, out);
			return true;
		case PAT_LITERAL:
			if (p2->u.literal.len != 1)
				return false;
			vm_charset_fill(&one,
			                (const uint8_t *)&p2->u.literal.str[0], 1);
			vm_charset_union(&p1->u.class.set, &one, out);
			return true;
		default:
			break;
		}
	}
	return false;
}

/*
 * Returns a possibly-simplified version of p.  Always read a
 * pattern through this before inspecting its kind.  The result is either
 * p itself, a node owned elsewhere (an inlined definition), or a freshly
 * created node registered as a transient.
 */
static pat *get_pat(pctx *ctx, pat *p)
{
	switch (p->kind) {
	case PAT_NONTERM:
		/* An inlined non-terminal reads as its definition. */
		if (p->u.nonterm.inlined != NULL)
			return p->u.nonterm.inlined;
		break;
	case PAT_ALT: {
		pat *l = get_pat(ctx, p->u.alt.l);
		pat *r = get_pat(ctx, p->u.alt.r);
		if (l->kind == PAT_EMPTY)
			return r;
		if (r->kind == PAT_EMPTY)
			return get_pat(ctx, transient(ctx, pat_optional(l)));

		vm_charset set;
		if (combine(l, r, &set))
			return transient(ctx, pat_set(&set));
		break;
	}
	case PAT_OPTIONAL:
		/* Optional of a star is just the star. */
		if (get_pat(ctx, p->u.unary.p)->kind == PAT_STAR)
			return get_pat(ctx, p->u.unary.p);
		break;
	case PAT_SEQ: {
		pat *l = get_pat(ctx, p->u.alt.l);
		pat *r = get_pat(ctx, p->u.alt.r);
		/* `a ""` and `"" a` are just `a`. */
		if (r->kind == PAT_EMPTY)
			return l;
		if (l->kind == PAT_EMPTY)
			return r;

		/* `![a-z] .` and friends become a complement/difference class. */
		if (l->kind != PAT_NOT)
			break;
		pat *np = l->u.unary.p;
		vm_charset set;
		if (np->kind == PAT_LITERAL && np->u.literal.len == 1) {
			uint8_t b = (uint8_t)np->u.literal.str[0];
			vm_charset_fill(&set, &b, 1);
		} else if (np->kind == PAT_CLASS) {
			set = np->u.class.set;
		} else {
			return p;
		}

		if (r->kind == PAT_DOT && r->u.dot.n == 1) {
			vm_charset_negate(&set, &set);
			return transient(ctx, pat_set(&set));
		}
		if (r->kind == PAT_CLASS) {
			vm_charset_sub(&r->u.class.set, &set, &set);
			return transient(ctx, pat_set(&set));
		}
		if (r->kind == PAT_LITERAL && r->u.literal.len == 1) {
			/* the right byte's singleton set, minus the not-set */
			uint8_t b = (uint8_t)r->u.literal.str[0];
			vm_charset one;
			vm_charset_fill(&one, &b, 1);
			vm_charset_sub(&one, &set, &set);
			return transient(ctx, pat_set(&set));
		}
		break;
	}
	default:
		break;
	}
	return p;
}

/* ---------------------------------------------------------------------- */
/* Shallow free for transient nodes                                       */
/* ---------------------------------------------------------------------- */

/*
 * Transient nodes (built by get_pat and by Search compilation) reference
 * children owned elsewhere; only the node itself and its owned strings
 * are released.
 */
static void transient_free(pat *p)
{
	if (p == NULL)
		return;
	switch (p->kind) {
	case PAT_GRAMMAR:
		for (size_t i = 0; i < p->u.grammar.ndefs; i++)
			free(p->u.grammar.names[i]);
		free(p->u.grammar.names);
		free(p->u.grammar.defs);
		free(p->u.grammar.start);
		break;
	case PAT_LITERAL:
		free(p->u.literal.str);
		break;
	case PAT_NONTERM:
		free(p->u.nonterm.name);
		break;
	case PAT_ERROR:
		free(p->u.error.msg);
		break;
	default:
		break;
	}
	free(p);
}

static void pctx_cleanup(pctx *ctx)
{
	for (size_t i = 0; i < ctx->ntrans; i++)
		transient_free(ctx->transients[i]);
	free(ctx->transients);
	ctx->transients = NULL;
	ctx->ntrans = ctx->transcap = 0;
}

/* ---------------------------------------------------------------------- */
/* Pattern walk                                                           */
/* ---------------------------------------------------------------------- */

typedef void (*walk_fn)(pat *p, void *ud);

static void walk_pat(pat *p, bool follow_inline, walk_fn fn, void *ud)
{
	if (p == NULL)
		return;
	fn(p, ud);
	switch (p->kind) {
	case PAT_ALT:
	case PAT_SEQ:
		walk_pat(p->u.alt.l, follow_inline, fn, ud);
		walk_pat(p->u.alt.r, follow_inline, fn, ud);
		break;
	case PAT_STAR:
	case PAT_PLUS:
	case PAT_OPTIONAL:
	case PAT_NOT:
	case PAT_AND:
	case PAT_SEARCH:
		walk_pat(p->u.unary.p, follow_inline, fn, ud);
		break;
	case PAT_CAP:
	case PAT_MEMO:
		walk_pat(p->u.cap.p, follow_inline, fn, ud);
		break;
	case PAT_CHECK:
		walk_pat(p->u.check.p, follow_inline, fn, ud);
		break;
	case PAT_ERROR:
		walk_pat(p->u.error.recover, follow_inline, fn, ud);
		break;
	case PAT_GRAMMAR:
		for (size_t i = 0; i < p->u.grammar.ndefs; i++)
			walk_pat(p->u.grammar.defs[i], follow_inline, fn, ud);
		break;
	case PAT_NONTERM:
		if (p->u.nonterm.inlined != NULL && follow_inline)
			walk_pat(p->u.nonterm.inlined, follow_inline, fn, ud);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------------- */
/* Instruction-list IR                                                    */
/* ---------------------------------------------------------------------- */

/* IR-only entry kinds, distinct from every vm_op value. */
enum {
	PIR_LABEL = 0x80,	/* a: label id */
	PIR_OPENCALL,		/* name: unresolved non-terminal */
};

typedef struct {
	uint8_t op;		/* vm_op value, or PIR_LABEL/PIR_OPENCALL */
	int32_t a;		/* primary operand (byte, count, id, ...) */
	int32_t b;		/* secondary operand */
	int label;		/* jump target label id, or -1 */
	vm_charset set;		/* Set/Span/TestSet operand */
	const char *msg;	/* Error message (borrowed) */
	const char *name;	/* OpenCall name (borrowed) */
	vm_checker_fn fn;	/* CheckEnd checker */
	void *ud;
} pir_insn;

typedef struct {
	pir_insn *insns;
	size_t n, cap;
} pir;

static void pir_app(pir *p, pir_insn insn)
{
	if (p->n == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 64;
		p->insns = xrealloc(p->insns, p->cap * sizeof(p->insns[0]));
	}
	p->insns[p->n++] = insn;
}

static void pir_append(pir *p, const pir *other)
{
	for (size_t i = 0; i < other->n; i++)
		pir_app(p, other->insns[i]);
}

static pir_insn insn_basic(uint8_t op)
{
	pir_insn in = { .op = op, .a = 0, .b = 0, .label = -1, .msg = NULL,
	                .name = NULL, .fn = NULL, .ud = NULL };
	return in;
}

static pir_insn insn_label(int label)
{
	pir_insn in = insn_basic(PIR_LABEL);
	in.a = label;
	return in;
}

static pir_insn insn_jump(uint8_t op, int label)
{
	pir_insn in = insn_basic(op);
	in.label = label;
	return in;
}

static pir_insn insn_u8(uint8_t op, int32_t a)
{
	pir_insn in = insn_basic(op);
	in.a = a;
	return in;
}

static pir_insn insn_set(uint8_t op, vm_charset set)
{
	pir_insn in = insn_basic(op);
	in.set = set;
	return in;
}

static pir_insn insn_test_set(uint8_t op, vm_charset set, int label)
{
	pir_insn in = insn_set(op, set);
	in.label = label;
	return in;
}

static pir_insn insn_test_char(uint8_t op, uint8_t byte, int label)
{
	pir_insn in = insn_basic(op);
	in.b = byte;
	in.label = label;
	return in;
}

static int pctx_label(pctx *ctx)
{
	return (int)ctx->nlabels++;
}

/* ---------------------------------------------------------------------- */
/* Compile                                                                */
/* ---------------------------------------------------------------------- */

static void grammar_inline(pat *g);
static int find_def(const pat *g, const char *name);

/*
 * Marks which grammar definitions are still referenced by an unresolved
 * non-terminal.
 */
struct used_data {
	const pat *g;
	bool *used;
	bool *any_used;
};

static void collect_used_fn(pat *sub, void *ud)
{
	struct used_data *d = ud;
	/* only still-unresolved non-terminals keep definitions alive */
	if (sub->kind != PAT_NONTERM || sub->u.nonterm.inlined != NULL)
		return;
	*d->any_used = true;
	int di = find_def(d->g, sub->u.nonterm.name);
	if (di >= 0)
		d->used[di] = true;
}

/* Index of the next real instruction (skipping labels and nops);
 * p->n if there is none. */
static size_t next_insn_idx(const pir *p, size_t from)
{
	for (size_t i = from; i < p->n; i++) {
		if (p->insns[i].op == PIR_LABEL || p->insns[i].op == VM_NOP)
			continue;
		return i;
	}
	return p->n;
}

/* Index of the next real instruction and whether a label preceded it
 * the operand of the label. */
static size_t next_insn_label_idx(const pir *p, size_t from, bool *hadlabel)
{
	*hadlabel = false;
	for (size_t i = from; i < p->n; i++) {
		if (p->insns[i].op == VM_NOP)
			continue;
		if (p->insns[i].op == PIR_LABEL) {
			*hadlabel = true;
			continue;
		}
		return i;
	}
	return p->n;
}

/* Find a grammar definition by name; -1 if absent. */
static int find_def(const pat *g, const char *name)
{
	for (size_t i = 0; i < g->u.grammar.ndefs; i++) {
		if (strcmp(g->u.grammar.names[i], name) == 0)
			return (int)i;
	}
	return -1;
}

static void set_err(pctx *ctx, const char *name)
{
	/* The name is borrowed from the pattern tree (a non-terminal's or a
	 * grammar's name), so the error outlives compilation. */
	if (ctx->err == NULL)
		ctx->err = name;
}

/* OpenCall placeholder for a recursive call. */
static pir_insn insn_opencall(const char *name)
{
	pir_insn in = insn_basic(PIR_OPENCALL);
	in.name = name;
	return in;
}

static pir compile_pat(pctx *ctx, pat *p);

/* GrammarNode.Compile: inline, emit used definitions, resolve open
 * calls, tail-call optimize. */
static pir compile_grammar(pctx *ctx, pat *p)
{
	pir code = {0};
	grammar_inline(p);

	/* Which non-terminals are still referenced unresolved? */
	bool *used = xcalloc(p->u.grammar.ndefs, sizeof(bool));
	bool any_used = false;
	for (size_t i = 0; i < p->u.grammar.ndefs; i++) {
		struct used_data data = {
			.g = p, .used = used, .any_used = &any_used,
		};
		walk_pat(p->u.grammar.defs[i], true, collect_used_fn, &data);
	}

	int startidx = find_def(p, p->u.grammar.start);
	if (startidx < 0) {
		set_err(ctx, p->u.grammar.start);
		free(used);
		return code;
	}

	if (!any_used) {
		/* Everything was inlined: no calls remain. */
		free(used);
		return compile_pat(ctx, p->u.grammar.defs[startidx]);
	}

	int lend = pctx_label(ctx);
	pir_app(&code, insn_opencall(p->u.grammar.start));
	pir_app(&code, insn_jump(VM_JUMP, lend));

	int *labels = xmalloc(p->u.grammar.ndefs * sizeof(int));
	for (size_t i = 0; i < p->u.grammar.ndefs; i++)
		labels[i] = -1;
	for (size_t i = 0; i < p->u.grammar.ndefs; i++) {
		if ((int)i != startidx && !used[i])
			continue;
		labels[i] = pctx_label(ctx);
		pir fn = compile_pat(ctx, p->u.grammar.defs[i]);
		pir_app(&code, insn_label(labels[i]));
		pir_append(&code, &fn);
		free(fn.insns);
		pir_app(&code, insn_basic(VM_RETURN));
	}
	free(used);

	/* Resolve open calls to real calls; a call in tail position (next
	 * real instruction is a Return with no label referring to it)
	 * becomes a jump and the return is nopped. */
	for (size_t i = 0; i < code.n; i++) {
		if (code.insns[i].op != PIR_OPENCALL)
			continue;
		int di = find_def(p, code.insns[i].name);
		if (di < 0 || labels[di] < 0) {
			set_err(ctx, code.insns[i].name);
			free(labels);
			return code;
		}

		size_t ni = next_insn_idx(&code, i + 1);
		if (ni < code.n && code.insns[ni].op == VM_RETURN) {
			code.insns[i] = insn_jump(VM_JUMP, labels[di]);
			bool hadlabel;
			size_t ri = next_insn_label_idx(&code, i + 1, &hadlabel);
			/* next_insn_label_idx yields an ABSOLUTE index, and
			 * !hadlabel implies ri == i + 1 == ni, so nop the
			 * Return that the Jump replaces.  (This used to read
			 * insns[i + 1 + ri], double-counting i: it nop-ed a
			 * live instruction belonging to another definition
			 * and could write past the array.) */
			if (!hadlabel && ri < code.n)
				code.insns[ri] = insn_basic(VM_NOP);
		} else {
			code.insns[i] = insn_jump(VM_CALL, labels[di]);
		}
	}
	free(labels);

	pir_app(&code, insn_label(lend));
	return code;
}

static pir compile_pat(pctx *ctx, pat *p)
{
	pir code = {0};
	switch (p->kind) {
	case PAT_ALT: {
		pat *lp = get_pat(ctx, p->u.alt.l);
		pat *rp = get_pat(ctx, p->u.alt.r);

		/* two single-byte matchers become one set */
		vm_charset set;
		if (combine(lp, rp, &set)) {
			pir_app(&code, insn_set(VM_SET, set));
			return code;
		}

		pir l = compile_pat(ctx, lp);
		pir r = compile_pat(ctx, rp);
		int l1 = pctx_label(ctx);

		/* If the two branches start with disjoint matchers, the
		 * NoChoice head-fail variants apply (the left branch is
		 * entered only when its matcher succeeds, so the choice
		 * point is unnecessary). */
		bool disjoint = false;
		pir_insn test = insn_basic(VM_NOP);
		size_t li = next_insn_idx(&l, 0);
		size_t ri = next_insn_idx(&r, 0);
		if (li < l.n && ri < r.n) {
			if (l.insns[li].op == VM_SET) {
				if (r.insns[ri].op == VM_CHAR)
					disjoint = !vm_charset_has(&l.insns[li].set,
					                           (uint8_t)r.insns[ri].a);
				test = insn_test_set(VM_TEST_SET_NOCHOICE,
				                     l.insns[li].set, l1);
			} else if (l.insns[li].op == VM_CHAR) {
				if (r.insns[ri].op == VM_CHAR)
					disjoint = l.insns[li].a != r.insns[ri].a;
				else if (r.insns[ri].op == VM_SET)
					disjoint = !vm_charset_has(&r.insns[ri].set,
					                           (uint8_t)l.insns[li].a);
				test = insn_test_char(VM_TEST_CHAR_NOCHOICE,
				                      (uint8_t)l.insns[li].a, l1);
			}
		}

		int l2 = pctx_label(ctx);
		if (disjoint) {
			/* The test already consumed the left branch's head
			 * matcher, so it is dropped: element 0 is dropped
			 * whatever it is — a literal, a set, or an Any. */
			pir_app(&code, test);
			for (size_t i = 1; i < l.n; i++)
				pir_app(&code, l.insns[i]);
			pir_app(&code, insn_jump(VM_JUMP, l2));
		} else {
			pir_app(&code, insn_jump(VM_CHOICE, l1));
			pir_append(&code, &l);
			pir_app(&code, insn_jump(VM_COMMIT, l2));
		}
		pir_app(&code, insn_label(l1));
		pir_append(&code, &r);
		pir_app(&code, insn_label(l2));
		free(l.insns);
		free(r.insns);
		return code;
	}
	case PAT_SEQ: {
		pir l = compile_pat(ctx, get_pat(ctx, p->u.alt.l));
		pir r = compile_pat(ctx, get_pat(ctx, p->u.alt.r));
		pir_append(&l, &r);
		free(r.insns);
		return l;
	}
	case PAT_STAR: {
		pat *sp = get_pat(ctx, p->u.unary.p);
		if (sp->kind == PAT_CLASS) {
			/* a repeated set is one Span */
			pir_app(&code, insn_set(VM_SPAN, sp->u.class.set));
			return code;
		}
		if (sp->kind == PAT_MEMO) {
			/* repeating a memoized pattern uses tree memoization
			 * for logarithmic re-parses (paper Section 5.2) */
			pir sub = compile_pat(ctx, get_pat(ctx, sp->u.cap.p));
			int l1 = pctx_label(ctx);
			int l2 = pctx_label(ctx);
			int l3 = pctx_label(ctx);
			int nojump = pctx_label(ctx);
			int id = pat_memo_counter++;

			pir_app(&code, insn_label(l1));
			pir_app(&code, insn_jump(VM_MEMO_TREE_OPEN, l3));
			code.insns[code.n - 1].b = id;
			pir_app(&code, insn_jump(VM_CHOICE, l2));
			pir_append(&code, &sub);
			free(sub.insns);
			pir_app(&code, insn_jump(VM_COMMIT, nojump));
			pir_app(&code, insn_label(nojump));
			pir_app(&code, insn_basic(VM_MEMO_TREE_INSERT));
			pir_app(&code, insn_label(l3));
			pir_app(&code, insn_basic(VM_MEMO_TREE));
			pir_app(&code, insn_jump(VM_JUMP, l1));
			pir_app(&code, insn_label(l2));
			pir_app(&code, insn_u8(VM_MEMO_TREE_CLOSE, id));
			return code;
		}

		pir sub = compile_pat(ctx, sp);
		int l1 = pctx_label(ctx);
		int l2 = pctx_label(ctx);
		pir_app(&code, insn_jump(VM_CHOICE, l2));
		pir_app(&code, insn_label(l1));
		pir_append(&code, &sub);
		free(sub.insns);
		pir_app(&code, insn_jump(VM_PARTIAL_COMMIT, l1));
		pir_app(&code, insn_label(l2));
		return code;
	}
	case PAT_PLUS: {
		/* p+ = p p* — the star is compiled first, matching the
		 * label numbering */
		pat *sp = get_pat(ctx, p->u.unary.p);
		pir star = compile_pat(ctx, transient(ctx, pat_star(sp)));
		pir sub = compile_pat(ctx, sp);
		pir_append(&sub, &star);
		free(star.insns);
		return sub;
	}
	case PAT_OPTIONAL: {
		pat *sp = get_pat(ctx, p->u.unary.p);
		if (sp->kind == PAT_LITERAL && sp->u.literal.len == 1) {
			int l1 = pctx_label(ctx);
			pir_app(&code, insn_test_char(VM_TEST_CHAR_NOCHOICE,
			                      (uint8_t)sp->u.literal.str[0], l1));
			pir_app(&code, insn_label(l1));
			return code;
		}
		if (sp->kind == PAT_CLASS) {
			int l1 = pctx_label(ctx);
			pir_app(&code, insn_test_set(VM_TEST_SET_NOCHOICE,
			                             sp->u.class.set, l1));
			pir_app(&code, insn_label(l1));
			return code;
		}
		/* p? = p / "" */
		return compile_pat(ctx, transient(ctx, pat_alt(sp,
			transient(ctx, pat_new(PAT_EMPTY)))));
	}
	case PAT_NOT: {
		pir sub = compile_pat(ctx, get_pat(ctx, p->u.unary.p));
		int l1 = pctx_label(ctx);
		pir_app(&code, insn_jump(VM_CHOICE, l1));
		pir_append(&code, &sub);
		free(sub.insns);
		pir_app(&code, insn_basic(VM_FAIL_TWICE));
		pir_app(&code, insn_label(l1));
		return code;
	}
	case PAT_AND: {
		pir sub = compile_pat(ctx, get_pat(ctx, p->u.unary.p));
		int l1 = pctx_label(ctx);
		int l2 = pctx_label(ctx);
		pir_app(&code, insn_jump(VM_CHOICE, l1));
		pir_append(&code, &sub);
		free(sub.insns);
		pir_app(&code, insn_jump(VM_BACK_COMMIT, l2));
		pir_app(&code, insn_label(l1));
		pir_app(&code, insn_basic(VM_FAIL));
		pir_app(&code, insn_label(l2));
		return code;
	}
	case PAT_CAP: {
		pir sub = compile_pat(ctx, get_pat(ctx, p->u.cap.p));

		/* Count the leading run of constant-width matchers so the
		 * capture can be opened late (or built in full) instead of
		 * stack-tracking through the body. */
		size_t i = 0;
		int back = 0;
		while (i < sub.n) {
			uint8_t op = sub.insns[i].op;
			if (op == VM_CHAR || op == VM_SET) {
				back++;
			} else if (op == VM_ANY) {
				back += sub.insns[i].a;
			} else {
				break;
			}
			i++;
		}

		if (i == 0 || back >= 256) {
			pir_app(&code, insn_u8(VM_CAPTURE_BEGIN, p->u.cap.id));
			i = 0;
		} else if (i == sub.n) {
			pir_append(&code, &sub);
			pir_app(&code, insn_u8(VM_CAPTURE_FULL, p->u.cap.id));
			code.insns[code.n - 1].b = back;
			free(sub.insns);
			return code;
		} else {
			for (size_t j = 0; j < i; j++)
				pir_app(&code, sub.insns[j]);
			pir_app(&code, insn_u8(VM_CAPTURE_LATE, p->u.cap.id));
			code.insns[code.n - 1].b = back;
		}
		for (size_t j = i; j < sub.n; j++)
			pir_app(&code, sub.insns[j]);
		pir_app(&code, insn_basic(VM_CAPTURE_END));
		free(sub.insns);
		return code;
	}
	case PAT_MEMO: {
		int l1 = pctx_label(ctx);
		pir sub = compile_pat(ctx, get_pat(ctx, p->u.cap.p));
		pir_app(&code, insn_jump(VM_MEMO_OPEN, l1));
		code.insns[code.n - 1].b = p->u.cap.id;
		pir_append(&code, &sub);
		free(sub.insns);
		pir_app(&code, insn_basic(VM_MEMO_CLOSE));
		pir_app(&code, insn_label(l1));
		return code;
	}
	case PAT_CHECK: {
		int l1 = pctx_label(ctx);
		pir sub = compile_pat(ctx, get_pat(ctx, p->u.check.p));
		pir_app(&code, insn_u8(VM_CHECK_BEGIN, p->u.check.id));
		code.insns[code.n - 1].b = p->u.check.flag;
		pir_append(&code, &sub);
		free(sub.insns);
		pir_app(&code, insn_basic(VM_CHECK_END));
		code.insns[code.n - 1].fn = p->u.check.fn;
		code.insns[code.n - 1].ud = p->u.check.ud;
		pir_app(&code, insn_label(l1));
		return code;
	}
	case PAT_SEARCH: {
		pat *sp = get_pat(ctx, p->u.unary.p);
		pir sub = compile_pat(ctx, sp);	/* analyzed, then discarded */

		/* Heuristic: if the searched pattern starts with a
		 * rare matcher, skip runs of everything else first. */
		bool opt = false;
		vm_charset set;
		size_t ni = next_insn_idx(&sub, 0);
		if (ni < sub.n) {
			if (sub.insns[ni].op == VM_CHAR) {
				uint8_t b = (uint8_t)sub.insns[ni].a;
				vm_charset_fill(&set, &b, 1);
				vm_charset_negate(&set, &set);
				opt = true;
			} else if (sub.insns[ni].op == VM_SET &&
			           vm_charset_count(&sub.insns[ni].set) < 10) {
				set = sub.insns[ni].set;
				vm_charset_negate(&set, &set);
				opt = true;
			}
		}
		free(sub.insns);

		/* S <- sp / . rsearch ; rsearch <- [skip]* S */
		pat *rsearch;
		if (opt) {
			pat *skip = transient(ctx, pat_set(&set));
			rsearch = transient(ctx, pat_concat(
				(pat *[]){ transient(ctx, pat_star(skip)),
				           transient(ctx, pat_nonterm("S")) },
				2));
		} else {
			rsearch = transient(ctx, pat_nonterm("S"));
		}
		pat *any = transient(ctx, pat_any(1));
		pat *rhs = transient(ctx, pat_concat(
			(pat *[]){ any, rsearch }, 2));
		const char *names[] = { "S" };
		pat *g = transient(ctx, pat_grammar("S", names,
			(pat *[]){ transient(ctx, pat_alt(sp, rhs)) }, 1));
		return compile_grammar(ctx, g);
	}
	case PAT_GRAMMAR:
		return compile_grammar(ctx, p);
	case PAT_CLASS:
		pir_app(&code, insn_set(VM_SET, p->u.class.set));
		return code;
	case PAT_LITERAL:
		for (size_t i = 0; i < p->u.literal.len; i++)
			pir_app(&code, insn_u8(VM_CHAR,
			                       (uint8_t)p->u.literal.str[i]));
		return code;
	case PAT_NONTERM:
		if (p->u.nonterm.inlined != NULL)
			return compile_pat(ctx, p->u.nonterm.inlined);
		pir_app(&code, insn_opencall(p->u.nonterm.name));
		return code;
	case PAT_DOT:
		pir_app(&code, insn_u8(VM_ANY, p->u.dot.n));
		return code;
	case PAT_ERROR: {
		pir_app(&code, insn_basic(VM_ERROR));
		code.insns[code.n - 1].msg = p->u.error.msg;
		if (p->u.error.recover == NULL) {
			pir_app(&code, insn_u8(VM_END, 1));
		} else {
			pir rec = compile_pat(ctx, p->u.error.recover);
			pir_append(&code, &rec);
			free(rec.insns);
		}
		return code;
	}
	case PAT_EMPTYOP:
		pir_app(&code, insn_u8(VM_EMPTY, p->u.emptyop.op));
		return code;
	case PAT_EMPTY:
		return code;
	case PAT_REPEAT: {
		/* p{n}: n independent compilations of p (a shared sub-tree
		 * sub-pattern in a SeqNode chain; each occurrence compiles
		 * separately there, with fresh labels — compiling n times
		 * matches that without pointer sharing). */
		for (int i = 0; i < p->u.repeat.n; i++) {
			pir sub = compile_pat(ctx, p->u.repeat.p);
			pir_append(&code, &sub);
			free(sub.insns);
		}
		return code;
	}
	}
	return code;
}

/* ---------------------------------------------------------------------- */
/* Grammar inlining                                                       */
/* ---------------------------------------------------------------------- */

/* Nodes with trees larger than this size will not be inlined. */
#define PAT_INLINE_THRESHOLD 100

/* Count a definition's sub-patterns and whether it references any
 * unresolved non-terminal (its size, and whether it is a leaf). */
struct size_leaf_data {
	int size;
	bool leaf;
};

static void count_size_leaf_fn(pat *sub, void *ud)
{
	struct size_leaf_data *d = ud;
	if (sub->kind == PAT_NONTERM && sub->u.nonterm.inlined == NULL)
		d->leaf = false;
	d->size++;
}

struct inline_data {
	pat *g;
	const int *sizes;
	const bool *leaves;
	bool did;
};

/* Substitute a small, leaf definition into a referencing non-terminal
 * (the `inlined` back-reference; see pattern.h on ownership). */
static void inline_fn(pat *sub, void *ud)
{
	struct inline_data *d = ud;
	if (sub->kind != PAT_NONTERM || sub->u.nonterm.inlined != NULL)
		return;
	int di = find_def(d->g, sub->u.nonterm.name);
	if (di < 0)
		return;
	/* We only inline nodes if they are small enough and don't use any
	 * non-terminals themselves. */
	if (d->sizes[di] < PAT_INLINE_THRESHOLD && d->leaves[di]) {
		d->did = true;
		sub->u.nonterm.inlined = d->g->u.grammar.defs[di];
	}
}

/*
 * One inlining pass.  Returns true if anything
 * was inlined; the caller loops to a fixed point.
 */
static bool grammar_inline_once(pat *g)
{
	size_t n = g->u.grammar.ndefs;
	int *sizes = xmalloc(n * sizeof(int));
	bool *leaves = xmalloc(n * sizeof(bool));

	for (size_t i = 0; i < n; i++) {
		struct size_leaf_data d = { .size = 0, .leaf = true };
		walk_pat(g->u.grammar.defs[i], true, count_size_leaf_fn, &d);
		sizes[i] = d.size;
		leaves[i] = d.leaf;
	}

	struct inline_data d = {
		.g = g, .sizes = sizes, .leaves = leaves, .did = false,
	};
	/* Walk the whole grammar (following inlined references), so nested
	 * grammars' non-terminals can resolve against these defs too. */
	walk_pat(g, true, inline_fn, &d);

	free(sizes);
	free(leaves);
	return d.did;
}

static void grammar_inline(pat *g)
{
	while (grammar_inline_once(g))
		;
}

/* ---------------------------------------------------------------------- */
/* Optimize                                                               */
/* ---------------------------------------------------------------------- */

/*
 * Two passes over the instruction list:
 *
 *   head-fail — a Choice immediately followed (no intervening label) by
 *   Char/Set/Any becomes TestChar/TestSet/TestAny, and the matcher
 *   becomes a Nop (its check already happened in the test);
 *
 *   jump replacement — a Jump whose target's next real instruction is
 *   another control-flow instruction (PartialCommit, BackCommit, Commit,
 *   Jump, Return, Fail, FailTwice, End) becomes that instruction.
 */
static void pat_optimize(pir *p)
{
	/* label id -> instruction index of its entry (label ids are dense
	 * from 0, so first find the range) */
	size_t nlabels = 0;
	for (size_t i = 0; i < p->n; i++) {
		if (p->insns[i].op == PIR_LABEL && (size_t)p->insns[i].a >= nlabels)
			nlabels = (size_t)p->insns[i].a + 1;
	}
	int *labidx = xmalloc((nlabels + 1) * sizeof(int));
	for (size_t i = 0; i < p->n; i++) {
		if (p->insns[i].op == PIR_LABEL)
			labidx[p->insns[i].a] = (int)i;
	}

	for (size_t i = 0; i < p->n; i++) {
		pir_insn *in = &p->insns[i];

		if (in->op == VM_CHOICE && i + 1 < p->n) {
			pir_insn *next = &p->insns[i + 1];
			switch (next->op) {
			case VM_CHAR:
				in->op = VM_TEST_CHAR;
				in->b = next->a;
				next->op = VM_NOP;
				break;
			case VM_SET:
				in->op = VM_TEST_SET;
				in->set = next->set;
				next->op = VM_NOP;
				break;
			case VM_ANY:
				in->op = VM_TEST_ANY;
				in->b = next->a;
				next->op = VM_NOP;
				break;
			}
		}

		if (in->op == VM_JUMP) {
			size_t from = labidx[in->label];
			size_t ni = next_insn_idx(p, from);
			if (ni < p->n) {
				switch (p->insns[ni].op) {
				case VM_PARTIAL_COMMIT:
				case VM_BACK_COMMIT:
				case VM_COMMIT:
				case VM_JUMP:
				case VM_RETURN:
				case VM_FAIL:
				case VM_FAIL_TWICE:
				case VM_END:
					p->insns[i] = p->insns[ni];
					break;
				}
			}
		}
	}
	free(labidx);
}

/* ---------------------------------------------------------------------- */
/* Encoding (lowering the IR through the streaming vm_emit_* builder)     */
/* ---------------------------------------------------------------------- */

/*
 * The IR's label ids are dense per compile; the streaming builder has its
 * own label space.  Map: IR label id -> builder label id, creating
 * builder labels lazily.
 */
struct encoder {
	vm_prog *b;
	int *map;		/* IR label id -> builder label id */
	size_t nmap;
	pir_insn *checkers;	/* CheckEnd insns in emission order */
	size_t ncheck, checkcap;
};

static int enc_label(struct encoder *e, int ir_label)
{
	if ((size_t)ir_label >= e->nmap) {
		size_t old = e->nmap;
		e->nmap = (size_t)ir_label + 1;
		e->map = xrealloc(e->map, e->nmap * sizeof(int));
		for (size_t i = old; i < e->nmap; i++)
			e->map[i] = -1;
	}
	if (e->map[ir_label] < 0)
		e->map[ir_label] = vm_prog_label(e->b);
	return e->map[ir_label];
}

static void enc(pir_insn *in, struct encoder *e)
{
	switch (in->op) {
	case PIR_LABEL:
		vm_prog_mark(e->b, enc_label(e, in->a));
		break;
	case PIR_OPENCALL:
		abort_msg("internal: unresolved OpenCall in encoder");
		break;
	case VM_CHAR:		vm_emit_char(e->b, (uint8_t)in->a); break;
	case VM_JUMP:		vm_emit_jump(e->b, enc_label(e, in->label)); break;
	case VM_CHOICE:		vm_emit_choice(e->b, enc_label(e, in->label)); break;
	case VM_CALL:		vm_emit_call(e->b, enc_label(e, in->label)); break;
	case VM_COMMIT:		vm_emit_commit(e->b, enc_label(e, in->label)); break;
	case VM_RETURN:		vm_emit_return(e->b); break;
	case VM_FAIL:		vm_emit_fail(e->b); break;
	case VM_SET:		vm_emit_set(e->b, &in->set); break;
	case VM_ANY:		vm_emit_any(e->b, (uint8_t)in->a); break;
	case VM_PARTIAL_COMMIT:
				vm_emit_partial_commit(e->b, enc_label(e, in->label)); break;
	case VM_SPAN:		vm_emit_span(e->b, &in->set); break;
	case VM_BACK_COMMIT:	vm_emit_back_commit(e->b, enc_label(e, in->label)); break;
	case VM_FAIL_TWICE:	vm_emit_fail_twice(e->b); break;
	case VM_EMPTY:		vm_emit_empty(e->b, (uint8_t)in->a); break;
	case VM_TEST_CHAR:
		vm_emit_test_char(e->b, (uint8_t)in->b, enc_label(e, in->label));
		break;
	case VM_TEST_CHAR_NOCHOICE:
		vm_emit_test_char_nochoice(e->b, (uint8_t)in->b, enc_label(e, in->label));
		break;
	case VM_TEST_SET:
		vm_emit_test_set(e->b, &in->set, enc_label(e, in->label));
		break;
	case VM_TEST_SET_NOCHOICE:
		vm_emit_test_set_nochoice(e->b, &in->set, enc_label(e, in->label));
		break;
	case VM_TEST_ANY:
		vm_emit_test_any(e->b, (uint8_t)in->b, enc_label(e, in->label));
		break;
	case VM_END:		vm_emit_end(e->b, in->a != 0); break;
	case VM_NOP:		break;	/* never encoded */
	case VM_CAPTURE_BEGIN:	vm_emit_capture_begin(e->b, (int16_t)in->a); break;
	case VM_CAPTURE_LATE:
		vm_emit_capture_late(e->b, (uint8_t)in->b, (int16_t)in->a);
		break;
	case VM_CAPTURE_END:	vm_emit_capture_end(e->b); break;
	case VM_CAPTURE_FULL:
		vm_emit_capture_full(e->b, (uint8_t)in->b, (int16_t)in->a);
		break;
	case VM_CHECK_BEGIN:
		vm_emit_check_begin(e->b, (int16_t)in->a, (int16_t)in->b);
		break;
	case VM_CHECK_END:
		/* remember the checker for registration after finish; the
		 * operand (its index) is assigned at emission */
		if (e->ncheck == e->checkcap) {
			e->checkcap = e->checkcap ? e->checkcap * 2 : 4;
			e->checkers = xrealloc(e->checkers,
			                       e->checkcap * sizeof(e->checkers[0]));
		}
		e->checkers[e->ncheck++] = *in;
		vm_emit_check_end(e->b);
		break;
	case VM_MEMO_OPEN:
		vm_emit_memo_open(e->b, enc_label(e, in->label), (int16_t)in->b);
		break;
	case VM_MEMO_CLOSE:	vm_emit_memo_close(e->b); break;
	case VM_MEMO_TREE_OPEN:
		vm_emit_memo_tree_open(e->b, enc_label(e, in->label), (int16_t)in->b);
		break;
	case VM_MEMO_TREE_INSERT: vm_emit_memo_tree_insert(e->b); break;
	case VM_MEMO_TREE:	vm_emit_memo_tree(e->b); break;
	case VM_MEMO_TREE_CLOSE:
		vm_emit_memo_tree_close(e->b, (int16_t)in->a);
		break;
	case VM_ERROR:		vm_emit_error(e->b, in->msg); break;
	default:
		abort_msg("internal: unknown IR opcode in encoder");
	}
}

/* ---------------------------------------------------------------------- */
/* Public entry points                                                    */
/* ---------------------------------------------------------------------- */

vm_code *pat_compile(pat *p, const char **err)
{
	pctx ctx = {0};
	pir code = compile_pat(&ctx, p);

	/* A non-terminal outside any (resolving) grammar stays an OpenCall
	 * placeholder; report it like a grammar's missing definition.
	 * lets it fall into the encoder's default case; we prefer a proper
	 * error.) */
	for (size_t i = 0; i < code.n && ctx.err == NULL; i++) {
		if (code.insns[i].op == PIR_OPENCALL)
			set_err(&ctx, code.insns[i].name);
	}

	if (ctx.err != NULL) {
		if (err != NULL)
			*err = ctx.err;
		free(code.insns);
		pctx_cleanup(&ctx);
		return NULL;
	}

	pat_optimize(&code);

	struct encoder e = {0};
	e.b = vm_prog_new();
	for (size_t i = 0; i < code.n; i++)
		enc(&code.insns[i], &e);
	vm_code *out = vm_prog_finish(e.b);
	vm_prog_free(e.b);
	free(e.map);

	/* Register the checkers in emission order (matching the indices the
	 * CheckEnd instructions encode). */
	for (size_t i = 0; i < e.ncheck; i++)
		vm_code_add_checker(out, e.checkers[i].fn, e.checkers[i].ud);
	free(e.checkers);

	free(code.insns);
	pctx_cleanup(&ctx);
	return out;
}

/* ---------------------------------------------------------------------- */
/* Prettify                                                               */
/* ---------------------------------------------------------------------- */

/* A small growable string buffer. */
struct sbuf {
	char *s;
	size_t n, cap;
};

static void sbuf_app(struct sbuf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int need = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (need < 0) {
		va_end(ap2);
		return;
	}
	if (b->n + (size_t)need + 1 > b->cap) {
		b->cap = b->n + (size_t)need + 1;
		b->s = xrealloc(b->s, b->cap);
	}
	vsnprintf(b->s + b->n, (size_t)need + 1, fmt, ap2);
	va_end(ap2);
	b->n += (size_t)need;
}

static void set_string_inner(const vm_charset *set, struct sbuf *b)
{
	bool in_range = false;
	for (int c = 0; c <= 255; c++) {
		bool has = vm_charset_has(set, (uint8_t)c);
		if (has && c == 255) {
			sbuf_app(b, "%c", (char)c);
		} else if (has && !in_range) {
			in_range = true;
			if (vm_charset_has(set, (uint8_t)(c + 1)))
				sbuf_app(b, "%c..", (char)c);
		} else if (!has && in_range) {
			in_range = false;
			sbuf_app(b, "%c,", (char)(c - 1));
		}
	}
	if (b->n > 0 && b->s[b->n - 1] == ',')
		b->s[--b->n] = '\0';
}

/* Rendering of a charset: contents in braces. */
static void set_string(const vm_charset *set, struct sbuf *b)
{
	sbuf_app(b, "{");
	set_string_inner(set, b);
	sbuf_app(b, "}");
}

static void prettify(pctx *ctx, pat *p, struct sbuf *b)
{
	/* pat_error accepts a NULL recovery pattern, so the printer has to
	 * be total over the constructor API's domain. */
	if (p == NULL) {
		sbuf_app(b, "nil");
		return;
	}
	p = get_pat(ctx, p);
	switch (p->kind) {
	case PAT_LITERAL:
		sbuf_app(b, "\"");
		for (size_t i = 0; i < p->u.literal.len; i++) {
			uint8_t c = (uint8_t)p->u.literal.str[i];
			if (c == '"' || c == '\\')
				sbuf_app(b, "\\%c", (char)c);
			else if (c >= 0x20 && c < 0x7f)
				sbuf_app(b, "%c", (char)c);
			else
				sbuf_app(b, "\\x%02x", c);
		}
		sbuf_app(b, "\"");
		break;
	case PAT_CLASS: {
		/* the charset
		 * renders with surrounding braces. */
		sbuf_app(b, "[");
		set_string(&p->u.class.set, b);
		sbuf_app(b, "]");
		break;
	}
	case PAT_DOT:
		sbuf_app(b, ".");
		break;
	case PAT_EMPTY:
		sbuf_app(b, "\"\"");
		break;
	case PAT_ALT:
		sbuf_app(b, "(");
		prettify(ctx, p->u.alt.l, b);
		sbuf_app(b, " / ");
		prettify(ctx, p->u.alt.r, b);
		sbuf_app(b, ")");
		break;
	case PAT_SEQ:
		sbuf_app(b, "(");
		prettify(ctx, p->u.alt.l, b);
		sbuf_app(b, " ");
		prettify(ctx, p->u.alt.r, b);
		sbuf_app(b, ")");
		break;
	case PAT_STAR:
		prettify(ctx, p->u.unary.p, b);
		sbuf_app(b, "*");
		break;
	case PAT_PLUS:
		prettify(ctx, p->u.unary.p, b);
		sbuf_app(b, "+");
		break;
	case PAT_OPTIONAL:
		prettify(ctx, p->u.unary.p, b);
		sbuf_app(b, "?");
		break;
	case PAT_NOT:
		sbuf_app(b, "!");
		prettify(ctx, p->u.unary.p, b);
		break;
	case PAT_AND:
		sbuf_app(b, "&");
		prettify(ctx, p->u.unary.p, b);
		break;
	case PAT_CAP:
		sbuf_app(b, "{ ");
		prettify(ctx, p->u.cap.p, b);
		sbuf_app(b, " }");
		break;
	case PAT_MEMO:
		sbuf_app(b, "{{ ");
		prettify(ctx, p->u.cap.p, b);
		sbuf_app(b, " }}");
		break;
	case PAT_SEARCH:
		sbuf_app(b, "search(");
		prettify(ctx, p->u.unary.p, b);
		sbuf_app(b, ")");
		break;
	case PAT_CHECK:
		sbuf_app(b, "check(");
		prettify(ctx, p->u.check.p, b);
		sbuf_app(b, ")");
		break;
	case PAT_ERROR:
		sbuf_app(b, "err(%s, ", p->u.error.msg);
		prettify(ctx, p->u.error.recover, b);
		sbuf_app(b, ")");
		break;
	case PAT_EMPTYOP:
		sbuf_app(b, "empty(%d)", p->u.emptyop.op);
		break;
	case PAT_GRAMMAR:
		sbuf_app(b, "%s\n", p->u.grammar.start);
		grammar_inline(p);
		for (size_t i = 0; i < p->u.grammar.ndefs; i++) {
			sbuf_app(b, "%s <- ", p->u.grammar.names[i]);
			prettify(ctx, p->u.grammar.defs[i], b);
			sbuf_app(b, "\n");
		}
		break;
	case PAT_NONTERM:
		if (p->u.nonterm.inlined != NULL) {
			prettify(ctx, p->u.nonterm.inlined, b);
			break;
		}
		sbuf_app(b, "%s", p->u.nonterm.name);
		break;
	default:
		sbuf_app(b, "<invalid>");
		break;
	}
}

char *pat_prettify(pat *p)
{
	pctx ctx = {0};
	struct sbuf b = {0};
	prettify(&ctx, p, &b);
	pctx_cleanup(&ctx);
	if (b.s == NULL)
		b.s = xstrdup("");
	return b.s;
}
