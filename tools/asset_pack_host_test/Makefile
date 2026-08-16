CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
ASSET_PACK_DIR := ../../applications/services/desktop/helpers

.PHONY: test clean

test: test_asset_pack
	./test_asset_pack

test_asset_pack: test_asset_pack.c $(ASSET_PACK_DIR)/asset_pack.h
	$(CC) $(CFLAGS) -o test_asset_pack test_asset_pack.c

clean:
	rm -f test_asset_pack
