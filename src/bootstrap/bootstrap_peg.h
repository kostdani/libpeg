/*
 * bootstrap_peg.h - the hand-written .peg reader, and the proof it is
 * the same grammar as the one libpeg compiles.
 *
 * This is the trusted end of the self-hosting argument (PLAN.org D11).
 * It is a plain recursive-descent parser, written to be obvious rather
 * than fast: every function below corresponds to exactly one rule of
 * grammars/peg.peg, and it records one node per rule match, so the tree
 * it returns is the derivation tree the grammar itself would produce.
 *
 * That is the whole trick.  Because the derivation tree is defined by the
 * grammar rather than by this file, the *same* builder that turns a
 * hand-parsed tree into a compiled grammar also turns a tree the compiled
 * grammar produced back into a compiled grammar.  Feeding one into the
 * other is the fixpoint test: peg.v1 parses peg.peg, and what comes out
 * compiles to peg.v2, which must be byte-identical to v1.
 *
 * Node tags are rule names, interned with peg_tag_intern, and spans are
 * byte ranges into the subject -- so the tree is an ordinary term
 * (peg_term.h) and its s-expression rendering is the comparison format.
 */
#ifndef PEG_BOOTSTRAP_PEG_H
#define PEG_BOOTSTRAP_PEG_H

#include <stddef.h>

#include "peg/peg_memo.h"
#include "peg/peg_pattern.h"
#include "peg/peg_term.h"

/*
 * Parse .peg source.  Returns the `file` node -- one node whose sub-list
 * is the whole derivation tree -- or NULL, in which case *errpos is the
 * byte offset the parse stopped at (errpos may be NULL).
 */
peg_node *bootstrap_parse(const char *src, size_t len, size_t *errpos);

/*
 * Build a grammar in which every definition is captured, from a
 * derivation tree rooted at `file`.  `src`/`len` are the subject the
 * spans index into; they must be the same bytes the tree was parsed
 * from.
 *
 * The rule names are read out of the tree in source order, and *names_out
 * receives them (ndefs NUL-terminated strings, owned by the caller) with
 * *ids_out the capture id each was assigned -- ids_out[i] always equals
 * i, but reading it from the API rather than assuming it is the point of
 * taking it.  Returns NULL if the tree has no rules or a reference does
 * not resolve.
 */
pat *bootstrap_grammar(const peg_node *file, const char *src, size_t len,
                       char ***names_out, size_t *nnames_out, int **ids_out);

/*
 * Release a name table from bootstrap_grammar.
 */
void bootstrap_names_free(char **names, size_t nnames);

/*
 * Convert a vm_exec result tree into the same shape bootstrap_parse
 * returns: a list of nodes tagged with their rule's name.  The dummy
 * root is dropped, so the result is the start rule's node alone.
 */
peg_node *bootstrap_capture_tree(const memo_capture *root, char **names);

#endif /* PEG_BOOTSTRAP_PEG_H */
