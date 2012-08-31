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

#define _GNU_SOURCE /* accept4, etc. */

#include "faio.h"
#include "bench.h"

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

enum parse_state
{
  ps_new,
  ps_eol,
  ps_eol_2,
  ps_connection_c,
  ps_connection_co,
  ps_connection_con,
  ps_connection_conn,
  ps_connection_conne,
  ps_connection_connec,
  ps_connection_connect,
  ps_connection_connecti,
  ps_connection_connectio,
  ps_connection_connection,
  ps_connection_connection_,
  ps_connection_connection__,
  ps_connection_connection__k,
  ps_connection_connection__ke,
  ps_connection_connection__kee,
  ps_connection_connection__keep,
  ps_connection_connection__keep_,
  ps_connection_connection__keep_a,
  ps_connection_connection__keep_al,
  ps_connection_connection__keep_ali,
  ps_connection_connection__keep_aliv,
  ps_connection_connection__keep_alive,
  ps_connection_connection__keep_alive_,
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
  unsigned int keep_alive:1;
};

static const char keepalive_response[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Length: 4\r\n"
  "Content-Type: text/plain\r\n"
  "Connection: keep-alive\r\n"
  "\r\n"
  "OK\r\n";

static const char connection_close_response[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Length: 4\r\n"
  "Content-Type: text/plain\r\n"
  "Connection: close\r\n"
  "\r\n"
  "OK\r\n";

#if defined(__linux__)

static int nb_socket(int family, int type, int proto)
{
  return socket(family, type | SOCK_NONBLOCK, proto);
}

static int nb_accept(int fd, struct sockaddr *saddr, socklen_t *slen)
{
  return accept4(fd, saddr, slen, SOCK_NONBLOCK);
}

#else /* !defined(__linux__) */

#include <sys/filio.h>
#include <sys/ioctl.h>

static void nbio(int fd)
{
  int on;

  on = 1;
  E(ioctl(fd, FIONBIO, &on));
}

static int nb_socket(int family, int type, int proto)
{
  int fd;

  fd = socket(family, type, proto);
  if (fd != -1)
    nbio(fd);

  return fd;
}

static int nb_accept(int sfd, struct sockaddr *saddr, socklen_t *slen)
{
  int fd;

  fd = accept(sfd, saddr, slen);
  if (fd != -1)
    nbio(fd);

  return fd;
}

#endif /* defined(__linux__) */

static int create_server(unsigned short port)
{
  struct sockaddr_in sin;
  int fd;
  int on;

  E(fd = nb_socket(AF_INET, SOCK_STREAM, 0));
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

static int client_parse(struct client *c, const char *buf, unsigned int len)
{
  enum parse_state ps;
  unsigned char ch;
  unsigned int i;

  ps = c->ps;

  for (i = 0; i < len; i++) {
    ch = buf[i];

    if (ch == '\r')
      continue;

    if (ch >= 'A' && ch <= 'Z')
      ch += 'a' - 'A';

    if (ch == 'c' && ps == ps_eol) {
      ps = ps_connection_co;
      continue;
    }

    if (ps >= ps_connection_c &&
        ps <= ps_connection_connection__keep_alive_ &&
        ch == "connection: keep-alive\n"[ps - ps_connection_c])
    {
      if (ch != '\n')
        ps += 1;
      else {
        ps = ps_eol;
        c->keep_alive = 1;
      }
      continue;
    }

    if (ch != '\n')
      ps = ps_new;
    else if (ps != ps_eol)
      ps = ps_eol;
    else if (i + 1 == len)
      ps = ps_eol_2;
    else
      return -1;
  }

  c->ps = ps;

  return 0;
}

static void client_send_response(struct client *c)
{
  if (c->keep_alive) {
    c->wr.buf = keepalive_response;
    c->wr.len = sizeof(keepalive_response) - 1;
  }
  else {
    c->wr.buf = connection_close_response;
    c->wr.len = sizeof(connection_close_response) - 1;
  }
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

    if (client_parse(c, buf, n))
      return -1;

    if (c->ps == ps_eol_2) {
      client_send_response(c);
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

  if (c->keep_alive == 0)
    return -1;

  c->keep_alive = 0;
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
  faio_del(loop, fh);
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

  while (-1 != (fd = nb_accept(fh->fd, NULL, NULL))) {
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
