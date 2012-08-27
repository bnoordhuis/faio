#define _GNU_SOURCE /* accept4, etc. */

#include "faio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/stat.h>
#include <sys/types.h>
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

enum parse_state
{
  ps_new,
  ps_eol,
  ps_eol_2,
  ps_error
};

struct write_req
{
  const void* buf;
  unsigned int len;
};

struct client
{
  struct faio_handle fh;
  enum parse_state ps;
  struct write_req wr;
};

static const char canned_response[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Length: 4\r\n"
  "Content-Type: text/plain\r\n"
  "Connection: Keep-Alive\r\n"
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

static int parse_req(enum parse_state ps, const char *buf, unsigned int len)
{
  const char *p;
  const char *pe;

  for (p = buf, pe = buf + len; p < pe; p++) {
    if (*p == '\r')
      ; /* skip */
    else if (*p != '\n')
      ps = ps_new;
    else if (ps != ps_eol)
      ps = ps_eol;
    else if (p + 1 == pe)
      ps = ps_eol_2;
    else {
      ps = ps_error;
      break;
    }
  }

  return ps;
}

static int client_read(struct faio_loop *loop, struct client *c)
{
  char buf[1024];
  ssize_t n;

  do {
    assert(c->ps != ps_error);

    do
      n = read(c->fh.fd, buf, sizeof(buf));
    while (n == -1 && errno == EINTR);

    if (n == -1) {
      assert(errno == EAGAIN);
      return 0;
    }

    if (n == 0)
      return -1; /* Connection closed by peer. */

    c->ps = parse_req(c->ps, buf, n);

    if (c->ps == ps_error)
      return -1;

    if (c->ps == ps_eol_2) {
      c->wr.buf = canned_response;
      c->wr.len = sizeof(canned_response) - 1;
      return faio_mod(loop, &c->fh, FAIO_POLLOUT);
    }
  }
  while (n == sizeof(buf));

  return 0;
}

static int client_write(struct faio_loop *loop, struct client *c)
{
  ssize_t n;

  do {
    assert(c->wr.buf != NULL);
    assert(c->wr.len != 0);

    do
      n = write(c->fh.fd, c->wr.buf, c->wr.len);
    while (n == 0 && errno == EINTR);

    if (n == -1) {
      assert(errno == EAGAIN);
      return 0;
    }

    if (n == 0)
      return -1; /* Connection closed by peer. */

    c->wr.buf = (const char *) c->wr.buf + n;
    c->wr.len -= n;
  }
  while (c->wr.len != 0);

  return faio_mod(loop, &c->fh, FAIO_POLLIN);
}

static void client_cb(struct faio_loop *loop,
                      struct faio_handle *fh,
                      unsigned int revents)
{
  struct client *c = CONTAINER_OF(fh, struct client, fh);

  if (revents & (FAIO_POLLERR | FAIO_POLLHUP))
    goto err;

  if (revents & FAIO_POLLIN)
    if (client_read(loop, c))
      goto err;

  if (revents & FAIO_POLLOUT)
    if (client_write(loop, c))
      goto err;

  return;

err:
  close(c->fh.fd);
  free(c);
}

static void accept_cb(struct faio_loop *loop,
                      struct faio_handle *fh,
                      unsigned int revents)
{
  struct client *c;
  int fd;

  assert(revents == FAIO_POLLIN);

  while (-1 != (fd = accept4(fh->fd, NULL, NULL, SOCK_NONBLOCK))) {
    c = calloc(1, sizeof(*c));

    if (c == NULL)
      abort();

    if (faio_add(loop, &c->fh, client_cb, fd, FAIO_POLLIN))
      abort();
  }

  assert(errno == EAGAIN);
}

int main(void)
{
  struct faio_handle server_handle;
  struct faio_loop main_loop;
  int server_fd;

  E(signal(SIGPIPE, SIG_IGN));

  server_fd = create_server(1234);
  if (server_fd == -1)
    abort();

  if (faio_init(&main_loop))
    abort();

  if (faio_add(&main_loop, &server_handle, accept_cb, server_fd, FAIO_POLLIN))
    abort();

  for (;;)
    faio_poll(&main_loop, -1);

  faio_fini(&main_loop);
  close(server_fd);

  return 0;
}
