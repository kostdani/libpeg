/*
 * peg.c - the end-to-end incremental parser.  See peg.h.
 */
#include <stdlib.h>
#include <string.h>

#include "peg/peg.h"
#include "util.h"

struct peg {
	vm_code *code;
	memo_table *tbl;
	int threshold;		/* memo entries below this size are dropped */

	uint8_t *buf;
	size_t len;
	size_t cap;
};

peg *peg_new(pat *p, int memo_threshold)
{
	peg *g = xcalloc(1, sizeof(*g));
	g->code = pat_compile(p, NULL);
	if (g->code == NULL)
		abort_msg("peg_new: pattern failed to compile");
	g->tbl = memo_table_new(memo_threshold);
	g->threshold = memo_threshold;
	return g;
}

void peg_free(peg *g)
{
	if (g == NULL)
		return;
	vm_code_free(g->code);
	memo_table_free(g->tbl);
	free(g->buf);
	free(g);
}

/* Ensure room for len bytes total. */
static void buf_reserve(peg *g, size_t len)
{
	if (len <= g->cap)
		return;
	g->cap = g->cap ? g->cap : 4096;
	while (len > g->cap)
		g->cap *= 2;
	g->buf = xrealloc(g->buf, g->cap);
}

vm_result peg_parse(peg *g, const uint8_t *data, size_t len)
{
	buf_reserve(g, len);
	memcpy(g->buf, data, len);
	g->len = len;

	/* A new subject invalidates every memo entry. */
	memo_table_free(g->tbl);
	g->tbl = memo_table_new(g->threshold);

	return vm_exec(g->code, g->buf, g->len, g->tbl, -1, -1);
}

void peg_edit(peg *g, int start, int end, const uint8_t *text,
              size_t textlen)
{
	if (start < 0 || end < start || (size_t)end > g->len)
		abort_msg("peg_edit: range out of bounds");

	size_t newlen = g->len - (size_t)(end - start) + textlen;
	buf_reserve(g, newlen);

	/* splice: move the tail, then fill the gap */
	memmove(g->buf + start + textlen, g->buf + end,
	        g->len - (size_t)end);
	if (textlen > 0)
		memcpy(g->buf + start, text, textlen);
	g->len = newlen;

	memo_table_apply_edit(g->tbl,
		(memo_edit){ .start = start, .end = end,
		             .len = (int)textlen });
}

vm_result peg_reparse(peg *g)
{
	return vm_exec(g->code, g->buf, g->len, g->tbl, -1, -1);
}

vm_result peg_capture_interval(peg *g, int low, int high)
{
	return vm_exec(g->code, g->buf, g->len, g->tbl, low, high);
}

const uint8_t *peg_subject(const peg *g)
{
	return g->buf;
}

size_t peg_subject_len(const peg *g)
{
	return g->len;
}
