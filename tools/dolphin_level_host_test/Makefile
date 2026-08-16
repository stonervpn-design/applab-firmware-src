CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
DOLPHIN_LEVEL_DIR := ../../applications/services/dolphin/helpers

.PHONY: test clean

test: test_level
	./test_level

test_level: test_level.c $(DOLPHIN_LEVEL_DIR)/dolphin_level.c $(DOLPHIN_LEVEL_DIR)/dolphin_level.h
	$(CC) $(CFLAGS) -o test_level test_level.c

clean:
	rm -f test_level
