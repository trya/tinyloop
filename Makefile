CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  = -ltinyalsa -lpthread

all: tinyloop alsalist

tinyloop: tinyloop.o ringbuf.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tinyloop.o: tinyloop.c ringbuf.h

alsalist: alsalist.c
	$(CC) $(CFLAGS) -o $@ $^ -ltinyalsa

ringbuf.o: ringbuf.c ringbuf.h

.PHONY: all clean
clean:
	rm -f tinyloop alsalist *.o
