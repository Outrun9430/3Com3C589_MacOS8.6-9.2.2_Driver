# Host-side logic test for the driver (any Unix with a C compiler).
CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas

test: test_el3
	./test_el3

test_el3: test_el3.c ../src/EL3PCCard.c ../src/EL3Reg.h ../src/EL3IO.h host_stubs.h
	$(CC) $(CFLAGS) -DEL3_HOST_TEST -I. -I../src -o $@ test_el3.c ../src/EL3PCCard.c

clean:
	rm -f test_el3

.PHONY: test clean
