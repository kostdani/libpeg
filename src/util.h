/*
 * util.h - allocation helpers for the peg C library.
 *
 * Thin wrappers around the standard allocators that abort the process on
 * out-of-memory.  A parsing library has no reasonable way to recover from a
 * failed allocation mid-parse, so we treat OOM as fatal; callers who need
 * different behavior can swap these out.
 */
#ifndef PEG_UTIL_H
#define PEG_UTIL_H

#include <stddef.h>

/* malloc that aborts the program on failure. */
void *xmalloc(size_t n);

/* calloc that aborts the program on failure. */
void *xcalloc(size_t n, size_t size);

/* realloc that aborts the program on failure. */
void *xrealloc(void *p, size_t n);

/* strdup that aborts the program on failure. */
char *xstrdup(const char *s);

/* Print a message to stderr and abort (internal invariant violation). */
void abort_msg(const char *msg);

#endif /* PEG_UTIL_H */
