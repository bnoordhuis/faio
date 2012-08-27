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

#include <errno.h>
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

struct faio_loop
{
  struct faio_handle *next_pending;
  int epoll_fd;
};

struct faio_handle
{
  struct faio_handle *next_pending;
  void (*cb)(struct faio_loop *, struct faio_handle *, unsigned int);
  unsigned int events;  /* What the user wants to get notified about. */
  unsigned int revents; /* What is actually active. */
  int fd;
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
  loop->next_pending = NULL;

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static void faio_fini(struct faio_loop *loop)
{
  close(loop->epoll_fd);
}

FAIO_ATTRIBUTE_UNUSED
static void faio_poll(struct faio_loop *loop, double timeout)
{
  struct epoll_event events[256]; /* 3 kB */
  struct faio_handle *handle;
  struct faio_handle *next;
  struct timespec before;
  struct timespec after;
  unsigned long elapsed;
  unsigned int dispatched;
  unsigned int maxevents;
  unsigned int revents;
  int ms;
  int i;
  int n;

  dispatched = 0;
  maxevents = sizeof(events) / sizeof(events[0]);

  if (loop->next_pending != NULL) {
    handle = loop->next_pending;
    loop->next_pending = NULL;

    for (/* empty */; handle != NULL; handle = next) {
      next = handle->next_pending;
      handle->next_pending = NULL;

      revents = handle->revents & handle->events;
      if (revents == 0)
        continue;

      handle->cb(loop, handle, revents);
      dispatched = 1;
    }

    if (dispatched)
      timeout = 0;
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
      handle = (struct faio_handle *) events[i].data.ptr;
      revents = events[i].events;
      handle->revents = revents;

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
      ms = 0;
      continue;
    }

    if (dispatched)
      return;

    /* We didn't invoke any callbacks, just updated some watchers.
     * From the perspective of the caller nothing happened so update
     * the timeout and poll again.
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
  struct epoll_event evt;

  events &= EPOLLIN | EPOLLOUT;
  events |= EPOLLERR | EPOLLHUP;

  handle->next_pending = NULL;
  handle->cb = cb;
  handle->fd = fd;
  handle->events = events;
  handle->revents = 0;

  evt.events = EPOLLIN | EPOLLOUT | EPOLLET;
  evt.data.ptr = handle;

  return epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &evt);
}

FAIO_ATTRIBUTE_UNUSED
static int faio_mod(struct faio_loop *loop,
                    struct faio_handle *handle,
                    unsigned int events)
{
  events &= EPOLLIN | EPOLLOUT;
  events |= EPOLLERR | EPOLLHUP;
  handle->events = events;

  if (0 == (events & handle->revents))
    return 0;

  if (handle->next_pending != NULL)
    return 0;

  if (loop->next_pending == handle)
    return 0;

  handle->next_pending = loop->next_pending;
  loop->next_pending = handle;

  return 0;
}

FAIO_ATTRIBUTE_UNUSED
static int faio_del(struct faio_loop *loop, struct faio_handle *handle)
{
  struct faio_handle *curr;

  if (loop->next_pending == handle)
    loop->next_pending = handle->next_pending;
  else if (handle->next_pending != NULL) {
    /* This kind of sucks. The handle to be deleted is pending so we need to
     * scan the list in O(n) fashion in order to remove it. Fortunately, the
     * list is usually short.
     */
    for (curr = loop->next_pending;
        curr->next_pending != handle;
        curr = curr->next_pending);
    curr->next_pending = handle->next_pending;
  }

  handle->next_pending = NULL;
  handle->events = 0;

  return epoll_ctl(loop->epoll_fd,
                   EPOLL_CTL_DEL,
                   handle->fd,
                   (struct epoll_event *) 1024); /* Work around kernel bug. */
}

#endif /* FAIO_EPOLL_H_ */
