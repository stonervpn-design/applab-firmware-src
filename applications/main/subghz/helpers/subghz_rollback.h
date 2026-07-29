#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ROLLBACK_MAX_SLOTS 8

typedef struct {
    uint64_t data;        // full decoded data word (informational)
    uint16_t bits;        // data_count_bit
    uint32_t serial;      // Fix & 0x0FFFFFFF
    uint32_t counter;     // Cnt (0 if "????")
    uint8_t  btn;         // (Fix >> 28) & 0x0F
    uint8_t  protocol_id; // 0 = unset, else app-assigned tag
    bool     decoded;     // MF known AND Cnt not "????"
} RollbackSlot;

typedef enum {
    RollbackOrderAsCaptured,
    RollbackOrderAsc,
    RollbackOrderDesc,
} RollbackOrder;

typedef struct {
    RollbackSlot slot[ROLLBACK_MAX_SLOTS];
    uint8_t count;
} RollbackStore;

// ---- pure parsing (shared with the read-only rolling analyzer) ----
uint32_t subghz_rollback_hex_after(const char* s, const char* marker);
bool     subghz_rollback_manufacturer(const char* s, char* out, size_t out_size);

// ---- pure slot math (host-tested) ----
void     subghz_rollback_store_reset(RollbackStore* st);
// Append a slot. Rejects an exact (serial,counter) duplicate and rejects when
// full. Only ever appends at slot[count] (never reorders) so an app-side
// parallel array stays index-aligned. Returns true if stored.
bool     subghz_rollback_store_add(RollbackStore* st, const RollbackSlot* s);
uint8_t  subghz_rollback_store_count(const RollbackStore* st);
// Ready = >=2 slots sharing one serial. Writes that serial to *same_serial
// (the serial of the largest same-serial group; first-seen wins on a tie).
bool     subghz_rollback_ready(const RollbackStore* st, uint32_t* same_serial);
// Fill out_idx with the slot indices whose serial == `serial`, in `order`.
// Asc/Desc sort by counter; AsCaptured keeps store order. Returns count written.
uint8_t  subghz_rollback_order_indices(
    const RollbackStore* st, uint32_t serial, RollbackOrder order,
    uint8_t* out_idx, uint8_t out_cap);

// ---- firmware glue (implemented in subghz_rollback.c, Task 2) ----
typedef struct SubGhz SubGhz;
typedef struct SubGhzProtocolDecoderBase SubGhzProtocolDecoderBase;
bool subghz_rollback_capture(SubGhz* subghz, SubGhzProtocolDecoderBase* decoder);
bool subghz_rollback_replay(
    SubGhz* subghz, RollbackOrder order, uint32_t delay_ms, volatile bool* abort);
