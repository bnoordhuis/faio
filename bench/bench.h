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

#ifndef FAIO_BENCH_H_
#define FAIO_BENCH_H_

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
# define FAIO_ATTRIBUTE_NORETURN __attribute__((noreturn))
# define FAIO_ATTRIBUTE_UNUSED   __attribute__((unused))
#else
# define FAIO_ATTRIBUTE_NORETURN
# define FAIO_ATTRIBUTE_UNUSED
#endif

#define ARRAY_SIZE(a)                                                         \
  (sizeof(a) / sizeof((a)[0]))

#define CONTAINER_OF(ptr, type, member)                                       \
  ((type *) ((char *) (ptr) - (unsigned long) &((type *) 0)->member))

#define E(expr)                                                               \
  do {                                                                        \
    errno = 0;                                                                \
    do { expr; } while (0);                                                   \
    if (errno) sys_error(#expr);                                              \
  }                                                                           \
  while (0)

FAIO_ATTRIBUTE_NORETURN
FAIO_ATTRIBUTE_UNUSED
static void sys_error(const char* what)
{
  fprintf(stderr, "%s: %s (errno=%d)\n", what, strerror(errno), errno);
  exit(42);
}

#endif /* FAIO_BENCH_H_ */
