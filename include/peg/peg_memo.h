/*
 * peg_memo.h - memoization table for incremental packrat parsing.
 *
 * Memoization table on top of the interval tree
 * (src/interval/interval_tree.c).
 *
 * The table is the packrat parser's cache: it remembers, for a grammar
 * rule (identified by an integer id) applied at an input position, how
 * much input the rule matched, how much it *examined* (PEGs have
 * unbounded lookahead, so examined can exceed matched), and the captures
 * the match produced.  During an incremental reparse the table lets the
 * parser skip re-deriving those results.
 *
 * TreeTable stores the entries in the lazy interval tree keyed by
 * (position, id), with each entry's interval covering its examined
 * extent.  An edit then invalidates exactly the entries that examined
 * any changed byte (interval overlap, paper Section 3.3/4.1) and lazily
 * shifts everything after the edit (Section 4.2).
 *
 * Entries and captures form a directed graph of plain heap objects owned
 * by the table (or, before insertion, by the parser).  Ownership rules:
 *
 *  - memo_table_put hands ownership of the entry's captures to the table;
 *    the caller must not touch or free them afterwards, except through
 *    references obtained from the table (memo_entry_captures, or capture
 *    traversal after a successful parse).
 *  - memo_table_free releases entries and captures.  Any Capture pointer
 *    obtained from the table becomes dangling; keep results alive across
 *    an edit session by copying what you need, or keep the table alive.
 *  - memo_entry_set_pos is internal (used on insertion); it makes the
 *    entry's captures relocatable by recording, in each capture, an
 *    offset relative to the entry's interval-tree location.
 */
#ifndef PEG_MEMO_H
#define PEG_MEMO_H

#include <stdbool.h>
#include <stddef.h>

#include "peg/peg_interval.h"

/*
 * C linkage for C++ callers (peg.hpp).  The definitions are compiled as
 * C, so without this every symbol here would be mangled on the way in.
 */
