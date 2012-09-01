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

#ifndef FAIO_PORT_H_
#define FAIO_PORT_H_

#include "faio-util.h"

#include <errno.h>
#include <stdlib.h>

#include <poll.h>
#include <port.h>
#include <time.h>
#include <unistd.h>

#define FAIO_POLLIN   POLLIN
#define FAIO_POLLOUT  POLLOUT
#define FAIO_POLLERR  POLLERR
#define FAIO_POLLHUP  POLLHUP

struct faio_loop
{
  struct faio__queue pending_queue;
  int port_fd;
};

struct faio_handle
{
  struct faio__queue pending_queue;
  void (*cb)(struct faio_loop *, struct faio_handle *, unsigned int);
  int events;
  int fd;
};

FAIO_ATTRIBUTE_UNUSED
static int faio_init(struct faio_loop *loop)
{
  int port_fd;

  if (-1 == (port_fd = port_create()))
    return -1;

  loop->port_fd = port_fd;
  faio__queue_init(&loop->pending_queue);

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static void faio_fini(struct faio_loop *loop)
{
  close(loop->port_fd);
  loop->port_fd = -1;
}

FAIO_ATTRIBUTE_UNUSED
static unsigned int faio__port_poll_nb(struct faio_loop *loop)
{
  struct port_event events[256];
  struct faio_handle *handle;
  struct timespec timeout;
  unsigned int maxevents;
  unsigned int nevents;
  unsigned int i;

  timeout.tv_sec = 0;
  timeout.tv_nsec = 0;

  maxevents = sizeof(events) / sizeof(events[0]);
  nevents = maxevents;

  /* Work around kernel bug where nevents is not updated. */
  events[0].portev_source = 0;

  if (port_getn(loop->port_fd, events, maxevents, &nevents, &timeout))
    if (errno != EINTR && errno != ETIME)
      abort();

  if (events[0].portev_source == 0)
    return 0;

  for (i = 0; i < nevents; i++) {
    handle = (struct faio_handle *) events[i].portev_user;

    if (faio__queue_empty(&handle->pending_queue))
      faio__queue_append(&loop->pending_queue, &handle->pending_queue);

    handle->cb(loop, handle, events[i].portev_events);
  }

  return nevents;
}

FAIO_ATTRIBUTE_UNUSED
static void faio__port_poll(struct faio_loop *loop, struct timespec *timeout)
{
  struct port_event events[256];
  struct faio_handle *handle;
  struct timespec before;
  struct timespec after;
  struct timespec diff;
  unsigned int maxevents;
  unsigned int nevents;
  unsigned int i;
  int saved_errno;

  /* Try a non-blocking poll first. If it fetches events, good - we can
   * bail out now and skip a few syscalls.
   */
  if (faio__port_poll_nb(loop))
    return;

  if (clock_gettime(CLOCK_MONOTONIC, &before))
    abort();

  for (;;) {
    maxevents = sizeof(events) / sizeof(events[0]);
    nevents = 1;

    /* Work around kernel bug where nevents is not updated. */
    events[0].portev_source = 0;

    if (port_getn(loop->port_fd, events, maxevents, &nevents, timeout) == 0)
      saved_errno = 0;
    else if (errno == EINTR || errno == ETIME)
      saved_errno = errno;
    else
      abort();

    if (events[0].portev_source == 0)
      return;

    for (i = 0; i < nevents; i++) {
      handle = (struct faio_handle *) events[i].portev_user;

      if (faio__queue_empty(&handle->pending_queue))
        faio__queue_append(&loop->pending_queue, &handle->pending_queue);

      handle->cb(loop, handle, events[i].portev_events);
    }

    if (nevents > 0)
      return;

    if (saved_errno == ETIME)
      return;

    if (timeout == NULL)
      continue;

    /* Update timeout after EINTR. */
    if (clock_gettime(CLOCK_MONOTONIC, &after))
      abort();

    FAIO_TIMESPEC_SUB(&after, &before, &diff);
    FAIO_TIMESPEC_SUB(timeout, &diff, timeout);

    if (timeout->tv_sec < 0)
      return;

    before = after;
  }
}

FAIO_ATTRIBUTE_UNUSED
static void faio_poll(struct faio_loop *loop, double timeout)
{
  struct faio_handle *handle;
  struct faio__queue *queue;

  while (!faio__queue_empty(&loop->pending_queue)) {
    queue = faio__queue_head(&loop->pending_queue);
    handle = faio__queue_data(queue, struct faio_handle, pending_queue);
    faio__queue_remove(queue);
    port_associate(loop->port_fd,
                   PORT_SOURCE_FD,
                   handle->fd,
                   handle->events,
                   handle);
  }

  if (timeout == 0)
    faio__port_poll_nb(loop);
  else if (timeout < 0)
    faio__port_poll(loop, NULL);
  else {
    struct timespec ts;
    ts.tv_nsec = (unsigned long) (timeout * 1e9) % 1000000000UL;
    ts.tv_sec = (unsigned long) timeout;
    faio__port_poll(loop, &ts);
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

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static int faio_mod(struct faio_loop *loop,
                    struct faio_handle *handle,
                    unsigned int events)
{
  events &= POLLIN | POLLOUT;
  events |= POLLERR | POLLHUP;
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

  return port_dissociate(loop->port_fd, PORT_SOURCE_FD, handle->fd);
}

#endif /* FAIO_PORT_H_ */
