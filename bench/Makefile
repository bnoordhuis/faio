CFLAGS	= -Wall -Wextra -g -O2 -I../include
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

.PHONY:	all clean

all:	http-simple udp-simple

http-simple.o:	http-simple.c ../include/faio.h ../include/$(INCLUDE)
http-simple:	http-simple.o
	$(CC) $^ -o $@ $(LDFLAGS)

udp-simple.o:	udp-simple.c ../include/faio.h ../include/$(INCLUDE)
udp-simple:	udp-simple.o
	$(CC) $^ -o $@ $(LDFLAGS)

clean:
	rm -f http-simple http-simple.o udp-simple udp-simple.o
