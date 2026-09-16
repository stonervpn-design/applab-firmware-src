#pragma once
// Genie Intellicode "next-code" sequence store. Genie is a rolling code with NO published
// algorithm, so the only way to open a receiver repeatedly is to RECORD many real-remote
// presses (each a 64-bit code) in the order the receiver will accept them, then EMIT the next
// unrecorded-but-captured code on demand. This is a capture-and-reorder tool, NOT a crack:
// it can only send codes it physically recorded; run out => re-record. Format is byte-exact
// with the ARF genie-recorder ".gne" so tables are cross-compatible.
//
// .gne layout (big-endian): 16-byte header + 65536 records x 4 bytes = 262160 bytes.
//   [0]  u16 MAGIC   0x472A
//   [2]  u16 VERSION (major<<8)|minor  (load rejects major>2)
//   [4]  u32 SN      = code_lo (static btn|serial); also the filename {code_lo:08X}.gne
//   [8]  u16 LAST_SENT  emit cursor (index of the next record to send)
//   [10] u16 REC_COUNT  records captured so far (append pointer); 0xFFFF => full
//   [12] u32 RESERVED
//   [16] u32 DATA[]  captured code_hi (dynamic hop) values, in capture order
// The 64-bit decoded Genie code splits as code_hi=(data>>32) [dynamic], code_lo=(u32)data
// [static, keys the file]. My genie encoder TXes generic.data verbatim, so emitting record R
// = transmit (code_hi=DATA[R], code_lo=SN).
#include <stdint.h>
#include <stdbool.h>

#define GENIE_SEQ_MAGIC       0x472Au
#define GENIE_SEQ_VER_MAJOR   2u
#define GENIE_SEQ_VER_MINOR   5u
#define GENIE_SEQ_OFF_MAGIC   0
#define GENIE_SEQ_OFF_VERSION 2
#define GENIE_SEQ_OFF_SN      4
#define GENIE_SEQ_OFF_LASTS   8
#define GENIE_SEQ_OFF_RECCNT  10
#define GENIE_SEQ_OFF_DATA    16
#define GENIE_SEQ_MAX_RECORDS 65536u
#define GENIE_SEQ_FILE_SIZE   (GENIE_SEQ_OFF_DATA + GENIE_SEQ_MAX_RECORDS * 4u) /* 262160 */
#define GENIE_SEQ_FULL        0x10000u /* append/emit result: table full */

static inline uint32_t genie_seq_code_lo(uint64_t data) {
    return (uint32_t)data;
}
static inline uint32_t genie_seq_code_hi(uint64_t data) {
    return (uint32_t)(data >> 32);
}

// ---- pure core over a GENIE_SEQ_FILE_SIZE byte buffer (no storage/radio; host-testable) ----
void genie_seq_buf_init(uint8_t* buf, uint32_t code_lo); // zero + stamp header
bool genie_seq_buf_valid(const uint8_t* buf); // magic + version(major<=2)
uint16_t genie_seq_buf_rec_count(const uint8_t* buf);
uint16_t genie_seq_buf_last_sent(const uint8_t* buf);
uint32_t genie_seq_buf_sn(const uint8_t* buf);
uint32_t genie_seq_buf_code_at(const uint8_t* buf, uint16_t index); // code_hi at record index
void genie_seq_buf_set_last_sent(uint8_t* buf, uint16_t idx);
// Append one code_hi at REC_COUNT (matches ARF genie_save_bin). Returns the new rec_count, or
// the existing rec_count if the slot already holds a DIFFERENT non-zero code (resume guard), or
// GENIE_SEQ_FULL when the table is full.
uint32_t genie_seq_buf_append(uint8_t* buf, uint32_t code_hi);

// ---- device storage wrappers (Flipper storage API; seek+patch, never loads the 262KB) ----
// folder defaults to the My-Garage dir; one file per code_lo. Returns new rec_count / existing /
// GENIE_SEQ_FULL / 0 on error. Creates the file (zeros) on first capture.
uint32_t genie_seq_append_code(const char* folder, uint64_t code);
// Load header fields for the file keyed by code_lo. Returns false if absent/invalid.
bool genie_seq_stat(const char* folder, uint32_t code_lo, uint32_t* sn, uint16_t* last_sent, uint16_t* rec_count);
// Full 64-bit code (code_hi<<32 | SN) at a record index, for the file at `path`. 0 on error.
uint64_t genie_seq_code_at_path(const char* path, uint16_t index);
// Read header fields (SN / cursor / count) for the .gne at `path`. false if absent/invalid.
bool genie_seq_stat_path(const char* path, uint32_t* sn, uint16_t* last_sent, uint16_t* rec_count);
// Persist the emit cursor for the file at `path`.
bool genie_seq_set_last_sent_path(const char* path, uint16_t idx);
// Build "{folder}/{code_lo:08X}.gne" into out (size n).
void genie_seq_path_for(const char* folder, uint32_t code_lo, char* out, unsigned n);
