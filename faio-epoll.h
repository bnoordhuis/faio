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

#ifndef FAIO_EPOLL_H_
#define FAIO_EPOLL_H_

#include "faio-util.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define FAIO_POLLIN   EPOLLIN
#define FAIO_POLLOUT  EPOLLOUT
#define FAIO_POLLERR  EPOLLERR
#define FAIO_POLLHUP  EPOLLHUP

#define FAIO_VECTOR_DECL(type)                                                \
  struct                                                                      \
  {                                                                           \
    type *elts;                                                               \
    unsigned int nelts;                                                       \
  }

#define FAIO_VECTOR_INIT(vec)                                                 \
  do {                                                                        \
    (vec)->elts = NULL;                                                       \
    (vec)->nelts = 0;                                                         \
  }                                                                           \
  while (0)

#define FAIO_VECTOR_FINI(vec)                                                 \
  do {                                                                        \
    free((vec)->elts);                                                        \
    (vec)->elts = NULL;                                                       \
    (vec)->nelts = 0;                                                         \
  }                                                                           \
  while (0)

#define FAIO_VECTOR_LEN(vec)                                                  \
  ((vec)->nelts)

#define FAIO_VECTOR_GET(vec, idx)                                             \
  ((vec)->elts[(unsigned int) (idx)])

#define FAIO_VECTOR_SET(vec, idx, val)                                        \
  do {                                                                        \
    if (FAIO_VECTOR_LEN(vec) < (unsigned int) (idx)) {                        \
      unsigned int v = (unsigned int) (idx);                                  \
      if (v < 256)                                                            \
        v = 256;                                                              \
      else {                                                                  \
        /* Next power of two. */                                              \
        v -= 1;                                                               \
        v |= v >> 1;                                                          \
        v |= v >> 2;                                                          \
        v |= v >> 4;                                                          \
        v |= v >> 8;                                                          \
        v |= v >> 16;                                                         \
        v += 1;                                                               \
      }                                                                       \
      (vec)->elts = realloc((vec)->elts, v * sizeof((vec)->elts[0]));         \
      (vec)->nelts = v;                                                       \
    }                                                                         \
    (vec)->elts[(unsigned int) (idx)] = (val);                                \
  }                                                                           \
  while (0)

struct faio_loop
{
  FAIO_VECTOR_DECL(struct faio_handle *) handles;
  struct faio__queue pending_queue;
  int epoll_fd;
};

struct faio_handle
{
  struct faio__queue pending_queue;
  void (*cb)(struct faio_loop *, struct faio_handle *, unsigned int);
  unsigned int events;
  int fd;
  int op;
};

