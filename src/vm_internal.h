/*
 * vm_internal.h - types shared between the VM's translation units
 * (vm_code.c: builder/encoder/predecoder, vm.c: input/stack/interpreter).
 * Not part of the public interface; include vm.h for that.
 */
#ifndef PEG_VM_INTERNAL_H
#define PEG_VM_INTERNAL_H

#include "peg/vm.h"

/*
 * One pre-decoded instruction.  `a` is the primary operand: the jump
 * target (as an instruction index, after predecode's second pass) for
 * control-flow ops, otherwise the instruction's value (char, count, id,
 * error/checker index).  `b` is the secondary operand where one exists
 * (byte/count for the test instructions, back amount for captures).
 * `set` points into the program's charset table for set instructions.
 */
typedef struct {
	uint8_t op;
	int32_t a;
	int32_t b;
	const vm_charset *set;
} vm_insn;

/*
 * The compiled program: the encoded byte stream
 * plus the set/error/checker tables and the pre-decoded executable form.
 */
struct vm_code {
	/* Encoded (serialized) form. */
	uint8_t *insns;
	size_t len;

	vm_charset *sets;
	size_t nsets;
	char **errors;
	size_t nerrors;

	/* Pre-decoded executable form (built by vm_prog_finish). */
	vm_insn *prog;
	size_t nprog;
	bool predecoded;

	/* Checkers (indexed by CheckEnd's 24-bit operand). */
	vm_checker_fn *checkers;
	void **checker_uds;
	size_t ncheckers;
};

#endif /* PEG_VM_INTERNAL_H */
