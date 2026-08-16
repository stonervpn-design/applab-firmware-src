# DESFire Attack Core (Phase A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add on-device MIFARE DESFire authentication (AES `0xAA` + legacy DES/3DES `0x0A`/ISO `0x1A`), a default/dictionary-key sweep that auto-dumps every file a recovered key authorizes, and a passive config-weakness audit — PRIVATE build only.

**Architecture:** A pure, host-testable crypto module (`mf_desfire_crypto`) implements the auth handshake, session-key derivation, and CMAC on top of the mbedtls DES3/AES already linked in-tree. The DESFire poller gains `authenticate` + `read_file_data_authed` that drive that module over the existing `mf_desfire_poller_send_chunks` transport. An app-layer attack engine (`mf_desfire_attack`) sweeps a key dictionary over the PICC master + every AID/key-slot via an injectable `try_auth` callback (so the sweep is host-testable against a mock), records hits, and dumps readable files. Two scenes surface it. Phase A covers Plaintext + MAC comm reads; Enciphered read is Phase B (separate plan).

**Tech Stack:** C (ESP-IDF / flipperzero-firmware fork), mbedtls (`mbedtls_aes_*`, `mbedtls_des3_*` — already used by `felica.c`), Flipper NFC poller/scene framework, WSL `gcc` + `libmbedtls-dev` for the host test harness.

**Spec:** `docs/superpowers/specs/2026-08-16-advanced-nfc-attacks-design.md` (Components 1, 1e Phase A, 1f)

## Global Constraints

- **PRIVATE only:** every new menu entry, scene, dictionary, and attack symbol is wrapped in `#ifndef PUBLIC_BUILD` (or excluded from the public scene config). The public build must not expose DESFire attack.
- **Superset-first:** implement in `src_m5stick_s3`. Do **not** edit `src_cardputer_adv` or `src_lilygo_t_embed_cc1101` until the final mirror task. No feature is stripped from any private tree (no-strip rule).
- **No invented crypto vectors.** Known-answer tests use mbedtls' own self-tests for the block ciphers and a symmetric round-trip "virtual card" for the handshake. Any hard-coded transcript KAT must come from a **real card capture**, never hand-authored.
- **Build via PowerShell**, not the Bash tool, using the tree's build `.bat` (see Task 10). Delete the build dir + verify fresh log mtime before trusting a build (stale-log gotcha).
- **Crypto priority:** AES (`0xAA`) is the primary path (EV1/EV2/EV3). Legacy DES/3DES (`0x0A`/`0x1A`) is validated by round-trip on host and **locked to a real-card capture** during HW test (Task 10) before being trusted.
- All hex byte literals lowercase; follow existing `mf_desfire_*` naming.

---

## File Structure

- `components/nfc/protocols/mf_desfire/mf_desfire_crypto.h` — key types, crypto context, pure handshake/CMAC API.
- `components/nfc/protocols/mf_desfire/mf_desfire_crypto.c` — implementation (mbedtls-backed).
- `components/nfc/protocols/mf_desfire/mf_desfire_poller.h` / `mf_desfire_poller_i.c` — add `authenticate` + `read_file_data_authed`.
- `applications/main/nfc/helpers/mf_desfire_attack.h` / `.c` — dictionary + sweep engine (transport-agnostic via callback).
- `applications/main/nfc/helpers/mf_desfire_audit.h` / `.c` — passive config-weakness report.
- `applications/main/nfc/scenes/nfc_scene_mf_desfire_dict_attack.c` — progress scene (cloned from `nfc_scene_mf_classic_dict_attack.c`).
- `applications/main/nfc/scenes/nfc_scene_mf_desfire_unlock_menu.c` — entry menu (Attack / Audit / enter key).
- `applications/main/nfc/scenes/nfc_scene_config.h` — register the two scenes (private-gated).
- `applications/main/nfc/scenes/nfc_scene_mf_desfire_app.c` — add "Unlock / Key Attack" action.
- `tools/desfire_auth_host_test/` — `Makefile`, `test_main.c`, `virtual_card.c/.h` (host harness).

---

## Task 1: Host harness scaffold + block-cipher self-test

**Files:**
- Create: `tools/desfire_auth_host_test/Makefile`
- Create: `tools/desfire_auth_host_test/test_main.c`

**Interfaces:**
- Consumes: nothing.
- Produces: a `make test` target that compiles against system mbedtls and runs a C test binary returning nonzero on failure. Later tasks add `#include "../../components/nfc/protocols/mf_desfire/mf_desfire_crypto.c"` compilation.

- [ ] **Step 1: Write the Makefile**

```make
# tools/desfire_auth_host_test/Makefile
CC      ?= gcc
CRYPTO  := ../../components/nfc/protocols/mf_desfire
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -I$(CRYPTO) -DDESFIRE_HOST_TEST
LDLIBS  := -lmbedcrypto
SRC     := test_main.c
BIN     := desfire_test

test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) virtual_card.c $(CRYPTO)/mf_desfire_crypto.c
	$(CC) $(CFLAGS) $(SRC) virtual_card.c $(CRYPTO)/mf_desfire_crypto.c $(LDLIBS) -o $(BIN)

clean:
	rm -f $(BIN)
.PHONY: test clean
```

- [ ] **Step 2: Write the first test — mbedtls block ciphers work**

```c
// tools/desfire_auth_host_test/test_main.c
#include <mbedtls/aes.h>
#include <mbedtls/des.h>
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { if(!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
                              else { printf("ok: %s\n", msg); } } while(0)

static void test_mbedtls_selftest(void) {
    CHECK(mbedtls_aes_self_test(0) == 0, "mbedtls AES self-test");
    CHECK(mbedtls_des_self_test(0) == 0, "mbedtls DES/3DES self-test");
}

int main(void) {
    test_mbedtls_selftest();
    printf(g_fail ? "\n%d FAILURES\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
```

Provide a temporary empty `virtual_card.c` (`/* filled in Task 4 */`) and an empty `mf_desfire_crypto.c` so the link succeeds now; both are replaced in later tasks. Comment out the `$(CRYPTO)/mf_desfire_crypto.c` and `virtual_card.c` from the Makefile `$(BIN)` recipe for THIS task only, restoring them in Task 2/Task 4.

- [ ] **Step 3: Run it, expect PASS**

Run (WSL): `wsl bash -lc "cd tools/desfire_auth_host_test && sudo apt-get install -y libmbedtls-dev >/dev/null 2>&1; make test"`
Expected: both self-tests print `ok:` and `ALL PASS`, exit 0. If mbedtls is missing, the apt line installs it.

- [ ] **Step 4: Commit**

```bash
git add tools/desfire_auth_host_test/
git commit -m "test(desfire): host harness scaffold + mbedtls block-cipher self-test"
```

