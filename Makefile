CFLAGS	= -Wall -Wextra

all:	main.o
	$(CC) $^ -o epollet

clean:
	rm -f main.o epollet

main.o:	main.c faio.h faio-epoll.h

.PHONY:	all clean
