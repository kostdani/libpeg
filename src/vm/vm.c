/*
 * vm.c - the libpeg parsing machine: input wrapper, value stack, and the
 * interpreter loop.
 *
 * Instruction semantics are documented in peg_vm.h; the encoding, in
 * vm_code.c.
 *
 * Capture ownership (captures are shared between the stack, the memo
 * table, and the result tree, and are reference counted — see peg_memo.h):
 *
 *  - Every container holding a capture pointer holds one reference per
 *    pointer: a stack entry's capture list, a capture's children array,
 *    a memo entry's capture list, and the result's dummy root.
 *  - stack_add_capt takes its own reference per added pointer.
 *  - pop(propagate=true) DUPLICATES the popped entry's references one
 *    level up; the popped entry's own references
 *    are handed to the caller inside `out`, which then either drops
 *    them (entry_clear_capt) or transfers them (memoize).
 *  - memoize takes full ownership (array + one ref per element) in
 *    every path: the table stores them, or they are freed (windowed
 *    passes and no-table runs never store captures).  Callers must not
 *    touch the array after memoize returns.
 *  - Where a still-live stack entry's capture list is handed to the
 *    memo table (MemoTreeInsert, MemoTree's merge), the entry survives
 *    the call, so the table receives a ref'd COPY of the pointer array
 *    — the same capture objects, a second set of references.  The
 *    table's entry and the stack entry see the same (relocatable)
 *    captures.
 */
#include <stdlib.h>
#include <string.h>

#include "peg/peg_util.h"
#include "vm_internal.h"

/* ---------------------------------------------------------------------- */
/* Input                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * The input: a 4KB-chunk cache over the flat subject buffer with a
 * furthest-read tracker.  The tracker is what memo entries record as
 * their examined extent, so every read path (peek, advance, span) must
 * update it exactly the same way.
 */
#define INPUT_CHUNK 4096

struct vm_input {
	const uint8_t *data;	/* the subject (not owned) */
	size_t len;

	uint8_t chunk[INPUT_CHUNK];	/* cached data */
	size_t nchunk;		/* valid bytes in chunk */
	size_t base;		/* subject offset of chunk[0] */
	size_t coff;		/* read offset within chunk */
	int furthest;		/* furthest position ever read */
};

vm_input *vm_input_new(const uint8_t *data, size_t len)
{
	vm_input *i = xmalloc(sizeof(*i));
	i->data = data;
	i->len = len;
	i->nchunk = 0;
	i->base = 0;
	i->coff = 0;
	i->furthest = 0;
	size_t n = len < INPUT_CHUNK ? len : INPUT_CHUNK;
	memcpy(i->chunk, data, n);
	i->nchunk = n;
	return i;
}

void vm_input_free(vm_input *i)
{
	free(i);
}

static void input_refill(vm_input *i, size_t pos)
{
	i->base = pos;
	i->coff = 0;
	size_t avail = i->len > pos ? i->len - pos : 0;
	i->nchunk = avail < INPUT_CHUNK ? avail : INPUT_CHUNK;
	if (i->nchunk > 0)
		memcpy(i->chunk, i->data + pos, i->nchunk);
}

int vm_input_pos(const vm_input *i)
{
	return (int)(i->base + i->coff);
}

int vm_input_furthest(const vm_input *i)
{
	return i->furthest;
}

void vm_input_reset_furthest(vm_input *i)
{
	i->furthest = 0;
}

/*
 * Peek at the current byte.  Returns false at end of input.  The
 * furthest tracker advances even at end of input (the tracker
 * follows the position, not the byte).
 */
static bool input_peek(vm_input *i, uint8_t *out)
{
	size_t pos = i->base + i->coff;
	if ((int)pos > i->furthest)
		i->furthest = (int)pos;
	if (i->nchunk == 0 || i->coff >= i->nchunk)
		return false;
	*out = i->chunk[i->coff];
	return true;
}

/* Advance n bytes; false if that moves past the end of the data. */
static bool input_advance(vm_input *i, int n)
{
	if (i->nchunk == 0)
		return false;
	i->coff += (size_t)n;
	if (i->coff >= i->nchunk) {
		bool over = i->coff > i->nchunk;
		input_refill(i, i->base + i->coff);
		return !over;
	}
	return true;
}

