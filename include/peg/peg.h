/*
 * peg.h - the end-to-end incremental parser.
 *
 * The top-level API: ties together a compiled grammar (peg_pattern.h),
 * the parsing machine (peg_vm.h), and the memoization table (peg_memo.h) into
 * one object that owns a subject buffer and can reparse incrementally
 * after edits.
 *
 * The whole incremental protocol is bundled into one type so the
 * invariants (the memo edit's coordinates matching the buffer splice,
 * one memo table per subject, the furthest-read reset between parses)
 * cannot be violated by the caller:
 *
 *   peg *g = peg_new(pat, 512);
 *   peg_parse(g, data, len);          // full parse, fills the table
 *   peg_edit(g, start, end, text, n); // splice buffer + update table
 *   peg_reparse(g);                   // incremental: reuses entries
 *
 * Correctness of the incremental result (it must equal a full reparse
 * of the edited text) is the subject of test_peg.c's differential test.
 */
#ifndef PEG_PEG_H
#define PEG_PEG_H

#include <stdbool.h>
#include <stddef.h>

#include "peg/peg_pattern.h"
#include "peg/peg_vm.h"

/*
 * C linkage for C++ callers (peg.hpp).  The definitions are compiled as
 * C, so without this every symbol here would be mangled on the way in.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct peg peg;

/*
 * Create a parser for the pattern (compiled once; the pattern may be
 * freed afterwards).  memo_threshold is the minimum examined-length for
 * a memo entry to be worth storing; 0 stores everything.
 */
peg *peg_new(pat *p, int memo_threshold);

void peg_free(peg *g);

/*
 * Full parse of a new subject: replaces the buffer, resets the memo
 * table, and executes.  The buffer is copied.
 */
vm_result peg_parse(peg *g, const uint8_t *data, size_t len);

/*
 * Replace [start, end) of the subject with text[0..textlen).  The
 * buffer is spliced and the memo table updated (entries that examined
 * any changed byte are evicted; the rest shift lazily).  This is the
 * whole incremental protocol — the next peg_reparse picks up from the
 * surviving entries.
 */
void peg_edit(peg *g, int start, int end, const uint8_t *text,
               size_t textlen);

/*
 * Incremental reparse of the (edited) subject, reusing memoized
 * results.  Equivalent to peg_parse on the same text but faster after
 * small edits.
 */
vm_result peg_reparse(peg *g);

/*
 * Extract captures overlapping [low, high) (the window optimization,
 * paper Section 4.4): runs the machine with a window so only the
 * affected region's captures are regenerated.  Requires a prior
 * peg_parse/peg_reparse of the current text.
 */
vm_result peg_capture_interval(peg *g, int low, int high);

/* The current subject and its length (the parse positions index it). */
const uint8_t *peg_subject(const peg *g);
size_t peg_subject_len(const peg *g);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PEG_PEG_H */