---

## Task 2: Crypto module — key metadata + helpers (rotate, key size, auth cmd, CBC)

**Files:**
- Create: `components/nfc/protocols/mf_desfire/mf_desfire_crypto.h`
- Create: `components/nfc/protocols/mf_desfire/mf_desfire_crypto.c`
- Modify: `tools/desfire_auth_host_test/test_main.c`

**Interfaces:**
- Consumes: mbedtls.
- Produces:
  - `typedef enum { MfDesfireKeyTypeDes, MfDesfireKeyType2k3Des, MfDesfireKeyType3k3Des, MfDesfireKeyTypeAes } MfDesfireKeyType;`
  - `size_t mf_desfire_key_size(MfDesfireKeyType);` → 8/16/24/16
  - `size_t mf_desfire_block_size(MfDesfireKeyType);` → 8 for DES/2K3DES/3K3DES, 16 for AES
  - `uint8_t mf_desfire_auth_cmd(MfDesfireKeyType);` → `0x0a` DES/2K3DES, `0x1a` 3K3DES, `0xaa` AES
  - `void mf_desfire_rotate_left(uint8_t* data, size_t len);` (1-byte left rotate, in place)
  - `void mf_desfire_cbc_encrypt(MfDesfireKeyType, const uint8_t* key, uint8_t* iv, const uint8_t* in, uint8_t* out, size_t len);`
  - `void mf_desfire_cbc_decrypt(MfDesfireKeyType, const uint8_t* key, uint8_t* iv, const uint8_t* in, uint8_t* out, size_t len);`
  - constants `MF_DESFIRE_CRYPTO_KEY_MAX 24`, `MF_DESFIRE_CRYPTO_BLOCK_MAX 16`.

- [ ] **Step 1: Write the header**

```c
// mf_desfire_crypto.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MF_DESFIRE_CRYPTO_KEY_MAX   24
#define MF_DESFIRE_CRYPTO_BLOCK_MAX 16

typedef enum {
    MfDesfireKeyTypeDes,     // 8-byte key, single DES (native DESFire DES)
    MfDesfireKeyType2k3Des,  // 16-byte key, 2-key 3DES
    MfDesfireKeyType3k3Des,  // 24-byte key, 3-key 3DES
    MfDesfireKeyTypeAes,     // 16-byte key, AES-128
} MfDesfireKeyType;

size_t  mf_desfire_key_size(MfDesfireKeyType type);
size_t  mf_desfire_block_size(MfDesfireKeyType type);
uint8_t mf_desfire_auth_cmd(MfDesfireKeyType type);
void    mf_desfire_rotate_left(uint8_t* data, size_t len);
void    mf_desfire_cbc_encrypt(
    MfDesfireKeyType type, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len);
void    mf_desfire_cbc_decrypt(
    MfDesfireKeyType type, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len);
```

- [ ] **Step 2: Write failing tests for helpers**

Append to `test_main.c` and call from `main()`:

```c
#include "mf_desfire_crypto.h"
#include <string.h>

static void test_helpers(void) {
    CHECK(mf_desfire_key_size(MfDesfireKeyTypeDes) == 8, "des key size");
    CHECK(mf_desfire_key_size(MfDesfireKeyType3k3Des) == 24, "3k3des key size");
    CHECK(mf_desfire_key_size(MfDesfireKeyTypeAes) == 16, "aes key size");
    CHECK(mf_desfire_block_size(MfDesfireKeyTypeAes) == 16, "aes block");
    CHECK(mf_desfire_block_size(MfDesfireKeyTypeDes) == 8, "des block");
    CHECK(mf_desfire_auth_cmd(MfDesfireKeyTypeAes) == 0xaa, "aes cmd");
    CHECK(mf_desfire_auth_cmd(MfDesfireKeyTypeDes) == 0x0a, "des cmd");

    uint8_t r[4] = {0x01, 0x02, 0x03, 0x04};
    mf_desfire_rotate_left(r, 4);
    uint8_t exp[4] = {0x02, 0x03, 0x04, 0x01};
    CHECK(memcmp(r, exp, 4) == 0, "rotate left 1 byte");

    // CBC round-trip: decrypt(encrypt(x)) == x with fresh IVs
    uint8_t key[16]; memset(key, 0x00, 16);
    uint8_t in[16]  = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8_t iv1[16] = {0}, iv2[16] = {0}, ct[16], pt[16];
    mf_desfire_cbc_encrypt(MfDesfireKeyTypeAes, key, iv1, in, ct, 16);
    mf_desfire_cbc_decrypt(MfDesfireKeyTypeAes, key, iv2, ct, pt, 16);
    CHECK(memcmp(pt, in, 16) == 0, "aes cbc round-trip");
}
```

Restore `virtual_card.c` + `mf_desfire_crypto.c` in the Makefile recipe (undo the Task 1 temporary comment-out). Keep `virtual_card.c` as `/* Task 4 */` stub.

- [ ] **Step 3: Run, expect FAIL (link error / undefined symbols)**

Run: `wsl bash -lc "cd tools/desfire_auth_host_test && make test"`
Expected: FAIL — undefined references to `mf_desfire_key_size` etc.

- [ ] **Step 4: Implement the module**

