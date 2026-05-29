CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2

TINYALSA_DIR   = tinyalsa
TINYALSA_SRC   = $(TINYALSA_DIR)/src/pcm.c $(TINYALSA_DIR)/src/pcm_hw.c
TINYALSA_OBJ   = $(TINYALSA_SRC:.c=.o)
TINYALSA_LIB   = $(TINYALSA_DIR)/libtinyalsa.a
TINYALSA_CFLAGS = -I$(TINYALSA_DIR)/include -I$(TINYALSA_DIR)/src

all: tinyloop alsalist

$(TINYALSA_OBJ): %.o: %.c
	$(CC) $(CFLAGS) $(TINYALSA_CFLAGS) -c -o $@ $<

$(TINYALSA_LIB): $(TINYALSA_OBJ)
	$(AR) rcs $@ $^

tinyloop: CFLAGS += -I$(TINYALSA_DIR)/include
tinyloop: tinyloop.o ringbuf.o $(TINYALSA_LIB)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

tinyloop.o: tinyloop.c ringbuf.h

alsalist: CFLAGS += -I$(TINYALSA_DIR)/include
alsalist: alsalist.c $(TINYALSA_LIB)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

ringbuf.o: ringbuf.c ringbuf.h

PREFIX ?= /usr/local

.PHONY: all clean install

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 tinyloop $(DESTDIR)$(PREFIX)/bin/
	install -m 755 alsalist $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f tinyloop alsalist *.o $(TINYALSA_OBJ) $(TINYALSA_LIB)
