CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  = -ltinyalsa -lpthread

all: tinyloop alsalist

tinyloop: main.o ringbuf.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

alsalist: alsalist.c
	$(CC) $(CFLAGS) -o $@ $^ -ltinyalsa

main.o: main.c ringbuf.h
ringbuf.o: ringbuf.c ringbuf.h

.PHONY: all clean
clean:
	rm -f tinyloop alsalist *.o
