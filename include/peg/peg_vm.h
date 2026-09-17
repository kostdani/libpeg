/*
 * peg_vm.h - the libpeg parsing machine.
 *
 * A grammar is compiled (by the compiler module) into a program of
 * instructions for a small virtual machine, in the style of LPeg's
 * parsing machine (Ierusalimschy, "A Parsing Machine for PEGs", DLS '08).
 * Executing the program parses the input; memoization instructions
 * consult and populate the memo table, and tree-memoization instructions
 * maintain the logarithmic skip structure described in "Fast Incremental
 * PEG Parsing" (Section 5.2).
 *
 * The parsing machine's public interface.  The instruction set, encoded byte
 * format (including the padding rule), and interpreter semantics are
 * is fixed so grammars compile to the
 * same bytecode.
 *
 * The machine is a register machine with two registers and a stack:
 *
 *   ip - instruction pointer (index into the pre-decoded program)
 *   sp - subject position (byte offset into the input)
 *
 * The stack holds heterogeneous entries: backtrack points, call return
 * addresses, capture and memo bookkeeping (see vm_stack_entry_kind).
 * Backtracking is driven by failure: instructions "goto fail", which
 * pops stack entries until a backtrack point is found, restoring its
 * (ip, sp) and discarding any captures the abandoned branch produced.
 *
 * Checkers
 * --------
 * A checker is user-supplied validation applied to a matched span after
 * the fact (e.g. accept [0-9]+ only if the integer fits in 8 bits).  The
 * A grammar can attach arbitrary
 * BackReference (define/use of named spans).  This port provides the
 * validation logic in function-pointer form
 * (the compiler front-end that emits CheckBegin/CheckEnd is not ported
 * yet either, see README).
 */
#ifndef PEG_VM_H
#define PEG_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "peg/peg_memo.h"

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
/* Opcodes                                                                */
/* ---------------------------------------------------------------------- */

/*
 * Opcodes and their encoded sizes are fixed
 * (byte-compatibility of the encoded program is a porting goal).
 *
 * Encoding: each instruction is one opcode byte, one padding byte if the
 * argument length is even (so 16-bit arguments land on an even offset —
 * see the padding rule below), then the arguments.
 * Jump targets are 24-bit little-endian byte offsets into the encoded
 * program, translated to pre-decoded instruction indices at load time.
 */
enum vm_op {
	VM_CHAR,		/* a: byte. Consume it or fail. */
	VM_JUMP,		/* a: target. ip = a. */
	VM_CHOICE,		/* a: target. Push backtrack (a, sp). */
	VM_CALL,		/* a: target. Push ret (ip+1); ip = a. */
	VM_COMMIT,		/* a: target. Pop; ip = a. */
	VM_RETURN,		/* Pop a ret entry; ip = its address. */
	VM_FAIL,		/* Goto fail. */
	VM_SET,			/* set. Consume if in set, else fail. */
	VM_ANY,			/* a: n. Consume n bytes or fail. */
	VM_PARTIAL_COMMIT,	/* a: target. Update top backtrack's sp; ip = a. */
	VM_SPAN,		/* set. Consume the run of bytes in set. */
	VM_BACK_COMMIT,		/* a: target. Pop backtrack; sp = its pos; ip = a. */
	VM_FAIL_TWICE,		/* Pop an entry; goto fail. */
	VM_EMPTY,		/* a: empty-op flags. Zero-width assertion. */
	VM_TEST_CHAR,		/* b: byte, a: target. Consume+push backtrack or jump. */
	VM_TEST_CHAR_NOCHOICE,	/* b: byte, a: target. Consume or jump. */
	VM_TEST_SET,		/* set, a: target. Consume+push backtrack or jump. */
	VM_TEST_SET_NOCHOICE,	/* set, a: target. Consume or jump. */
	VM_TEST_ANY,		/* b: n, a: target. Consume+push backtrack or jump. */
	VM_END,			/* a: fail flag. Finish; success = (a != 1). */
	VM_NOP,
	VM_CAPTURE_BEGIN,	/* a: id. Push capture entry (id, sp). */
	VM_CAPTURE_LATE,	/* b: back, a: id. Push capture entry at sp-b. */
	VM_CAPTURE_END,		/* Pop capture entry; build the capture node. */
	VM_CAPTURE_FULL,	/* b: back, a: id. Build a completed capture now. */
	VM_CHECK_BEGIN,		/* a: id, b: flag. Push check entry. */
	VM_CHECK_END,		/* a: checker index. Run the checker. */
	VM_MEMO_OPEN,		/* a: target, b: id. Memo lookup at sp. */
	VM_MEMO_CLOSE,		/* Pop memo entry; memoize the result. */
	VM_MEMO_TREE_OPEN,	/* a: target, b: id. Tree-memo lookup at sp. */
	VM_MEMO_TREE_INSERT,	/* Update the top tree entry; memoize. */
	VM_MEMO_TREE,		/* Merge stack tree entries bottom-up. */
	VM_MEMO_TREE_CLOSE,	/* a: id. Pop tree entries with this id. */
	VM_ERROR,		/* a: error index. Record a parse error at sp. */
	VM_NUM_OPS
};

