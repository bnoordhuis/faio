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
#include <mach/mach_time.h>
#include <unistd.h>

#define FAIO_POLLIN   0x1
#define FAIO_POLLOUT  0x2
#define FAIO_POLLERR  0x4
#define FAIO_POLLHUP  0x8

struct faio_loop
{
  int kq;
  struct faio__queue pending_queue;
};

struct faio_handle
{
  struct faio__queue pending_queue;
  void (*cb)(struct faio_loop *, struct faio_handle *, unsigned int);
  unsigned int events;  /* What the user wants to get notified about. */
  unsigned int revents; /* What is actually active. */
  int fd;
};

FAIO_ATTRIBUTE_UNUSED
static int faio_init(struct faio_loop *loop)
{
  int kq;

  kq = kqueue();

  if (kq == -1)
    return -1;

  loop->kq = kq;

  faio__queue_init(&loop->pending_queue);

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
  struct kevent events[96]; /* 3 kB */
  struct faio_handle *handle;
  struct faio__queue *queue;
  mach_timebase_info_t tb;
  uint64_t before;
  uint64_t after;
  uint64_t diff;
  struct timespec ts;
  unsigned int dispatched;
  unsigned int maxevents;
  unsigned int revents;
  int i;
  int n;

  dispatched = 0;
  maxevents = sizeof(events) / sizeof(events[0]);

  while (!faio__queue_empty(&loop->pending_queue)) {
    queue = faio__queue_head(&loop->pending_queue);
    handle = faio__queue_data(queue, struct faio_handle, pending_queue);
    faio__queue_remove(queue);

    revents = handle->revents & handle->events;
    if (revents == 0)
      continue;

    handle->cb(loop, handle, revents);
    dispatched = 1;
    timeout = 0;
  }

  ts.tv_nsec = (unsigned long) (timeout * 1e9) % 1000000000UL;
  ts.tv_sec = (unsigned long) timeout;

  tb = NULL;
  if (mach_timebase_info((mach_timebase_info_t) &tb) != KERN_SUCCESS)
    abort();

  before = mach_absolute_time();

  for (;;) {
    n = kevent(loop->kq,
               NULL,
               0,
               events,
               maxevents,
               (ts.tv_sec < 0 || ts.tv_nsec < 0) ? NULL : &ts);

    if (n == 0) return;

    if (n == -1) {
      if (errno == EINTR)
        goto update_timeout;
      else
        abort();
    }

    for (i = 0; i < n; i++) {
      handle = (struct faio_handle *) events[i].udata;
      revents = 0;
      handle->revents = revents;

      if (events[i].filter == EVFILT_READ)
        revents |= FAIO_POLLIN;
      if (events[i].filter == EVFILT_WRITE)
        revents |= FAIO_POLLOUT;
      if (events[i].flags & EV_ERROR)
        revents |= FAIO_POLLERR;
      if (events[i].flags & EV_EOF)
        revents |= FAIO_POLLHUP;

      revents &= handle->events;
      if (revents == 0)
        continue;

      handle->cb(loop, handle, revents);
      dispatched = 1;
    }

    /* We read as many events as we could but there might still be more.
     * Poll again but don't block this time.
     */
    if (maxevents == (unsigned int) n) {
      ts.tv_sec = 0;
      ts.tv_nsec = 0;
      continue;
    }

    if (dispatched)
      return;

update_timeout:
    /* Update timeout after EINTR. */
    after = mach_absolute_time();

    diff = after - before;
    diff *= tb->numer;
    diff /= tb->denom;

    ts.tv_sec -= diff / 1000000000UL;
    ts.tv_nsec -= diff % 1000000000UL;

    if (ts.tv_sec < 0 || ts.tv_nsec < 0)
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
  struct kevent evt[2];
  struct timespec ts;
  int evtn;

  events &= FAIO_POLLIN | FAIO_POLLOUT | FAIO_POLLERR | FAIO_POLLHUP;

  faio__queue_init(&handle->pending_queue);
  handle->cb = cb;
  handle->fd = fd;
  handle->events = events;
  handle->revents = 0;

  evtn = 0;
  EV_SET(&evt[evtn++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, handle);
  EV_SET(&evt[evtn++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, handle);

  if (evtn == 0) return 0;

  ts.tv_sec = 0;
  ts.tv_nsec = 0;
  return kevent(loop->kq, evt, evtn, NULL, 0, &ts);
}

FAIO_ATTRIBUTE_UNUSED
static int faio_mod(struct faio_loop *loop,
                    struct faio_handle *handle,
                    unsigned int events)
{
  events &= FAIO_POLLIN | FAIO_POLLOUT | FAIO_POLLERR | FAIO_POLLHUP;
  handle->events = events;

  if (0 == (events & handle->revents))
    return 0;

  if (faio__queue_empty(&handle->pending_queue))
    faio__queue_append(&loop->pending_queue, &handle->pending_queue);

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static int faio_del(struct faio_loop *loop, struct faio_handle *handle)
{
  struct kevent evt[2];
  struct timespec ts;
  int fd;

  handle->events = 0;

  if (!faio__queue_empty(&handle->pending_queue))
    faio__queue_remove(&handle->pending_queue);

  fd = handle->fd;
  EV_SET(&evt[0], fd, EVFILT_READ, EV_DELETE | EV_DISABLE, 0, 0, handle);
  EV_SET(&evt[1], fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, 0, 0, handle);

  ts.tv_sec = 0;
  ts.tv_nsec = 0;
  return kevent(loop->kq, evt, 2, NULL, 0, &ts);
}

#endif /* FAIO_KQUEUE_H_ */
