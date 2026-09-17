/*
 * sexpr.c - the s-expression printer: the debug printout and the wire
 * format for pegvm replies.
 *
 * One traversal serves both destinations.  The traversal is iterative for
 * the same reason teardown is (node.c): a deeply nested term is an
 * ordinary parse result, and a printer that recursed would run out of
 * stack on the target this code is meant to fit.  One frame per open
 * sub-list, grown on demand.
 */
#include "peg/peg_term.h"

#include "term_internal.h"

/* Where the bytes end up; the traversal never knows more than this. */
struct sink {
	void (*write)(void *ctx, const char *s, size_t n);
	void *ctx;
};

struct printer {
	struct sink out;
	const char *subject;	/* NULL: tags only */
	size_t subject_len;
};

/* One open sibling chain. */
struct print_frame {
	const peg_node *child;	/* next sibling to print, NULL when done */
	bool first;		/* nothing printed at this level yet */
	bool top;		/* the outermost chain takes no parentheses */
};

static void emit(struct printer *p, const char *s, size_t n)
{
	if (n != 0)
		p->out.write(p->out.ctx, s, n);
}

static void emit_str(struct printer *p, const char *s)
{
	emit(p, s, strlen(s));
}

/*
 * A tag that was never interned has no name.  Print its numeric id rather
 * than dropping the node, so the s-expression still describes the shape
 * it claims to describe.
 */
static void emit_tag(struct printer *p, peg_tag tag)
{
	const char *name = peg_tag_name(tag);
	char digits[10];
	char buf[12];
	size_t nd = 0;
	size_t n = 0;

	if (name != NULL) {
		emit_str(p, name);
		return;
	}

	do {
		digits[nd++] = (char)('0' + tag % 10);
		tag /= 10;
	} while (tag != 0);

	buf[n++] = '#';
	while (nd > 0)
		buf[n++] = digits[--nd];
	emit(p, buf, n);
}

/*
 * Quoting rules (see peg_term.h): quote, backslash and the three common
 * whitespace escapes get their short form; every byte outside printable
 * ASCII gets \xHH.  Escaping the high bytes is not decoration -- a
 * byte-oriented parser cannot tell UTF-8 text from binary, and the wire
 * format is line-oriented text.
 */
static void emit_escaped(struct printer *p, const char *text, size_t len)
{
	static const char hex[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)text[i];
		char esc[4];
		size_t n = 0;

		switch (c) {
		case '"':
			esc[n++] = '\\';
			esc[n++] = '"';
			break;
		case '\\':
			esc[n++] = '\\';
			esc[n++] = '\\';
			break;
		case '\n':
			esc[n++] = '\\';
			esc[n++] = 'n';
			break;
		case '\r':
			esc[n++] = '\\';
			esc[n++] = 'r';
			break;
		case '\t':
			esc[n++] = '\\';
			esc[n++] = 't';
			break;
		default:
			if (c >= 0x20 && c < 0x7f) {
				esc[n++] = (char)c;
			} else {
				esc[n++] = '\\';
				esc[n++] = 'x';
				esc[n++] = hex[c >> 4];
				esc[n++] = hex[c & 0xf];
			}
			break;
		}
		emit(p, esc, n);
	}
}

/*
 * A leaf's matched text, clamped to the subject we were given: a span can
 * outlive the buffer it was measured against (that is the whole point of
 * the memo table's lazy shifting), and reading past the end of the
 * caller's buffer would be a bug in the debug path.
 */
static void emit_text(struct printer *p, const peg_node *n)
{
	size_t lo = n->span.lo;
	size_t hi = n->span.hi;

	if (p->subject == NULL)
		return;

	if (lo > p->subject_len)
		lo = p->subject_len;
	if (hi > p->subject_len)
		hi = p->subject_len;
	if (hi < lo)
		hi = lo;

	emit_str(p, " \"");
	emit_escaped(p, p->subject + lo, hi - lo);
	emit_str(p, "\"");
}

static void print_list(struct printer *p, const peg_node *head)
{
	struct print_frame *stack;
	size_t depth = 1;
	size_t cap = 8;

	if (head == NULL) {
		emit_str(p, "()");
		return;
	}

	stack = peg_term_xmalloc(cap * sizeof(stack[0]));
	stack[0].child = head;
	stack[0].first = true;
	stack[0].top = true;

	while (depth > 0) {
		struct print_frame *f = &stack[depth - 1];
		const peg_node *n = f->child;

		if (n == NULL) {
			/*
			 * The chain ended.  Unless it is the outermost one,
			 * the frame was pushed by a node with a sub-list,
			 * so closing it closes that node's form.
			 */
			if (!f->top)
				emit_str(p, ")");
			depth--;
			continue;
		}

		if (!f->first)
			emit_str(p, " ");
		f->first = false;
		f->child = n->next;

		emit_str(p, "(");
		emit_tag(p, n->tag);

		if (n->sub != NULL) {
			/*
			 * The space separates the tag from its first child;
			 * the level's own separators then fall between the
			 * siblings, so none is left before the ")".
			 */
			emit_str(p, " ");
			if (depth == cap) {
				cap *= 2;
				stack = peg_term_xrealloc(stack,
				                          cap * sizeof(stack[0]));
			}
			stack[depth].child = n->sub;
			stack[depth].first = true;
			stack[depth].top = false;
			depth++;
		} else {
			emit_text(p, n);
			emit_str(p, ")");
		}
	}

	peg_term_xfree(stack);
}

/* ---------------------------------------------------------------------- */
/* Sinks                                                                  */
/* ---------------------------------------------------------------------- */

/* A growable string; the caller ends up owning ->s. */
struct strbuf {
	char *s;
	size_t len;
	size_t cap;
};

static void sink_buf(void *ctx, const char *s, size_t n)
{
	struct strbuf *b = ctx;

	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap;

		while (cap < b->len + n + 1)
			cap *= 2;
		b->s = peg_term_xrealloc(b->s, cap);
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

#ifndef PEG_TERM_NO_STDIO
static void sink_file(void *ctx, const char *s, size_t n)
{
	size_t wrote = fwrite(s, 1, n, (FILE *)ctx);

	(void)wrote;	/* a short write on a debug stream is not reportable */
}
#endif

/* ---------------------------------------------------------------------- */
/* Entry points                                                           */
/* ---------------------------------------------------------------------- */

#ifndef PEG_TERM_NO_STDIO
void peg_term_print(FILE *out, const peg_node *head, const char *subject)
{
	struct printer p;

	if (out == NULL)
		return;

	p.out.write = sink_file;
	p.out.ctx = out;
	p.subject = subject;
	p.subject_len = subject != NULL ? strlen(subject) : 0;
	print_list(&p, head);
}

void peg_term_print_tags(FILE *out, const peg_node *head)
{
	peg_term_print(out, head, NULL);
}
#endif /* PEG_TERM_NO_STDIO */

char *peg_term_to_string(const peg_node *head, const char *subject)
{
	struct printer p;
	struct strbuf b;

	/* Start with an empty string, so an empty term is still a string. */
	b.s = peg_term_xmalloc(1);
	b.s[0] = '\0';
	b.len = 0;
	b.cap = 1;

	p.out.write = sink_buf;
	p.out.ctx = &b;
	p.subject = subject;
	p.subject_len = subject != NULL ? strlen(subject) : 0;
	print_list(&p, head);
	return b.s;
}

char *peg_term_to_tags_string(const peg_node *head)
{
	return peg_term_to_string(head, NULL);
}

void peg_term_string_free(char *s)
{
	peg_term_xfree(s);
}
