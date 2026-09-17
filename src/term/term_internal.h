/*
 * term_internal.h - allocation, and nothing else shared between the term
 * module's translation units.
 *
 * The term type is on the device boundary (PLAN.org's L3), where every
 * allocation must eventually come from the peg_alloc vtable rather than
 * from libc -- a bump arena on the device, malloc on the host.  Until
 * that vtable exists, the whole module allocates through the four
 * functions below, and this file is the single place that changes when it
 * arrives: nothing else in src/term/ names an allocator.
 */
#ifndef PEG_TERM_INTERNAL_H
#define PEG_TERM_INTERNAL_H

#include <stdlib.h>
#include <string.h>

/*
 * Out-of-memory is fatal: a parser has no way to unwind a half-built term
 * and no caller that could act on the failure.  Same policy as the rest
 * of the library (peg_util.h), kept local so this module builds on its
 * own.
 */
static inline void *peg_term_xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (p == NULL)
		abort();
	return p;
}

static inline void *peg_term_xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);

	if (q == NULL)
		abort();
	return q;
}

static inline char *peg_term_xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = peg_term_xmalloc(n);

	memcpy(p, s, n);
	return p;
}

/* Release memory obtained from the three functions above. */
static inline void peg_term_xfree(void *p)
{
	free(p);
}

#endif /* PEG_TERM_INTERNAL_H */
