#
# Author: Genie (C) 2023
#
# This file has been put into the public domain.
# You can do whatever you want with this file.
#

CC = gcc
CFLAGS = -O2 -g
#LDFLAGS = -llzma -larchive

PROGS = \
	client \

all: $(PROGS)

.c:
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	-rm -f $(PROGS)
	
run:
	./client 127.0.0.1 9000
