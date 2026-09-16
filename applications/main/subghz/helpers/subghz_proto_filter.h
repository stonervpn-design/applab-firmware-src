#pragma once

// [PROTO FILTER] User-configurable per-protocol receiver allowlist.
//
// Stores the set of protocol NAMES the user has switched OFF in the Receiver
// Config -> "Protocol Filter" scene. The receiver's add-to-history callback
// consults subghz_proto_filter_contains() and silently drops any decode whose
// protocol name is in the OFF set, cutting noise / false-positives on a busy
// band without disabling the decoders in firmware.
//
// The OFF set is persisted to /ext/subghz/assets/proto_filter.txt (one protocol
// name per line) and is lazily (re)loaded on first access within a session.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True if `name` is currently in the OFF set (this protocol is filtered out). */
bool subghz_proto_filter_contains(const char* name);

/**
 * Add (`disabled` == true) or remove (`disabled` == false) `name` from the OFF
 * set. Persists to disk immediately unless a batch is open. Returns true if the
 * set actually changed.
 */
bool subghz_proto_filter_set(const char* name, bool disabled);

/** Enable everything: clears the OFF set (persists immediately unless batched). */
void subghz_proto_filter_clear_all(void);

/**
 * Batch helpers: defer the disk write across many subghz_proto_filter_set()
 * calls (used by the "All ON/OFF" master row). end_batch() writes once.
 */
void subghz_proto_filter_begin_batch(void);
void subghz_proto_filter_end_batch(void);

/** Drop the in-memory cache so the next access reloads from disk. */
void subghz_proto_filter_reload(void);

#ifdef __cplusplus
}
#endif
