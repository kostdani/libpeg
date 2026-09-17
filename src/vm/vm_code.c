/*
 * vm_code.c - program building, encoding, and pre-decoding.
 *
 * Program builder, bytecode encoder, and pre-decoder.  The encoded
 * byte format: opcode byte, then a padding byte when the
 * argument length is even (so 16-bit arguments land on even offsets),
 * then the arguments.  Jump targets are 24-bit little-endian byte
 * offsets into the encoded stream.
 *
 * Pre-decoding turns that byte stream into an array of fixed-size
 * vm_insn structs with jump targets already translated to instruction
 * indices, so the interpreter loop does no operand decoding at runtime.
 */
#include <stdlib.h>
#include <string.h>

#include "peg/peg_util.h"
#include "peg/peg_vm.h"
#include "vm_internal.h"

/* Encoded byte size of each opcode (padding included). */
static const uint8_t op_sizes[VM_NUM_OPS] = {
	[VM_CHAR] = 2,			[VM_JUMP] = 4,
	[VM_CHOICE] = 4,		[VM_CALL] = 4,
	[VM_COMMIT] = 4,		[VM_RETURN] = 2,
	[VM_FAIL] = 2,			[VM_SET] = 2,
	[VM_ANY] = 2,			[VM_PARTIAL_COMMIT] = 4,
	[VM_SPAN] = 2,			[VM_BACK_COMMIT] = 4,
	[VM_FAIL_TWICE] = 2,		[VM_EMPTY] = 2,
	[VM_TEST_CHAR] = 6,		[VM_TEST_CHAR_NOCHOICE] = 6,
	[VM_TEST_SET] = 6,		[VM_TEST_SET_NOCHOICE] = 6,
	[VM_TEST_ANY] = 6,		[VM_END] = 2,
	[VM_NOP] = 2,			/* never emitted */
	[VM_CAPTURE_BEGIN] = 4,		[VM_CAPTURE_LATE] = 4,
	[VM_CAPTURE_END] = 2,		[VM_CAPTURE_FULL] = 4,
	[VM_CHECK_BEGIN] = 6,		[VM_CHECK_END] = 4,
	[VM_MEMO_OPEN] = 6,		[VM_MEMO_CLOSE] = 2,
	[VM_MEMO_TREE_OPEN] = 6,	[VM_MEMO_TREE_INSERT] = 2,
	[VM_MEMO_TREE] = 2,		[VM_MEMO_TREE_CLOSE] = 4,
	[VM_ERROR] = 4,
};

/* ---------------------------------------------------------------------- */
/* Program builder                                                        */
/* ---------------------------------------------------------------------- */

struct vm_prog {
	uint8_t *code;
	size_t len;
	size_t cap;

	vm_charset *sets;
	size_t nsets;
	size_t setcap;

	char **errors;
	size_t nerrors;
	size_t errorcap;

	/* Label byte offsets.  A label's position is where the cursor was
	 * when it was created (vm_prog_label) or last marked
	 * (vm_prog_mark); because grammars jump forward as often as
	 * backward, label operands cannot be written at emit time —
	 * they are backpatched by vm_prog_finish. */
	size_t *labels;
	size_t nlabels;
	size_t labelcap;

	/* Pending label operands to backpatch: the byte offset of the
	 * 3-byte operand and its label id. */
	struct {
		size_t at;
		int label;
	} *fixups;
	size_t nfixups;
	size_t fixupcap;

	/* CheckEnd operands are assigned in emission order; the actual
	 * checker functions are registered on the finished program in
	 * the same order (vm_code_add_checker). */
	size_t ncheckers;
};

vm_prog *vm_prog_new(void)
{
	return xcalloc(1, sizeof(vm_prog));
}

void vm_prog_free(vm_prog *p)
{
	if (p == NULL)
		return;
	free(p->code);
	free(p->sets);
	for (size_t i = 0; i < p->nerrors; i++)
		free(p->errors[i]);
	free(p->errors);
	free(p->labels);
	/* Pending label operands.  vm_prog_finish clears these, but a
	 * builder abandoned before finishing still owns them. */
	free(p->fixups);
	free(p);
}

int vm_prog_label(vm_prog *p)
{
	if (p->nlabels == p->labelcap) {
		p->labelcap = p->labelcap ? p->labelcap * 2 : 16;
		p->labels = xrealloc(p->labels,
		                     p->labelcap * sizeof(p->labels[0]));
	}
	/* Default position: the next byte to be emitted (a label used
	 * only backward needs no mark). */
	p->labels[p->nlabels] = p->len;
	return (int)p->nlabels++;
}

