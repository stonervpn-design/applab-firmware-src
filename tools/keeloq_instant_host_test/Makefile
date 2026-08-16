# tools/keeloq_instant_host_test/Makefile
#
# Host round-trip test for applications/main/subghz/helpers/keeloq_instant.c
# against the REAL lib/subghz/protocols/keeloq_common.c crypto.
#
# keeloq_common.c's own header (keeloq_common.h) does `#include "base.h"`
# (quote-form), which -- because base.h lives right next to keeloq_common.h
# in lib/subghz/protocols/ -- always resolves to the REAL base.h regardless
# of -I ordering, and that drags in the entire furi_hal/flipper_format
# stack via ../types.h. keeloq_common.c does not need any of that (verified:
# it uses no furi_*/FURI_LOG_*/m-array symbol), so we stage a byte-identical
# COPY of the real keeloq_common.{c,h} into stage/ next to a trivial stub
# base.h (shim/base.h) so the quoted include resolves to the stub instead.
# The crypto source itself is never modified, only copied.
CC      ?= gcc
ROOT    := ../..
PROTO   := $(ROOT)/lib/subghz/protocols
HELPERS := $(ROOT)/applications/main/subghz/helpers
STAGE   := stage

CFLAGS := -std=c11 -Wall -Wextra -Werror -g -Ishim
BIN    := keeloq_instant_test

SRC := test_keeloq_instant.c $(HELPERS)/keeloq_instant.c $(STAGE)/keeloq_common.c

test: $(BIN)
	./$(BIN)

$(STAGE)/keeloq_common.c: $(PROTO)/keeloq_common.c $(PROTO)/keeloq_common.h shim/base.h shim/furi.h shim/m-array.h
	mkdir -p $(STAGE)
	cp $(PROTO)/keeloq_common.c $(STAGE)/keeloq_common.c
	cp $(PROTO)/keeloq_common.h $(STAGE)/keeloq_common.h
	cp shim/base.h $(STAGE)/base.h

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN)

clean:
	rm -rf $(BIN) $(STAGE)

.PHONY: test clean
