/**
 * Mazda V2 SubGHz Protocol — 2-FSK Manchester (Mazda 6 2011)
 *
 * Physical: 2-FSK @ 433.92 MHz, FuriHalSubGhzPreset2FSKDev476Async
 * CC1101 demodulates to baseband -> raw timings = Manchester edges.
 *
 * Manchester: te_short=500us, te_long=1000us
 * Preamble: ~80-180 alternating SHORT pairs (Manchester zeros)
 * Sync: EC (8 bits = 11101100) — breaks preamble pattern
 * Frame: 71 bits = EC(8) + data(63)
 *   [Button:8] [Serial:8] [Rolling:47]
 *   Button: 0x06=Lock, 0x0A=Unlock (bit 7 = repeat flag)
 *   Serial: 8 bits, fixed per remote (e.g. 0x5A)
 *   Rolling: 47 bits = hop[0:5] (hop[5] is 7 bits)
 *
 * Rolling code structure (obfuscation, NOT encryption):
 *   hop[0]: button/counter nibble encoding (redundant)
 *   hop[1]: counter-dependent with formula
 *   hop[2]: counter upper bits + XOR scramble
 *   hop[3]: counter low byte (increments ~8 per press)
 *   hop[4:5] (15 bits): f[h3 & 0x3F] ^ G[h3 >> 6] ^ M[h2]
 *     f = 64-entry lookup (21 known from captures)
 *     G = {0x0000, 0x2462, 0x48C4, 0x1A12}
 *     M = per-h2 XOR mask (13 known values)
 */

#include "mazda_v2.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include "../blocks/custom_btn_i.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolMazdaV2"

#define MAZDA_V2_DATA_BITS       63
#define MAZDA_V2_SYNC_BYTE       0xEC
#define MAZDA_V2_TOTAL_BURSTS    3
#define MAZDA_V2_PREAMBLE_PAIRS  80
#define MAZDA_V2_PREDICT_COUNT   3

static const SubGhzBlockConst subghz_protocol_mazda_v2_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 150,
    .min_count_bit_for_found = MAZDA_V2_DATA_BITS,
};

// ============================================================================
// ROLLING CODE TABLE
// Complete 256-entry lookup: counter -> {hop0, hop1, hop2_hi5, hop3_lo3, hop4, hop5_7b}
// Counter = (hop[2] & 0x07) << 5 | (hop[3] >> 3), 8 bits, +1 per button press
// 160/256 observed from captures, rest extrapolated via period-32 XOR
// ============================================================================