/* ---------------------------------------------------------------------- */
/* Charset                                                                */
/* ---------------------------------------------------------------------- */

/*
 * A set of bytes, as a 256-bit bitmap.  Has() is on the interpreter's
 * hottest path, so it lives here in the header for inlining.
 */
typedef struct {
	uint64_t bits[4];
} vm_charset;

static inline bool vm_charset_has(const vm_charset *s, uint8_t c)
{
	return (s->bits[c >> 6] >> (c & 63)) & 1;
}

/* Set membership from bits (build helper used by the compiler). */
static inline void vm_charset_set(vm_charset *s, uint8_t c, bool on)
{
	uint64_t bit = (uint64_t)1 << (c & 63);
	if (on)
		s->bits[c >> 6] |= bit;
	else
		s->bits[c >> 6] &= ~bit;
}

/* Build a set from a byte list (n <= 256). */
static inline void vm_charset_fill(vm_charset *s, const uint8_t *chars,
                                   size_t n)
{
	for (size_t i = 0; i < 4; i++)
		s->bits[i] = 0;
	for (size_t i = 0; i < n; i++)
		vm_charset_set(s, chars[i], true);
}

/* Build a set accepting every byte in [lo, hi]. */
static inline void vm_charset_range(vm_charset *s, uint8_t lo, uint8_t hi)
{
	for (size_t i = 0; i < 4; i++)
		s->bits[i] = 0;
	for (unsigned c = lo; c <= hi; c++)
		vm_charset_set(s, (uint8_t)c, true);
}

/* Build the set of every byte NOT in another set. */
static inline void vm_charset_negate(const vm_charset *s, vm_charset *out)
{
	for (size_t i = 0; i < 4; i++)
		out->bits[i] = ~s->bits[i];
}

/* Build the difference a \ b. */
static inline void vm_charset_sub(const vm_charset *a, const vm_charset *b,
                                  vm_charset *out)
{
	for (size_t i = 0; i < 4; i++)
		out->bits[i] = a->bits[i] & ~b->bits[i];
}

/* The number of bytes in the set. */
static inline size_t vm_charset_count(const vm_charset *s)
{
	size_t n = 0;
	for (size_t i = 0; i < 4; i++) {
		uint64_t w = s->bits[i];
		while (w) {
			n += w & 1;
			w >>= 1;
		}
	}
	return n;
}

/* Build the union of two sets. */
static inline void vm_charset_union(const vm_charset *a, const vm_charset *b,
                                    vm_charset *out)
{
	for (size_t i = 0; i < 4; i++)
		out->bits[i] = a->bits[i] | b->bits[i];
}