#ifdef __cplusplus
extern "C" {
#endif

struct memo_entry;
struct memo_capture;

/* ---------------------------------------------------------------------- */
/* Edit                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * An edit replaces the byte range [start, end) of the subject with `len`
 * bytes of new text.  len == 0 is a pure deletion; start == end is a pure
 * insertion.  Real edits always satisfy len >= 0 and thus never shrink
 * the text by more than (end - start), which the interval tree relies on.
 */
typedef struct {
	int start;
	int end;
	int len;
} memo_edit;

/* ---------------------------------------------------------------------- */
/* Capture                                                                */
/* ---------------------------------------------------------------------- */

/*
 * A capture is a node of the parse tree produced while parsing.  It has
 * an id (grammar-defined), an extent, and children.
 *
 * Captures stored in the memo table are *relocatable*: instead of an
 * absolute start position, a capture stores an offset from the start of
 * the memo entry it belongs to (paper Section 4.2, "Relocatable Parse
 * Results").  The entry's position lives in the interval tree, so lazy
 * shifts move the whole entry — and with it every capture — for free.
 * memo_capture_start resolves the two into an absolute position on
 * demand.  The indirection costs one pointer chase per query and makes
 * each capture 16 bytes larger; it buys O(1)-per-edit position updates
 * for arbitrarily many captures.
 *
 * Two kinds exist: nodes (grammar captures, carrying an id) and dummies
 * (transparent wrappers introduced by tree memoization when several
 * stack items are merged; they hold children but no id of their own, and
 * iterators skip over them).
 *
 * Reference counting
 * ------------------
 * Captures are shared freely between the memo table, the parser
 * stack, and the result tree.  There is no garbage collector, so they
 * are reference counted:
 *
 *  - A newly created capture has one reference, held by its creator.
 *  - Every container that stores a capture pointer (a stack capture
 *    list, a capture's children array, a memo entry's capture list,
 *    the result tree) holds one reference per pointer.
 *  - memo_capture_ref adds a reference; memo_capture_free drops one and
 *    destroys the capture (recursively) when the count reaches zero.
 *  - A capture's `ment` (owning entry) is a *weak* reference: the entry
 *    outlives its captures' use of it because the entry's own capture
 *    list holds strong references to the flat-list captures, and every
 *    capture attached to an entry is a descendant of a flat-list
 *    capture (attachment is by capture_set_ment recursion and capture
 *    structure
 *    is immutable once built).  When an entry is destroyed it walks its
 *    captures and re-absolutizes any still-living positions, so no
 *    dangling `ment` can survive.
 */
typedef struct memo_capture memo_capture;

/* Create a node capture with the given id, start, and length (one ref). */
memo_capture *memo_capture_node(int id, int start, int length);

/* Create a dummy capture (a transparent group of children) (one ref). */
memo_capture *memo_capture_dummy(int start, int length);

/* Add a reference.  NULL is allowed. */
void memo_capture_ref(memo_capture *c);

/*
 * Drop a reference; destroys the capture and its children array when the
 * count reaches zero.  NULL is allowed.
 */
void memo_capture_free(memo_capture *c);

/*
 * Prepend/add a child to a capture.  Children are stored in parse order;
 * memo_capture_add appends.  The child's ownership passes to the parent.
 */
void memo_capture_add(memo_capture *c, memo_capture *child);

/* The capture's id (0 for dummies). */
int memo_capture_id(const memo_capture *c);

/* True if this capture is a dummy (transparent wrapper). */
bool memo_capture_dummy_p(const memo_capture *c);

/* The number of children, not counting those inside dummies. */
int memo_capture_num_children(const memo_capture *c);

/*
 * The capture's start position in the current (post-edit) coordinates.
 * Resolves the entry-relative offset through the memo table, applying
 * any pending lazy shifts on the way.
 */
int memo_capture_start(const memo_capture *c);

/*
 * Forget the owning memo entry of this capture and every descendant
 * attached to the same entry, converting their offsets back to absolute
 * positions using the entry's current (post-shift) start.  Used when an
 * entry is destroyed while some of its captures live on in a result
 * tree.
 */
void memo_capture_absolutize(memo_capture *c, struct memo_entry *entry);

/* The capture's length. */
int memo_capture_len(const memo_capture *c);

/* The capture's end position (start + length). */
int memo_capture_end(const memo_capture *c);

/*
 * Call fn(capture, ud) for each non-dummy child of `c`, left to right,
 * recursing through dummy wrappers.  This is the capture tree traversal
 * for consumers.
 */
void memo_capture_each_child(const memo_capture *c,
                             void (*fn)(const memo_capture *c, void *ud),
                             void *ud);

/*
 * The n-th non-dummy child (see memo_capture_each_child), or NULL if
 * there is none.  O(n) in the number of children.
 */
const memo_capture *memo_capture_child(const memo_capture *c, int n);

/* Recursively free a capture tree not owned by a memo table. */
void memo_capture_free(memo_capture *c);

/* ---------------------------------------------------------------------- */
/* Entry                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * A memoized parse result: the rule id is the tree key, so it is not
 * repeated here.  A length of -1 means the rule failed to match at this
 * position (it may still have examined input while failing, which is
 * exactly why examined is tracked separately).
 *
 * `count` is the number of repetitions folded into this entry by tree
 * memoization (paper Section 4.3): 1 for a plain entry, >1 for a merged
 * parent entry covering several back-to-back occurrences of the repeated
 * pattern.  The count tells the parser how many stack items the entry
 * stands in for when it is recovered.
 */
typedef struct memo_entry memo_entry;

/* The number of bytes the rule matched; -1 for a failed match. */
int memo_entry_length(const memo_entry *e);

/* The number of bytes the rule examined (>= length when it matched). */
int memo_entry_examined(const memo_entry *e);

/* The number of repetitions merged into this entry (1 unless merged). */
int memo_entry_count(const memo_entry *e);

/* The entry's starting position, in current coordinates. */
int memo_entry_pos(const memo_entry *e);

/*
 * The captures produced inside this entry, in parse order.  The array is
 * owned by the entry; its length is *n (set even when the return is
 * NULL).
 */
memo_capture **memo_entry_captures(const memo_entry *e, size_t *n);

/* ---------------------------------------------------------------------- */
/* Table                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * The memoization table: entries stored in a lazy interval tree, plus a
 * memoization threshold.  Not thread-safe; guarding concurrent
 * mutex; wrap externally if needed).
 */
typedef struct memo_table memo_table;

/*
 * Create a table with the given memoization threshold: entries whose
 * examined extent is below the threshold (or which matched nothing) are
 * not stored.  Because tree memoization guarantees repeated small
 * matches are covered by larger parent entries, the threshold can be
 * aggressive — the paper's measurements find 128–512 bytes a good
 * speed/space tradeoff.  threshold <= 0 disables the cutoff.
 */
memo_table *memo_table_new(int threshold);

/* Free the table and every entry and capture it owns.  NULL is allowed. */
void memo_table_free(memo_table *t);

/*
 * Return the entry for rule `id` at `pos`, choosing the entry with the
 * largest matched length if several exist (so a reparse can skip as much
 * input as possible — this is what makes tree memoization fast).
 * Returns NULL if there is no entry.
 */
memo_entry *memo_table_get(memo_table *t, int id, int pos);

/*
 * Record a parse result for rule `id` starting at `start`: it matched
 * `length` bytes (-1 = failed) while examining `examined` bytes, folding
 * in `count` repetitions (1 unless tree memoization merged entries), and
 * producing `captures` (ownership of the array and its elements passes
 * to the table; may be NULL if n == 0).
 *
 * Entries below the threshold are dropped (the captures are freed).
 * Examined is normalized to at least length, and the stored interval is
 * [start, start+examined) — the examined extent is what edits must
 * invalidate.
 */
void memo_table_put(memo_table *t, int id, int start, int length,
                    int examined, int count, memo_capture **captures,
                    size_t ncap);

/*
 * Apply an edit: evict every entry that examined a byte in
 * [edit.start, edit.end) and lazily shift entries after it.  After this
 * returns the table is a valid starting state for an incremental
 * reparse of the edited input.
 */
void memo_table_apply_edit(memo_table *t, memo_edit edit);

/*
 * The number of entries currently stored.
 */
size_t memo_table_size(memo_table *t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PEG_MEMO_H */
