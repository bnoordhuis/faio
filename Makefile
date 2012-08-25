CFLAGS	= -Wall -Wextra

all:	main.o
	$(CC) $^ -o epollet

clean:
	rm -f main.o epollet

.PHONY:	all clean