/* True if the two sets are equal (used for set dedup in the encoder). */
static inline bool vm_charset_eq(const vm_charset *a, const vm_charset *b)
{
	for (size_t i = 0; i < 4; i++)
		if (a->bits[i] != b->bits[i])
			return false;
	return true;
}

/* ---------------------------------------------------------------------- */
/* Program building                                                       */
/* ---------------------------------------------------------------------- */

/*
 * A program under construction, and the finished compiled program.
 *
 * The builder (vm_prog) accumulates instructions and labels; labels are
 * symbolic during building and resolved to byte offsets by vm_prog_finish
 * builder.  The finished program (vm_code) holds the
 * encoded byte stream plus the set/error/checker tables, and lazily
 * pre-decodes into the executable form on first execution.
 */
typedef struct vm_prog vm_prog;
typedef struct vm_code vm_code;

/* Create an empty program builder. */
vm_prog *vm_prog_new(void);

/* Release the builder (finished programs are independent). */
void vm_prog_free(vm_prog *vm_prog);

/* Emit a new label and return its id (ids are dense and increasing).  A
 * label's position defaults to the offset where it was created; if
 * instructions are emitted before the label's target position, call
 * vm_prog_mark to set it (labels are backpatched at finish, so both
 * forward and backward references work). */
int vm_prog_label(vm_prog *p);

/* Set a label's position to the current offset (see vm_prog_label). */
void vm_prog_mark(vm_prog *p, int label);

/* Byte offset of the next emitted instruction (for manual jumps). */
size_t vm_prog_here(vm_prog *p);

/* Emit instructions.  Each returns the byte offset it was written at. */
size_t vm_emit_char(vm_prog *p, uint8_t c);
size_t vm_emit_jump(vm_prog *p, int label);
size_t vm_emit_choice(vm_prog *p, int label);
size_t vm_emit_call(vm_prog *p, int label);
size_t vm_emit_commit(vm_prog *p, int label);
size_t vm_emit_return(vm_prog *p);
size_t vm_emit_fail(vm_prog *p);
size_t vm_emit_set(vm_prog *p, const vm_charset *set);
size_t vm_emit_any(vm_prog *p, uint8_t n);
size_t vm_emit_partial_commit(vm_prog *p, int label);
size_t vm_emit_span(vm_prog *p, const vm_charset *set);
size_t vm_emit_back_commit(vm_prog *p, int label);
size_t vm_emit_fail_twice(vm_prog *p);
size_t vm_emit_empty(vm_prog *p, uint8_t op);
size_t vm_emit_test_char(vm_prog *p, uint8_t c, int label);
size_t vm_emit_test_char_nochoice(vm_prog *p, uint8_t c, int label);
size_t vm_emit_test_set(vm_prog *p, const vm_charset *set, int label);
size_t vm_emit_test_set_nochoice(vm_prog *p, const vm_charset *set, int label);
size_t vm_emit_test_any(vm_prog *p, uint8_t n, int label);
size_t vm_emit_capture_begin(vm_prog *p, int16_t id);
size_t vm_emit_capture_late(vm_prog *p, uint8_t back, int16_t id);
size_t vm_emit_capture_end(vm_prog *p);
size_t vm_emit_capture_full(vm_prog *p, uint8_t back, int16_t id);
size_t vm_emit_memo_open(vm_prog *p, int label, int16_t id);
size_t vm_emit_memo_close(vm_prog *p);
size_t vm_emit_memo_tree_open(vm_prog *p, int label, int16_t id);
size_t vm_emit_memo_tree_insert(vm_prog *p);
size_t vm_emit_memo_tree(vm_prog *p);
size_t vm_emit_memo_tree_close(vm_prog *p, int16_t id);
size_t vm_emit_error(vm_prog *p, const char *msg);
size_t vm_emit_check_begin(vm_prog *p, int16_t id, int16_t flag);

/*
 * Emit a CheckEnd instruction.  Returns the checker index it encodes:
 * the caller must register that many checkers, in emission order, on
 * the finished program (vm_code_add_checker), matching the indices the
 * CheckEnd instructions reference.
 */