FAIO_ATTRIBUTE_UNUSED
static int faio_init(struct faio_loop *loop)
{
  int epoll_fd;

  do {
#if defined(SYS_epoll_create1)
    epoll_fd = syscall(SYS_epoll_create1, 0x80000 /* EPOLL_CLOEXEC */);

    if (epoll_fd != -1)
      break;

    if (errno != ENOSYS && errno != EINVAL)
      break;
#endif /* defined(SYS_epoll_create1) */

    epoll_fd = syscall(SYS_epoll_create, 1);

    if (epoll_fd == -1)
      break;

    ioctl(epoll_fd, FIOCLEX);
  }
  while (0);

  if (epoll_fd == -1)
    return -1;

  loop->epoll_fd = epoll_fd;
  FAIO_VECTOR_INIT(&loop->handles);
  faio__queue_init(&loop->pending_queue);

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static void faio_fini(struct faio_loop *loop)
{
  FAIO_VECTOR_FINI(&loop->handles);
  close(loop->epoll_fd);
  loop->epoll_fd = -1;
}

FAIO_ATTRIBUTE_UNUSED
static void faio_poll(struct faio_loop *loop, double timeout)
{
  struct epoll_event events[1024]; /* 12 kB */
  struct faio_handle *handle;
  struct faio__queue *queue;
  struct timespec before;
  struct timespec after;
  unsigned long elapsed;
  unsigned int maxevents;
  unsigned int dispatched;
  int fd;
  int ms;
  int i;
  int n;

  /* Silence compiler warning. */
  before.tv_sec = 0;
  before.tv_nsec = 0;

  dispatched = 0;
  maxevents = sizeof(events) / sizeof(events[0]);

  while (!faio__queue_empty(&loop->pending_queue)) {
    queue = faio__queue_head(&loop->pending_queue);
    handle = faio__queue_data(queue, struct faio_handle, pending_queue);
    faio__queue_remove(queue);

    events[0].events = handle->events;
    events[0].data.ptr = 0; /* Silence valgrind. */
    events[0].data.fd = handle->fd;

    /* With EPOLL_CTL_ADD, it's possible for the operation to fail with EEXIST
     * if a file descriptor:
     *
     *  a) has been unregistered with faio_del()
     *  b) but not closed and not deleted with EPOLL_CTL_DEL
     *  c) and re-registered again with faio_add()
     */
    if (epoll_ctl(loop->epoll_fd, handle->op, handle->fd, events)) {
      if (handle->op == EPOLL_CTL_ADD && errno == EEXIST)
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, handle->fd, events);
      else
        abort();
    }
  }

  if (timeout < 0)
    ms = -1;
  else
    ms = timeout * 1000;

  if (ms > 0)
    if (clock_gettime(CLOCK_MONOTONIC, &before))
      abort();

  for (;;) {
    n = epoll_wait(loop->epoll_fd, events, maxevents, ms);

    if (n == 0) {
      /* A -1 timeout means "wait indefinitely" and modern kernels do
       * but old (ancient) kernels wait for LONG_MAX milliseconds.
       */
      if (ms == -1)
        continue;
      else
        return;
    }

    if (n == -1) {
      if (errno == EINTR)
        goto update_timeout;
      else
        abort();
    }

    for (i = 0; i < n; i++) {
      fd = events[i].data.fd;
      handle = FAIO_VECTOR_GET(&loop->handles, fd);

      if (handle == NULL) {
        /* Event for a fd we stopped watching. Kill it. */
        epoll_ctl(loop->epoll_fd,
                  EPOLL_CTL_DEL,
                  fd,
                  (struct epoll_event *) 1024); /* Work around kernel bug. */
        continue;
      }

      handle->cb(loop, handle, events[i].events);
      dispatched = 1;
    }

    /* We read as many events as we could but there might still be more.
     * Poll again but don't block this time.
     */
    if (maxevents == (unsigned int) n) {
      ms = 0;
      continue;
    }

    if (dispatched)
      return;

    /* We only got events for file descriptors we're no longer watching.
     * No callbacks were invoked so from the perspective of the user nothing
     * happened. Update the timeout and poll again.
     */

update_timeout:
    if (ms == 0)
      return;

    if (ms == -1)
      continue;

    if (clock_gettime(CLOCK_MONOTONIC, &after))
      abort();

    /* Guard against an unlikely overflow in calculating the elapsed time. */
    if ((after.tv_sec - before.tv_sec) > (ms / 1000))
      return;

    elapsed  = 1000UL;
    elapsed -= before.tv_nsec / 1000000UL;
    elapsed += after.tv_nsec / 1000000UL;
    elapsed %= 1000UL;
    elapsed += 1000UL * (after.tv_sec - before.tv_sec);

    if (elapsed >= (unsigned long) ms)
      return;

    ms -= elapsed;
    before = after;
  }
}

FAIO_ATTRIBUTE_UNUSED
static int faio_add(struct faio_loop *loop,
                    struct faio_handle *handle,
                    void (*cb)(struct faio_loop *loop,
                               struct faio_handle *handle,
                               unsigned int revents),
                    int fd,
                    unsigned int events)
{
  events &= EPOLLIN | EPOLLOUT;
  events |= EPOLLERR | EPOLLHUP;

  FAIO_VECTOR_SET(&loop->handles, fd, handle);
  faio__queue_append(&loop->pending_queue, &handle->pending_queue);
  handle->cb = cb;
  handle->fd = fd;
  handle->op = EPOLL_CTL_ADD;
  handle->events = events;

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static int faio_mod(struct faio_loop *loop,
                    struct faio_handle *handle,
                    unsigned int events)
{
  events &= EPOLLIN | EPOLLOUT;
  events |= EPOLLERR | EPOLLHUP;

  handle->op = EPOLL_CTL_MOD;
  handle->events = events;

  if (faio__queue_empty(&handle->pending_queue))
    faio__queue_append(&loop->pending_queue, &handle->pending_queue);

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static int faio_del(struct faio_loop *loop, struct faio_handle *handle)
{
  FAIO_VECTOR_SET(&loop->handles, handle->fd, NULL);

  if (!faio__queue_empty(&handle->pending_queue))
    faio__queue_remove(&handle->pending_queue);

  return 0;
}

#endif /* FAIO_EPOLL_H_ */