void vm_prog_mark(vm_prog *p, int label)
{
	p->labels[label] = p->len;
}

/*
 * Record a label operand for backpatching (labels are resolved all
 * labels in a pre-pass; here operands are patched at finish so forward
 * references work).
 */
static void add_fixup(vm_prog *p, size_t at, int label)
{
	if (p->nfixups == p->fixupcap) {
		p->fixupcap = p->fixupcap ? p->fixupcap * 2 : 16;
		p->fixups = xrealloc(p->fixups,
		                     p->fixupcap * sizeof(p->fixups[0]));
	}
	p->fixups[p->nfixups].at = at;
	p->fixups[p->nfixups].label = label;
	p->nfixups++;
}

size_t vm_prog_here(vm_prog *p)
{
	return p->len;
}

/* Ensure room for n more bytes; returns a pointer to the write cursor. */
static uint8_t *prog_reserve(vm_prog *p, size_t n)
{
	if (p->len + n > p->cap) {
		p->cap = p->cap ? p->cap * 2 : 256;
		while (p->len + n > p->cap)
			p->cap *= 2;
		p->code = xrealloc(p->code, p->cap);
	}
	uint8_t *at = p->code + p->len;
	p->len += n;
	return at;
}

/* Deduplicate and store a charset; returns its table index. */
static uint8_t add_set(vm_prog *p, const vm_charset *set)
{
	for (size_t i = 0; i < p->nsets; i++) {
		if (vm_charset_eq(&p->sets[i], set))
			return (uint8_t)i;
	}
	if (p->nsets >= 256)
		abort_msg("charset table overflow (max 256)");
	if (p->nsets == p->setcap) {
		p->setcap = p->setcap ? p->setcap * 2 : 16;
		p->sets = xrealloc(p->sets, p->setcap * sizeof(p->sets[0]));
	}
	p->sets[p->nsets] = *set;
	return (uint8_t)p->nsets++;
}

/* Deduplicate and store an error message; returns its table index. */
static uint32_t add_error(vm_prog *p, const char *msg)
{
	for (size_t i = 0; i < p->nerrors; i++) {
		if (strcmp(p->errors[i], msg) == 0)
			return (uint32_t)i;
	}
	if (p->nerrors == p->errorcap) {
		p->errorcap = p->errorcap ? p->errorcap * 2 : 8;
		p->errors = xrealloc(p->errors,
		                     p->errorcap * sizeof(p->errors[0]));
	}
	size_t len = strlen(msg) + 1;
	p->errors[p->nerrors] = xmalloc(len);
	memcpy(p->errors[p->nerrors], msg, len);
	return (uint32_t)p->nerrors++;
}

/*
 * The raw emit helpers.  Each writes: opcode, optional padding byte
 * (when the argument length is even, which keeps 16-bit operands
 * aligned), then arguments.  Operand order matches the
 * encoder exactly.
 */
static size_t emit1(vm_prog *p, uint8_t op)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 2);
	w[0] = op;
	w[1] = 0;			/* pad: 0 args, even */
	return at;
}

static size_t emit_op_u8(vm_prog *p, uint8_t op, uint8_t a)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 2);	/* 1 arg, odd: no pad */
	w[0] = op;
	w[1] = a;
	return at;
}

static size_t emit_op_label(vm_prog *p, uint8_t op, int label)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 4);	/* 3 args, odd: no pad */
	w[0] = op;
	/* Placeholder; backpatched by vm_prog_finish (forward refs). */
	add_fixup(p, at + 1, label);
	return at;
}

static size_t emit_op_i16(vm_prog *p, uint8_t op, int16_t a)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 4);	/* 2 args, even: pad */
	w[0] = op;
	w[1] = 0;
	w[2] = (uint8_t)(a & 0xff);
	w[3] = (uint8_t)((uint16_t)a >> 8);
	return at;
}

static size_t emit_op_label_i16(vm_prog *p, uint8_t op, int label, int16_t b)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 6);	/* 5 args, odd: no pad */
	w[0] = op;
	add_fixup(p, at + 1, label);
	w[4] = (uint8_t)(b & 0xff);
	w[5] = (uint8_t)((uint16_t)b >> 8);
	return at;
}

static size_t emit_op_set_label(vm_prog *p, uint8_t op, const vm_charset *set,
                                int label)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 6);	/* 4 args, even: pad */
	w[0] = op;
	w[1] = 0;
	w[2] = add_set(p, set);
	add_fixup(p, at + 3, label);
	return at;
}

static size_t emit_op_set(vm_prog *p, uint8_t op, const vm_charset *set)
{
	return emit_op_u8(p, op, add_set(p, set));
}

