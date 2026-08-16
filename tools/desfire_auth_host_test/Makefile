# tools/desfire_auth_host_test/Makefile
CC      ?= gcc
CRYPTO  := ../../components/nfc/protocols/mf_desfire
HELPERS := ../../applications/main/nfc/helpers
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -I$(CRYPTO) -I$(HELPERS) -DDESFIRE_HOST_TEST
LDLIBS  := -lmbedcrypto
SRC     := test_main.c test_attack.c virtual_card.c $(CRYPTO)/mf_desfire_crypto.c $(HELPERS)/mf_desfire_attack.c
BIN     := desfire_test

test: $(BIN)
	./$(BIN)

# virtual_card.c remains a stub until Task 4; it compiles as an empty
# translation unit until then.
$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDLIBS) -o $(BIN)

clean:
	rm -f $(BIN)
.PHONY: test clean
