// mf_desfire_attack.h
//
// DESFire dictionary + key-sweep attack engine.
//
// This module is PURE LOGIC: it has no dependency on the poller or any
// firmware headers, only on mf_desfire_crypto.h for MfDesfireKeyType /
// MF_DESFIRE_CRYPTO_KEY_MAX. All card interaction happens through the
// injected MfDesfireTryAuthFn callback, which is what lets this file
// compile unchanged on host (tools/desfire_auth_host_test, against a mock
// try_auth) and on firmware (Task 8 supplies a real try_auth that calls
// mf_desfire_poller_authenticate + re-selects the AID).
#pragma once
#include "mf_desfire_crypto.h"

// Attempts a single authentication against the target card/mock.
// Returns true if `key` (of type `type`) authenticates key slot `key_no`
// on application `aid`.
typedef bool (*MfDesfireTryAuthFn)(
    void* ctx, uint16_t aid, uint8_t key_no, MfDesfireKeyType type, const uint8_t* key);

// A key found to authenticate a given (aid, key_no) slot.
typedef struct {
    uint16_t         aid;
    uint8_t          key_no;
    MfDesfireKeyType type;
    uint8_t          key[MF_DESFIRE_CRYPTO_KEY_MAX];
} MfDesfireFoundKey;

// A single dictionary entry: a candidate key of a known type. `key` must
// point to at least mf_desfire_key_size(type) bytes and must outlive the
// sweep call.
typedef struct {
    const uint8_t*   key;
    MfDesfireKeyType type;
} MfDesfireDictEntry;

// Returns the built-in DESFire factory-default key dictionary via *out
// (a pointer to static storage) and its entry count. Entries: 8x0x00 (DES),
// 16x0x00 (2K3DES), 24x0x00 (3K3DES), 16x0x00 (AES).
size_t mf_desfire_dict_builtin(const MfDesfireDictEntry** out);

// Sweeps every (aid x key_no in [0, keys_per_app) x dict entry) trying
// try_auth(ctx, aid, key_no, entry.type, entry.key). On the first hit for a
// given (aid, key_no) slot, records a MfDesfireFoundKey into `out` (up to
// out_max entries) and moves on to the next key_no — it does not keep
// trying further dictionary entries against an already-cracked slot.
//
// progress, if non-NULL, is called after each try_auth attempt with the
// number of attempts done so far and the total planned attempts.
//
// Returns the number of MfDesfireFoundKey entries written to `out` (capped
// at out_max).
size_t mf_desfire_attack_sweep(
    MfDesfireTryAuthFn try_auth, void* ctx, const uint16_t* aids, size_t aid_count,
    uint8_t keys_per_app, const MfDesfireDictEntry* dict, size_t dict_count,
    MfDesfireFoundKey* out, size_t out_max, void (*progress)(void* p, size_t done, size_t total),
    void* progress_ctx);
