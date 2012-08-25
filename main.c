#define _GNU_SOURCE /* accept4, etc. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

#include <netinet/in.h>

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

struct io
{
  void (*cb)(struct io *, unsigned int);
  int fd;
};

enum parse_state
{
  ps_new,
  ps_eol,
  ps_dead
};

struct write_req
{
  const void* buf;
  unsigned int len;
};

struct client
{
  struct io io;
  enum parse_state ps;
  struct write_req wr;
};

static int epoll_fd = -1;

static const char canned_response[] =
  "HTTP/1.0 200 OK\r\n"
  "Content-Length: 4\r\n"
  "Content-Type: text/plain\r\n"
  "\r\n"
  "OK\r\n";

__attribute__((noreturn))
static void sys_error(const char* what)
{
  fprintf(stderr, "%s: %s (errno=%d)\n", what, strerror(errno), errno);
  exit(42);
}

static int create_server(unsigned short port)
{
  struct sockaddr_in sin;
  int fd;
  int on;

  E(fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
  on = 1;
  E(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)));

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = INADDR_ANY;
  E(bind(fd, (const struct sockaddr *) &sin, sizeof(sin)));
  E(listen(fd, 1024));

  return fd;
}

static void io_add(struct io *w,
                   void (*cb)(struct io *, unsigned int),
                   int fd,
                   unsigned int events)
{
  struct epoll_event ev;
  w->cb = cb;
  w->fd = fd;
  ev.data.ptr = w;
  ev.events = events;
  E(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev));
}

static void io_mod(struct io *w, unsigned int events)
{
  struct epoll_event ev;
  ev.data.ptr = w;
  ev.events = events;
  E(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, w->fd, &ev));
}

static void io_poll(void)
{
  struct epoll_event events[256];
  int i;
  int n;

  do
    n = epoll_wait(epoll_fd, events, ARRAY_SIZE(events), -1);
  while (n == -1 && errno == EINTR);

  assert(n != 0);
  assert(n != -1);

  for (i = 0; i < n; i++) {
    struct epoll_event *ev = events + i;
    struct io *w = ev->data.ptr;
    w->cb(w, ev->events);
  }
}

static int client_read(struct client *c)
{
  char buf[1024];
  ssize_t n;
  ssize_t i;

again:
  assert(c->ps != ps_dead);

  do
    n = read(c->io.fd, buf, sizeof(buf));
  while (n == -1 && errno == EINTR);

  if (n == -1) {
    assert(errno == EAGAIN);
    return 0;
  }

  if (n == 0)
    return -1; // connection closed by peer

  for (i = 0; i < n; i++) {
    if (buf[i] == '\r')
      continue;

    if (buf[i] != '\n')
      c->ps = ps_new;
    else if (c->ps != ps_eol)
      c->ps = ps_eol;
    else {
      assert(i + 1 == n);
      c->ps = ps_dead;
      c->wr.buf = canned_response;
      c->wr.len = sizeof(canned_response) - 1;
      io_mod(&c->io, EPOLLOUT);
      break;
    }
  }

  if (n == sizeof(buf))
    goto again;

  return 0;
}

static int client_write(struct client *c)
{
  ssize_t n;

  do
    n = write(c->io.fd, c->wr.buf, c->wr.len);
  while (n == 0 && errno == EINTR);

  if (n == -1) {
    assert(errno == EAGAIN);
    return 0;
  }

  if (n == 0)
    return -1; // connection closed by peer

  c->wr.buf = (const char *) c->wr.buf + n;
  c->wr.len -= n;

  if (c->wr.len == 0)
    return -1;

  return 0;
}

static void client_cb(struct io *w, unsigned int revents)
{
  struct client *c = CONTAINER_OF(w, struct client, io);

  if (revents & (EPOLLERR | EPOLLHUP))
    goto err;

  if (revents & EPOLLIN)
    if (client_read(c))
      goto err;

  if (revents & EPOLLOUT)
    if (client_write(c))
      goto err;

  return;

err:
  close(c->io.fd);
  free(c);
}

static void accept_cb(struct io *w, unsigned int revents)
{
  struct client *c;
  int fd;

  assert(revents == EPOLLIN);

  while (-1 != (fd = accept4(w->fd, NULL, NULL, SOCK_NONBLOCK))) {
    c = calloc(1, sizeof(*c));
    io_add(&c->io, client_cb, fd, EPOLLIN | EPOLLET);
  }

  assert(errno == EAGAIN);
}

int main(void)
{
  struct io server_io;

  E(signal(SIGPIPE, SIG_IGN));
  E(epoll_fd = epoll_create1(0));
  io_add(&server_io, accept_cb, create_server(1234), EPOLLIN);
  for (;;) io_poll();

  return 0;
}
