# tools/desfire_auth_host_test/Makefile
CC      ?= gcc
CRYPTO  := ../../components/nfc/protocols/mf_desfire
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -I$(CRYPTO) -DDESFIRE_HOST_TEST
LDLIBS  := -lmbedcrypto
SRC     := test_main.c
BIN     := desfire_test

test: $(BIN)
	./$(BIN)

# NOTE: crypto.c / virtual_card.c are empty stubs for Task 1 only; they are
# intentionally left out of this recipe so the link doesn't dangle on unused
# empty translation units. Restored in Task 2 / Task 4.
$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDLIBS) -o $(BIN)

clean:
	rm -f $(BIN)
.PHONY: test clean