static void input_seek_to(vm_input *i, int pos)
{
	size_t chunk_end = i->base + i->nchunk;
	if ((size_t)pos < chunk_end && (size_t)pos >= i->base) {
		i->coff = (size_t)pos - i->base;
		return;
	}
	input_refill(i, (size_t)pos);
}

/* Advance past the longest run of bytes in set (opSpan), updating the
 * furthest tracker the same way peek does. */
static void input_span(vm_input *i, const vm_charset *set)
{
	for (;;) {
		while (i->coff < i->nchunk &&
		       vm_charset_has(set, i->chunk[i->coff]))
			i->coff++;
		int pos = (int)(i->base + i->coff);
		if (pos > i->furthest)
			i->furthest = pos;
		if (i->coff < i->nchunk || i->nchunk == 0)
			return;
		/* Chunk exhausted mid-span: refill and continue. */
		input_refill(i, i->base + i->coff);
	}
}

/* ---------------------------------------------------------------------- */
/* Stack                                                                  */
/* ---------------------------------------------------------------------- */

enum {
	stRet,
	stBtrack,
	stMemo,
	stMemoTree,
	stCapt,
	stCheck
};

/*
 * A stack entry.  Btrack entries carry (ip, sp); ret entries a return
 * instruction index; memo/memoTree/capt/check entries carry
 * (id, pos, count).  All kinds can accumulate a capture list: captures
 * produced while the entry is on the stack belong to it and are
 * propagated up (pop with propagate) when the entry is popped, or
 * dropped when the branch is abandoned.  The list holds one capture
 * reference per pointer.
 */
typedef struct {
	uint8_t stype;
	int32_t ret;		/* stRet: return instruction index */
	int32_t btrack_ip;	/* stBtrack: backtrack instruction index */
	int32_t btrack_off;	/* stBtrack: backtrack subject position */
	int16_t id;		/* stMemo/stMemoTree/stCapt/stCheck */
	int32_t pos;		/* entry's subject position */
	int32_t count;		/* stMemoTree: rep count; stCheck: flag */

	memo_capture **capt;
	size_t ncapt;
	size_t captcap;
} stack_entry;

typedef struct {
	stack_entry *entries;
	size_t n;
	size_t cap;

	/* Captures below every entry (the result list). */
	memo_capture **capt;
	size_t ncapt;
	size_t captcap;
} vm_stack;

static stack_entry *stack_grow(vm_stack *s)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 16;
		s->entries = xrealloc(s->entries,
		                      s->cap * sizeof(s->entries[0]));
	}
	stack_entry *e = &s->entries[s->n++];
	/* Reused (and realloc-fresh) slots must start with an empty
	 * capture list; the push helpers write the type's own fields. */
	e->capt = NULL;
	e->ncapt = 0;
	e->captcap = 0;
	return e;
}

/* Ensure room for n more pointers in a capture list.  ncur is the
 * current used count; the capacity (doubled as needed) is *cap. */
static memo_capture **capt_list_grow(memo_capture **list, size_t *cap,
                                     size_t ncur, size_t n)
{
	size_t c = *cap ? *cap : 4;
	while (ncur + n > c)
		c *= 2;
	*cap = c;
	return xrealloc(list, c * sizeof(memo_capture *));
}

/* Append captures to an entry's list, taking a reference per pointer. */
static void entry_add_capt(stack_entry *e, memo_capture **capt, size_t ncapt)
{
	if (ncapt == 0)
		return;
	if (e->ncapt + ncapt > e->captcap)
		e->capt = capt_list_grow(e->capt, &e->captcap, e->ncapt,
		                         ncapt);
	for (size_t i = 0; i < ncapt; i++) {
		e->capt[e->ncapt++] = capt[i];
		memo_capture_ref(capt[i]);
	}
}

/* Release every reference held by an entry's capture list. */
static void entry_clear_capt(stack_entry *e)
{
	for (size_t i = 0; i < e->ncapt; i++)
		memo_capture_free(e->capt[i]);
	e->ncapt = 0;
}

/* Append captures to the top entry's list, or to the final (result)
 * list if the stack is empty. */