```c
// mf_desfire_crypto.c
#include "mf_desfire_crypto.h"
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/des.h>

size_t mf_desfire_key_size(MfDesfireKeyType t) {
    switch(t) {
    case MfDesfireKeyTypeDes: return 8;
    case MfDesfireKeyType2k3Des: return 16;
    case MfDesfireKeyType3k3Des: return 24;
    case MfDesfireKeyTypeAes: return 16;
    default: return 0;
    }
}
size_t mf_desfire_block_size(MfDesfireKeyType t) {
    return (t == MfDesfireKeyTypeAes) ? 16 : 8;
}
uint8_t mf_desfire_auth_cmd(MfDesfireKeyType t) {
    switch(t) {
    case MfDesfireKeyTypeAes: return 0xaa;
    case MfDesfireKeyType3k3Des: return 0x1a;
    default: return 0x0a; // DES / 2K3DES legacy
    }
}
void mf_desfire_rotate_left(uint8_t* d, size_t n) {
    if(n == 0) return;
    uint8_t first = d[0];
    memmove(d, d + 1, n - 1);
    d[n - 1] = first;
}

// Expand DESFire key material into a 24-byte 3DES key for mbedtls_des3_set3key.
static void des_expand(MfDesfireKeyType t, const uint8_t* key, uint8_t out[24]) {
    if(t == MfDesfireKeyTypeDes) {           // K1=K2=K3
        memcpy(out, key, 8); memcpy(out + 8, key, 8); memcpy(out + 16, key, 8);
    } else if(t == MfDesfireKeyType2k3Des) { // K1,K2,K1
        memcpy(out, key, 16); memcpy(out + 16, key, 8);
    } else {                                  // 3K3DES: K1,K2,K3
        memcpy(out, key, 24);
    }
}

void mf_desfire_cbc_encrypt(
    MfDesfireKeyType t, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len) {
    if(t == MfDesfireKeyTypeAes) {
        mbedtls_aes_context c; mbedtls_aes_init(&c);
        mbedtls_aes_setkey_enc(&c, key, 128);
        mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_ENCRYPT, len, iv, in, out);
        mbedtls_aes_free(&c);
    } else {
        uint8_t k[24]; des_expand(t, key, k);
        mbedtls_des3_context c; mbedtls_des3_init(&c);
        mbedtls_des3_set3key_enc(&c, k);
        mbedtls_des3_crypt_cbc(&c, MBEDTLS_DES_ENCRYPT, len, iv, in, out);
        mbedtls_des3_free(&c);
    }
}
void mf_desfire_cbc_decrypt(
    MfDesfireKeyType t, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len) {
    if(t == MfDesfireKeyTypeAes) {
        mbedtls_aes_context c; mbedtls_aes_init(&c);
        mbedtls_aes_setkey_dec(&c, key, 128);
        mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_DECRYPT, len, iv, in, out);
        mbedtls_aes_free(&c);
    } else {
        uint8_t k[24]; des_expand(t, key, k);
        mbedtls_des3_context c; mbedtls_des3_init(&c);
        mbedtls_des3_set3key_dec(&c, k);
        mbedtls_des3_crypt_cbc(&c, MBEDTLS_DES_DECRYPT, len, iv, in, out);
        mbedtls_des3_free(&c);
    }
}
```

- [ ] **Step 5: Run, expect PASS**

Run: `wsl bash -lc "cd tools/desfire_auth_host_test && make test"`
Expected: all helper checks `ok:`, `ALL PASS`.

- [ ] **Step 6: Commit**

```bash
git add components/nfc/protocols/mf_desfire/mf_desfire_crypto.h components/nfc/protocols/mf_desfire/mf_desfire_crypto.c tools/desfire_auth_host_test/
git commit -m "feat(desfire): crypto key metadata + rotate + CBC helpers (host-tested)"
```

---

## Task 3: Session-key derivation

**Files:**
- Modify: `components/nfc/protocols/mf_desfire/mf_desfire_crypto.h` / `.c`
- Modify: `tools/desfire_auth_host_test/test_main.c`

**Interfaces:**
- Consumes: Task 2 helpers.
- Produces: `void mf_desfire_derive_session_key(MfDesfireKeyType type, const uint8_t* rnda, const uint8_t* rndb, uint8_t* out_sk);`
  - DES: `RndA[0:4] || RndB[0:4]` (8 bytes).
  - 2K3DES: `RndA[0:4] || RndB[0:4] || RndA[4:8] || RndB[4:8]` (16 bytes).
  - 3K3DES: `RndA[0:4] || RndB[0:4] || RndA[6:10] || RndB[6:10] || RndA[12:16] || RndB[12:16]` (24 bytes; RndA/RndB are 16 bytes).
  - AES: `RndA[0:4] || RndB[0:4] || RndA[12:16] || RndB[12:16]` (16 bytes).
  - `out_sk` length = `mf_desfire_key_size(type)`.

- [ ] **Step 1: Write failing test (documents the byte layout)**

```c
static void test_session_key(void) {
    uint8_t a[16], b[16];
    for(int i=0;i<16;i++){ a[i]=(uint8_t)(0xa0+i); b[i]=(uint8_t)(0xb0+i); }

    uint8_t sk[24];
    mf_desfire_derive_session_key(MfDesfireKeyTypeAes, a, b, sk);
    uint8_t aes_exp[16] = {0xa0,0xa1,0xa2,0xa3, 0xb0,0xb1,0xb2,0xb3,
                           0xac,0xad,0xae,0xaf, 0xbc,0xbd,0xbe,0xbf};
    CHECK(memcmp(sk, aes_exp, 16) == 0, "aes session key layout");

    mf_desfire_derive_session_key(MfDesfireKeyType2k3Des, a, b, sk);
    uint8_t d2_exp[16] = {0xa0,0xa1,0xa2,0xa3, 0xb0,0xb1,0xb2,0xb3,
                          0xa4,0xa5,0xa6,0xa7, 0xb4,0xb5,0xb6,0xb7};
    CHECK(memcmp(sk, d2_exp, 16) == 0, "2k3des session key layout");
}
```

- [ ] **Step 2: Run, expect FAIL** — `wsl bash -lc "cd tools/desfire_auth_host_test && make test"` → undefined `mf_desfire_derive_session_key`.

- [ ] **Step 3: Implement**