static const uint8_t mazda_v2_rc_lock[256][6] = {
    {0x5F,0x1B,0x11,0,0x3E,0x75}, // 0
    {0x5D,0x23,0x11,0,0x3E,0x47}, // 1
    {0x5F,0x33,0x11,2,0x23,0x7F}, // 2
    {0x5D,0x3B,0x11,3,0xF6,0x0F}, // 3
    {0x77,0x3B,0x13,2,0x0D,0x79}, // 4
    {0x76,0x43,0x12,2,0x02,0x27}, // 5
    {0x77,0x53,0x11,2,0x2E,0x3D}, // 6
    {0x76,0x5B,0x10,2,0xF9,0x67}, // 7
    {0x5F,0x1B,0x17,1,0x63,0x4D}, // 8
    {0x5E,0x63,0x16,1,0x6C,0x13}, // 9
    {0x5F,0x33,0x15,0,0x4D,0x4F}, // 10
    {0x5E,0x7B,0x14,1,0x97,0x53}, // 11
    {0x77,0x3B,0x13,2,0x45,0x1B}, // 12
    {0x76,0x83,0x12,1,0x5C,0x0F}, // 13
    {0x77,0x53,0x11,2,0x66,0x5F}, // 14
    {0x76,0x9B,0x10,1,0xA7,0x4F}, // 15
    {0x5F,0x1B,0x17,1,0xBA,0x6B}, // 16
    {0x5E,0xA3,0x16,2,0xA3,0x7F}, // 17
    {0x5F,0x33,0x15,0,0x94,0x69}, // 18
    {0x5E,0xBB,0x14,2,0x58,0x3F}, // 19
    {0x77,0x3B,0x13,2,0x9C,0x3D}, // 20
    {0x77,0xC3,0x12,0,0x2A,0x25}, // 21
    {0x77,0x53,0x11,2,0xBF,0x79}, // 22
    {0x76,0xDB,0x10,0,0x73,0x2F}, // 23
    {0x5F,0x1B,0x17,1,0xF2,0x09}, // 24
    {0x5E,0xE3,0x16,3,0xE6,0x5B}, // 25
    {0x5F,0x33,0x15,0,0xDC,0x0B}, // 26
    {0x5E,0xFA,0x16,0,0x19,0x0B}, // 27
    {0x77,0x3A,0x11,1,0xD0,0x4F}, // 28
    {0x77,0x02,0x10,0,0xA4,0x0D}, // 29
    {0x77,0x52,0x13,1,0xF3,0x0B}, // 30
    {0x77,0x1A,0x12,0,0x5F,0x4D}, // 31
    {0x5F,0x18,0x15,1,0x24,0x17}, // 32
    {0x5D,0x20,0x15,1,0x24,0x25}, // 33
    {0x5F,0x30,0x15,3,0x39,0x1D}, // 34
    {0x5D,0x38,0x15,2,0xEC,0x6D}, // 35
    {0x77,0x3B,0x11,0,0x1D,0x79}, // 36
    {0x76,0x43,0x10,0,0x12,0x27}, // 37
    {0x77,0x53,0x13,0,0x3E,0x3D}, // 38
    {0x76,0x5B,0x12,0,0xE9,0x67}, // 39
    {0x5F,0x1B,0x15,3,0x73,0x4D}, // 40
    {0x5E,0x63,0x14,3,0x7C,0x13}, // 41
    {0x5F,0x33,0x17,2,0x5D,0x4F}, // 42
    {0x5E,0x7B,0x16,3,0x87,0x53}, // 43
    {0x77,0x3B,0x11,0,0x55,0x1B}, // 44
    {0x76,0x83,0x10,3,0x4C,0x0F}, // 45
    {0x77,0x53,0x13,0,0x76,0x5F}, // 46
    {0x76,0x9B,0x12,3,0xB7,0x4F}, // 47
    {0x5F,0x1B,0x15,3,0xAA,0x6B}, // 48
    {0x5E,0xA3,0x14,0,0xB3,0x7F}, // 49
    {0x5F,0x33,0x17,2,0x84,0x69}, // 50
    {0x5E,0xBB,0x16,0,0x48,0x3F}, // 51
    {0x77,0x3B,0x11,0,0x8C,0x3D}, // 52
    {0x76,0xC3,0x10,2,0x98,0x6F}, // 53
    {0x77,0x53,0x13,0,0xAF,0x79}, // 54
    {0x76,0xDB,0x12,2,0x63,0x2F}, // 55
    {0x5F,0x1B,0x15,3,0xE2,0x09}, // 56
    {0x5E,0xE3,0x14,1,0xF6,0x5B}, // 57
    {0x5F,0x33,0x17,2,0xCC,0x0B}, // 58
    {0x5E,0xFB,0x16,1,0x0D,0x1B}, // 59
    {0x77,0x3B,0x11,0,0xC4,0x5F}, // 60
    {0x77,0x03,0x10,1,0xB0,0x1D}, // 61
    {0x77,0x53,0x13,0,0xE7,0x1B}, // 62
    {0x77,0x1B,0x12,1,0x4B,0x5D}, // 63
    {0x5F,0x19,0x15,0,0x30,0x07}, // 64
    {0x5D,0x21,0x15,0,0x30,0x35}, // 65
    {0x5F,0x31,0x15,2,0x2D,0x0D}, // 66
    {0x5D,0x39,0x15,3,0xF8,0x7D}, // 67
    {0x77,0x39,0x15,1,0x30,0x03}, // 68
    {0x75,0x41,0x15,0,0x3D,0x77}, // 69
    {0x77,0x51,0x15,2,0x20,0x4F}, // 70
    {0x75,0x59,0x15,3,0xF5,0x3F}, // 71
    {0x5F,0x19,0x1D,0,0x75,0x03}, // 72
    {0x5D,0x61,0x1D,1,0x78,0x77}, // 73
    {0x5F,0x31,0x1D,2,0x68,0x09}, // 74
    {0x5D,0x79,0x1D,2,0xB0,0x3F}, // 75
    {0x77,0x39,0x1D,1,0x75,0x07}, // 76
    {0x75,0x81,0x1D,3,0x6E,0x39}, // 77
    {0x77,0x51,0x1D,2,0x65,0x4B}, // 78
    {0x75,0x99,0x1D,0,0xA6,0x71}, // 79
    {0x5F,0x19,0x05,0,0xBA,0x0F}, // 80
    {0x5D,0xA1,0x05,2,0xA1,0x31}, // 81
    {0x5F,0x31,0x05,2,0xA7,0x05}, // 82
    {0x5D,0xB9,0x05,1,0x69,0x79}, // 83
    {0x77,0x39,0x05,1,0xBA,0x0B}, // 84
    {0x75,0xC1,0x05,2,0xAC,0x73}, // 85
    {0x77,0x51,0x05,2,0xAA,0x47}, // 86
    {0x75,0xD9,0x05,1,0x64,0x3B}, // 87
    {0x5F,0x19,0x0D,0,0xFF,0x0B}, // 88
    {0x5D,0xE1,0x0D,3,0xE9,0x73}, // 89
    {0x5F,0x31,0x0D,2,0xE2,0x01}, // 90
    {0x5D,0xF9,0x0D,0,0x21,0x3B}, // 91
    {0x77,0x39,0x0D,1,0xFF,0x0F}, // 92
    {0x76,0x01,0x0D,1,0x64,0x53}, // 93
    {0x77,0x51,0x0D,2,0xEF,0x43}, // 94
    {0x76,0x19,0x0D,2,0xAC,0x1B}, // 95
    {0x5F,0x18,0x15,1,0x24,0x17}, // 96
    {0x5C,0x20,0x15,1,0x52,0x7F}, // 97
    {0x5F,0x30,0x15,3,0x39,0x1D}, // 98
    {0x5C,0x38,0x15,2,0x9A,0x37}, // 99
    {0x77,0x38,0x15,0,0x24,0x13}, // 100
    {0x74,0x40,0x15,1,0x5F,0x3D}, // 101
    {0x77,0x50,0x15,3,0x34,0x5F}, // 102
    {0x74,0x58,0x15,2,0x97,0x75}, // 103
    {0x5F,0x18,0x1D,1,0x61,0x13}, // 104
    {0x5C,0x60,0x1D,0,0x1A,0x3D}, // 105
    {0x5F,0x30,0x1D,3,0x7C,0x19}, // 106
    {0x5C,0x78,0x1D,3,0xD2,0x75}, // 107
    {0x77,0x38,0x1D,0,0x61,0x17}, // 108
    {0x74,0x80,0x1D,2,0x0C,0x73}, // 109
    {0x77,0x50,0x1D,3,0x71,0x5B}, // 110
    {0x74,0x98,0x1D,1,0xC4,0x3B}, // 111
    {0x5F,0x18,0x05,1,0xAE,0x1F}, // 112
    {0x5C,0xA0,0x05,3,0xC3,0x7B}, // 113
    {0x5F,0x30,0x05,3,0xB3,0x15}, // 114
    {0x5C,0xB8,0x05,0,0x0B,0x33}, // 115
    {0x77,0x38,0x05,0,0xAE,0x1B}, // 116
    {0x74,0xC0,0x05,3,0xCE,0x39}, // 117
    {0x77,0x50,0x05,3,0xBE,0x57}, // 118
    {0x74,0xD8,0x05,0,0x06,0x71}, // 119
    {0x5F,0x18,0x0D,1,0xEB,0x1B}, // 120
    {0x5C,0xE0,0x0D,2,0x8B,0x39}, // 121
    {0x5F,0x30,0x0D,3,0xF6,0x11}, // 122
    {0x5C,0xF8,0x0D,1,0x43,0x71}, // 123
    {0x77,0x38,0x0D,0,0xEB,0x1F}, // 124
    {0x75,0x00,0x0D,0,0xEB,0x2D}, // 125
    {0x77,0x50,0x0D,3,0xFB,0x53}, // 126
    {0x75,0x18,0x0D,3,0x23,0x65}, // 127
    {0x5B,0x1B,0x15,3,0x0E,0x2D}, // 128
    {0x5B,0x23,0x14,2,0x7A,0x6F}, // 129
    {0x5B,0x33,0x17,2,0x20,0x2F}, // 130
    {0x5B,0x3B,0x16,2,0x81,0x2F}, // 131
    {0x77,0x3B,0x11,1,0xF2,0x13}, // 132
    {0x77,0x43,0x10,1,0x8B,0x17}, // 133
    {0x77,0x53,0x13,1,0xD1,0x57}, // 134
    {0x77,0x5B,0x12,1,0x70,0x57}, // 135
    {0x5B,0x1B,0x15,3,0x46,0x4F}, // 136
    {0x5B,0x63,0x14,3,0x3F,0x4B}, // 137
    {0x5B,0x33,0x17,2,0x68,0x4D}, // 138
    {0x5B,0x7B,0x16,3,0xC4,0x0B}, // 139
    {0x77,0x3B,0x11,1,0xBA,0x71}, // 140
    {0x77,0x83,0x10,2,0xD5,0x3F}, // 141
    {0x77,0x53,0x13,1,0x99,0x35}, // 142
    {0x77,0x9B,0x12,2,0x2E,0x7F}, // 143
    {0x5B,0x1B,0x15,3,0x9F,0x69}, // 144
    {0x5B,0xA3,0x14,0,0xF0,0x27}, // 145
    {0x5B,0x33,0x17,2,0xB1,0x6B}, // 146
    {0x5B,0xBB,0x16,0,0x0B,0x67}, // 147
    {0x77,0x3B,0x11,1,0x63,0x57}, // 148
    {0x77,0xC3,0x10,3,0x01,0x5F}, // 149
    {0x77,0x53,0x13,1,0x40,0x13}, // 150
    {0x77,0xDB,0x12,3,0xFA,0x1F}, // 151
    {0x5B,0x1B,0x15,3,0xD7,0x0B}, // 152
    {0x5B,0xE3,0x14,1,0xB5,0x03}, // 153
    {0x5B,0x33,0x17,2,0xF9,0x09}, // 154
    {0x5B,0xFB,0x16,1,0x4E,0x43}, // 155
    {0x77,0x3B,0x11,1,0x2B,0x35}, // 156
    {0x78,0x03,0x10,2,0xAB,0x21}, // 157
    {0x77,0x53,0x13,1,0x08,0x71}, // 158
    {0x78,0x1B,0x12,2,0x50,0x61}, // 159
    {0x5B,0x1B,0x15,2,0x2D,0x25}, // 160
    {0x5A,0x23,0x14,3,0x2F,0x3D}, // 161
    {0x5B,0x33,0x17,3,0x03,0x27}, // 162
    {0x5A,0x3B,0x16,3,0xD4,0x7D}, // 163
    {0x77,0x3B,0x11,0,0xD1,0x1B}, // 164
    {0x76,0x43,0x10,0,0xDE,0x45}, // 165
    {0x77,0x53,0x13,0,0xF2,0x5F}, // 166
    {0x76,0x5B,0x12,0,0x25,0x05}, // 167
    {0x5B,0x1B,0x15,2,0x65,0x47}, // 168
    {0x5A,0x63,0x14,2,0x6A,0x19}, // 169
    {0x5B,0x33,0x17,3,0x4B,0x45}, // 170
    {0x5A,0x7B,0x16,2,0x91,0x59}, // 171
    {0x77,0x3B,0x11,0,0x99,0x79}, // 172
    {0x76,0x83,0x10,3,0x80,0x6D}, // 173
    {0x77,0x53,0x13,0,0xBA,0x3D}, // 174
    {0x76,0x9B,0x12,3,0x7B,0x2D}, // 175
    {0x5B,0x1B,0x15,2,0xBC,0x61}, // 176
    {0x5A,0xA3,0x14,1,0xA5,0x75}, // 177
    {0x5B,0x33,0x17,3,0x92,0x63}, // 178
    {0x5A,0xBB,0x16,1,0x5E,0x35}, // 179
    {0x77,0x3B,0x11,0,0x40,0x5F}, // 180
    {0x76,0xC3,0x10,2,0x54,0x0D}, // 181
    {0x77,0x53,0x13,0,0x63,0x1B}, // 182
    {0x76,0xDB,0x12,2,0xAF,0x4D}, // 183
    {0x5B,0x1B,0x15,2,0xF4,0x03}, // 184
    {0x5A,0xE3,0x14,0,0xE0,0x51}, // 185
    {0x5B,0x33,0x17,3,0xDA,0x01}, // 186
    {0x5A,0xFB,0x16,0,0x1B,0x11}, // 187
    {0x77,0x3B,0x11,0,0x08,0x3D}, // 188
    {0x77,0x03,0x10,1,0x7C,0x7F}, // 189
    {0x77,0x53,0x13,0,0x2B,0x79}, // 190
    {0x77,0x1B,0x12,1,0x87,0x3F}, // 191
    {0x5B,0x19,0x15,1,0x26,0x0D}, // 192
    {0x59,0x21,0x15,1,0x26,0x3F}, // 193
    {0x5B,0x31,0x15,3,0x3B,0x07}, // 194
    {0x59,0x39,0x15,2,0xEE,0x77}, // 195
    {0x77,0x39,0x15,1,0xFC,0x61}, // 196
    {0x75,0x41,0x15,0,0xF1,0x15}, // 197
    {0x77,0x51,0x15,2,0xEC,0x2D}, // 198
    {0x75,0x59,0x15,3,0x39,0x5D}, // 199
    {0x5B,0x19,0x1D,1,0x63,0x09}, // 200
    {0x59,0x61,0x1D,0,0x6E,0x7D}, // 201
    {0x5B,0x31,0x1D,3,0x7E,0x03}, // 202
    {0x59,0x79,0x1D,3,0xA6,0x35}, // 203
    {0x77,0x39,0x1D,1,0xB9,0x65}, // 204
    {0x75,0x81,0x1D,3,0xA2,0x5B}, // 205
    {0x77,0x51,0x1D,2,0xA9,0x29}, // 206
    {0x75,0x99,0x1D,0,0x6A,0x13}, // 207
    {0x5B,0x19,0x05,1,0xAC,0x05}, // 208
    {0x59,0xA1,0x05,3,0xB7,0x3B}, // 209
    {0x5B,0x31,0x05,3,0xB1,0x0F}, // 210
    {0x59,0xB9,0x05,0,0x7F,0x73}, // 211
    {0x77,0x39,0x05,1,0x76,0x69}, // 212
    {0x75,0xC1,0x05,2,0x60,0x11}, // 213
    {0x77,0x51,0x05,2,0x66,0x25}, // 214
    {0x75,0xD9,0x05,1,0xA8,0x59}, // 215
    {0x5B,0x19,0x0D,1,0xE9,0x01}, // 216
    {0x59,0xE1,0x0D,2,0xFF,0x79}, // 217
    {0x5B,0x31,0x0D,3,0xF4,0x0B}, // 218
    {0x59,0xF9,0x0D,1,0x37,0x31}, // 219
    {0x77,0x39,0x0D,1,0x33,0x6D}, // 220
    {0x76,0x01,0x0D,1,0xA8,0x31}, // 221
    {0x77,0x51,0x0D,2,0x23,0x21}, // 222
    {0x76,0x19,0x0D,2,0x60,0x79}, // 223
    {0x5B,0x18,0x15,0,0x32,0x1D}, // 224
    {0x58,0x20,0x15,0,0x44,0x75}, // 225
    {0x5B,0x30,0x15,2,0x2F,0x17}, // 226
    {0x58,0x38,0x15,3,0x8C,0x3D}, // 227
    {0x77,0x3B,0x11,0,0x1D,0x79}, // 228
    {0x76,0x43,0x10,0,0x12,0x27}, // 229
    {0x77,0x50,0x16,1,0x86,0x61}, // 230
    {0x76,0x5B,0x12,0,0xE9,0x67}, // 231
    {0x5F,0x1B,0x15,3,0x73,0x4D}, // 232
    {0x5E,0x63,0x14,3,0x7C,0x13}, // 233
    {0x43,0x30,0x1E,0,0x8A,0x7D}, // 234
    {0x5E,0x7B,0x16,3,0x87,0x53}, // 235
    {0x77,0x3B,0x11,0,0x55,0x1B}, // 236
    {0x76,0x83,0x10,3,0x4C,0x0F}, // 237
    {0x77,0x53,0x13,0,0x76,0x5F}, // 238
    {0x76,0x9B,0x12,3,0xB7,0x4F}, // 239
    {0x5F,0x1B,0x15,3,0xAA,0x6B}, // 240
    {0x5E,0xA3,0x14,0,0xB3,0x7F}, // 241
    {0x43,0x30,0x06,0,0x91,0x61}, // 242
    {0x5E,0xBB,0x16,0,0x48,0x3F}, // 243
    {0x77,0x3B,0x11,0,0x8C,0x3D}, // 244
    {0x77,0xC3,0x10,2,0x3A,0x25}, // 245
    {0x77,0x53,0x13,0,0xAF,0x79}, // 246
    {0x76,0xDB,0x12,2,0x63,0x2F}, // 247
    {0x5F,0x1B,0x15,3,0xE2,0x09}, // 248
    {0x5E,0xE3,0x14,1,0xF6,0x5B}, // 249
    {0x5F,0x33,0x17,2,0xCC,0x0B}, // 250
    {0x5E,0xFA,0x14,2,0x09,0x0B}, // 251
    {0x77,0x3A,0x13,3,0xC0,0x4F}, // 252
    {0x77,0x02,0x12,2,0xB4,0x0D}, // 253
    {0x77,0x52,0x11,3,0xE3,0x0B}, // 254
    {0x75,0x18,0x0E,1,0x91,0x5B} // 255
};

