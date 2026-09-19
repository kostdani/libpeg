/*
 * pattern.h - patterns and the grammar compiler (PEG -> VM program).
 *
 * A pattern is an AST node for a PEG expression.  Patterns are built with
 * the pat_* constructors, then compiled by pat_compile into a program for
 * the parsing machine (vm.h).  The compilation pipeline has three stages:
 *
 *   1. compile: each node expands to a sequence of typed instructions
 *      with symbolic labels (an instruction-list IR);
 *   2. optimize: head-fail (Choice followed by Char/Set/Any becomes
 *      TestChar/TestSet/TestAny) and jump replacement (a jump to another
 *      control-flow instruction becomes that instruction);
 *   3. encode: the IR is lowered through the streaming program builder
 *      (vm_emit_*).
 *
 * Grammars
 * --------
 * Recursive grammars are built with pat_grammar: a start name plus named
 * definitions referencing each other through pat_nonterm.  During
 * compilation, small leaf definitions are inlined (below InlineThreshold,
 * see pattern.c) and the remaining non-terminals compile to Call/Return
 * pairs; a call in tail position compiles to a Jump (tail-call
 * optimization).
 *
 * Grammar definitions are stored in an array and compiled in the
 * given order, so compilation is deterministic.
 *
 * Ownership
 * ---------
 * A pattern owns its sub-patterns; pat_free releases a whole tree.
 * Exceptions:
 *
 *   - a grammar owns its definitions (the defs passed to pat_grammar are
 *     transferred to it);
 *   - inlining during compilation sets a non-terminal's `inlined`
 *     back-reference to a definition owned by the grammar.  The
 *     back-reference is not owned: a tree containing non-terminals that a
 *     grammar inlined must not outlive that grammar.
 *
 * Sub-patterns that are shared between trees by the caller must not be
 * passed to pat_free twice; in general, build trees, not DAGs.
 */
#ifndef PEG_PATTERN_H
#define PEG_PATTERN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "peg/vm.h"

typedef struct pat pat;

/* ---------------------------------------------------------------------- */
/* Constructors                                                           */
/* ---------------------------------------------------------------------- */

/* All constructors abort on allocation failure (see util.h). */

/* Ordered choice p1 / p2 (right-assoc: prefers p1). */
pat *pat_alt(pat *l, pat *r);

/* Sequence p1 p2. */
pat *pat_seq(pat *l, pat *r);

/* Concatenation of n patterns: p1 p2 p3... (n == 0 is the empty pattern). */
pat *pat_concat(pat **patts, size_t n);

/* Ordered choice of n patterns: p1 / p2 / p3... (n == 0 is the empty
 * pattern).  The chain is right-associative. */
pat *pat_or(pat **patts, size_t n);

/* Kleene star p*: zero or more occurrences. */
pat *pat_star(pat *p);

/* Kleene plus p+: one or more occurrences. */
pat *pat_plus(pat *p);

/* Optional p?: at most one occurrence. */
pat *pat_optional(pat *p);

/* Not predicate !p: succeeds (consuming nothing) iff p fails. */
pat *pat_not(pat *p);

/* And predicate &p: succeeds (consuming nothing) iff p succeeds. */
pat *pat_and(pat *p);

/* Capture the span matched by p with the given id (the {{ }} syntax of
 * the peg language). */
pat *pat_cap(pat *p, int id);

/* Checker validation: after p matches, run the checker over the matched
 * span (see vm.h); it may consume additional bytes or fail the match. */
pat *pat_check(pat *p, vm_checker_fn fn, void *ud);
pat *pat_check_flags(pat *p, vm_checker_fn fn, void *ud, int id, int flag);

/* Memoization (the {{ }} memoization syntax of the peg language).
 * pat_memo assigns a fresh id from the same global counter the compiler
 * uses internally; pat_memo_id uses an explicit id (and advances the
 * counter past it). */
pat *pat_memo(pat *p);
pat *pat_memo_id(pat *p, int id);

/* A string literal. */
pat *pat_literal(const char *s, size_t len);

/* Any single byte in the set. */
pat *pat_set(const vm_charset *set);

/* Any n bytes (fails if fewer remain). */
pat *pat_any(uint8_t n);

/* Exactly n repetitions of p (n <= 0 is the empty pattern). */
pat *pat_repeat(pat *p, int n);

/* Search: match the first occurrence of p (see the paper's Search
 * operator; use Star(Search(p)) for the last occurrence of a
 * non-overlapping p). */
pat *pat_search(pat *p);

/* Zero-width assertion; op is a mask of the empty-op bits:
 * BeginLine=1, EndLine=2, BeginText=4, EndText=8, WordBoundary=16,
 * NoWordBoundary=32. */
pat *pat_emptyop(uint8_t op);

/* An unresolved non-terminal, to be defined by a pat_grammar. */
pat *pat_nonterm(const char *name);

/* A grammar: `start` names the entry definition; the n `defs` (with
 * their names) are TRANSFERRED to the returned grammar. */
pat *pat_grammar(const char *start, const char **names, pat **defs,
                 size_t ndefs);

/* A grammar in which every definition is captured.  ids[i] (n entries)
 * receives the capture id assigned to names[i].  As with pat_grammar,
 * the defs are transferred to the returned grammar. */
pat *pat_cap_grammar(const char *start, const char **names, pat **defs,
                     size_t ndefs, int *ids);

/* Fail with an error message; if recover is non-NULL, continue parsing
 * with it after recording the error (otherwise the parse fails). */
pat *pat_error(const char *msg, pat *recover);

/* Release a pattern tree (NULL is allowed). */
void pat_free(pat *p);

/* ---------------------------------------------------------------------- */
/* Compilation                                                            */
/* ---------------------------------------------------------------------- */

/*
 * Compile a pattern into an optimized VM program.  Returns NULL if a
 * non-terminal was not found in its grammar; *err (if err is non-NULL)
 * is then set to the missing name (valid as long as the pattern tree
 * lives).  The returned program is owned by the caller (vm_code_free).
 */
vm_code *pat_compile(pat *p, const char **err);

/* ---------------------------------------------------------------------- */
/* Debug printing                                                         */
/* ---------------------------------------------------------------------- */

/* A PEG-ish rendering of the pattern.  The returned
 * string is allocated; free it. */
char *pat_prettify(pat *p);

#endif /* PEG_PATTERN_H */
