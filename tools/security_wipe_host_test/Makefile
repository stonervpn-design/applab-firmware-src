CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
SECURITY_WIPE_DIR := ../../applications/services/desktop/helpers

.PHONY: test clean

test: test_wipe
	./test_wipe

test_wipe: test_wipe.c $(SECURITY_WIPE_DIR)/security_wipe.h
	$(CC) $(CFLAGS) -o test_wipe test_wipe.c

clean:
	rm -f test_wipe
