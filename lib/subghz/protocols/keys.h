#pragma once

#include <stdint.h>

#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h>

// Kia/Hyundai V3/V4 use a KeeLoq manufacturer key (keystore type KIA_KEY1). The
// keystore can legitimately carry several distinct KIA_KEY1 entries (different
// model years / regions all tagged "KIA_Hyundai_V3&4"). Keep ALL of them so the
// decoder can try each, instead of a single last-one-wins slot that silently
// shadows every earlier key.
#define KIA_MF_KEY_MAX 8u
extern uint64_t kia_mf_keys[KIA_MF_KEY_MAX];
extern uint8_t kia_mf_key_count;

extern uint64_t kia_mf_key;
extern uint64_t kia_v6_a_key;
extern uint64_t kia_v6_b_key;
extern uint64_t kia_v5_key;

uint64_t get_kia_mf_key();
uint8_t get_kia_mf_key_count(void);
uint64_t get_kia_mf_key_at(uint8_t index);

uint64_t get_kia_v6_keystore_a();

uint64_t get_kia_v6_keystore_b();

uint64_t get_kia_v5_key();

void protopirate_keys_load(SubGhzEnvironment* environment);
