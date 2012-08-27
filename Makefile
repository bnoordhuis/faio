CFLAGS	= -Wall -Wextra -g -O2

all:	main.o
	$(CC) $^ -o epollet

clean:
	rm -f main.o epollet

main.o:	main.c faio.h faio-epoll.h

.PHONY:	all clean