static size_t emit_op_u24(vm_prog *p, uint8_t op, uint32_t a)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 4);	/* 3 args, odd: no pad */
	w[0] = op;
	w[1] = (a >> 16) & 0xff;
	w[2] = a & 0xff;
	w[3] = (a >> 8) & 0xff;
	return at;
}

static size_t emit_op_i16_i16(vm_prog *p, uint8_t op, int16_t a, int16_t b)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 6);	/* 4 args, even: pad */
	w[0] = op;
	w[1] = 0;
	w[2] = (uint8_t)(a & 0xff);
	w[3] = (uint8_t)((uint16_t)a >> 8);
	w[4] = (uint8_t)(b & 0xff);
	w[5] = (uint8_t)((uint16_t)b >> 8);
	return at;
}

static size_t emit_op_u8_i16(vm_prog *p, uint8_t op, uint8_t b, int16_t a)
{
	/* args: Back byte, then the id as I16
	 * = 3 argument bytes, no pad. */
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 4);
	w[0] = op;
	w[1] = b;
	w[2] = (uint8_t)(a & 0xff);
	w[3] = (uint8_t)((uint16_t)a >> 8);
	return at;
}

size_t vm_emit_char(vm_prog *p, uint8_t c)
{
	return emit_op_u8(p, VM_CHAR, c);
}

size_t vm_emit_jump(vm_prog *p, int label)
{
	return emit_op_label(p, VM_JUMP, label);
}

size_t vm_emit_choice(vm_prog *p, int label)
{
	return emit_op_label(p, VM_CHOICE, label);
}

size_t vm_emit_call(vm_prog *p, int label)
{
	return emit_op_label(p, VM_CALL, label);
}

size_t vm_emit_commit(vm_prog *p, int label)
{
	return emit_op_label(p, VM_COMMIT, label);
}

size_t vm_emit_return(vm_prog *p)
{
	return emit1(p, VM_RETURN);
}

size_t vm_emit_fail(vm_prog *p)
{
	return emit1(p, VM_FAIL);
}

size_t vm_emit_set(vm_prog *p, const vm_charset *set)
{
	return emit_op_set(p, VM_SET, set);
}

size_t vm_emit_any(vm_prog *p, uint8_t n)
{
	return emit_op_u8(p, VM_ANY, n);
}

size_t vm_emit_partial_commit(vm_prog *p, int label)
{
	return emit_op_label(p, VM_PARTIAL_COMMIT, label);
}

size_t vm_emit_span(vm_prog *p, const vm_charset *set)
{
	return emit_op_set(p, VM_SPAN, set);
}

size_t vm_emit_back_commit(vm_prog *p, int label)
{
	return emit_op_label(p, VM_BACK_COMMIT, label);
}

size_t vm_emit_fail_twice(vm_prog *p)
{
	return emit1(p, VM_FAIL_TWICE);
}

size_t vm_emit_empty(vm_prog *p, uint8_t op)
{
	return emit_op_u8(p, VM_EMPTY, op);
}

size_t vm_emit_test_char(vm_prog *p, uint8_t c, int label)
{
	/* args: Byte, then the label as U24 = 4 bytes total.
	 * bytes, even: pad. */
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 6);
	w[0] = VM_TEST_CHAR;
	w[1] = 0;			/* pad */
	w[2] = c;
	add_fixup(p, at + 3, label);
	return at;
}

size_t vm_emit_test_char_nochoice(vm_prog *p, uint8_t c, int label)
{
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 6);
	w[0] = VM_TEST_CHAR_NOCHOICE;
	w[1] = 0;
	w[2] = c;
	add_fixup(p, at + 3, label);
	return at;
}

size_t vm_emit_test_set(vm_prog *p, const vm_charset *set, int label)
{
	return emit_op_set_label(p, VM_TEST_SET, set, label);
}

size_t vm_emit_test_set_nochoice(vm_prog *p, const vm_charset *set, int label)
{
	return emit_op_set_label(p, VM_TEST_SET_NOCHOICE, set, label);
}

size_t vm_emit_test_any(vm_prog *p, uint8_t n, int label)
{
	/* args: N, then the label as U24 = 4 bytes total,
	 * even: pad. */
	size_t at = p->len;
	uint8_t *w = prog_reserve(p, 6);
	w[0] = VM_TEST_ANY;
	w[1] = 0;
	w[2] = n;
	add_fixup(p, at + 3, label);
	return at;
}

size_t vm_emit_capture_begin(vm_prog *p, int16_t id)
{
	return emit_op_i16(p, VM_CAPTURE_BEGIN, id);
}