size_t vm_emit_check_end(vm_prog *p);
size_t vm_emit_end(vm_prog *p, bool fail);

/*
 * Finish building: resolve labels, append the terminating End, and
 * pre-decode into an executable program.  The builder may then be freed.
 * The returned program is owned by the caller (vm_code_free).
 */
vm_code *vm_prog_finish(vm_prog *p);

/*
 * The compiled program: encoded bytecode plus set/error tables and the
 * pre-decoded executable form built on first execution (or immediately
 * by vm_prog_finish).
 */

/* Free a compiled program.  NULL is allowed. */
void vm_code_free(vm_code *c);

/* The encoded (serialized) bytecode and its length in bytes (the same
 * format; for serialization/tests). */
const uint8_t *vm_code_insns(const vm_code *c);
size_t vm_code_size(const vm_code *c);

/* The number of pre-decoded instructions (for tests). */
size_t vm_code_ninsn(const vm_code *c);

/* ---------------------------------------------------------------------- */
/* Input                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * The input wrapper: a 4KB-chunk cache over a flat byte buffer with a
 * furthest-read tracker, which is what memo entries record as their
 * examined extent.
 */
typedef struct vm_input vm_input;

/* Wrap a byte buffer (not copied; must outlive the input). */
vm_input *vm_input_new(const uint8_t *data, size_t len);

void vm_input_free(vm_input *i);

/* The current read position. */
int vm_input_pos(const vm_input *i);

/* The furthest position ever read (drives memo examined extents). */
int vm_input_furthest(const vm_input *i);

/* Reset the furthest tracker (used at parse start). */
void vm_input_reset_furthest(vm_input *i);

/* ---------------------------------------------------------------------- */
/* Checkers                                                               */
/* ---------------------------------------------------------------------- */

/*
 * A checker validates a matched span after the fact.  It receives the
 * matched bytes [start, end) of the subject, the whole subject, and the
 * id/flag from the CheckBegin instruction.  It returns the number of
 * additional bytes to consume (>= 0) on success, or -1 to fail the
 * match.  A NULL checker accepts anything.
 */
typedef int (*vm_checker_fn)(const uint8_t *match, size_t matchlen,
                             const uint8_t *subject, size_t subjectlen,
                             int id, int flag, void *ud);

/*
 * Register a checker with a compiled program and return its index (the
 * value CheckEnd's operand selects; checkers are registered before the
 * program that references them is built).  A NULL fn always accepts.
 */
size_t vm_code_add_checker(vm_code *c, vm_checker_fn fn, void *ud);

/* ---------------------------------------------------------------------- */
/* Execution                                                              */
/* ---------------------------------------------------------------------- */

/* A parse error recorded by the Error instruction. */
typedef struct {
	int pos;
	char *message;		/* owned by the result */
} vm_error;

/* The result of one execution. */
typedef struct {
	bool success;
	int pos;		/* final subject position */
	memo_capture *captures;	/* result tree (dummy root); NULL on failure */
	vm_error *errors;	/* array of vm_result.nerrors entries */
	size_t nerrors;
} vm_result;

/*
 * Execute the program against the input with the memo table.  The
 * memo table is read and written; on success the result tree's captures
 * are freshly created (the caller owns them via the result, see
 * vm_result_free).  memtbl may be NULL for a parse without memoization.
 *
 * With interval != NULL, only captures overlapping [low, high) are
 * constructed (the window optimization, paper Section 4.4): the machine
 * first invalidates memo entries in the window so that captures inside
 * it are regenerated, and memoized results skip capture storage (they
 * would be outside the window by construction).
 */
vm_result vm_exec(const vm_code *code, const uint8_t *input, size_t inputlen,
                  memo_table *memtbl, int window_low, int window_high);

/* Free a result: the capture tree, error strings, and error array. */
void vm_result_free(vm_result *r);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PEG_VM_H */