static const uint8_t mazda_v2_rc_unlock[256][6] = {
    {0x47,0x1B,0x16,3,0x17,0x19}, // 0
    {0x47,0x23,0x17,2,0x63,0x5B}, // 1
    {0x47,0x33,0x14,2,0x39,0x1B}, // 2
    {0x47,0x3B,0x15,2,0x98,0x1B}, // 3
    {0x77,0x3B,0x12,0,0xAF,0x7D}, // 4
    {0x77,0x43,0x13,0,0xD6,0x79}, // 5
    {0x77,0x53,0x10,0,0x8C,0x39}, // 6
    {0x77,0x5B,0x11,0,0x2D,0x39}, // 7
    {0x47,0x1B,0x16,3,0x5F,0x7B}, // 8
    {0x47,0x63,0x17,3,0x26,0x7F}, // 9
    {0x47,0x33,0x14,2,0x71,0x79}, // 10
    {0x47,0x7B,0x15,3,0xDD,0x3F}, // 11
    {0x77,0x3B,0x12,0,0xE7,0x1F}, // 12
    {0x77,0x83,0x13,3,0x88,0x51}, // 13
    {0x77,0x53,0x10,0,0xC4,0x5B}, // 14
    {0x77,0x9B,0x11,3,0x73,0x11}, // 15
    {0x47,0x1B,0x16,3,0x86,0x5D}, // 16
    {0x47,0xA3,0x17,0,0xE9,0x13}, // 17
    {0x47,0x33,0x14,2,0xA8,0x5F}, // 18
    {0x47,0xBB,0x15,0,0x12,0x53}, // 19
    {0x77,0x3B,0x12,0,0x3E,0x39}, // 20
    {0x77,0xC3,0x13,2,0x5C,0x31}, // 21
    {0x77,0x53,0x10,0,0x1D,0x7D}, // 22
    {0x77,0xDB,0x11,2,0xA7,0x71}, // 23
    {0x47,0x1B,0x16,3,0xCE,0x3F}, // 24
    {0x47,0xE3,0x17,1,0xAC,0x37}, // 25
    {0x47,0x33,0x14,2,0xE0,0x3D}, // 26
    {0x47,0xFB,0x15,1,0x57,0x77}, // 27
    {0x77,0x3B,0x12,0,0x76,0x5B}, // 28
    {0x78,0x03,0x13,3,0xF6,0x4F}, // 29
    {0x77,0x53,0x10,0,0x55,0x1F}, // 30
    {0x78,0x1B,0x11,3,0x0D,0x0F}, // 31
    {0x47,0x1B,0x16,2,0x34,0x11}, // 32
    {0x46,0x23,0x17,3,0x36,0x09}, // 33
    {0x47,0x33,0x14,3,0x1A,0x13}, // 34
    {0x46,0x3B,0x15,3,0xCD,0x49}, // 35
    {0x77,0x3B,0x12,1,0x8C,0x75}, // 36
    {0x76,0x43,0x13,1,0x83,0x2B}, // 37
    {0x77,0x53,0x10,1,0xAF,0x31}, // 38
    {0x76,0x5B,0x11,1,0x78,0x6B}, // 39
    {0x47,0x1B,0x16,2,0x7C,0x73}, // 40
    {0x46,0x63,0x17,2,0x73,0x2D}, // 41
    {0x47,0x33,0x14,3,0x52,0x71}, // 42
    {0x46,0x7B,0x15,2,0x88,0x6D}, // 43
    {0x77,0x3B,0x12,1,0xC4,0x17}, // 44
    {0x76,0x83,0x13,2,0xDD,0x03}, // 45
    {0x77,0x53,0x10,1,0xE7,0x53}, // 46
    {0x76,0x9B,0x11,2,0x26,0x43}, // 47
    {0x47,0x1B,0x16,2,0xA5,0x55}, // 48
    {0x46,0xA3,0x17,1,0xBC,0x41}, // 49
    {0x47,0x33,0x14,3,0x8B,0x57}, // 50
    {0x46,0xBB,0x15,1,0x47,0x01}, // 51
    {0x77,0x3B,0x12,1,0x1D,0x31}, // 52
    {0x76,0xC3,0x13,3,0x09,0x63}, // 53
    {0x77,0x53,0x10,1,0x3E,0x75}, // 54
    {0x76,0xDB,0x11,3,0xF2,0x23}, // 55
    {0x47,0x1B,0x16,2,0xED,0x37}, // 56
    {0x46,0xE3,0x17,0,0xF9,0x65}, // 57
    {0x47,0x33,0x14,3,0xC3,0x35}, // 58
    {0x46,0xFB,0x15,0,0x02,0x25}, // 59
    {0x77,0x3B,0x12,1,0x55,0x53}, // 60
    {0x77,0x03,0x13,0,0x21,0x11}, // 61
    {0x77,0x53,0x10,1,0x76,0x17}, // 62
    {0x77,0x1B,0x11,0,0xDA,0x51}, // 63
    {0x47,0x19,0x16,1,0x3F,0x39}, // 64
    {0x45,0x21,0x16,1,0x3F,0x0B}, // 65
    {0x47,0x31,0x16,3,0x22,0x33}, // 66
    {0x45,0x39,0x16,2,0xF7,0x43}, // 67
    {0x77,0x39,0x16,0,0xA1,0x0F}, // 68
    {0x75,0x41,0x16,1,0xAC,0x7B}, // 69
    {0x77,0x51,0x16,3,0xB1,0x43}, // 70
    {0x75,0x59,0x16,2,0x64,0x33}, // 71
    {0x47,0x19,0x1E,1,0x7A,0x3D}, // 72
    {0x45,0x61,0x1E,0,0x77,0x49}, // 73
    {0x47,0x31,0x1E,3,0x67,0x37}, // 74
    {0x45,0x79,0x1E,3,0xBF,0x01}, // 75
    {0x77,0x39,0x1E,0,0xE4,0x0B}, // 76
    {0x75,0x81,0x1E,2,0xFF,0x35}, // 77
    {0x77,0x51,0x1E,3,0xF4,0x47}, // 78
    {0x75,0x99,0x1E,1,0x37,0x7D}, // 79
    {0x47,0x19,0x06,1,0xB5,0x31}, // 80
    {0x45,0xA1,0x06,3,0xAE,0x0F}, // 81
    {0x47,0x31,0x06,3,0xA8,0x3B}, // 82
    {0x45,0xB9,0x06,0,0x66,0x47}, // 83
    {0x77,0x39,0x06,0,0x2B,0x07}, // 84
    {0x75,0xC1,0x06,3,0x3D,0x7F}, // 85
    {0x77,0x51,0x06,3,0x3B,0x4B}, // 86
    {0x75,0xD9,0x06,0,0xF5,0x37}, // 87
    {0x47,0x19,0x0E,1,0xF0,0x35}, // 88
    {0x45,0xE1,0x0E,2,0xE6,0x4D}, // 89
    {0x47,0x31,0x0E,3,0xED,0x3F}, // 90
    {0x45,0xF9,0x0E,1,0x2E,0x05}, // 91
    {0x77,0x39,0x0E,0,0x6E,0x03}, // 92
    {0x76,0x01,0x0E,0,0xF5,0x5F}, // 93
    {0x77,0x51,0x0E,3,0x7E,0x4F}, // 94
    {0x76,0x19,0x0E,3,0x3D,0x17}, // 95
    {0x47,0x18,0x16,0,0x2B,0x29}, // 96
    {0x44,0x20,0x16,0,0x5D,0x41}, // 97
    {0x47,0x30,0x16,2,0x36,0x23}, // 98
    {0x44,0x38,0x16,3,0x95,0x09}, // 99
    {0x77,0x38,0x16,1,0xB5,0x1F}, // 100
    {0x74,0x40,0x16,0,0xCE,0x31}, // 101
    {0x77,0x50,0x16,2,0xA5,0x53}, // 102
    {0x74,0x58,0x16,3,0x06,0x79}, // 103
    {0x47,0x18,0x1E,0,0x6E,0x2D}, // 104
    {0x44,0x60,0x1E,1,0x15,0x03}, // 105
    {0x47,0x30,0x1E,2,0x73,0x27}, // 106
    {0x44,0x78,0x1E,2,0xDD,0x4B}, // 107
    {0x77,0x38,0x1E,1,0xF0,0x1B}, // 108
    {0x74,0x80,0x1E,3,0x9D,0x7F}, // 109
    {0x77,0x50,0x1E,2,0xE0,0x57}, // 110
    {0x74,0x98,0x1E,0,0x55,0x37}, // 111
    {0x47,0x18,0x06,0,0xA1,0x21}, // 112
    {0x44,0xA0,0x06,2,0xCC,0x45}, // 113
    {0x47,0x30,0x06,2,0xBC,0x2B}, // 114
    {0x44,0xB8,0x06,1,0x04,0x0D}, // 115
    {0x77,0x38,0x06,1,0x3F,0x17}, // 116
    {0x74,0xC0,0x06,2,0x5F,0x35}, // 117
    {0x77,0x50,0x06,2,0x2F,0x5B}, // 118
    {0x74,0xD8,0x06,1,0x97,0x7D}, // 119
    {0x47,0x18,0x0E,0,0xE4,0x25}, // 120
    {0x44,0xE0,0x0E,3,0x84,0x07}, // 121
    {0x47,0x30,0x0E,2,0xF9,0x2F}, // 122
    {0x44,0xF8,0x0E,0,0x4C,0x4F}, // 123
    {0x77,0x38,0x0E,1,0x7A,0x13}, // 124
    {0x75,0x00,0x0E,1,0x7A,0x21}, // 125
    {0x77,0x50,0x0E,2,0x6A,0x5F}, // 126
    {0x75,0x18,0x0E,2,0xB2,0x69}, // 127
    {0x43,0x1B,0x16,2,0x01,0x13}, // 128
    {0x43,0x23,0x17,3,0x75,0x51}, // 129
    {0x43,0x33,0x14,3,0x2F,0x11}, // 130
    {0x4B,0x3B,0x1E,2,0x48,0x7B}, // 131
    {0x78,0x3B,0x11,0,0x3D,0x05}, // 132
    {0x79,0x43,0x10,0,0x32,0x5B}, // 133
    {0x65,0x53,0x1E,2,0x4A,0x21}, // 134
    {0x67,0x5B,0x1E,3,0x9F,0x51}, // 135
    {0x49,0x1B,0x16,1,0xC5,0x05}, // 136
    {0x4B,0x63,0x16,0,0xC8,0x71}, // 137
    {0x49,0x33,0x16,3,0xD8,0x0F}, // 138
    {0x4B,0x7B,0x16,3,0x00,0x39}, // 139
    {0x65,0x3B,0x16,1,0x1F,0x69}, // 140
    {0x67,0x83,0x16,3,0x04,0x57}, // 141
    {0x65,0x53,0x16,2,0x0F,0x25}, // 142
    {0x67,0x9B,0x16,0,0xCC,0x1F}, // 143
    {0x49,0x1B,0x0E,1,0x0A,0x09}, // 144
    {0x45,0xA3,0x14,2,0x62,0x09}, // 145
    {0x49,0x33,0x0E,3,0x17,0x03}, // 146
    {0x4B,0xBB,0x0E,0,0xD9,0x7F}, // 147
    {0x65,0x3B,0x0E,1,0xD0,0x65}, // 148
    {0x67,0xC3,0x0E,2,0xC6,0x1D}, // 149
    {0x76,0x51,0x1A,1,0xB1,0x0B}, // 150
    {0x77,0xD9,0x1B,3,0x7D,0x5D}, // 151
    {0x46,0x19,0x1C,2,0x62,0x49}, // 152
    {0x47,0xE1,0x1D,0,0x76,0x1B}, // 153
    {0x54,0x33,0x15,0,0xD1,0x07}, // 154
    {0x5A,0xFA,0x16,2,0x34,0x41}, // 155
    {0x77,0x3A,0x11,2,0x27,0x6D}, // 156
    {0x78,0x03,0x12,2,0x77,0x79}, // 157
    {0x77,0x52,0x13,2,0x04,0x29}, // 158
    {0x77,0x1A,0x12,3,0xA8,0x6F}, // 159
    {0x5B,0x18,0x15,3,0x09,0x5D}, // 160
    {0x59,0x20,0x15,3,0x09,0x6F}, // 161
    {0x4B,0x33,0x15,0,0xF4,0x1D}, // 162
    {0x59,0x38,0x15,0,0xC1,0x27}, // 163
    {0x77,0x3B,0x13,0,0x0D,0x43}, // 164
    {0x76,0x43,0x12,0,0x02,0x1D}, // 165
    {0x77,0x50,0x15,0,0xC3,0x7D}, // 166
    {0x75,0x58,0x15,1,0x16,0x0D}, // 167
    {0x5B,0x18,0x1D,3,0x4C,0x59}, // 168
    {0x59,0x60,0x1D,2,0x41,0x2D}, // 169
    {0x5B,0x30,0x1D,1,0x51,0x53}, // 170
    {0x59,0x78,0x1D,1,0x89,0x65}, // 171
    {0x77,0x38,0x1D,3,0x96,0x35}, // 172
    {0x75,0x80,0x1D,1,0x8D,0x0B}, // 173
    {0x77,0x50,0x1D,0,0x86,0x79}, // 174
    {0x75,0x98,0x1D,2,0x45,0x43}, // 175
    {0x5B,0x18,0x05,3,0x83,0x55}, // 176
    {0x4A,0xA3,0x16,2,0x52,0x4F}, // 177
    {0x5B,0x30,0x05,1,0x9E,0x5F}, // 178
    {0x59,0xB8,0x05,2,0x50,0x23}, // 179
    {0x77,0x38,0x05,3,0x59,0x39}, // 180
    {0x75,0xC0,0x05,0,0x4F,0x41}, // 181
    {0x77,0x50,0x05,0,0x49,0x75}, // 182
    {0x75,0xD8,0x05,3,0x87,0x09}, // 183
    {0x5B,0x18,0x0D,3,0xC6,0x51}, // 184
    {0x59,0xE0,0x0D,0,0xD0,0x29}, // 185
    {0x5B,0x33,0x17,0,0xE1,0x41}, // 186
    {0x5A,0xFB,0x16,3,0x20,0x51}, // 187
    {0x77,0x3B,0x11,3,0x33,0x7D}, // 188
    {0x77,0x03,0x10,2,0x47,0x3F}, // 189
    {0x77,0x53,0x13,3,0x10,0x39}, // 190
    {0x77,0x1B,0x12,2,0xBC,0x7F}, // 191
    {0x5B,0x19,0x15,2,0x1D,0x4D}, // 192
    {0x59,0x21,0x15,2,0x1D,0x7F}, // 193
    {0x5B,0x31,0x15,0,0x00,0x47}, // 194
    {0x59,0x39,0x15,1,0xD5,0x37}, // 195
    {0x77,0x39,0x15,2,0xC7,0x21}, // 196
    {0x75,0x41,0x15,3,0xCA,0x55}, // 197
    {0x77,0x51,0x15,1,0xD7,0x6D}, // 198
    {0x75,0x59,0x15,0,0x02,0x1D}, // 199
    {0x5B,0x19,0x1D,2,0x58,0x49}, // 200
    {0x59,0x61,0x1D,3,0x55,0x3D}, // 201
    {0x5B,0x31,0x1D,0,0x45,0x43}, // 202
    {0x59,0x79,0x1D,0,0x9D,0x75}, // 203
    {0x77,0x39,0x1D,2,0x82,0x25}, // 204
    {0x75,0x81,0x1D,0,0x99,0x1B}, // 205
    {0x77,0x51,0x1D,1,0x92,0x69}, // 206
    {0x75,0x99,0x1D,3,0x51,0x53}, // 207
    {0x5B,0x19,0x05,2,0x97,0x45}, // 208
    {0x59,0xA1,0x05,0,0x8C,0x7B}, // 209
    {0x5B,0x31,0x05,0,0x8A,0x4F}, // 210
    {0x59,0xB9,0x05,3,0x44,0x33}, // 211
    {0x77,0x39,0x05,2,0x4D,0x29}, // 212
    {0x75,0xC1,0x05,1,0x5B,0x51}, // 213
    {0x77,0x51,0x05,1,0x5D,0x65}, // 214
    {0x75,0xD9,0x05,2,0x93,0x19}, // 215
    {0x5B,0x19,0x0D,2,0xD2,0x41}, // 216
    {0x59,0xE1,0x0D,1,0xC4,0x39}, // 217
    {0x5B,0x31,0x0D,0,0xCF,0x4B}, // 218
    {0x59,0xF9,0x0D,2,0x0C,0x71}, // 219
    {0x77,0x39,0x0D,2,0x08,0x2D}, // 220
    {0x76,0x01,0x0D,2,0x93,0x71}, // 221
    {0x77,0x51,0x0D,1,0x18,0x61}, // 222
    {0x76,0x19,0x0D,1,0x5B,0x39}, // 223
    {0x5B,0x18,0x15,3,0x09,0x5D}, // 224
    {0x58,0x20,0x15,3,0x7F,0x35}, // 225
    {0x5B,0x30,0x15,1,0x14,0x57}, // 226
    {0x58,0x38,0x15,0,0xB7,0x7D}, // 227
    {0x77,0x38,0x15,3,0xD3,0x31}, // 228
    {0x74,0x40,0x15,2,0xA8,0x1F}, // 229
    {0x77,0x50,0x15,0,0xC3,0x7D}, // 230
    {0x74,0x58,0x15,1,0x60,0x57}, // 231
    {0x5B,0x18,0x1D,3,0x4C,0x59}, // 232
    {0x58,0x60,0x1D,2,0x37,0x77}, // 233
    {0x5B,0x30,0x1D,1,0x51,0x53}, // 234
    {0x58,0x78,0x1D,1,0xFF,0x3F}, // 235
    {0x77,0x38,0x1D,3,0x96,0x35}, // 236
    {0x74,0x80,0x1D,1,0xFB,0x51}, // 237
    {0x77,0x50,0x1D,0,0x86,0x79}, // 238
    {0x74,0x98,0x1D,2,0x33,0x19}, // 239
    {0x5B,0x18,0x05,3,0x83,0x55}, // 240
    {0x58,0xA0,0x05,1,0xEE,0x31}, // 241
    {0x5B,0x30,0x05,1,0x9E,0x5F}, // 242
    {0x58,0xB8,0x05,2,0x26,0x79}, // 243
    {0x77,0x38,0x05,3,0x59,0x39}, // 244
    {0x74,0xC0,0x05,0,0x39,0x1B}, // 245
    {0x77,0x50,0x05,0,0x49,0x75}, // 246
    {0x74,0xD8,0x05,3,0xF1,0x53}, // 247
    {0x5B,0x18,0x0D,3,0xC6,0x51}, // 248
    {0x58,0xE0,0x0D,0,0xA6,0x73}, // 249
    {0x5B,0x30,0x0D,1,0xDB,0x5B}, // 250
    {0x58,0xF8,0x0D,3,0x6E,0x3B}, // 251
    {0x77,0x38,0x0D,3,0x1C,0x3D}, // 252
    {0x75,0x00,0x0D,3,0x1C,0x0F}, // 253
    {0x77,0x50,0x0D,0,0x0C,0x71}, // 254
    {0x75,0x18,0x0D,0,0xD4,0x47} // 255
};


// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    MazdaV2DecoderStepReset = 0,
    MazdaV2DecoderStepCollect,
} MazdaV2DecoderStep;

struct SubGhzProtocolDecoderMazdaV2 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    bool half_bit;

    uint8_t all_bits[400];
    uint16_t all_bit_count;
};

struct SubGhzProtocolEncoderMazdaV2 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    // Cycle XOR mask for slot (ctr/2)%4, detected at deserialization
    uint8_t cycle_xor[6];
    uint8_t cycle_xor_slot; // which slot the mask belongs to
    bool cycle_xor_active;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

#ifdef SUBGHZ_DECODER_TEST
static SubGhzBlockGeneric* subghz_protocol_decoder_mazda_v2_get_generic(void* context) {
    SubGhzProtocolDecoderMazdaV2* instance = context;
    return &instance->generic;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_mazda_v2_decoder = {
    .alloc = subghz_protocol_decoder_mazda_v2_alloc,
    .free = subghz_protocol_decoder_mazda_v2_free,
    .feed = subghz_protocol_decoder_mazda_v2_feed,
    .reset = subghz_protocol_decoder_mazda_v2_reset,
    .get_hash_data = subghz_protocol_decoder_mazda_v2_get_hash_data,
    .serialize = subghz_protocol_decoder_mazda_v2_serialize,
    .deserialize = subghz_protocol_decoder_mazda_v2_deserialize,
    .get_string = subghz_protocol_decoder_mazda_v2_get_string,
#ifdef SUBGHZ_DECODER_TEST
    .get_generic = subghz_protocol_decoder_mazda_v2_get_generic,
#endif
};

const SubGhzProtocolEncoder subghz_protocol_mazda_v2_encoder = {
    .alloc = subghz_protocol_encoder_mazda_v2_alloc,
    .free = subghz_protocol_encoder_mazda_v2_free,
    .deserialize = subghz_protocol_encoder_mazda_v2_deserialize,
    .stop = subghz_protocol_encoder_mazda_v2_stop,
    .yield = subghz_protocol_encoder_mazda_v2_yield,
};

const SubGhzProtocol subghz_protocol_mazda_v2 = {
    .name = SUBGHZ_PROTOCOL_MAZDA_V2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_mazda_v2_decoder,
    .encoder = &subghz_protocol_mazda_v2_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_mazda_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMazdaV2* instance = malloc(sizeof(SubGhzProtocolDecoderMazdaV2));
    instance->base.protocol = &subghz_protocol_mazda_v2;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_mazda_v2_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_mazda_v2_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV2* instance = context;
    instance->decoder.parser_step = MazdaV2DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->half_bit = false;
    instance->all_bit_count = 0;
}

/**
 * Derive cycle XOR mask for a target slot from an observed mask at a source slot.
 * The mask relates table A (base cycle) to the actual cycle.
 * Bytes 0-3 follow deterministic rules based on slot index.
 * Bytes 4-5 use inter-slot XOR constants.
 *
 * Slot = (counter / 2) % 4. Mask structure:
 *   byte0: slots 0,1 have value X, slots 2,3 have 0x00
 *   byte1: even slots have 0x04, odd slots 0x00  (if non-zero mask)
 *   byte2: constant across all slots
 *   byte3: slots 0,1 have value A, slots 2,3 have value A ^ half_xor3
 *   bytes 4,5: chain via known inter-slot XOR constants
 */
/**
 * Region conversion XOR: converts between lock-type and unlock-type masks.
 * Lock-type (byte2=0x0D): used by lock buttons, and unlock counters 224-255.
 * Unlock-type (byte2=0x0F): used by unlock counters 0-223.
 * The conversion XOR differs between hi-half (slots 0,1) and lo-half (slots 2,3).
 */
static const uint8_t mazda_v2_region_xor_hi[6] = {0x10, 0x00, 0x02, 0x00, 0x18, 0x6A};
static const uint8_t mazda_v2_region_xor_lo[6] = {0x00, 0x00, 0x02, 0x03, 0x33, 0x08};

static void mazda_v2_derive_slot_mask(
    const uint8_t src_mask[6], uint8_t src_slot,
    uint8_t dst_slot, bool dst_unlock_region, uint8_t dst_mask[6]) {

    bool src_lock_type = (src_mask[2] == 0x0D);
    bool dst_lock_type = !dst_unlock_region;

    // Start with source mask
    uint8_t work[6];
    for(int i = 0; i < 6; i++) work[i] = src_mask[i];

    // Convert region if source and destination use different mask types
    if(src_lock_type != dst_lock_type) {
        const uint8_t* rxor = (src_slot < 2) ? mazda_v2_region_xor_hi : mazda_v2_region_xor_lo;
        for(int i = 0; i < 6; i++) work[i] ^= rxor[i];
    }

    if(src_slot == dst_slot) {
        for(int i = 0; i < 6; i++) dst_mask[i] = work[i];
        return;
    }

    // Now derive slot mask using destination region's constants
    bool lt = dst_lock_type;

    // byte0: hi half has nonzero value, lo half has 0x00
    uint8_t byte0_half = lt ? 0x28 : 0x38;
    uint8_t byte0_hi = (src_slot < 2) ? work[0] : byte0_half;
    dst_mask[0] = (dst_slot < 2) ? byte0_hi : 0x00;

    // byte1: even slots have value, odd slots differ by 0x04
    uint8_t byte1_even = (src_slot % 2 == 0) ? work[1] : (work[1] ^ 0x04);
    dst_mask[1] = (dst_slot % 2 == 0) ? byte1_even : (byte1_even ^ 0x04);

    // byte2: constant across slots within same region
    dst_mask[2] = lt ? 0x0D : 0x0F;

    // byte3: hi/lo half differ by half_xor3
    uint8_t half_xor3 = lt ? 0x01 : 0x02;
    uint8_t byte3_hi = (src_slot < 2) ? work[3] : (work[3] ^ half_xor3);
    dst_mask[3] = (dst_slot < 2) ? byte3_hi : (byte3_hi ^ half_xor3);

    // bytes 4,5: chain via inter-slot XOR
    uint8_t oe4 = lt ? 0x7E : 0x55;
    uint8_t oe5 = lt ? 0x36 : 0x54;

    dst_mask[4] = work[4];
    dst_mask[5] = work[5];

    int s = src_slot;
    while(s != dst_slot) {
        if(s % 2 == 0) {
            dst_mask[4] ^= 0xDC;
            dst_mask[5] ^= 0x60;
        } else {
            dst_mask[4] ^= oe4;
            dst_mask[5] ^= oe5;
        }
        s = (s + 1) % 4;
    }
}

static const char* mazda_v2_btn_name(uint8_t btn) {
    switch(btn & 0x7F) {
    case 0x06: return "Lock";
    case 0x0A: return "Unlock";
    case 0x0E: return "Trunk";
    default: return "Unknown";
    }
}

// D-pad button mapping: OK=original, UP=next, DOWN=after-that
static const uint8_t mazda_v2_btns[] = {0x06, 0x0A, 0x0E};

/**
 * Search for sync byte EC (11101100) or inverted ~EC=13 (00010011)
 * in accumulated bits and extract 64-bit frame.
 */
static void mazda_v2_process_frame(SubGhzProtocolDecoderMazdaV2* instance) {
    if(instance->all_bit_count < (8 + MAZDA_V2_DATA_BITS)) return;

    // Sync pattern: EC = 1110 1100
    static const uint8_t sync_ec[8] = {1,1,1,0,1,1,0,0};

    int sync_pos = -1;
    int search_end = instance->all_bit_count - 8 - MAZDA_V2_DATA_BITS;

    // Search for EC sync pattern
    for(int i = 0; i <= search_end; i++) {
        bool match = true;
        for(int j = 0; j < 8; j++) {
            if(instance->all_bits[i + j] != sync_ec[j]) {
                match = false;
                break;
            }
        }
        if(match) {
            sync_pos = i;
            break;
        }
    }

    if(sync_pos < 0) {
        FURI_LOG_D(TAG, "No sync in %d bits", instance->all_bit_count);
        return;
    }

    // Data starts after EC sync byte
    int data_start = sync_pos + 8;
    int data_avail = instance->all_bit_count - data_start;
    if(data_avail < MAZDA_V2_DATA_BITS) {
        FURI_LOG_D(TAG, "Short frame: %d bits (need %d)", data_avail, MAZDA_V2_DATA_BITS);
        return;
    }

    // Extract 63 data bits into generic.data (left-aligned in 64 bits, LSB padded with 0)
    instance->generic.data = 0;
    for(int i = 0; i < MAZDA_V2_DATA_BITS; i++) {
        instance->generic.data = (instance->generic.data << 1) | instance->all_bits[data_start + i];
    }
    instance->generic.data <<= 1; // pad LSB with 0 to fill 64 bits

    // Validate: hop byte 0 (bits 47:40) must have bit pattern 01xxxxxx
    // Frames with hop[0] bits 7:6 != 01 are from wrong Manchester polarity
    uint8_t hop0 = (uint8_t)((instance->generic.data >> 40) & 0xFF);
    if((hop0 & 0xC0) != 0x40) {
        FURI_LOG_D(TAG, "Bad hop[0]=0x%02X (expect 01xxxxxx), skipping", hop0);
        return;
    }

    // Validate: hop[3] low 3 bits = rc[3] which is always in {0,1,2,3} per the
    // rolling-code lookup tables (bit 2 always 0). This rejects false positives
    // where the EC sync byte was found inside random rolling-code data of an
    // unrelated Manchester protocol (e.g. Mazda V1 frames).
    uint8_t hop3_check = (uint8_t)((instance->generic.data >> 16) & 0xFF);
    if((hop3_check & 0x04) != 0) {
        FURI_LOG_D(TAG, "Bad hop[3]=0x%02X (rc[3] bit2 must be 0), skipping", hop3_check);
        return;
    }

    // Validate: button must be a known V2 button (Lock/Unlock/Trunk, optional repeat-flag bit 7)
    uint8_t btn_check = (uint8_t)((instance->generic.data >> 56) & 0xFF);
    uint8_t btn_base = btn_check & 0x7F;
    if(btn_base != 0x06 && btn_base != 0x0A && btn_base != 0x0E) {
        FURI_LOG_D(TAG, "Bad btn=0x%02X (expect 06/0A/0E ± repeat-flag), skipping", btn_check);
        return;
    }

    instance->generic.data_count_bit = MAZDA_V2_DATA_BITS;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = MAZDA_V2_DATA_BITS;

    // Button: byte 0 (bits 56-63)
    instance->generic.btn = btn_check;
    // Serial: byte 1 (bits 48-55)
    instance->generic.serial = (uint32_t)((instance->generic.data >> 48) & 0xFF);
    // Counter: hop[3] upper 5 bits = per-press counter (0-31, +1 per press)
    // hop[2] lower bits provide upper counter (increments at each wrap of 5-bit counter)
    uint8_t hop3 = hop3_check;
    uint8_t hop2 = (uint8_t)((instance->generic.data >> 24) & 0xFF);
    uint8_t cnt_lo = hop3 >> 3;           // 5-bit counter (0-31)
    uint8_t cnt_hi = hop2 & 0x07;         // upper 3 bits from hop[2]
    instance->generic.cnt = (cnt_hi << 5) | cnt_lo;

    FURI_LOG_D(TAG, "Sync at bit %d: Sn=%02lX Btn:%02X [%s]",
        sync_pos,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        mazda_v2_btn_name(instance->generic.btn));

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void subghz_protocol_decoder_mazda_v2_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV2* instance = context;

    uint32_t te_s = subghz_protocol_mazda_v2_const.te_short;
    uint32_t te_l = subghz_protocol_mazda_v2_const.te_long;
    uint32_t te_d = subghz_protocol_mazda_v2_const.te_delta;

    // 2-FSK noise filter: ignore very short timings (<250us) as demodulation artifacts
    if(duration < 250) return;

    switch(instance->decoder.parser_step) {
    case MazdaV2DecoderStepReset:
        if(DURATION_DIFF(duration, te_s) <= te_d) {
            instance->decoder.parser_step = MazdaV2DecoderStepCollect;
            instance->all_bit_count = 0;
            instance->half_bit = true;
        }
        break;

    case MazdaV2DecoderStepCollect: {
        if(instance->all_bit_count >= 380) {
            mazda_v2_process_frame(instance);
            instance->decoder.parser_step = MazdaV2DecoderStepReset;
            break;
        }

        bool is_short = DURATION_DIFF(duration, te_s) <= te_d;
        bool is_long = DURATION_DIFF(duration, te_l) <= te_d;

        if(is_long) {
            if(instance->all_bit_count < sizeof(instance->all_bits)) {
                instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
            }
            instance->half_bit = false;
        } else if(is_short) {
            if(instance->half_bit) {
                if(instance->all_bit_count < sizeof(instance->all_bits)) {
                    instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
                }
                instance->half_bit = false;
            } else {
                instance->half_bit = true;
            }
        } else {
            // Non-matching timing: process what we have and reset
            mazda_v2_process_frame(instance);
            instance->decoder.parser_step = MazdaV2DecoderStepReset;
        }
    } break;

    default:
        instance->decoder.parser_step = MazdaV2DecoderStepReset;
        break;
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_mazda_v2_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV2* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_mazda_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV2* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    FuriString* disp = furi_string_alloc();
    furi_string_printf(disp, "Mazda 6 - Sn:%02lX Btn:%s",
        (unsigned long)instance->generic.serial,
        mazda_v2_btn_name(instance->generic.btn));
    flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(disp));
    furi_string_free(disp);

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_mazda_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV2* instance = context;

    /* Validate the stored Bit: count matches this protocol (siblings do), so a
     * hand-edited .sub with a mismatched bit length is rejected, not misparsed. */
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_mazda_v2_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) return ret;

    instance->generic.btn = (uint8_t)((instance->generic.data >> 56) & 0xFF);
    instance->generic.serial = (uint32_t)((instance->generic.data >> 48) & 0xFF);
    uint8_t dh3 = (uint8_t)((instance->generic.data >> 16) & 0xFF);
    uint8_t dh2 = (uint8_t)((instance->generic.data >> 24) & 0xFF);
    instance->generic.cnt = ((dh2 & 0x07) << 5) | (dh3 >> 3);
    instance->generic.protocol_name = instance->base.protocol->name;

    // Enable d-pad remap for receiver list (3 buttons -> 2 custom slots)
    subghz_custom_btn_set_original_if_unset(instance->generic.btn & 0x7F);
    subghz_custom_btn_set_max(2);

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// NEXT-CODE PREDICTOR
// ============================================================================

/**
 * Compute the full 63-bit frame (packed in a 64-bit container, same layout as
 * generic.data) that this fob transmits for an ARBITRARY absolute 8-bit counter.
 *
 * The Mazda V2 rolling code is cleartext obfuscation, NOT encryption: no secret
 * key exists. The complete counter->frame mapping is the in-firmware lookup
 * tables (mazda_v2_rc_lock / mazda_v2_rc_unlock) plus the deterministic,
 * reversible cycle-XOR derivation (mazda_v2_derive_slot_mask). This helper reuses
 * exactly those (no crypto reimplementation) and the encoder's packing, so the
 * predicted cnt+i frames are byte-exact (load/emulate/save-able).
 *
 * src_frame:  the captured 64-bit frame (instance->generic.data)
 * target_cnt: absolute 8-bit counter to encode
 * returns:    packed 64-bit frame for target_cnt
 */
static uint64_t mazda_v2_predict_frame(uint64_t src_frame, uint8_t target_cnt) {
    uint8_t btn = (uint8_t)((src_frame >> 56) & 0x7F); // strip repeat flag (as encoder)
    uint8_t sn = (uint8_t)((src_frame >> 48) & 0xFF);

    // Recover source counter and decoded hop columns from the captured frame
    uint8_t sh3 = (uint8_t)((src_frame >> 16) & 0xFF);
    uint8_t sh2 = (uint8_t)((src_frame >> 24) & 0xFF);
    uint8_t src_cnt = ((sh2 & 0x07) << 5) | (sh3 >> 3);

    bool is_lock = (btn == 0x06);
    const uint8_t (*rc_table)[6] = is_lock ? mazda_v2_rc_lock : mazda_v2_rc_unlock;

    uint8_t src_hop[6];
    src_hop[0] = (uint8_t)((src_frame >> 40) & 0xFF);
    src_hop[1] = (uint8_t)((src_frame >> 32) & 0xFF);
    src_hop[2] = sh2 >> 3;
    src_hop[3] = sh3 & 0x07;
    src_hop[4] = (uint8_t)((src_frame >> 8) & 0xFF);
    src_hop[5] = (uint8_t)((src_frame >> 1) & 0x7F);

    // Detect cycle XOR mask by comparing captured hop vs base table at src counter
    const uint8_t* src_tbl = rc_table[src_cnt];
    bool cycle_active = false;
    uint8_t src_mask[6];
    for(int i = 0; i < 6; i++) {
        src_mask[i] = src_hop[i] ^ src_tbl[i];
        if(src_mask[i] != 0) cycle_active = true;
    }
    uint8_t src_slot = (uint8_t)((src_cnt / 2) % 4);

    // Look up target rolling code, then apply the derived cycle mask for its slot
    uint8_t rc[6];
    for(int i = 0; i < 6; i++) rc[i] = rc_table[target_cnt][i];
    if(cycle_active) {
        uint8_t target_slot = (uint8_t)((target_cnt / 2) % 4);
        bool dst_unlock_region = (!is_lock && target_cnt < 224);
        uint8_t target_mask[6];
        mazda_v2_derive_slot_mask(
            src_mask, src_slot, target_slot, dst_unlock_region, target_mask);
        for(int i = 0; i < 6; i++) rc[i] ^= target_mask[i];
    }

    // Rebuild hop bytes from table + target counter (identical to encoder)
    uint8_t hop0 = rc[0];
    uint8_t hop1 = rc[1];
    uint8_t hop2 = (rc[2] << 3) | ((target_cnt >> 5) & 0x07);
    uint8_t hop3 = ((target_cnt & 0x1F) << 3) | (rc[3] & 0x07);
    uint8_t hop4 = rc[4];
    uint8_t hop5 = rc[5]; // 7 bits

    return ((uint64_t)btn << 56) | ((uint64_t)sn << 48) |
           ((uint64_t)hop0 << 40) | ((uint64_t)hop1 << 32) |
           ((uint64_t)hop2 << 24) | ((uint64_t)hop3 << 16) |
           ((uint64_t)hop4 << 8) | ((uint64_t)(hop5 << 1));
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_mazda_v2_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV2* instance = context;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    // Hop: bytes 2-7 = 48 bits rolling code
    uint64_t hop = instance->generic.data & 0x0000FFFFFFFFFFFFULL;

    // Expose counter + button state for UI and d-pad remap
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.cnt_length_bit = 8;
    subghz_block_generic_global.current_cnt = instance->generic.cnt;

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = 8;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_custom_btn_fill_labels_positional(
        instance->generic.btn & 0x7F,
        mazda_v2_btns,
        COUNT_OF(mazda_v2_btns),
        mazda_v2_btn_name);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%02lX Btn:%02X [%s]\r\n"
        "Cnt:%02lX Hop:%012llX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        mazda_v2_btn_name(instance->generic.btn),
        (unsigned long)instance->generic.cnt,
        (unsigned long long)hop);

    // Next-code predictor: the rolling code is cleartext obfuscation (lookup
    // tables + reversible cycle-XOR, no secret key), so future frames are
    // byte-exact. Only run for a valid, fully-populated V2 frame.
    uint8_t pred_btn = instance->generic.btn & 0x7F;
    if(instance->generic.data_count_bit == MAZDA_V2_DATA_BITS &&
       (pred_btn == 0x06 || pred_btn == 0x0A || pred_btn == 0x0E)) {
        for(uint8_t i = 1; i <= MAZDA_V2_PREDICT_COUNT; i++) {
            uint8_t next_cnt = (uint8_t)((instance->generic.cnt + i) & 0xFF);
            uint64_t next_frame = mazda_v2_predict_frame(instance->generic.data, next_cnt);
            furi_string_cat_printf(
                output, "\r\nNext+%u:%016llX", i, (unsigned long long)next_frame);
        }
    }
}

