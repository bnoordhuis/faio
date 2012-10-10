/*
 * Copyright (c) 2012, Fedor Indutny <fedor@indutny.com>
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

#ifndef FAIO_KQUEUE_H_
#define FAIO_KQUEUE_H_

#include "faio-util.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <poll.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#define FAIO_POLLIN   POLLIN
#define FAIO_POLLOUT  POLLOUT
#define FAIO_POLLERR  POLLERR
#define FAIO_POLLHUP  POLLHUP

struct faio_loop
{
  struct faio__queue pending_queue;
  int kq;
};

struct faio_handle
{
  struct faio__queue pending_queue;
  void (*cb)(struct faio_loop *, struct faio_handle *, unsigned int);
  unsigned int revents;
  unsigned int events;
  int fd;
};

static int faio__gettime_monotonic(struct timespec *spec)
{
#if defined(__APPLE__)
  uint64_t val;

  val = mach_absolute_time();
  spec->tv_sec = val / (uint64_t) 1e9;
  spec->tv_nsec = val % (uint64_t) 1e9;

  return 0;
#else
  return clock_gettime(CLOCK_MONOTONIC, spec);
#endif
}

FAIO_ATTRIBUTE_UNUSED
static int faio_init(struct faio_loop *loop)
{
  int kq;

  kq = kqueue();

  if (kq == -1)
    return -1;

  faio__queue_init(&loop->pending_queue);
  loop->kq = kq;

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static void faio_fini(struct faio_loop *loop)
{
  close(loop->kq);
  loop->kq = -1;
}

FAIO_ATTRIBUTE_UNUSED
static void faio_poll(struct faio_loop *loop, double timeout)
{
  struct kevent events[256]; /* 8 kB */
  struct faio_handle *handle;
  struct faio__queue *queue;
  struct timespec before;
  struct timespec after;
  struct timespec diff;
  struct timespec *pts;
  struct timespec ts;
  unsigned int maxevents;
  unsigned int revents;
  int op;
  int i;
  int n;

  /* Silence compiler warning. */
  before.tv_nsec = 0;
  before.tv_sec = 0;

  maxevents = sizeof(events) / sizeof(events[0]);

  n = 0;

  while (!faio__queue_empty(&loop->pending_queue)) {
    queue = faio__queue_head(&loop->pending_queue);
    handle = faio__queue_data(queue, struct faio_handle, pending_queue);
    faio__queue_remove(queue);

    if ((handle->events & POLLIN) != (handle->revents & POLLIN)) {
      if (handle->events & POLLIN)
        op = EV_ADD | EV_ENABLE;
      else
        op = EV_DELETE | EV_DISABLE;

      EV_SET(events + n, handle->fd, EVFILT_READ, op, 0, 0, handle);

      if (maxevents == (unsigned int) ++n) {
        kevent(loop->kq, events, n, NULL, 0, NULL);
        n = 0;
      }
    }

    if ((handle->events & POLLOUT) != (handle->revents & POLLOUT)) {
      if (handle->events & POLLOUT)
        op = EV_ADD | EV_ENABLE;
      else
        op = EV_DELETE | EV_DISABLE;

      EV_SET(events + n, handle->fd, EVFILT_WRITE, op, 0, 0, handle);

      if (maxevents == (unsigned int) ++n) {
        kevent(loop->kq, events, n, NULL, 0, NULL);
        n = 0;
      }
    }
  }

  if (n != 0)
    kevent(loop->kq, events, n, NULL, 0, NULL);

  if (timeout < 0)
    pts = NULL;
  else if (timeout == 0) {
    ts.tv_nsec = 0;
    ts.tv_sec = 0;
    pts = &ts;
  }
  else {
    ts.tv_nsec = (unsigned long) (timeout * 1e9) % 1000000000UL;
    ts.tv_sec = (unsigned long) timeout;
    pts = &ts;
  }

  if (pts != NULL)
    if (faio__gettime_monotonic(&before))
      abort();

  for (;;) {
    n = kevent(loop->kq, NULL, 0, events, maxevents, pts);

    if (n == 0)
      return;

    if (n == -1) {
      if (errno == EINTR)
        goto update_timeout;
      else
        abort();
    }

    for (i = 0; i < n; i++) {
      handle = (struct faio_handle *) events[i].udata;
      revents = 0;

      if (events[i].filter == EVFILT_READ)
        revents |= FAIO_POLLIN;
      if (events[i].filter == EVFILT_WRITE)
        revents |= FAIO_POLLOUT;
      if (events[i].flags & EV_ERROR)
        revents |= FAIO_POLLERR;
      if (events[i].flags & EV_EOF)
        revents |= FAIO_POLLHUP;

      handle->cb(loop, handle, revents);
    }

    /* We read as many events as we could but there might still be more.
     * Poll again but don't block this time.
     */
    if (maxevents == (unsigned int) n) {
      ts.tv_nsec = 0;
      ts.tv_sec = 0;
      pts = &ts;
      continue;
    }

    return;

update_timeout:
    if (pts == NULL)
      continue;

    if (ts.tv_sec == 0 && ts.tv_nsec == 0)
      return;

    if (faio__gettime_monotonic(&after))
      abort();

    FAIO_TIMESPEC_SUB(&after, &before, &diff);
    FAIO_TIMESPEC_SUB(&ts, &diff, &ts);

    if (ts.tv_sec < 0)
      return;

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
  events &= POLLIN | POLLOUT;
  events |= POLLERR | POLLHUP;

  faio__queue_append(&loop->pending_queue, &handle->pending_queue);
  handle->cb = cb;
  handle->fd = fd;
  handle->events = events;
  handle->revents = 0;

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static int faio_mod(struct faio_loop *loop,
                    struct faio_handle *handle,
                    unsigned int events)
{
  events &= POLLIN | POLLOUT;
  events |= POLLERR | POLLHUP;
  handle->revents = handle->events;
  handle->events = events;

  if (handle->events != handle->revents)
    if (faio__queue_empty(&handle->pending_queue))
      faio__queue_append(&loop->pending_queue, &handle->pending_queue);

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static int faio_del(struct faio_loop *loop, struct faio_handle *handle)
{
  struct kevent events[2];
  int fd;

  handle->revents = 0;
  handle->events = 0;

  if (!faio__queue_empty(&handle->pending_queue))
    faio__queue_remove(&handle->pending_queue);

  fd = handle->fd;
  EV_SET(events + 0, fd, EVFILT_READ, EV_DELETE | EV_DISABLE, 0, 0, handle);
  EV_SET(events + 1, fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, 0, 0, handle);

  return kevent(loop->kq, events, 2, NULL, 0, NULL);
}

#endif /* FAIO_KQUEUE_H_ */
