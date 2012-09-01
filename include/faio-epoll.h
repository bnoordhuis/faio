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

#define FAIO_EVENT_TYPE                                                       \
  struct epoll_event

#define FAIO_EVENT_ARRAY_GET_EVENTS(var)                                      \
  ((var)->events)

#define FAIO_EVENT_ARRAY_GET_DATA(var)                                        \
  ((var)->data.ptr)

struct faio_loop
{
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
  faio__queue_init(&loop->pending_queue);

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static void faio_fini(struct faio_loop *loop)
{
  close(loop->epoll_fd);
  loop->epoll_fd = -1;
}

FAIO_ATTRIBUTE_UNUSED
static unsigned int faio_poll(struct faio_loop *loop,
                              struct epoll_event *events,
                              unsigned int maxevents,
                              double timeout)
{
  struct faio_handle *handle;
  struct faio__queue *queue;
  struct timespec before;
  struct timespec after;
  unsigned long elapsed;
  int ms;
  int i;
  int n;

  /* Silence compiler warning. */
  before.tv_sec = 0;
  before.tv_nsec = 0;

  while (!faio__queue_empty(&loop->pending_queue)) {
    struct epoll_event evt;

    queue = faio__queue_head(&loop->pending_queue);
    handle = faio__queue_data(queue, struct faio_handle, pending_queue);
    faio__queue_remove(queue);

    evt.events = handle->events;
    evt.data.ptr = handle;
    epoll_ctl(loop->epoll_fd, handle->op, handle->fd, &evt);
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

    if (n != -1)
      return n;

    if (errno != EINTR)
      abort();

    if (ms == 0)
      return 0;

    if (ms == -1)
      continue;

    if (clock_gettime(CLOCK_MONOTONIC, &after))
      abort();

    /* Guard against an unlikely overflow in calculating the elapsed time. */
    if ((after.tv_sec - before.tv_sec) > (ms / 1000))
      return 0;

    elapsed  = 1000UL;
    elapsed -= before.tv_nsec / 1000000UL;
    elapsed += after.tv_nsec / 1000000UL;
    elapsed %= 1000UL;
    elapsed += 1000UL * (after.tv_sec - before.tv_sec);

    if (elapsed >= (unsigned long) ms)
      return 0;

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
  handle->events = 0;

  if (!faio__queue_empty(&handle->pending_queue))
    faio__queue_remove(&handle->pending_queue);

  return epoll_ctl(loop->epoll_fd,
                   EPOLL_CTL_DEL,
                   handle->fd,
                   (struct epoll_event *) 1024); /* Work around kernel bug. */
}

#endif /* FAIO_EPOLL_H_ */
