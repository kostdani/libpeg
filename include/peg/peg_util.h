/*
 * peg_util.h - allocation helpers for the libpeg C library.
 *
 * Thin wrappers around the standard allocators that abort the process on
 * out-of-memory.  A parsing library has no reasonable way to recover from a
 * failed allocation mid-parse, so we treat OOM as fatal; callers who need
 * different behavior can swap these out.
 */
#ifndef PEG_UTIL_H
#define PEG_UTIL_H

#include <stddef.h>

/*
 * The noreturn marker, spelled for the translation unit that includes
 * this header.  C11 has it as a keyword (_Noreturn); C++ does not -- it
 * spells the same thing [[noreturn]] -- so a C++ caller (peg.hpp) would
 * not compile without this indirection.  Older C gets the plain
 * declaration: the attribute is a diagnostic aid, never load-bearing.
 */
#if defined(__cplusplus)
#define PEG_NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define PEG_NORETURN _Noreturn
#else
#define PEG_NORETURN
#endif

/*
 * C linkage for C++ callers (peg.hpp).  The definitions are compiled as
 * C, so without this every symbol here would be mangled on the way in.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* malloc that aborts the program on failure. */
void *xmalloc(size_t n);

/* calloc that aborts the program on failure. */
void *xcalloc(size_t n, size_t size);

/* realloc that aborts the program on failure. */
void *xrealloc(void *p, size_t n);

/* strdup that aborts the program on failure. */
char *xstrdup(const char *s);

/* Print a message to stderr and abort (internal invariant violation). */
PEG_NORETURN void abort_msg(const char *msg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PEG_UTIL_H */
