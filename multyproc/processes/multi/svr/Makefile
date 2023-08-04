#
# Author: Genie (C) 2022
#
# This file has been put into the public domain.
# You can do whatever you want with this file.
#

CC = gcc
CFLAGS = -O2 -g
LDFLAGS = -lpthread

PROGS = \
	server \

all: $(PROGS)

.c:
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	-rm -f $(PROGS)
	
run:
	./svr_proc 0.0.0.0 9000
