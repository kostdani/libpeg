/*
 * util.c - allocation helpers for the libpeg C library.
 */
#include "peg/peg_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *check(void *p, size_t n)
{
	if (p == NULL && n != 0) {
		fprintf(stderr, "libpeg: out of memory (%zu bytes)\n", n);
		abort();
	}
	return p;
}

void *xmalloc(size_t n)
{
	return check(malloc(n), n);
}

void *xcalloc(size_t n, size_t size)
{
	return check(calloc(n, size), n * size);
}

void *xrealloc(void *p, size_t n)
{
	return check(realloc(p, n), n);
}

void abort_msg(const char *msg)
{
	fprintf(stderr, "libpeg: %s\n", msg);
	abort();
}

char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *dup = xmalloc(n);
	memcpy(dup, s, n);
	return dup;
}
