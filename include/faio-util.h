/*
 * Copyright (c) 2012, Ben Noordhuis <info@bnoordhuis.nl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef FAIO_UTIL_H_
#define FAIO_UTIL_H_

struct faio__queue
{
  struct faio__queue *prev;
  struct faio__queue *next;
};

#define faio__queue_data(ptr, type, member)                                   \
  ((type *) ((char *) (ptr) - (uintptr_t) &((type *) 0)->member))

FAIO_ATTRIBUTE_UNUSED
static void faio__queue_init(struct faio__queue *q)
{
  q->prev = q;
  q->next = q;
}

FAIO_ATTRIBUTE_UNUSED
static int faio__queue_empty(const struct faio__queue *q)
{
  return q == q->prev;
}

FAIO_ATTRIBUTE_UNUSED
static struct faio__queue *faio__queue_head(const struct faio__queue *q)
{
  return q->next;
}

FAIO_ATTRIBUTE_UNUSED
static void faio__queue_append(struct faio__queue *q, struct faio__queue *n)
{
  q->prev->next = n;
  n->prev = q->prev;
  n->next = q;
  q->prev = n;
}

FAIO_ATTRIBUTE_UNUSED
static void faio__queue_remove(struct faio__queue *n)
{
  n->next->prev = n->prev;
  n->prev->next = n->next;
  faio__queue_init(n);
}

#endif /* FAIO_UTIL_H_ */
