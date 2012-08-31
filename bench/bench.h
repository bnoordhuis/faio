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

#ifndef _GNU_SOURCE
# define _GNU_SOURCE /* accept4, etc. */
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#if !defined(__linux__)
# include <sys/filio.h>
#endif

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

FAIO_ATTRIBUTE_UNUSED
static void nbio(int fd)
{
  int on;

  on = 1;
  E(ioctl(fd, FIONBIO, &on));
}

FAIO_ATTRIBUTE_UNUSED
static int nb_socket(int family, int type, int proto)
{
#if defined(__linux__)
  return socket(family, type | SOCK_NONBLOCK, proto);
#else
  int fd;

  fd = socket(family, type, proto);
  if (fd != -1)
    nbio(fd);

  return fd;
#endif
}

FAIO_ATTRIBUTE_UNUSED
static int nb_accept(int fd, struct sockaddr *saddr, socklen_t *slen)
{
#if defined(__linux__)
  return accept4(fd, saddr, slen, SOCK_NONBLOCK);
#else
  int fd;

  fd = accept(sfd, saddr, slen);
  if (fd != -1)
    nbio(fd);

  return fd;
#endif
}

static int create_inet_server(int type, unsigned short port)
{
  struct sockaddr_in sin;
  int fd;
  int on;

  E(fd = nb_socket(AF_INET, type, 0));
  on = 1;
  E(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)));

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = INADDR_ANY;
  E(bind(fd, (const struct sockaddr *) &sin, sizeof(sin)));

  if (type == SOCK_STREAM)
    E(listen(fd, 1024));

  return fd;
}

#undef FAIO_ATTRIBUTE_NORETURN
#undef FAIO_ATTRIBUTE_UNUSED

#endif /* FAIO_BENCH_H_ */