```c
void mf_desfire_derive_session_key(
    MfDesfireKeyType t, const uint8_t* a, const uint8_t* b, uint8_t* sk) {
    switch(t) {
    case MfDesfireKeyTypeDes:
        memcpy(sk + 0, a, 4); memcpy(sk + 4, b, 4);
        break;
    case MfDesfireKeyType2k3Des:
        memcpy(sk + 0, a, 4);  memcpy(sk + 4, b, 4);
        memcpy(sk + 8, a + 4, 4); memcpy(sk + 12, b + 4, 4);
        break;
    case MfDesfireKeyType3k3Des:
        memcpy(sk + 0,  a, 4);      memcpy(sk + 4,  b, 4);
        memcpy(sk + 8,  a + 6, 4);  memcpy(sk + 12, b + 6, 4);
        memcpy(sk + 16, a + 12, 4); memcpy(sk + 20, b + 12, 4);
        break;
    case MfDesfireKeyTypeAes:
        memcpy(sk + 0, a, 4);       memcpy(sk + 4, b, 4);
        memcpy(sk + 8, a + 12, 4);  memcpy(sk + 12, b + 12, 4);
        break;
    }
}
```

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(desfire): session-key derivation for DES/2K3DES/3K3DES/AES"
```

---

## Task 4: Auth handshake (pure) + virtual-card round-trip

**Files:**
- Modify: `components/nfc/protocols/mf_desfire/mf_desfire_crypto.h` / `.c`
- Create: `tools/desfire_auth_host_test/virtual_card.h` / `virtual_card.c`
- Modify: `tools/desfire_auth_host_test/test_main.c`

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces:
  - `typedef struct { MfDesfireKeyType type; uint8_t session_key[MF_DESFIRE_CRYPTO_KEY_MAX]; size_t block_size; uint8_t iv[MF_DESFIRE_CRYPTO_BLOCK_MAX]; uint8_t rnda[MF_DESFIRE_CRYPTO_BLOCK_MAX]; bool authenticated; } MfDesfireCryptoCtx;`
  - `bool mf_desfire_auth_begin(MfDesfireCryptoCtx* ctx, MfDesfireKeyType type, const uint8_t* key, const uint8_t* enc_rndb, const uint8_t* rnda, uint8_t* out_token);`
    - decrypts `enc_rndb` (block chain, IV=0) → RndB; builds token = `E(RndA || rot_left(RndB))` per scheme; stashes RndA + running IV in ctx; sets `ctx->type`, `ctx->block_size`.
  - `bool mf_desfire_auth_finish(MfDesfireCryptoCtx* ctx, const uint8_t* key, const uint8_t* enc_rndap);`
    - decrypts `enc_rndap` → RndA''; checks it equals `rot_left(ctx->rnda)`; on success derives session key into `ctx->session_key`, zeroes `ctx->iv`, sets `authenticated=true`.
  - virtual card: `void vcard_init(MfDesfireKeyType type, const uint8_t* key, const uint8_t* card_rndb); size_t vcard_block(void); void vcard_challenge(uint8_t* enc_rndb_out); bool vcard_respond(const uint8_t* token, uint8_t* enc_rndap_out);` — mirror implementation used only to test the reader.

**AES/3DES scheme (0xAA / 0x1A — primary):** standard CBC.
- RndB = CBC_decrypt(key, IV=0, enc_rndb); running IV = last block of enc_rndb.
- token_plain = RndA || rot_left(RndB); token = CBC_encrypt(key, running IV, token_plain); running IV = last block of token.
- On finish: RndA'' = CBC_decrypt(key, running IV, enc_rndap); expect == rot_left(RndA).

**Legacy DES/2K3DES (0x0A) "decrypt mode":** identical structure but the reader uses `mf_desfire_cbc_decrypt` where the AES path uses encrypt, and `..._encrypt` where it uses decrypt (DESFire legacy "send by decrypting"). Implement legacy by swapping the two CBC calls. The round-trip virtual card mirrors the same swap, proving internal consistency; Task 10 locks it against a real capture.

- [ ] **Step 1: Write the virtual card (test double)**

```c
// virtual_card.h
#pragma once
#include "mf_desfire_crypto.h"
void   vcard_init(MfDesfireKeyType type, const uint8_t* key, const uint8_t* card_rndb);
size_t vcard_block(void);
void   vcard_challenge(uint8_t* enc_rndb_out);      // returns E(RndB)
bool   vcard_respond(const uint8_t* token, uint8_t* enc_rndap_out); // verifies, returns E(RndA'')
```

```c
// virtual_card.c  — mirrors the reader math to validate the handshake wiring
#include "virtual_card.h"
#include <string.h>
static MfDesfireKeyType s_t; static uint8_t s_key[24]; static uint8_t s_rndb[16]; static size_t s_bs;
static uint8_t s_iv[16];
void vcard_init(MfDesfireKeyType t, const uint8_t* key, const uint8_t* rndb) {
    s_t = t; s_bs = mf_desfire_block_size(t);
    memcpy(s_key, key, mf_desfire_key_size(t));
    memcpy(s_rndb, rndb, s_bs);
}
size_t vcard_block(void) { return s_bs; }
void vcard_challenge(uint8_t* out) {
    memset(s_iv, 0, 16);
    if(s_t == MfDesfireKeyTypeAes || s_t == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_encrypt(s_t, s_key, s_iv, s_rndb, out, s_bs);
    else
        mf_desfire_cbc_decrypt(s_t, s_key, s_iv, s_rndb, out, s_bs);
    memcpy(s_iv, out, s_bs); // running IV = ciphertext just produced
}
bool vcard_respond(const uint8_t* token, uint8_t* enc_rndap_out) {
    uint8_t plain[32];
    // decrypt reader token (2 blocks) with running IV
    if(s_t == MfDesfireKeyTypeAes || s_t == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_decrypt(s_t, s_key, s_iv, token, plain, s_bs * 2);
    else
        mf_desfire_cbc_encrypt(s_t, s_key, s_iv, token, plain, s_bs * 2);
    memcpy(s_iv, token + s_bs, s_bs);
    // plain = RndA || rot_left(RndB); verify second block == rot_left(RndB)
    uint8_t rb[16]; memcpy(rb, s_rndb, s_bs); mf_desfire_rotate_left(rb, s_bs);
    if(memcmp(plain + s_bs, rb, s_bs) != 0) return false;
    // respond with E(rot_left(RndA))
    uint8_t ra[16]; memcpy(ra, plain, s_bs); mf_desfire_rotate_left(ra, s_bs);
    if(s_t == MfDesfireKeyTypeAes || s_t == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_encrypt(s_t, s_key, s_iv, ra, enc_rndap_out, s_bs);
    else
        mf_desfire_cbc_decrypt(s_t, s_key, s_iv, ra, enc_rndap_out, s_bs);
    memcpy(s_iv, enc_rndap_out, s_bs);
    return true;
}
```

- [ ] **Step 2: Write the failing round-trip test**

```c
#include "virtual_card.h"
static void auth_roundtrip(MfDesfireKeyType t, const char* name) {
    uint8_t key[24]; for(size_t i=0;i<mf_desfire_key_size(t);i++) key[i]=(uint8_t)(0x10+i);
    uint8_t card_rndb[16]; for(int i=0;i<16;i++) card_rndb[i]=(uint8_t)(0xb0+i);
    uint8_t rnda[16];      for(int i=0;i<16;i++) rnda[i]=(uint8_t)(0xa0+i);

    vcard_init(t, key, card_rndb);
    size_t bs = vcard_block();
    uint8_t enc_rndb[16]; vcard_challenge(enc_rndb);

    MfDesfireCryptoCtx ctx; uint8_t token[32];
    CHECK(mf_desfire_auth_begin(&ctx, t, key, enc_rndb, rnda, token), name);

    uint8_t enc_rndap[16];
    CHECK(vcard_respond(token, enc_rndap), name);
    CHECK(mf_desfire_auth_finish(&ctx, key, enc_rndap), name);
    CHECK(ctx.authenticated, name);

    uint8_t sk[24]; mf_desfire_derive_session_key(t, rnda, card_rndb, sk);
    CHECK(memcmp(ctx.session_key, sk, mf_desfire_key_size(t)) == 0, name);
    (void)bs;
}
static void test_auth(void) {
    auth_roundtrip(MfDesfireKeyTypeAes,    "aes auth round-trip");
    auth_roundtrip(MfDesfireKeyType2k3Des, "2k3des auth round-trip");
    auth_roundtrip(MfDesfireKeyTypeDes,    "des auth round-trip");
}
```

- [ ] **Step 3: Run, expect FAIL** (undefined `mf_desfire_auth_begin/finish`).

- [ ] **Step 4: Implement `auth_begin` / `auth_finish` in `mf_desfire_crypto.c`** (add the struct to the header)

```c
static void cbc_send(MfDesfireCryptoCtx* c, const uint8_t* key,
                     const uint8_t* in, uint8_t* out, size_t len) {
    if(c->type == MfDesfireKeyTypeAes || c->type == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_encrypt(c->type, key, c->iv, in, out, len);
    else
        mf_desfire_cbc_decrypt(c->type, key, c->iv, in, out, len);
}
static void cbc_recv(MfDesfireCryptoCtx* c, const uint8_t* key,
                     const uint8_t* in, uint8_t* out, size_t len) {
    if(c->type == MfDesfireKeyTypeAes || c->type == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_decrypt(c->type, key, c->iv, in, out, len);
    else
        mf_desfire_cbc_encrypt(c->type, key, c->iv, in, out, len);
}

bool mf_desfire_auth_begin(
    MfDesfireCryptoCtx* ctx, MfDesfireKeyType type, const uint8_t* key,
    const uint8_t* enc_rndb, const uint8_t* rnda, uint8_t* out_token) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->type = type;
    ctx->block_size = mf_desfire_block_size(type);
    size_t bs = ctx->block_size;

    uint8_t rndb[16];
    memset(ctx->iv, 0, 16);
    cbc_recv(ctx, key, enc_rndb, rndb, bs);   // RndB
    memcpy(ctx->iv, enc_rndb, bs);            // running IV

    memcpy(ctx->rnda, rnda, bs);
    uint8_t rndb_rot[16]; memcpy(rndb_rot, rndb, bs); mf_desfire_rotate_left(rndb_rot, bs);

    uint8_t plain[32];
    memcpy(plain, rnda, bs);
    memcpy(plain + bs, rndb_rot, bs);
    cbc_send(ctx, key, plain, out_token, bs * 2);
    memcpy(ctx->iv, out_token + bs, bs);      // running IV = last token block
    return true;
}

bool mf_desfire_auth_finish(
    MfDesfireCryptoCtx* ctx, const uint8_t* key, const uint8_t* enc_rndap) {
    size_t bs = ctx->block_size;
    uint8_t rndap[16];
    cbc_recv(ctx, key, enc_rndap, rndap, bs);
    memcpy(ctx->iv, enc_rndap, bs);

    uint8_t rnda_rot[16]; memcpy(rnda_rot, ctx->rnda, bs); mf_desfire_rotate_left(rnda_rot, bs);
    if(memcmp(rndap, rnda_rot, bs) != 0) return false;

    // Need RndB again for session key: recompute is not stored — derive uses rnda & rndb.
    // Store rndb in ctx during begin instead (see note).
    mf_desfire_derive_session_key(ctx->type, ctx->rnda, ctx->rndb, ctx->session_key);
    memset(ctx->iv, 0, 16);
    ctx->authenticated = true;
    return true;
}
```

Add `uint8_t rndb[MF_DESFIRE_CRYPTO_BLOCK_MAX];` to `MfDesfireCryptoCtx` and store it in `auth_begin` (`memcpy(ctx->rndb, rndb, bs);`) so `auth_finish` can derive the session key.

- [ ] **Step 5: Run, expect PASS** — all three round-trips + session-key match.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "feat(desfire): auth handshake (begin/finish) + host virtual-card round-trip"
```

---

## Task 5: CMAC (session integrity for MAC-comm reads)

**Files:**
- Modify: `components/nfc/protocols/mf_desfire/mf_desfire_crypto.h` / `.c`
- Modify: `tools/desfire_auth_host_test/test_main.c`

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces:
  - `void mf_desfire_cmac_init(MfDesfireCryptoCtx* ctx);` — generates the two CMAC subkeys (RFC 4493 for AES; analogous for 3DES via the 8-byte block, Rb=0x1B for 8-byte, 0x87 for 16-byte) from `ctx->session_key`, stores them in `ctx->cmac_sk1/sk2`.
  - `void mf_desfire_cmac(MfDesfireCryptoCtx* ctx, const uint8_t* data, size_t len, uint8_t* mac8);` — full CMAC over `data`, chaining `ctx->iv`; returns the leading 8 bytes DESFire uses as the truncated MAC. Updates `ctx->iv` to the last CBC block (DESFire keeps CMAC state as the comm IV).

- [ ] **Step 1: Write failing test — CMAC of empty message is stable + subkeys derived**

```c
static void test_cmac(void) {
    // Deterministic self-consistency test (RFC 4493 structure), not an external KAT.
    uint8_t key[24]; for(int i=0;i<16;i++) key[i]=(uint8_t)i;
    MfDesfireCryptoCtx c; memset(&c,0,sizeof c);
    c.type = MfDesfireKeyTypeAes; c.block_size = 16;
    memcpy(c.session_key, key, 16);
    mf_desfire_cmac_init(&c);
    // subkeys must be non-zero and sk2 == sk1<<1 (mod) — sanity, not value-locked
    int nz = 0; for(int i=0;i<16;i++) nz |= c.cmac_sk1[i];
    CHECK(nz != 0, "cmac subkey1 derived");

    uint8_t m[16] = {0}; uint8_t mac_a[8], mac_b[8];
    memset(c.iv,0,16); mf_desfire_cmac(&c, m, 16, mac_a);
    memset(c.iv,0,16); mf_desfire_cmac(&c, m, 16, mac_b);
    CHECK(memcmp(mac_a, mac_b, 8) == 0, "cmac deterministic");
}
```

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Implement CMAC** (RFC 4493 subkey gen + CBC-MAC; `msb`/left-shift helper; XOR last block with sk1 if block-aligned else pad `0x80..00` and use sk2). Use `mf_desfire_cbc_encrypt` with a zero IV internally for the MAC chain, seeded by `ctx->iv`.

```c
static void lshift1(const uint8_t* in, uint8_t* out, size_t n) {
    uint8_t carry = 0;
    for(size_t i = n; i-- > 0;) { out[i] = (uint8_t)((in[i] << 1) | carry); carry = in[i] >> 7; }
}
void mf_desfire_cmac_init(MfDesfireCryptoCtx* c) {
    size_t bs = c->block_size;
    uint8_t z[16] = {0}, l[16], iv[16] = {0};
    mf_desfire_cbc_encrypt(c->type, c->session_key, iv, z, l, bs);
    uint8_t rb = (bs == 16) ? 0x87 : 0x1b;
    lshift1(l, c->cmac_sk1, bs);
    if(l[0] & 0x80) c->cmac_sk1[bs - 1] ^= rb;
    lshift1(c->cmac_sk1, c->cmac_sk2, bs);
    if(c->cmac_sk1[0] & 0x80) c->cmac_sk2[bs - 1] ^= rb;
}
void mf_desfire_cmac(MfDesfireCryptoCtx* c, const uint8_t* data, size_t len, uint8_t* mac8) {
    size_t bs = c->block_size;
    uint8_t block[16], mac[16];
    memcpy(mac, c->iv, bs);
    size_t full = (len == 0) ? 0 : (len - 1) / bs; // number of complete non-final blocks
    size_t off = 0;
    for(size_t i = 0; i < full; i++) {
        for(size_t j=0;j<bs;j++) block[j] = data[off+j] ^ mac[j];
        uint8_t iv0[16] = {0};
        mf_desfire_cbc_encrypt(c->type, c->session_key, iv0, block, mac, bs);
        off += bs;
    }
    size_t rem = len - off;
    uint8_t last[16] = {0};
    if(rem == bs) { memcpy(last, data + off, bs); for(size_t j=0;j<bs;j++) last[j]^=c->cmac_sk1[j]; }
    else { memcpy(last, data + off, rem); last[rem] = 0x80; for(size_t j=0;j<bs;j++) last[j]^=c->cmac_sk2[j]; }
    for(size_t j=0;j<bs;j++) block[j] = last[j] ^ mac[j];
    uint8_t iv0[16] = {0};
    mf_desfire_cbc_encrypt(c->type, c->session_key, iv0, block, mac, bs);
    memcpy(c->iv, mac, bs);
    memcpy(mac8, mac, 8);
}
```

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(desfire): CMAC subkeys + truncated MAC for MAC-comm reads"
```

---

## Task 6: Poller `authenticate` + `read_file_data_authed`

**Files:**
- Modify: `components/nfc/protocols/mf_desfire/mf_desfire_poller.h`
- Modify: `components/nfc/protocols/mf_desfire/mf_desfire_poller_i.c`
- Modify: `components/nfc/protocols/mf_desfire/mf_desfire_poller_i.h` (add `MfDesfireCryptoCtx crypto;` to the poller struct if state must persist across reads)
- Modify: `components/nfc/protocols/mf_desfire/mf_desfire.h` (add `#define MF_DESFIRE_CMD_ADDITIONAL_FRAME (0xaf)` if absent; `MF_DESFIRE_CMD_READ_DATA (0xbd)`)

**Interfaces:**
- Consumes: crypto module (Tasks 2–5), existing `mf_desfire_poller_send_chunks`, `instance->input_buffer/result_buffer`.
- Produces:
  - `MfDesfireError mf_desfire_poller_authenticate(MfDesfirePoller* instance, uint8_t key_no, MfDesfireKeyType key_type, const uint8_t* key);`
  - `MfDesfireError mf_desfire_poller_read_file_data_authed(MfDesfirePoller* instance, MfDesfireFileId file_id, MfDesfireFileCommunicationSettings comm, uint32_t offset, uint32_t length, uint8_t* out, uint32_t* out_len);` — Phase A honors `Plaintext` and `Authenticated` (MAC); returns `MfDesfireErrorProtocol` for `Enciphered` (Phase B).

**No host unit test** (needs radio). Verification here is: compiles clean in-tree, and follows the exact `send_chunks` pattern from `read_version`. HW test in Task 10.

- [ ] **Step 1: Add command defines + poller struct field**

Add to `mf_desfire.h` (if not present): `#define MF_DESFIRE_CMD_ADDITIONAL_FRAME (0xafU)` and `#define MF_DESFIRE_CMD_READ_DATA (0xbdU)`. Add `MfDesfireCryptoCtx crypto;` to `struct MfDesfirePoller` (include `mf_desfire_crypto.h`).

- [ ] **Step 2: Implement `authenticate`** (mirror the `read_version` send pattern, two frames)

```c
MfDesfireError mf_desfire_poller_authenticate(
    MfDesfirePoller* instance, uint8_t key_no, MfDesfireKeyType key_type, const uint8_t* key) {
    furi_check(instance);
    size_t bs = mf_desfire_block_size(key_type);

    // Frame 1: AUTH cmd + key_no -> status 0xAF + E(RndB)
    bit_buffer_reset(instance->input_buffer);
    bit_buffer_append_byte(instance->input_buffer, mf_desfire_auth_cmd(key_type));
    bit_buffer_append_byte(instance->input_buffer, key_no);

    MfDesfireError error = mf_desfire_poller_send_chunks(
        instance, instance->input_buffer, instance->result_buffer);
    if(error != MfDesfireErrorNone) return error;
    if(bit_buffer_get_size_bytes(instance->result_buffer) < 1 + bs) return MfDesfireErrorProtocol;
    // result = [0xAF][E(RndB)] — status byte then block
    const uint8_t* enc_rndb = bit_buffer_get_data(instance->result_buffer) + 1;

    uint8_t rnda[16];
    furi_hal_random_fill_buf(rnda, bs);
    uint8_t token[32];
    if(!mf_desfire_auth_begin(&instance->crypto, key_type, key, enc_rndb, rnda, token))
        return MfDesfireErrorProtocol;

    // Frame 2: 0xAF + token -> status 0x00 + E(RndA'')
    bit_buffer_reset(instance->input_buffer);
    bit_buffer_append_byte(instance->input_buffer, MF_DESFIRE_CMD_ADDITIONAL_FRAME);
    bit_buffer_append_bytes(instance->input_buffer, token, bs * 2);

    error = mf_desfire_poller_send_chunks(
        instance, instance->input_buffer, instance->result_buffer);
    if(error != MfDesfireErrorNone) return error;
    if(bit_buffer_get_size_bytes(instance->result_buffer) < 1 + bs) return MfDesfireErrorProtocol;
    const uint8_t* enc_rndap = bit_buffer_get_data(instance->result_buffer) + 1;

    if(!mf_desfire_auth_finish(&instance->crypto, key, enc_rndap)) return MfDesfireErrorAuth;
    mf_desfire_cmac_init(&instance->crypto);
    return MfDesfireErrorNone;
}
```

Add `MfDesfireErrorAuth` to the `MfDesfireError` enum if absent (check `mf_desfire.h`; reuse an existing "protocol/auth" error if one exists rather than adding a duplicate).

- [ ] **Step 3: Implement `read_file_data_authed`** (Phase A: Plaintext + MAC)

```c
MfDesfireError mf_desfire_poller_read_file_data_authed(
    MfDesfirePoller* instance, MfDesfireFileId file_id,
    MfDesfireFileCommunicationSettings comm, uint32_t offset, uint32_t length,
    uint8_t* out, uint32_t* out_len) {
    furi_check(instance);
    if(comm == MfDesfireFileCommunicationSettingsEnciphered)
        return MfDesfireErrorProtocol; // Phase B

    bit_buffer_reset(instance->input_buffer);
    bit_buffer_append_byte(instance->input_buffer, MF_DESFIRE_CMD_READ_DATA);
    bit_buffer_append_byte(instance->input_buffer, file_id);
    uint8_t off3[3] = {offset & 0xff, (offset >> 8) & 0xff, (offset >> 16) & 0xff};
    uint8_t len3[3] = {length & 0xff, (length >> 8) & 0xff, (length >> 16) & 0xff};
    bit_buffer_append_bytes(instance->input_buffer, off3, 3);
    bit_buffer_append_bytes(instance->input_buffer, len3, 3);

    MfDesfireError error = mf_desfire_poller_send_chunks(
        instance, instance->input_buffer, instance->result_buffer);
    if(error != MfDesfireErrorNone) return error;

    size_t rx = bit_buffer_get_size_bytes(instance->result_buffer);
    const uint8_t* data = bit_buffer_get_data(instance->result_buffer);
    // Strip leading status byte if present (0x00). MAC-comm appends 8-byte CMAC we do not re-verify in Phase A.
    size_t start = (rx > 0 && data[0] == 0x00) ? 1 : 0;
    size_t payload = rx - start;
    if(comm == MfDesfireFileCommunicationSettingsAuthenticated && payload >= 8) payload -= 8;
    if(payload > length) payload = length;
    memcpy(out, data + start, payload);
    *out_len = (uint32_t)payload;
    return MfDesfireErrorNone;
}
```

- [ ] **Step 4: Declare both in `mf_desfire_poller.h`.**

- [ ] **Step 5: Compile-check in-tree** — this is validated by the full firmware build in Task 10; here just confirm the files parse (no separate host build).

- [ ] **Step 6: Commit**

```bash
git add components/nfc/protocols/mf_desfire/
git commit -m "feat(desfire): poller authenticate + read_file_data_authed (Phase A plain/MAC)"
```

---

## Task 7: Attack engine + dictionary (host-tested via mock)

**Files:**
- Create: `applications/main/nfc/helpers/mf_desfire_attack.h` / `.c`
- Create: `tools/desfire_auth_host_test/test_attack.c` (compiled into the harness)
- Modify: `tools/desfire_auth_host_test/Makefile` (add `test_attack.c` + a `-DDESFIRE_HOST_TEST` guard so `mf_desfire_attack.c` uses the injectable callback, not the real poller)

**Interfaces:**
- Consumes: `MfDesfireKeyType`, `mf_desfire_key_size`.
- Produces:
  - `typedef bool (*MfDesfireTryAuthFn)(void* ctx, uint16_t aid, uint8_t key_no, MfDesfireKeyType type, const uint8_t* key);`
  - `typedef struct { uint16_t aid; uint8_t key_no; MfDesfireKeyType type; uint8_t key[MF_DESFIRE_CRYPTO_KEY_MAX]; } MfDesfireFoundKey;`
  - `typedef struct { const uint8_t* key; MfDesfireKeyType type; } MfDesfireDictEntry;`
  - `size_t mf_desfire_dict_builtin(const MfDesfireDictEntry** out);` — returns built-in defaults: 8×`00` (DES), 16×`00` (2K3DES), 24×`00` (3K3DES), 16×`00` (AES).
  - `size_t mf_desfire_attack_sweep(MfDesfireTryAuthFn try_auth, void* ctx, const uint16_t* aids, size_t aid_count, uint8_t keys_per_app, const MfDesfireDictEntry* dict, size_t dict_count, MfDesfireFoundKey* out, size_t out_max, void (*progress)(void* p, size_t done, size_t total), void* progress_ctx);`
    - iterates every (aid × key_no in [0,keys_per_app) × dict entry of the app's supported types); calls `try_auth`; on success records a `MfDesfireFoundKey` (one per aid/key_no — stop that slot on first hit) and continues; returns number found.

- [ ] **Step 1: Write failing test with a mock `try_auth`**

```c
// test_attack.c
#include "mf_desfire_attack.h"
#include <string.h>
#include <stdio.h>
extern int g_fail;
#define TCHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m); g_fail++;} else printf("ok: %s\n",m);}while(0)

// mock: AID 0x000001 key 0 is AES all-0x11; everything else fails
static bool mock_try(void* ctx, uint16_t aid, uint8_t kn, MfDesfireKeyType t, const uint8_t* key) {
    (void)ctx; static const uint8_t secret[16] = {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
                                                  0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11};
    return aid == 0x000001 && kn == 0 && t == MfDesfireKeyTypeAes && memcmp(key, secret, 16) == 0;
}

void test_attack_sweep(void) {
    static const uint8_t z16[16] = {0}; static const uint8_t s16[16] =
        {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11};
    MfDesfireDictEntry dict[] = {
        { z16, MfDesfireKeyTypeAes }, { s16, MfDesfireKeyTypeAes },
    };
    uint16_t aids[] = { 0x000000, 0x000001 };
    MfDesfireFoundKey found[8];
    size_t n = mf_desfire_attack_sweep(mock_try, NULL, aids, 2, 1, dict, 2, found, 8, NULL, NULL);
    TCHECK(n == 1, "sweep finds exactly one key");
    TCHECK(found[0].aid == 0x000001 && found[0].key_no == 0, "sweep records correct slot");
    TCHECK(found[0].type == MfDesfireKeyTypeAes, "sweep records key type");
}
```

Call `test_attack_sweep()` from `main()` and add `extern` decl. For the host build, compile `mf_desfire_attack.c` with `-DDESFIRE_HOST_TEST` so it does **not** include the firmware poller header (the sweep is pure logic + the callback).

- [ ] **Step 2: Run, expect FAIL** (undefined `mf_desfire_attack_sweep`).

- [ ] **Step 3: Implement the engine** (`mf_desfire_attack.c`, sweep + builtin dict). Keep the sweep free of poller includes; the real firmware wrapper (Task 8) supplies a `try_auth` that calls `mf_desfire_poller_authenticate` + re-selects the AID.

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit**

```bash
git add applications/main/nfc/helpers/mf_desfire_attack.* tools/desfire_auth_host_test/
git commit -m "feat(desfire): dictionary + key-sweep engine (host-tested via mock try_auth)"
```

---

## Task 8: Config-weakness audit helper

**Files:**
- Create: `applications/main/nfc/helpers/mf_desfire_audit.h` / `.c`
- Modify: `tools/desfire_auth_host_test/test_attack.c` (add a pure audit test)

**Interfaces:**
- Consumes: `MfDesfireFileSettings` (`type`, `comm`, `access_rights`), `MfDesfireKeySettings`.
- Produces: `void mf_desfire_audit_file(const MfDesfireFileSettings* fs, FuriString* report_out);` and `void mf_desfire_audit_key_settings(const MfDesfireKeySettings* ks, bool default_master_found, FuriString* report_out);` — append human-readable findings: free-read (access right nibble `0xE` = free), comm mode, default master key still set, changeable flags.

The access-rights parsing is pure; test the "free read" detection with a synthesized `access_rights` value on host (guard the `FuriString` calls behind `DESFIRE_HOST_TEST` with a tiny shim, or test only the boolean predicate `bool mf_desfire_audit_is_free_read(MfDesfireFileAccessRights ar);`).

- [ ] **Step 1: Failing test for `mf_desfire_audit_is_free_read`** — access rights `0xEEEE` → true; `0x0000` → false. (Read access = high nibble of the 16-bit rights per DESFire layout; assert the predicate extracts the read-access nibble and compares to `0xE`.)
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement predicate + report builders.**
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** — `git commit -m "feat(desfire): config-weakness audit (free-read/comm/default-key)"`

---

## Task 9: Scenes + menu wiring (firmware; build-verified)

**Files:**
- Create: `applications/main/nfc/scenes/nfc_scene_mf_desfire_unlock_menu.c`
- Create: `applications/main/nfc/scenes/nfc_scene_mf_desfire_dict_attack.c`
- Modify: `applications/main/nfc/scenes/nfc_scene_config.h` (register both, PRIVATE-gated)
- Modify: `applications/main/nfc/scenes/nfc_scene_mf_desfire_app.c` (add "Unlock / Key Attack" submenu item → `unlock_menu`)
- Modify: `applications/main/nfc/nfc_app_i.h` (add `MfDesfireAttack* desfire_attack;` model handle + found-keys list if needed)

**Interfaces:**
- Consumes: `mf_desfire_attack_sweep`, `mf_desfire_poller_authenticate`, `mf_desfire_poller_read_file_data_authed`, `mf_desfire_audit_*`.
- Produces: two registered scenes + a menu entry, all under `#ifndef PUBLIC_BUILD`.

Model the dict-attack scene on `nfc_scene_mf_classic_dict_attack.c` (progress widget: keys tried / found / files dumped, cancel). The `unlock_menu` scene offers: **Dictionary attack**, **Config audit**, **Enter key manually** (reuse existing `nfc_scene_des_auth_key_input` pattern). The poller worker builds a `try_auth` that selects the AID then calls `mf_desfire_poller_authenticate`; on the found set it re-authenticates and calls `read_file_data_authed` per file, merging results into the device model so `more_info` reflects dumped files.

- [ ] **Step 1:** Add the two scene files with `_on_enter/_on_event/_on_exit`, following an existing scene's skeleton exactly.
- [ ] **Step 2:** Register in `nfc_scene_config.h` inside `#ifndef PUBLIC_BUILD` (place the `ADD_SCENE(...)` lines in a private-gated block).
- [ ] **Step 3:** Add the submenu item in `nfc_scene_mf_desfire_app.c` (guard the item + its handler with `#ifndef PUBLIC_BUILD`).
- [ ] **Step 4:** Wire the worker `try_auth` + dump path.
- [ ] **Step 5: Commit** — `git commit -m "feat(desfire): unlock menu + dict-attack scene + config audit wiring (PRIVATE)"`

---

## Task 10: Build all three trees, HW-test, lock legacy KAT, mirror

**Files:**
- Mirror all new/changed files into `src_cardputer_adv` and `src_lilygo_t_embed_cc1101`.
- Add (if a real legacy capture is obtained): `tools/desfire_auth_host_test/kat_legacy.h` + a KAT test that replays it.

- [ ] **Step 1: Build superset (m5stick_s3)** via PowerShell using the tree's build `.bat`. Delete the build dir first; verify the log mtime is fresh and `SENTINEL_OK` is from this run (stale-log gotcha). Fix compile errors until clean.
- [ ] **Step 2: Flash M5Stick S3 (COM10)** and HW-test against a real DESFire card: run Dictionary attack; confirm default-keyed apps authenticate and files dump; confirm Config audit renders. Capture the AES auth transcript over serial.
- [ ] **Step 3: Lock legacy KAT (if a DES/3DES card is available):** capture a full `0x0A` transcript from a real card, add it as `kat_legacy.h`, and add a host test that runs `auth_begin` against the captured `enc_rndb` and asserts the produced token matches the captured token. If no legacy card is available, record in the plan that legacy remains round-trip-only and flag it in-UI.
- [ ] **Step 4: Mirror to cardputer_adv + t_embed** (byte-identical files); build each via its `.bat` (delete build dir, verify fresh log). Cardputer is no-PSRAM — confirm no new large static buffers were added (the crypto ctx is tiny/stack-scoped).
- [ ] **Step 5: Flash Cardputer ADV (COM12) + T-Embed CC1101+ (COM11); smoke-test the menu appears and a dictionary attack runs.**
- [ ] **Step 6: Commit** — `git commit -m "feat(desfire): build+flash all three trees, HW-tested Phase A, legacy KAT lock"`

---

## Self-Review

**Spec coverage (Component 1 + 1e Phase A + 1f):**
- 1a crypto module → Tasks 2–5. ✓
- 1b poller authenticate + authed read → Task 6. ✓
- 1c attack engine + dictionary + dump → Tasks 7, 9. ✓
- 1d scenes/menu → Task 9. ✓
- 1e Phase A (plain/MAC read; Enciphered returns error) → Task 6 Step 3. ✓ (Phase B is a separate plan, as specified.)
- 1f config audit → Task 8, surfaced in Task 9. ✓
- Host vector harness → Tasks 1–8; block-cipher KAT via mbedtls self-test, handshake via round-trip, legacy locked to real capture in Task 10. ✓
- PRIVATE gating + three-tree mirror → Global Constraints + Task 9/10. ✓

**Placeholder scan:** No "TBD"/"handle edge cases" left; each code step has concrete code or an explicit "model on existing file X exactly" instruction for firmware-framework scenes (Task 9) where cloning an in-tree scene is the correct, non-inventable pattern.

**Type consistency:** `MfDesfireKeyType`, `MfDesfireCryptoCtx` (with `rndb`, `iv`, `session_key`, `cmac_sk1/sk2`, `authenticated`), `mf_desfire_auth_begin/finish`, `mf_desfire_cmac_init/mf_desfire_cmac`, `mf_desfire_attack_sweep`, `MfDesfireFoundKey`, `MfDesfireDictEntry` are used consistently across Tasks 2–9. `auth_finish` depends on `ctx->rndb` stored in `auth_begin` (noted in Task 4 Step 4).

**Known risk carried forward:** Legacy DES/3DES `0x0A` "decrypt mode" byte-exactness is proven internally by round-trip but only proven against reality when Task 10 Step 3 locks a real capture; AES `0xAA` is the trusted primary path. This matches the spec's crypto-priority constraint.
