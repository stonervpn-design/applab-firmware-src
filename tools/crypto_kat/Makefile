# tools/crypto_kat/Makefile
#
# Known-answer + invariant regression tests for the firmware's crypto
# primitives, compiled from the REAL, UNMODIFIED lib/subghz/protocols sources:
#
#   keeloq_common.c  - KeeLoq cipher + learning       (hadipourh/KeeLoq vectors)
#   aes_common.c     - AES-128 block cipher           (NIST FIPS-197 App. C.1)
#   hitag2_cipher.c  - Hitag2 stream cipher + RKE      (MIKRON vector)
#   aut64.c          - AUT64 block cipher              (invertibility + snapshot)
#
# keeloq_common.h and aes_common.h `#include "base.h"` (quote-form) which, in
# the real tree, drags in the whole furi_hal stack. Neither cipher uses any of
# it, so we stage byte-identical COPIES next to a stub base.h (shim/base.h) so
# the quoted include resolves to the stub. hitag2_cipher.c / aut64.c have no
# such dependency but are staged uniformly. The crypto sources are NEVER edited.
CC     ?= gcc
ROOT   := ../..
PROTO  := $(ROOT)/lib/subghz/protocols
STAGE  := stage
CFLAGS := -std=c11 -Wall -Wextra -g -Ishim -I$(STAGE)
BIN    := crypto_kat

STAGED := keeloq_common aes_common hitag2_cipher aut64
STAGED_SRC := $(addprefix $(STAGE)/,$(addsuffix .c,$(STAGED)))
REAL_SRC   := $(addprefix $(PROTO)/,$(addsuffix .c,$(STAGED)))
REAL_HDR   := $(addprefix $(PROTO)/,$(addsuffix .h,$(STAGED)))

test: $(BIN)
	./$(BIN)

$(BIN): test_crypto_kat.c $(REAL_SRC) $(REAL_HDR) shim/base.h shim/furi.h
	mkdir -p $(STAGE)
	cp $(REAL_SRC) $(REAL_HDR) $(STAGE)/
	cp shim/base.h $(STAGE)/base.h
	$(CC) $(CFLAGS) test_crypto_kat.c $(STAGED_SRC) -o $(BIN)

clean:
	rm -rf $(BIN) $(STAGE)

.PHONY: test clean