// ============================================================================
// ENCODER
// ============================================================================

void* subghz_protocol_encoder_mazda_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderMazdaV2* instance = malloc(sizeof(SubGhzProtocolEncoderMazdaV2));
    instance->base.protocol = &subghz_protocol_mazda_v2;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = MAZDA_V2_TOTAL_BURSTS;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_mazda_v2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderMazdaV2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete frame using rolling code lookup tables.
 * [PREAMBLE: 80x SHORT pairs] [SYNC EC Manchester] [63 data bits Manchester] [END]
 */
static void mazda_v2_build_upload(SubGhzProtocolEncoderMazdaV2* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint32_t te_s = subghz_protocol_mazda_v2_const.te_short;
    uint32_t te_l = subghz_protocol_mazda_v2_const.te_long;

    // Save original button (strip repeat flag), apply d-pad remap, allow editor override
    subghz_custom_btn_set_original_if_unset(instance->generic.btn & 0x7F);
    uint8_t btn = instance->generic.btn & 0x7F;
    subghz_custom_btn_apply_positional(
        &btn,
        subghz_custom_btn_get_original(),
        subghz_custom_btn_get(),
        mazda_v2_btns,
        sizeof(mazda_v2_btns));
    subghz_block_generic_global_button_override_get(&btn);
    instance->generic.btn = btn;

    // Advance 8-bit rolling counter (re-encrypts via table lookup below)
    subghz_block_generic_advance_counter(&instance->generic.cnt, 0xFF);
    uint8_t cnt = (uint8_t)(instance->generic.cnt & 0xFF);

    // Look up rolling code from table
    bool is_lock = ((btn & 0x7F) == 0x06);
    const uint8_t (*rc_table)[6] = is_lock ? mazda_v2_rc_lock : mazda_v2_rc_unlock;
    uint8_t rc[6];
    for(int i = 0; i < 6; i++) rc[i] = rc_table[cnt][i];

    // Apply cycle XOR mask if not in base cycle
    if(instance->cycle_xor_active) {
        uint8_t target_slot = (cnt / 2) % 4;
        // Unlock counters 0-223 use unlock-type region, everything else lock-type
        bool dst_unlock_region = (!is_lock && cnt < 224);
        uint8_t target_mask[6];
        mazda_v2_derive_slot_mask(
            instance->cycle_xor, instance->cycle_xor_slot,
            target_slot, dst_unlock_region, target_mask);
        for(int i = 0; i < 6; i++) rc[i] ^= target_mask[i];
    }

    // Rebuild hop bytes from table + counter
    uint8_t hop0 = rc[0];
    uint8_t hop1 = rc[1];
    uint8_t hop2 = (rc[2] << 3) | ((cnt >> 5) & 0x07);  // hi5 from table, lo3 = cnt upper
    uint8_t hop3 = ((cnt & 0x1F) << 3) | (rc[3] & 0x07); // hi5 = cnt lower, lo3 from table
    uint8_t hop4 = rc[4];
    uint8_t hop5 = rc[5]; // 7 bits

    // Rebuild generic.data: [btn:8][sn:8][hop0-4:40][hop5:7][pad:1] = 64 bits
    uint8_t sn = (uint8_t)(instance->generic.serial & 0xFF);
    instance->generic.data =
        ((uint64_t)btn << 56) | ((uint64_t)sn << 48) |
        ((uint64_t)hop0 << 40) | ((uint64_t)hop1 << 32) |
        ((uint64_t)hop2 << 24) | ((uint64_t)hop3 << 16) |
        ((uint64_t)hop4 << 8) | ((uint64_t)(hop5 << 1));

    // PREAMBLE: alternating SHORT pairs (produces Manchester zeros)
    for(int p = 0; p < MAZDA_V2_PREAMBLE_PAIRS; p++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_s);
        instance->encoder.upload[index++] = level_duration_make(false, te_s);
    }

    // Frame = 71 bits: EC(8) + data(63)
    uint8_t sync_and_data[9];
    sync_and_data[0] = MAZDA_V2_SYNC_BYTE;
    for(int i = 0; i < 8; i++) {
        sync_and_data[i + 1] = (uint8_t)((instance->generic.data >> (56 - i * 8)) & 0xFF);
    }
    int total_bits = 71;

    // Preamble ends LOW (mid-0). Decoder's stateless Manchester:
    //   LONG → bit = level, SHORT+SHORT → bit = level of 2nd SHORT
    // So to encode bit=1 from LOW state: need HIGH-LONG (+1000)
    //    to encode bit=0 from LOW state: need HIGH-SHORT, LOW-SHORT (+500, -500)
    uint8_t manchester_state = ManchesterStateMid0;

    for(int bit_num = 0; bit_num < total_bits; bit_num++) {
        int byte_idx = bit_num / 8;
        int bit_idx = 7 - (bit_num % 8);
        bool b = (sync_and_data[byte_idx] >> bit_idx) & 1;

        switch(manchester_state) {
        case ManchesterStateMid0: // currently at LOW level
            if(b == 1) {
                instance->encoder.upload[index++] = level_duration_make(true, te_l);
                manchester_state = ManchesterStateMid1;
            } else {
                instance->encoder.upload[index++] = level_duration_make(true, te_s);
                instance->encoder.upload[index++] = level_duration_make(false, te_s);
                manchester_state = ManchesterStateMid0;
            }
            break;
        case ManchesterStateMid1: // currently at HIGH level
            if(b == 0) {
                instance->encoder.upload[index++] = level_duration_make(false, te_l);
                manchester_state = ManchesterStateMid0;
            } else {
                instance->encoder.upload[index++] = level_duration_make(false, te_s);
                instance->encoder.upload[index++] = level_duration_make(true, te_s);
                manchester_state = ManchesterStateMid1;
            }
            break;
        default:
            manchester_state = ManchesterStateMid0;
            break;
        }
    }

    // Complete any pending Manchester half-bit: add a transition to ensure
    // the decoder processes the last data bit before the gap
    if(manchester_state == ManchesterStateMid1) {
        // Currently HIGH, transition to LOW to complete
        instance->encoder.upload[index++] = level_duration_make(false, te_s);
        instance->encoder.upload[index++] = level_duration_make(true, te_s);
    } else {
        // Currently LOW, transition to HIGH
        instance->encoder.upload[index++] = level_duration_make(true, te_s);
        instance->encoder.upload[index++] = level_duration_make(false, te_s);
    }

    // END: guard pulse + gap
    instance->encoder.upload[index++] = level_duration_make(true, 3000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_mazda_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderMazdaV2* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        // Restore fields from generic.data
        instance->generic.btn = (uint8_t)((instance->generic.data >> 56) & 0xFF);
        instance->generic.serial = (uint32_t)((instance->generic.data >> 48) & 0xFF);
        uint8_t eh3 = (uint8_t)((instance->generic.data >> 16) & 0xFF);
        uint8_t eh2 = (uint8_t)((instance->generic.data >> 24) & 0xFF);
        instance->generic.cnt = ((eh2 & 0x07) << 5) | (eh3 >> 3);

        // Detect cycle: compare decoded hop vs base table
        uint8_t cur_cnt = (uint8_t)instance->generic.cnt;
        const uint8_t (*table)[6] = ((instance->generic.btn & 0x7F) == 0x06) ?
            mazda_v2_rc_lock : mazda_v2_rc_unlock;
        const uint8_t* tbl = table[cur_cnt];

        // Extract decoded hop columns from generic.data
        uint8_t dec_hop[6];
        dec_hop[0] = (uint8_t)((instance->generic.data >> 40) & 0xFF);
        dec_hop[1] = (uint8_t)((instance->generic.data >> 32) & 0xFF);
        dec_hop[2] = eh2 >> 3;
        dec_hop[3] = eh3 & 0x07;
        dec_hop[4] = (uint8_t)((instance->generic.data >> 8) & 0xFF);
        dec_hop[5] = (uint8_t)((instance->generic.data >> 1) & 0x7F);

        // Check if decoded hop matches base table
        bool matches_table = true;
        for(int i = 0; i < 6; i++) {
            if(dec_hop[i] != tbl[i]) { matches_table = false; break; }
        }

        if(matches_table) {
            instance->cycle_xor_active = false;
        } else {
            // Compute XOR mask for current slot and store it
            // mazda_v2_derive_slot_mask() will derive other slots as needed
            instance->cycle_xor_active = true;
            instance->cycle_xor_slot = (cur_cnt / 2) % 4;
            for(int i = 0; i < 6; i++) {
                instance->cycle_xor[i] = dec_hop[i] ^ tbl[i];
            }
        }

        // Enable d-pad remap (3 buttons: Lock, Unlock, Trunk -> 2 custom slots)
        subghz_custom_btn_set_original_if_unset(instance->generic.btn & 0x7F);
        subghz_custom_btn_set_max(2);

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        mazda_v2_build_upload(instance);

        // Persist new Key (advanced counter, remapped btn) back to file
        if(flipper_format_rewind(flipper_format)) {
            uint8_t key_data[sizeof(uint64_t)];
            uint64_t data_be = __builtin_bswap64(instance->generic.data);
            memcpy(key_data, &data_be, sizeof(key_data));
            flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t));
        }

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_mazda_v2_stop(void* context) {
    SubGhzProtocolEncoderMazdaV2* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_mazda_v2_yield(void* context) {
    SubGhzProtocolEncoderMazdaV2* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}
