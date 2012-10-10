CFLAGS	= -Wall -Wextra -g -O2
LDFLAGS	=

UNAME	:= $(shell uname)

ifeq ($(UNAME),Linux)
INCLUDE += faio-epoll.h
LDFLAGS += -lrt
endif

ifeq ($(UNAME),SunOS)
INCLUDE += faio-port.h
LDFLAGS += -lsocket -lnsl
endif

ifeq ($(UNAME),Darwin)
INCLUDE += faio-kqueue.h
endif

ifeq ($(UNAME),DragonFly)
INCLUDE += faio-kqueue.h
endif

ifeq ($(UNAME),FreeBSD)
INCLUDE += faio-kqueue.h
endif

ifeq ($(UNAME),NetBSD)
INCLUDE += faio-kqueue.h
endif

ifeq ($(UNAME),OpenBSD)
INCLUDE += faio-kqueue.h
endif

all:	bench.o
	$(CC) $^ -o bench $(LDFLAGS)

clean:
	rm -f bench.o bench

bench.o:	bench.c faio.h $(INCLUDE)

.PHONY:	all clean