size_t vm_emit_capture_late(vm_prog *p, uint8_t back, int16_t id)
{
	return emit_op_u8_i16(p, VM_CAPTURE_LATE, back, id);
}

size_t vm_emit_capture_end(vm_prog *p)
{
	return emit1(p, VM_CAPTURE_END);
}

size_t vm_emit_capture_full(vm_prog *p, uint8_t back, int16_t id)
{
	return emit_op_u8_i16(p, VM_CAPTURE_FULL, back, id);
}

size_t vm_emit_memo_open(vm_prog *p, int label, int16_t id)
{
	return emit_op_label_i16(p, VM_MEMO_OPEN, label, id);
}

size_t vm_emit_memo_close(vm_prog *p)
{
	return emit1(p, VM_MEMO_CLOSE);
}

size_t vm_emit_memo_tree_open(vm_prog *p, int label, int16_t id)
{
	return emit_op_label_i16(p, VM_MEMO_TREE_OPEN, label, id);
}

size_t vm_emit_memo_tree_insert(vm_prog *p)
{
	return emit1(p, VM_MEMO_TREE_INSERT);
}

size_t vm_emit_memo_tree(vm_prog *p)
{
	return emit1(p, VM_MEMO_TREE);
}

size_t vm_emit_memo_tree_close(vm_prog *p, int16_t id)
{
	return emit_op_i16(p, VM_MEMO_TREE_CLOSE, id);
}

size_t vm_emit_error(vm_prog *p, const char *msg)
{
	return emit_op_u24(p, VM_ERROR, add_error(p, msg));
}

size_t vm_emit_check_begin(vm_prog *p, int16_t id, int16_t flag)
{
	return emit_op_i16_i16(p, VM_CHECK_BEGIN, id, flag);
}

size_t vm_emit_check_end(vm_prog *p)
{
	/*
	 * The operand is the index this CheckEnd's checker will be
	 * registered under (via vm_code_add_checker, in emission order).
	 */
	return emit_op_u24(p, VM_CHECK_END, (uint32_t)p->ncheckers++);
}

size_t vm_emit_end(vm_prog *p, bool fail)
{
	return emit_op_u8(p, VM_END, fail ? 1 : 0);
}

/* Register a checker; returns its index for CheckEnd's operand. */
size_t vm_code_add_checker(vm_code *c, vm_checker_fn fn, void *ud)
{
	size_t n = c->ncheckers;
	c->checkers = xrealloc(c->checkers, (n + 1) * sizeof(c->checkers[0]));
	c->checker_uds = xrealloc(c->checker_uds,
	                          (n + 1) * sizeof(c->checker_uds[0]));
	c->checkers[n] = fn;
	c->checker_uds[n] = ud;
	c->ncheckers = n + 1;
	return n;
}

void vm_code_free(vm_code *c)
{
	if (c == NULL)
		return;
	free(c->insns);
	free(c->sets);
	for (size_t i = 0; i < c->nerrors; i++)
		free(c->errors[i]);
	free(c->errors);
	free(c->prog);
	free(c->checkers);
	free(c->checker_uds);
	free(c);
}

const uint8_t *vm_code_insns(const vm_code *c)
{
	return c->insns;
}

size_t vm_code_size(const vm_code *c)
{
	return c->len;
}

size_t vm_code_ninsn(const vm_code *c)
{
	return c->nprog;
}

/* ---------------------------------------------------------------------- */
/* Pre-decoding                                                           */
/* ---------------------------------------------------------------------- */

/* Decode helpers.  U24 is mixed-endian (bits 16-23 in b[0], bits
 * 0-15 little-endian in b[1:3]):
 * b[0] = bits 16-23, b[1:3] = bits 0-15 little-endian. */
static int32_t decode_u24(const uint8_t *b)
{
	return (int32_t)(((uint32_t)b[0] << 16) | (uint32_t)b[1] |
	                 ((uint32_t)b[2] << 8));
}