static void stack_add_capt(vm_stack *s, memo_capture **capt, size_t ncapt)
{
	if (ncapt == 0)
		return;
	if (s->n == 0) {
		if (s->ncapt + ncapt > s->captcap)
			s->capt = capt_list_grow(s->capt, &s->captcap,
			                         s->ncapt, ncapt);
		for (size_t i = 0; i < ncapt; i++) {
			s->capt[s->ncapt++] = capt[i];
			memo_capture_ref(capt[i]);
		}
	} else {
		entry_add_capt(&s->entries[s->n - 1], capt, ncapt);
	}
}

/*
 * Move the top entry's captures to the container below it, WITHOUT
 * changing any reference counts (the pointers move, and the count
 * already reflects one reference per side...  in fact propCapt is only
 * ever called when the top entry is about to keep living with an empty
 * list, so a pure move is exactly right).
 */
static void stack_prop_capt(vm_stack *s)
{
	if (s->n == 0)
		return;
	stack_entry *top = &s->entries[s->n - 1];
	if (top->ncapt > 0) {
		if (s->n == 1) {
			if (s->ncapt + top->ncapt > s->captcap)
				s->capt = capt_list_grow(s->capt, &s->captcap,
				                         s->ncapt,
				                         top->ncapt);
			memcpy(s->capt + s->ncapt, top->capt,
			       top->ncapt * sizeof(top->capt[0]));
			s->ncapt += top->ncapt;
		} else {
			stack_entry *below = &s->entries[s->n - 2];
			if (below->ncapt + top->ncapt > below->captcap)
				below->capt = capt_list_grow(
					below->capt, &below->captcap,
					below->ncapt, top->ncapt);
			memcpy(below->capt + below->ncapt, top->capt,
			       top->ncapt * sizeof(top->capt[0]));
			below->ncapt += top->ncapt;
		}
		top->ncapt = 0;		/* references moved, not dropped */
	}
	free(top->capt);
	top->capt = NULL;
	top->captcap = 0;
}

/*
 * Pop the top entry into *out (with its capture array and the
 * references it holds).  With propagate, duplicate the references one
 * level up first (see the file comment).  Returns false if the stack
 * was empty (out untouched).  The
 * caller disposes of out.capt: entry_clear_capt + free to drop, or
 * memoize to transfer.
 */
static bool stack_pop(vm_stack *s, bool propagate, stack_entry *out)
{
	if (s->n == 0)
		return false;
	stack_entry *e = &s->entries[--s->n];
	if (propagate)
		stack_add_capt(s, e->capt, e->ncapt);
	*out = *e;
	e->capt = NULL;
	e->ncapt = 0;
	e->captcap = 0;
	return true;
}

/* Push helpers: write the fields the entry type uses and leave the
 * (already cleared) capture list empty. */
static void push_ret(vm_stack *s, int32_t ret)
{
	stack_entry *e = stack_grow(s);
	e->stype = stRet;
	e->ret = ret;
}

static void push_btrack(vm_stack *s, int32_t ip, int32_t off)
{
	stack_entry *e = stack_grow(s);
	e->stype = stBtrack;
	e->btrack_ip = ip;
	e->btrack_off = off;
}

static void push_memo(vm_stack *s, int16_t id, int32_t pos)
{
	stack_entry *e = stack_grow(s);
	e->stype = stMemo;
	e->id = id;
	e->pos = pos;
	e->count = 0;
}

static void push_memotree(vm_stack *s, int16_t id, int32_t pos, int32_t count)
{
	stack_entry *e = stack_grow(s);
	e->stype = stMemoTree;
	e->id = id;
	e->pos = pos;
	e->count = count;
}

static void push_capt(vm_stack *s, int16_t id, int32_t pos)
{
	stack_entry *e = stack_grow(s);
	e->stype = stCapt;
	e->id = id;
	e->pos = pos;
}

static void push_check(vm_stack *s, int16_t id, int32_t pos, int32_t flag)
{
	stack_entry *e = stack_grow(s);
	e->stype = stCheck;
	e->id = id;
	e->pos = pos;
	e->count = flag;
}

/* Release every remaining entry's and the final list's references. */
static void stack_destroy(vm_stack *s)
{
	for (size_t i = 0; i < s->n; i++)
		entry_clear_capt(&s->entries[i]);
	free(s->entries);
	for (size_t i = 0; i < s->ncapt; i++)
		memo_capture_free(s->capt[i]);
	free(s->capt);
}

