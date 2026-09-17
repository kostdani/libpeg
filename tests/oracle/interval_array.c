/*
 * interval_array.c - naive reference implementation of the interval map.
 * See interval_array.h.
 */
#include "interval_array.h"

#include <stdlib.h>

#include "peg/peg_util.h"

struct iarray {
	iarray_slot **slots;	/* array of pointers: handles stay stable */
	size_t n;
	size_t cap;
};

iarray *iarray_new(void)
{
	return xcalloc(1, sizeof(iarray));
}

void iarray_free(iarray *a)
{
	if (a == NULL)
		return;
	for (size_t i = 0; i < a->n; i++)
		free(a->slots[i]);
	free(a->slots);
	free(a);
}

iarray_slot *iarray_add(iarray *a, int id, int low, int high, void *value)
{
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 16;
		a->slots = xrealloc(a->slots, a->cap * sizeof(a->slots[0]));
	}
	iarray_slot *s = xmalloc(sizeof(*s));
	s->id = id;
	s->low = low;
	s->high = high;
	s->value = value;
	a->slots[a->n++] = s;
	return s;
}

void *iarray_find_largest(iarray *a, int id, int pos)
{
	iarray_slot *best = NULL;
	for (size_t i = 0; i < a->n; i++) {
		iarray_slot *s = a->slots[i];
		if (s->low != pos || s->id != id)
			continue;
		if (best == NULL || s->high - s->low > best->high - best->low)
			best = s;
	}
	return best ? best->value : NULL;
}

void iarray_remove_and_shift(iarray *a, int low, int high, int amt)
{
	/* Remove overlapping slots (swap with last). */
	for (size_t i = 0; i < a->n;) {
		iarray_slot *s = a->slots[i];
		if (s->low < high && s->high > low) {
			free(s);
			a->slots[i] = a->slots[--a->n];
		} else {
			i++;
		}
	}

	if (amt == 0)
		return;

	/* Shift everything at or after the edit point. */
	for (size_t i = 0; i < a->n; i++) {
		iarray_slot *s = a->slots[i];
		if (s->low >= low) {
			s->low += amt;
			s->high += amt;
		}
	}
}

size_t iarray_size(iarray *a)
{
	return a->n;
}