static int32_t decode_i16(const uint8_t *b)
{
	return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

/*
 * Pre-decode the encoded stream into the executable form: first pass
 * decodes operands and records the mapping from byte offsets to
 * instruction indices, second pass translates jump targets from byte
 * offsets to instruction indices.
 */
static void vm_predecode(vm_code *c)
{
	const uint8_t *insns = c->insns;
	size_t len = c->len;

	int32_t *idx = xmalloc((len + 1) * sizeof(*idx));
	vm_insn *prog = xmalloc((len / 2 + 1) * sizeof(*prog));
	size_t nprog = 0;

	size_t ip = 0;
	while (ip < len) {
		uint8_t op = insns[ip];
		idx[ip] = (int32_t)nprog;
		vm_insn in = { .op = op, .a = 0, .b = 0, .set = NULL };
		switch (op) {
		case VM_CHAR:
		case VM_ANY:
		case VM_EMPTY:
		case VM_END:
			in.a = insns[ip + 1];
			break;
		case VM_JUMP:
		case VM_CHOICE:
		case VM_CALL:
		case VM_COMMIT:
		case VM_PARTIAL_COMMIT:
		case VM_BACK_COMMIT:
		case VM_ERROR:
		case VM_CHECK_END:
			in.a = decode_u24(insns + ip + 1);
			break;
		case VM_SET:
		case VM_SPAN:
			in.set = &c->sets[insns[ip + 1]];
			break;
		case VM_TEST_CHAR:
		case VM_TEST_CHAR_NOCHOICE:
		case VM_TEST_ANY:
			in.b = insns[ip + 2];
			in.a = decode_u24(insns + ip + 3);
			break;
		case VM_TEST_SET:
		case VM_TEST_SET_NOCHOICE:
			in.set = &c->sets[insns[ip + 2]];
			in.a = decode_u24(insns + ip + 3);
			break;
		case VM_CAPTURE_BEGIN:
		case VM_MEMO_TREE_CLOSE:
			in.a = decode_i16(insns + ip + 2);
			break;
		case VM_CAPTURE_LATE:
		case VM_CAPTURE_FULL:
			in.b = insns[ip + 1];
			in.a = decode_i16(insns + ip + 2);
			break;
		case VM_CHECK_BEGIN:
			in.a = decode_i16(insns + ip + 2);
			in.b = decode_i16(insns + ip + 4);
			break;
		case VM_MEMO_OPEN:
		case VM_MEMO_TREE_OPEN:
			in.a = decode_u24(insns + ip + 1);
			in.b = decode_i16(insns + ip + 4);
			break;
		default:
			break;		/* no operands */
		}
		prog[nprog++] = in;
		ip += op_sizes[op];
	}
	idx[len] = (int32_t)nprog;

	/* Translate jump targets to instruction indices. */
	for (size_t i = 0; i < nprog; i++) {
		switch (prog[i].op) {
		case VM_JUMP:
		case VM_CHOICE:
		case VM_CALL:
		case VM_COMMIT:
		case VM_PARTIAL_COMMIT:
		case VM_BACK_COMMIT:
		case VM_TEST_CHAR:
		case VM_TEST_CHAR_NOCHOICE:
		case VM_TEST_SET:
		case VM_TEST_SET_NOCHOICE:
		case VM_TEST_ANY:
		case VM_MEMO_OPEN:
		case VM_MEMO_TREE_OPEN:
			prog[i].a = idx[prog[i].a];
			break;
		default:
			break;
		}
	}

	free(idx);
	c->prog = prog;
	c->nprog = nprog;
	c->predecoded = true;
}

vm_code *vm_prog_finish(vm_prog *p)
{
	/* Terminating End (success). */
	emit_op_u8(p, VM_END, 0);

	/* Backpatch every pending label operand recorded above. */
	for (size_t i = 0; i < p->nfixups; i++) {
		size_t off = p->labels[p->fixups[i].label];
		uint8_t *w = &p->code[p->fixups[i].at];
		/* Truncating here would silently encode a jump to an
		 * unrelated instruction, and the result still decodes and
		 * runs -- a miscompile, not a crash. */
		if (off > 0xffffff)
			abort_msg("program too large: "
			    "jump target exceeds 24 bits");
		w[0] = (off >> 16) & 0xff;	/* mixed-endian U24 */
		w[1] = off & 0xff;
		w[2] = (off >> 8) & 0xff;
	}
	free(p->fixups);
	p->fixups = NULL;
	p->nfixups = p->fixupcap = 0;

	vm_code *c = xmalloc(sizeof(*c));
	c->insns = p->code;
	c->len = p->len;
	c->sets = p->sets;
	c->nsets = p->nsets;
	c->errors = p->errors;
	c->nerrors = p->nerrors;
	c->prog = NULL;
	c->nprog = 0;
	c->predecoded = false;
	c->checkers = NULL;
	c->checker_uds = NULL;
	c->ncheckers = 0;

	/* The builder's arrays moved into the program. */
	p->code = NULL;
	p->len = p->cap = 0;
	p->sets = NULL;
	p->nsets = p->setcap = 0;
	p->errors = NULL;
	p->nerrors = p->errorcap = 0;

	vm_predecode(c);
	return c;
}