/* ---------------------------------------------------------------------- */
/* Empty-op context (for the Empty instruction)                           */
/* ---------------------------------------------------------------------- */

/*
 * The empty-op context between two adjacent bytes.  Bit values:
 * EmptyBeginLine=1, EmptyEndLine=2, EmptyBeginText=4, EmptyEndText=8,
 * EmptyWordBoundary=16, EmptyNoWordBoundary=32.  A byte of -1 means
 * "no character" (start/end of input).
 */
static bool is_word_byte(int r)
{
	return (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') ||
	       (r >= '0' && r <= '9') || r == '_';
}

static int empty_op_context(int r1, int r2)
{
	int op = 32;			/* EmptyNoWordBoundary */
	int boundary = 0;
	if (is_word_byte(r1)) {
		boundary = 1;
	} else if (r1 == '\n') {
		op |= 1;			/* EmptyBeginLine */
	} else if (r1 < 0) {
		op |= 4 | 1;			/* BeginText | BeginLine */
	}
	if (is_word_byte(r2)) {
		boundary ^= 1;
	} else if (r2 == '\n') {
		op |= 2;			/* EmptyEndLine */
	} else if (r2 < 0) {
		op |= 8 | 2;			/* EndText | EndLine */
	}
	if (boundary != 0)
		op ^= (16 | 32);		/* WordBoundary swap */
	return op;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ---------------------------------------------------------------------- */
/* Errors                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
	vm_error *v;
	size_t n;
	size_t cap;
} errlist;

static void errlist_add(errlist *l, int pos, const char *msg)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 4;
		l->v = xrealloc(l->v, l->cap * sizeof(l->v[0]));
	}
	size_t len = strlen(msg) + 1;
	l->v[l->n].message = xmalloc(len);
	memcpy(l->v[l->n].message, msg, len);
	l->v[l->n].pos = pos;
	l->n++;
}

/* ---------------------------------------------------------------------- */
/* Execution                                                              */
/* ---------------------------------------------------------------------- */

static bool overlaps(int wl, int wh, int low2, int high2)
{
	return wl < high2 && wh > low2;
}

/*
 * A ref'd copy of a capture pointer array: the same capture objects,
 * a second set of references.  Used where a live stack entry's
 * captures are handed to the memo table (the entry keeps using them;
 * the table stores them; both hold references).
 */
static memo_capture **capt_copy_ref(memo_capture **capt, size_t n)
{
	if (n == 0)
		return NULL;
	memo_capture **c = xmalloc(n * sizeof(*c));
	for (size_t i = 0; i < n; i++) {
		c[i] = capt[i];
		memo_capture_ref(capt[i]);
	}
	return c;
}

/*
 * The memoize helper: record a parse result
 * with its examined extent, which is the furthest byte read since the
 * entry was pushed minus the entry's position, plus one.  Takes full
 * ownership of capt (array + one reference per element) on every
 * path: the table stores it, or it is freed — windowed passes and
 * table-less runs never store captures.
 */
static void memoize(memo_table *tbl, vm_input *src, bool windowed,
                    int id, int pos, int mlen, int count,
                    memo_capture **capt, size_t ncapt)
{
	if (tbl == NULL || windowed) {
		for (size_t i = 0; i < ncapt; i++)
			memo_capture_free(capt[i]);
		free(capt);
		return;
	}
	int f = src->furthest;
	int p = vm_input_pos(src);
	int mexam = (f > p ? f : p) - pos + 1;
	memo_table_put(tbl, id, pos, mlen, mexam, count, capt, ncapt);
}

vm_result vm_exec(const vm_code *code, const uint8_t *input, size_t inputlen,
                  memo_table *memtbl, int window_low, int window_high)
{
	vm_result res = { 0 };
	bool windowed = window_low >= 0;

	if (windowed && memtbl == NULL)
		abort_msg("windowed execution requires a memo table");

	if (code->prog == NULL || code->nprog == 0) {
		res.success = true;
		res.pos = 0;
		res.captures = memo_capture_dummy(0, 0);
		return res;
	}

	vm_stack st = { 0 };
	vm_input *src = vm_input_new(input, inputlen);

	int caprange_low = 0, caprange_high = 0;

	if (windowed && window_high > window_low) {
		/*
		 * Apply an edit that clears all memoized entries in the
		 * window so that every capture inside it is regenerated
		 * (paper Section 4.4).  An empty window (a capture-less
		 * memo-filling pass) is skipped: the edit would be a
		 * no-op, but recording it stamps a shift that every later
		 * memo access pays to examine.
		 */
		memo_table_apply_edit(memtbl,
			(memo_edit){ .start = window_low, .end = window_high,
			             .len = window_high - window_low });
	}

	const vm_insn *prog = code->prog;
	size_t ip = 0;
	bool success = true;
	bool failed = false;
	errlist errs = { 0 };

loop:
	for (;;) {
		const vm_insn *in = &prog[ip];
		switch (in->op) {
		case VM_CHAR: {
			uint8_t b;
			if (input_peek(src, &b) && b == (uint8_t)in->a) {
				input_advance(src, 1);
				ip++;
			} else {
				goto fail;
			}
			break;
		}
		case VM_JUMP:
			ip = (size_t)in->a;
			break;
		case VM_CHOICE:
			push_btrack(&st, in->a, vm_input_pos(src));
			ip++;
			break;
		case VM_CALL:
			push_ret(&st, (int32_t)(ip + 1));
			ip = (size_t)in->a;
			break;
		case VM_COMMIT: {
			stack_entry ent;
			if (!stack_pop(&st, true, &ent))
				abort_msg("Commit failed");
			entry_clear_capt(&ent);
			free(ent.capt);
			ip = (size_t)in->a;
			break;
		}
		case VM_RETURN: {
			stack_entry ent;
			if (!stack_pop(&st, true, &ent) || ent.stype != stRet)
				abort_msg("Return failed");
			entry_clear_capt(&ent);
			free(ent.capt);
			ip = (size_t)ent.ret;
			break;
		}
		case VM_FAIL:
			goto fail;
		case VM_SET: {
			uint8_t b;
			if (input_peek(src, &b) &&
			    vm_charset_has(in->set, b)) {
				input_advance(src, 1);
				ip++;
			} else {
				goto fail;
			}
			break;
		}
		case VM_ANY:
			if (input_advance(src, in->a))
				ip++;
			else
				goto fail;
			break;
		case VM_PARTIAL_COMMIT: {
			stack_entry *ent = st.n ? &st.entries[st.n - 1] : NULL;
			if (ent == NULL || ent->stype != stBtrack)
				abort_msg("PartialCommit failed");
			ent->btrack_off = vm_input_pos(src);
			stack_prop_capt(&st);
			ip = (size_t)in->a;
			break;
		}
		case VM_SPAN:
			input_span(src, in->set);
			ip++;
			break;
		case VM_BACK_COMMIT: {
			stack_entry ent;
			if (!stack_pop(&st, true, &ent) ||
			    ent.stype != stBtrack)
				abort_msg("BackCommit failed");
			input_seek_to(src, ent.btrack_off);
			entry_clear_capt(&ent);
			free(ent.capt);
			ip = (size_t)in->a;
			break;
		}
		case VM_FAIL_TWICE: {
			stack_entry ent;
			/* Pop may find an empty stack here (a FailTwice
			 * can be reached with the choice already consumed
			 * by an earlier failure); the following fail
			 * handles it. */
			if (stack_pop(&st, false, &ent)) {
				entry_clear_capt(&ent);
				free(ent.capt);
			}
			goto fail;
		}
		case VM_EMPTY: {
			int pos = vm_input_pos(src);
			int r1 = pos > 0 ? input[pos - 1] : -1;
			int r2 = (size_t)pos < inputlen ? input[pos] : -1;
			if ((empty_op_context(r1, r2) & in->a) != 0)
				ip++;
			else
				goto fail;
			break;
		}
		case VM_TEST_CHAR: {
			uint8_t b;
			if (input_peek(src, &b) && b == (uint8_t)in->b) {
				push_btrack(&st, in->a, vm_input_pos(src));
				input_advance(src, 1);
				ip++;
			} else {
				ip = (size_t)in->a;
			}
			break;
		}
		case VM_TEST_CHAR_NOCHOICE: {
			uint8_t b;
			if (input_peek(src, &b) && b == (uint8_t)in->b) {
				input_advance(src, 1);
				ip++;
			} else {
				ip = (size_t)in->a;
			}
			break;
		}
		case VM_TEST_SET: {
			uint8_t b;
			if (input_peek(src, &b) &&
			    vm_charset_has(in->set, b)) {
				push_btrack(&st, in->a, vm_input_pos(src));
				input_advance(src, 1);
				ip++;
			} else {
				ip = (size_t)in->a;
			}
			break;
		}
		case VM_TEST_SET_NOCHOICE: {
			uint8_t b;
			if (input_peek(src, &b) &&
			    vm_charset_has(in->set, b)) {
				input_advance(src, 1);
				ip++;
			} else {
				ip = (size_t)in->a;
			}
			break;
		}
		case VM_TEST_ANY: {
			int off = vm_input_pos(src);
			if (input_advance(src, in->b)) {
				push_btrack(&st, in->a, off);
				ip++;
			} else {
				ip = (size_t)in->a;
			}
			break;
		}
		case VM_CAPTURE_BEGIN:
			push_capt(&st, (int16_t)in->a, vm_input_pos(src));
			ip++;
			break;
		case VM_CAPTURE_LATE:
			push_capt(&st, (int16_t)in->a,
			          vm_input_pos(src) - in->b);
			ip++;
			break;
		case VM_CAPTURE_FULL: {
			int back = in->b;
			int pos = vm_input_pos(src);
			if (!windowed || overlaps(window_low, window_high,
			                          pos - back, pos)) {
				if (windowed) {
					caprange_low = imin(caprange_low,
					                   pos - back);
					caprange_high = imax(caprange_high,
					                     pos);
				}
				memo_capture *capt = memo_capture_node(
					in->a, pos - back, back);
				stack_add_capt(&st, &capt, 1);
				memo_capture_free(capt); /* add took a ref */
			}
			ip++;
			break;
		}
		case VM_CAPTURE_END: {
			stack_entry ent;
			if (!stack_pop(&st, false, &ent) ||
			    ent.stype != stCapt)
				abort_msg("CaptureEnd found no capture entry");
			int end = vm_input_pos(src);
			if (!windowed || overlaps(window_low, window_high,
			                          ent.pos, end)) {
				if (windowed) {
					caprange_low = imin(caprange_low,
					                   ent.pos);
					caprange_high = imax(caprange_high,
					                     end);
				}
				memo_capture *capt = memo_capture_node(
					ent.id, ent.pos, end - ent.pos);
				/* The children move from the entry's list
				 * into the node (add takes its own ref per
				 * child, so the entry's refs are dropped
				 * after). */
				for (size_t i = 0; i < ent.ncapt; i++)
					memo_capture_add(capt, ent.capt[i]);
				entry_clear_capt(&ent);
				free(ent.capt);
				stack_add_capt(&st, &capt, 1);
				memo_capture_free(capt);
			} else {
				entry_clear_capt(&ent);
				free(ent.capt);
			}
			ip++;
			break;
		}
		case VM_END:
			success = in->a != 1;
			goto done;
		case VM_MEMO_OPEN: {
			memo_entry *ment = memtbl != NULL
				? memo_table_get(memtbl, in->b,
				                 vm_input_pos(src))
				: NULL;
			if (ment != NULL) {
				if (memo_entry_length(ment) == -1)
					goto fail;
				size_t ncap;
				memo_capture **capt =
					memo_entry_captures(ment, &ncap);
				for (size_t i = 0; i < ncap; i++)
					stack_add_capt(&st, &capt[i], 1);
				input_advance(src, memo_entry_length(ment));
				ip = (size_t)in->a;
			} else {
				push_memo(&st, (int16_t)in->b,
				          vm_input_pos(src));
				ip++;
			}
			break;
		}
		case VM_MEMO_CLOSE: {
			stack_entry ent;
			if (!stack_pop(&st, true, &ent) ||
			    ent.stype != stMemo)
				abort_msg("memo close failed");
			int mlen = vm_input_pos(src) - ent.pos;
			/* Ownership of the capture array and its
			 * references transfers to the table (or is
			 * freed) inside memoize. */
			memoize(memtbl, src, windowed, ent.id, ent.pos,
			        mlen, 1, ent.capt, ent.ncapt);
			ip++;
			break;
		}
		case VM_MEMO_TREE_OPEN: {
			memo_entry *ment = memtbl != NULL
				? memo_table_get(memtbl, in->b,
				                 vm_input_pos(src))
				: NULL;
			if (ment != NULL) {
				if (memo_entry_length(ment) == -1)
					goto fail;
				push_memotree(&st, (int16_t)in->b,
				              vm_input_pos(src),
				              memo_entry_count(ment));
				size_t ncap;
				memo_capture **capt =
					memo_entry_captures(ment, &ncap);
				for (size_t i = 0; i < ncap; i++)
					stack_add_capt(&st, &capt[i], 1);
				input_advance(src, memo_entry_length(ment));
				uint8_t b;
				input_peek(src, &b); /* track furthest */
				ip = (size_t)in->a;
			} else {
				push_memotree(&st, (int16_t)in->b,
				              vm_input_pos(src), 0);
				ip++;
			}
			break;
		}
		case VM_MEMO_TREE_CLOSE: {
			int16_t id = (int16_t)in->a;
			for (;;) {
				stack_entry *p = st.n ? &st.entries[st.n - 1]
				                      : NULL;
				if (p == NULL || p->stype != stMemoTree ||
				    p->id != id)
					break;
				stack_entry ent;
				if (!stack_pop(&st, true, &ent))
					abort_msg("MemoTreeClose failed");
				entry_clear_capt(&ent);
				free(ent.capt);
			}
			ip++;
			break;
		}
		case VM_MEMO_TREE_INSERT: {
			stack_entry *ent = st.n ? &st.entries[st.n - 1] : NULL;
			if (ent == NULL || ent->stype != stMemoTree)
				abort_msg("no memo entry on stack");
			int mlen = vm_input_pos(src) - ent->pos;
			ent->count++;
			/* The entry survives and keeps its captures, so
			 * the table receives a ref'd copy of the list. */
			memoize(memtbl, src, windowed, ent->id, ent->pos,
			        mlen, ent->count,
			        capt_copy_ref(ent->capt, ent->ncapt),
			        ent->ncapt);
			ip++;
			break;
		}
		case VM_MEMO_TREE: {
			int seen = 0;
			int accum = 0;
			for (;;) {
				stack_entry *top = st.n > (size_t)seen
					? &st.entries[st.n - 1 - seen] : NULL;
				stack_entry *next = st.n > (size_t)(seen + 1)
					? &st.entries[st.n - 2 - seen] : NULL;
				if (top == NULL || next == NULL ||
				    top->stype != stMemoTree ||
				    next->stype != stMemoTree)
					break;

				seen++;
				accum += top->count;

				if (accum < next->count)
					continue;

				for (int i = 0; i < seen - 1; i++) {
					stack_entry ent;
					if (!stack_pop(&st, true, &ent))
						abort_msg("MemoTree merge underflow");
					entry_clear_capt(&ent);
					free(ent.capt);
				}
				/* next is now the top of the stack; ent is
				 * the last merged entry above it. */
				stack_entry ent;
				if (!stack_pop(&st, false, &ent))
					abort_msg("MemoTree merge failed");

				if (ent.ncapt > 0 && !windowed) {
					/* Wrap the merged captures in a
					 * dummy so the parent memoizes
					 * them as one subtree. */
					memo_capture *dummy =
						memo_capture_dummy(
							ent.pos,
							vm_input_pos(src) -
								ent.pos);
					for (size_t i = 0; i < ent.ncapt; i++)
						memo_capture_add(dummy,
							ent.capt[i]);
					entry_clear_capt(&ent);
					free(ent.capt);
					stack_add_capt(&st, &dummy, 1);
					memo_capture_free(dummy);
				} else if (ent.ncapt > 0) {
					/* Windowed: the captures are added
					 * flat (no dummy subtree). */
					stack_add_capt(&st, ent.capt,
					               ent.ncapt);
					entry_clear_capt(&ent);
					free(ent.capt);
				} else {
					free(ent.capt);
				}

				stack_entry *nextent = &st.entries[st.n - 1];
				nextent->count = accum + nextent->count;
				int mlen = vm_input_pos(src) - nextent->pos;
				memoize(memtbl, src, windowed, nextent->id,
				        nextent->pos, mlen, nextent->count,
				        capt_copy_ref(nextent->capt,
				                      nextent->ncapt),
				        nextent->ncapt);

				accum = 0;
				seen = 0;
			}

			ip++;
			break;
		}
		case VM_CHECK_BEGIN:
			push_check(&st, (int16_t)in->a, vm_input_pos(src),
			           in->b);
			ip++;
			break;
		case VM_CHECK_END: {
			stack_entry ent;
			if (!stack_pop(&st, true, &ent) ||
			    ent.stype != stCheck)
				abort_msg("check end needs check stack entry");
			if ((size_t)in->a >= code->ncheckers ||
			    code->checkers[in->a] == NULL) {
				/* Unregistered checkers accept (NULL fn). */
				entry_clear_capt(&ent);
				free(ent.capt);
				ip++;
				break;
			}
			int start = ent.pos;
			int end = vm_input_pos(src);
			int n = code->checkers[in->a](
				input + start, (size_t)(end - start),
				input, inputlen, ent.id, ent.count,
				code->checker_uds[in->a]);
			entry_clear_capt(&ent);
			free(ent.capt);
			if (n < 0)
				goto fail;
			/* input_advance() reports an over-advance by returning
			 * false, but it has already moved the chunk past the
			 * end by then, and later instructions (Empty reads
			 * input[pos - 1] and input[pos]) index the raw buffer
			 * unchecked.  A checker is external code, so its
			 * return value is not trusted: reject the match here,
			 * before any state moves. */
			if ((size_t)vm_input_pos(src) + (size_t)n > inputlen)
				goto fail;
			input_advance(src, n);
			ip++;
			break;
		}
		case VM_ERROR: {
			const char *msg = (size_t)in->a < code->nerrors
				? code->errors[in->a] : "";
			errlist_add(&errs, vm_input_pos(src), msg);
			ip++;
			break;
		}
		default:
			abort_msg("invalid opcode");
		}
	}

fail:
	/*
	 * Failure unwinding: pop until a backtrack point, dropping the
	 * captures of everything popped (the abandoned branch produced
	 * them).  stMemo entries memoize their failure first.  stMemoTree
	 * entries simply die: reaching the fail handler from inside a
	 * tree-memoized body is unreachable (every MemoTreeOpen is
	 * immediately shielded by the following Choice; failing here is
	 * the safe equivalent).
	 */
	for (;;) {
		if (st.n == 0) {
			failed = true;
			goto done;
		}
		stack_entry ent;
		stack_pop(&st, false, &ent);
		switch (ent.stype) {
		case stBtrack:
			ip = (size_t)ent.btrack_ip;
			input_seek_to(src, ent.btrack_off);
			entry_clear_capt(&ent);
			free(ent.capt);
			goto loop;
		case stMemo:
			memoize(memtbl, src, windowed, ent.id, ent.pos, -1,
			        0, ent.capt, ent.ncapt);
			continue;
		default:
			entry_clear_capt(&ent);
			free(ent.capt);
			continue;
		}
	}

done:
	if (failed) {
		res.success = false;
		res.pos = vm_input_pos(src);
		res.captures = NULL;
	} else {
		res.success = success;
		res.pos = vm_input_pos(src);
		if (windowed)
			res.captures = memo_capture_dummy(
				caprange_low, caprange_high - caprange_low);
		else
			res.captures = memo_capture_dummy(0, res.pos);
		for (size_t i = 0; i < st.ncapt; i++)
			memo_capture_add(res.captures, st.capt[i]);
	}

	stack_destroy(&st);
	vm_input_free(src);

	res.errors = errs.v;
	res.nerrors = errs.n;
	return res;
}

void vm_result_free(vm_result *r)
{
	if (r == NULL)
		return;
	memo_capture_free(r->captures);
	for (size_t i = 0; i < r->nerrors; i++)
		free(r->errors[i].message);
	free(r->errors);
	r->captures = NULL;
	r->errors = NULL;
	r->nerrors = 0;
}
