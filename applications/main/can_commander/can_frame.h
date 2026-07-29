#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Pure CAN frame model + hex/preset codec. No IDF/furi deps -> host-testable.

#define CAN_MAX_DATA 8
#define CAN_ID_STD_MAX 0x7FFu
#define CAN_ID_EXT_MAX 0x1FFFFFFFu

typedef struct {
    uint32_t id; // 11-bit (std) or 29-bit (ext)
    bool extended; // 29-bit extended identifier
    bool rtr; // remote transmission request (no data)
    uint8_t dlc; // data length, 0..8
    uint8_t data[CAN_MAX_DATA];
} CanFrame;

// Parse a hex identifier string ("1FF", "0x1FF", "18DAF110"). If force_ext is
// true the id is treated as extended; otherwise it is extended iff it exceeds
// the 11-bit range. Returns false on empty/invalid hex or id past the 29-bit max.
bool can_frame_parse_id(const char* s, bool force_ext, uint32_t* out_id, bool* out_ext);

// Parse a hex data-byte string. Accepts spaces, commas or colons between bytes
// ("DE AD BE EF", "deadbeef", "DE,AD"). Each byte is two hex nibbles. Returns
// false on invalid hex, an odd nibble count, or more than 8 bytes.
bool can_frame_parse_data(const char* s, uint8_t* out, uint8_t* out_len);

// Build a validated frame from id + data strings. rtr frames carry no data.
bool can_frame_build(
    const char* id_str,
    const char* data_str,
    bool force_ext,
    bool rtr,
    CanFrame* out);

// Format a frame as "ID#DATA" (uppercase hex; "ID#R" for RTR). Returns the
// number of characters written (excluding the NUL), 0 on error.
size_t can_frame_format(const CanFrame* f, char* out, size_t out_max);

// Preset line: "name,ext,rtr,idhex,datahex". Encode returns chars written (0 on
// error/overflow). Decode parses one line back into name + frame.
size_t can_frame_preset_encode(const char* name, const CanFrame* f, char* out, size_t out_max);
bool can_frame_preset_decode(const char* line, char* name_out, size_t name_max, CanFrame* out);
