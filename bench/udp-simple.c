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

#include "faio.h"
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>

struct client_handle
{
  struct faio_handle faio_handle;
};

static unsigned int num_concurrent = 1;
static unsigned int num_packets = 1;
static unsigned int num_bytes = 1024;
static int use_child_proc = 0; /* Start clients in a separate process? */
static int use_keep_alive = 0; /* Reuse socket? */

static const char *progname; /* Set in main(). */

static void usage(void)
{
  fprintf(stderr, "%s [-b <size>] [-c <num>] [-k] [-n <num>] [-x]\n", progname);
  exit(1);
}

static void parse_opts(int argc, char **argv)
{
  int opt;

  while (-1 != (opt = getopt(argc, argv, "c:kn:x"))) {
    switch (opt) {
    case 'b':
      num_bytes = atoi(optarg);
      break;

    case 'c':
      num_concurrent = atoi(optarg);
      break;

    case 'k':
      use_keep_alive = 1;
      break;

    case 'n':
      num_packets = atoi(optarg);
      break;

    case 'x':
      use_child_proc = 1;
      break;

    default:
      usage();
    }
  }

  if (num_concurrent == 0)
    usage();

  if (num_packets == 0)
    usage();
}

static char scratch[65536];

static int send_buf(int fd, struct sockaddr_in to, unsigned int size)
{
  struct msghdr msg;
  struct iovec vec;
  ssize_t n;

  vec.iov_base = scratch;
  vec.iov_len = size;

  memset(&msg, 0, sizeof(msg));
  msg.msg_name = &to;
  msg.msg_namelen = sizeof(to);
  msg.msg_iov = &vec;
  msg.msg_iovlen = 1;
  msg.msg_control = NULL;

  n = sendmsg(fd, &msg, 0);
  if (n == size)
    return 0;
  else
    return -1;
}

static unsigned int autodetect_localhost_mtu(unsigned int max)
{
  struct sockaddr_in addr;
  socklen_t addrlen;
  unsigned int size;
  unsigned int min;
  void *buf;
  int fd;

  E(fd = socket(AF_INET, SOCK_DGRAM, 0));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  E(bind(fd, (struct sockaddr *) &addr, sizeof(addr)));

  addrlen = sizeof(addr);
  E(getsockname(fd, (struct sockaddr *) &addr, &addrlen));

  buf = malloc(max);
  if (buf == NULL)
    abort();

  min = 0;

  while (min + 1 < max) {
    size = (min + max) / 2;

    if (send_buf(fd, addr, size) == 0)
      min = size;
    else if (errno == EMSGSIZE)
      max = size;
    else
      abort();
  }

  free(buf);
  close(fd);

  return size;
}

static void client_send(struct faio_loop *loop,
                        struct client_handle *handle)
{
}

static void client_cb(struct faio_loop *loop,
                      struct faio_handle *handle,
                      unsigned int events)
{
}

static void pummel(void)
{
  struct client_handle *handles;
  struct client_handle *handle;
  unsigned int i;
  int fd;

  if (NULL == (handles = calloc(num_concurrent, sizeof(handles[0]))))
    abort();

  for (i = 0; i < num_concurrent; i++) {
    handle = handles + i;
    E(fd = nb_socket(AF_INET, SOCK_DGRAM, 0));
  }
}

static void start_child_proc(void)
{
  pid_t pid;

  E(pid = fork());

  if (pid == 0)
    pummel();
}

static void server_cb(struct faio_loop *loop,
                      struct faio_handle *handle,
                      unsigned int events)
{
}

int main(int argc, char **argv)
{
  struct faio_handle server_handle;
  struct faio_loop main_loop;
  unsigned int mtu_len;
  int server_fd;

  progname = argv[0];
  parse_opts(argc, argv);
  E(signal(SIGPIPE, SIG_IGN));

  mtu_len = autodetect_localhost_mtu(1 << 18); /* 256 kB */
  printf("localhost MTU is %u\n", mtu_len);
  return 0;

  server_fd = create_inet_server(SOCK_DGRAM, 1234);
  if (server_fd == -1)
    abort();

  if (faio_init(&main_loop))
    abort();

  if (faio_add(&main_loop, &server_handle, server_cb, server_fd, FAIO_POLLIN))
    abort();

  if (use_child_proc) {
    start_child_proc();
    exit(0);
  }

  pummel();

  for (;;)
    faio_poll(&main_loop, 1000);

  faio_fini(&main_loop);
  close(server_fd);

  return 0;
}
