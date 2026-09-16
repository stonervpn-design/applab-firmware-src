#include "subghz_history.h"
#include <lib/subghz/receiver.h>

#include <furi.h>

#define SUBGHZ_HISTORY_MAX       55
// Free-heap floor below which a decode is NOT added to history (avoids OOM on add).
// Stock value 20480 (20KB) is calibrated for a Flipper with spare RAM; on this
// no-PSRAM ESP32 port the runtime heap during Read sits just under 20KB once the
// (now much larger) SubGhz app + all its views are allocated, so the stock guard
// silently dropped EVERY decode (decoder fires, but add_to_history returns false).
// A history item is <1KB, so 8KB is a safe floor for the add on this board.
// NOTE: proper fix is trimming the SubGhz app's baseline RAM; this restores decode.
#define SUBGHZ_HISTORY_FREE_HEAP 8192
#define TAG                      "SubGhzHistory"

typedef struct {
    FuriString* item_str;
    FlipperFormat* flipper_string;
    uint8_t type;
    SubGhzRadioPreset* preset;
    DateTime datetime;
    bool fired; // (private) auto-fired 0-PSI at this captured sensor -> tick in the list
} SubGhzHistoryItem;

ARRAY_DEF(SubGhzHistoryItemArray, SubGhzHistoryItem, M_POD_OPLIST) //-V658

#define M_OPL_SubGhzHistoryItemArray_t() ARRAY_OPLIST(SubGhzHistoryItemArray, M_POD_OPLIST)

typedef struct {
    SubGhzHistoryItemArray_t data;
} SubGhzHistoryStruct;

struct SubGhzHistory {
    uint32_t last_update_timestamp;
    uint16_t last_index_write;
    uint8_t code_last_hash_data;
    FuriString* code_last_payload;
    FuriString* tmp_string;
    SubGhzHistoryStruct* history;
};

SubGhzHistory* subghz_history_alloc(void) {
    SubGhzHistory* instance = malloc(sizeof(SubGhzHistory));
    instance->tmp_string = furi_string_alloc();
    instance->code_last_payload = furi_string_alloc();
    instance->history = malloc(sizeof(SubGhzHistoryStruct));
    SubGhzHistoryItemArray_init(instance->history->data);
    return instance;
}

void subghz_history_free(SubGhzHistory* instance) {
    furi_assert(instance);
    furi_string_free(instance->tmp_string);
    furi_string_free(instance->code_last_payload);
    for
        M_EACH(item, instance->history->data, SubGhzHistoryItemArray_t) {
            furi_string_free(item->item_str);
            furi_string_free(item->preset->name);
            free(item->preset);
            flipper_format_free(item->flipper_string);
            item->type = 0;
        }
    SubGhzHistoryItemArray_clear(instance->history->data);
    free(instance->history);
    free(instance);
}

uint32_t subghz_history_get_frequency(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item->preset->frequency;
}

SubGhzRadioPreset* subghz_history_get_radio_preset(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item->preset;
}

const char* subghz_history_get_preset(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return furi_string_get_cstr(item->preset->name);
}

void subghz_history_reset(SubGhzHistory* instance) {
    furi_assert(instance);
    furi_string_reset(instance->tmp_string);
    furi_string_reset(instance->code_last_payload);
    for
        M_EACH(item, instance->history->data, SubGhzHistoryItemArray_t) {
            furi_string_free(item->item_str);
            furi_string_free(item->preset->name);
            free(item->preset);
            flipper_format_free(item->flipper_string);
            item->type = 0;
        }
    SubGhzHistoryItemArray_reset(instance->history->data);
    instance->last_index_write = 0;
    instance->code_last_hash_data = 0;
}

void subghz_history_delete_item(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);

    if(idx < SubGhzHistoryItemArray_size(instance->history->data)) {
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
        furi_string_free(item->item_str);
        furi_string_free(item->preset->name);
        free(item->preset);
        flipper_format_free(item->flipper_string);
        item->type = 0;
        SubGhzHistoryItemArray_remove_v(instance->history->data, idx, idx + 1);
        instance->last_index_write--;
    }
}

uint16_t subghz_history_get_item(SubGhzHistory* instance) {
    furi_assert(instance);
    return instance->last_index_write;
}

uint8_t subghz_history_get_type_protocol(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item->type;
}

const char* subghz_history_get_protocol_name(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    if(!item || !item->flipper_string) {
        FURI_LOG_E(TAG, "Missing Item");
        furi_string_reset(instance->tmp_string);
        return furi_string_get_cstr(instance->tmp_string);
    }
    flipper_format_rewind(item->flipper_string);
    if(!flipper_format_read_string(item->flipper_string, "Protocol", instance->tmp_string)) {
        FURI_LOG_E(TAG, "Missing Protocol");
        furi_string_reset(instance->tmp_string);
    }
    return furi_string_get_cstr(instance->tmp_string);
}

DateTime subghz_history_get_datetime(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    if(item) {
        return item->datetime;
    } else {
        return (DateTime){};
    }
}

FlipperFormat* subghz_history_get_raw_data(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    if(item->flipper_string) {
        return item->flipper_string;
    } else {
        return NULL;
    }
}
bool subghz_history_get_text_space_left(SubGhzHistory* instance, FuriString* output) {
    furi_assert(instance);
    if(memmgr_get_free_heap() < SUBGHZ_HISTORY_FREE_HEAP) {
        if(output != NULL) furi_string_printf(output, "  RAM almost FULL");
        return true;
    }
    if(instance->last_index_write == SUBGHZ_HISTORY_MAX) {
        if(output != NULL) furi_string_printf(output, "   Memory is FULL");
        return true;
    }
    if(output != NULL)
        furi_string_printf(output, "%02u/%02u", instance->last_index_write, SUBGHZ_HISTORY_MAX);
    return false;
}

void subghz_history_set_fired(SubGhzHistory* instance, uint16_t idx, bool fired) {
    furi_assert(instance);
    if(idx < SubGhzHistoryItemArray_size(instance->history->data)) {
        SubGhzHistoryItemArray_get(instance->history->data, idx)->fired = fired;
    }
}

bool subghz_history_get_fired(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    if(idx < SubGhzHistoryItemArray_size(instance->history->data)) {
        return SubGhzHistoryItemArray_get(instance->history->data, idx)->fired;
    }
    return false;
}

uint16_t subghz_history_get_last_index(SubGhzHistory* instance) {
    return instance->last_index_write;
}
void subghz_history_get_text_item_menu(SubGhzHistory* instance, FuriString* output, uint16_t idx) {
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    furi_string_set(output, item->item_str);
}

void subghz_history_get_time_item_menu(SubGhzHistory* instance, FuriString* output, uint16_t idx) {
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    DateTime* t = &item->datetime;
    furi_string_printf(output, "%.2d:%.2d:%.2d ", t->hour, t->minute, t->second);
}

bool subghz_history_add_to_history(
    SubGhzHistory* instance,
    void* context,
    SubGhzRadioPreset* preset) {
    furi_assert(instance);
    furi_assert(context);

    if(memmgr_get_free_heap() < SUBGHZ_HISTORY_FREE_HEAP) return false;
    if(instance->last_index_write >= SUBGHZ_HISTORY_MAX) return false;

    SubGhzProtocolDecoderBase* decoder_base = context;
    FURI_LOG_I(
        TAG,
        "add_to_history: protocol='%s' type=%d hash=%d",
        decoder_base->protocol ? decoder_base->protocol->name : "NULL",
        decoder_base->protocol ? decoder_base->protocol->type : -1,
        subghz_protocol_decoder_base_get_hash_data(decoder_base));

    // Finding #28: dedup on the full serialized payload (built below), not the
    // collision-prone 8-bit hash. Snapshot the PREVIOUS hash/timestamp here (before
    // overwriting them) so the RAW/name-only fallback at the end can still gate on the
    // old 8-bit-hash-within-500ms behavior for unparsed captures with no Key/Id.
    uint8_t new_hash = subghz_protocol_decoder_base_get_hash_data(decoder_base);
    bool raw_recent_dup = (instance->code_last_hash_data == new_hash) &&
                          ((furi_get_tick() - instance->last_update_timestamp) < 500);

    instance->code_last_hash_data = new_hash;
    instance->last_update_timestamp = furi_get_tick();

    FuriString* text = furi_string_alloc();
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_push_raw(instance->history->data);
    item->preset = malloc(sizeof(SubGhzRadioPreset));
    item->type = decoder_base->protocol->type;
    item->fired = false;
    item->preset->frequency = preset->frequency;
    item->preset->name = furi_string_alloc();
    furi_string_set(item->preset->name, preset->name);
    item->preset->data = preset->data;
    item->preset->data_size = preset->data_size;
    furi_hal_rtc_get_datetime(&item->datetime);

    item->item_str = furi_string_alloc();
    item->flipper_string = flipper_format_string_alloc();
    SubGhzProtocolStatus ser_status =
        subghz_protocol_decoder_base_serialize(decoder_base, item->flipper_string, preset);
    FURI_LOG_I(
        TAG,
        "serialize: status=%d preset='%s' freq=%lu",
        (int)ser_status,
        preset->name ? furi_string_get_cstr(preset->name) : "NULL",
        preset->frequency);

    if(ser_status != SubGhzProtocolStatusOk) {
        // Serialize failed → flipper_string has no valid "Protocol"/"Key", so this
        // entry would later miss decoder-by-name lookup and show "Error history parse".
        // Roll the half-built item back instead of committing a broken entry (matches
        // upstream, which drops the signal on serialize failure).
        FURI_LOG_E(TAG, "serialize failed (%d) - dropping history entry", (int)ser_status);
        furi_string_free(text);
        furi_string_free(item->item_str);
        furi_string_free(item->preset->name);
        free(item->preset);
        flipper_format_free(item->flipper_string);
        item->type = 0;
        SubGhzHistoryItemArray_remove_v(
            instance->history->data,
            SubGhzHistoryItemArray_size(instance->history->data) - 1,
            SubGhzHistoryItemArray_size(instance->history->data));
        return false;
    }

    // Set true once a distinguishing Key/Id is folded into item_str; a name-only
    // (pure RAW / unparsed >64-bit) item_str keeps the old hash+window dedup instead.
    bool has_key_payload = false;
    do {
        if(!flipper_format_rewind(item->flipper_string)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(!flipper_format_read_string(item->flipper_string, "Protocol", instance->tmp_string)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            break;
        }
        if(item->type == SubGhzProtocolTypeTpms) {
            // TPMS protocols carry an "Id" (uint32) instead of a "Key" (uint64 hex).
            // Format as "<Protocol> <hex-id>" and skip the generic Key path below.
            uint32_t id = 0;
            if(flipper_format_read_uint32(item->flipper_string, "Id", &id, 1)) {
                furi_string_printf(
                    item->item_str,
                    "%s %08lX",
                    furi_string_get_cstr(instance->tmp_string),
                    (unsigned long)id);
                has_key_payload = true;
            } else {
                furi_string_set(item->item_str, instance->tmp_string);
            }
            break;
        }
        if(!strcmp(furi_string_get_cstr(instance->tmp_string), "KeeLoq")) {
            furi_string_set(instance->tmp_string, "KL ");
            if(!flipper_format_read_string(item->flipper_string, "Manufacture", text)) {
                FURI_LOG_E(TAG, "Missing Protocol");
                break;
            }
            furi_string_cat(instance->tmp_string, text);
        }
#ifndef PUBLIC_BUILD
        else if(!strcmp(furi_string_get_cstr(instance->tmp_string), "Star Line")) {
            // Star Line is a car alarm protocol (stripped on public builds).
            furi_string_set(instance->tmp_string, "SL ");
            if(!flipper_format_read_string(item->flipper_string, "Manufacture", text)) {
                FURI_LOG_E(TAG, "Missing Protocol");
                break;
            }
            furi_string_cat(instance->tmp_string, text);
        }
#endif
        if(!flipper_format_rewind(item->flipper_string)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        uint8_t key_data[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(item->flipper_string, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_D(TAG, "No Key");
        }
        uint64_t data = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            data = (data << 8) | key_data[i];
        }
        if(data != 0) {
            has_key_payload = true;
            if(!(uint32_t)(data >> 32)) {
                furi_string_printf(
                    item->item_str,
                    "%s %lX",
                    furi_string_get_cstr(instance->tmp_string),
                    (uint32_t)(data & 0xFFFFFFFF));
            } else {
                furi_string_printf(
                    item->item_str,
                    "%s %lX%08lX",
                    furi_string_get_cstr(instance->tmp_string),
                    (uint32_t)(data >> 32),
                    (uint32_t)(data & 0xFFFFFFFF));
            }
        } else {
            furi_string_printf(item->item_str, "%s", furi_string_get_cstr(instance->tmp_string));
        }

    } while(false);

    furi_string_free(text);

    // Finding #28 dedup: suppress only a byte-identical CONSECUTIVE frame.
    bool is_duplicate;
    if(has_key_payload) {
        // Keyed car/KeeLoq/TPMS frame: compare the full serialized label. A genuine
        // rolling-code re-press yields a different Key/Id -> different item_str -> kept.
        is_duplicate = furi_string_equal(item->item_str, instance->code_last_payload);
    } else {
        // Name-only (pure RAW / unparsed >64-bit): keep the legacy 8-bit-hash +
        // <500ms window so distinct RAW captures are not over-suppressed.
        is_duplicate = raw_recent_dup;
    }

    if(is_duplicate) {
        FURI_LOG_D(TAG, "Duplicate payload, skipping");
        // Roll the just-pushed item back (same sequence as the serialize-fail path,
        // minus `text` which is already freed above).
        furi_string_free(item->item_str);
        furi_string_free(item->preset->name);
        free(item->preset);
        flipper_format_free(item->flipper_string);
        item->type = 0;
        SubGhzHistoryItemArray_remove_v(
            instance->history->data,
            SubGhzHistoryItemArray_size(instance->history->data) - 1,
            SubGhzHistoryItemArray_size(instance->history->data));
        return false;
    }

    if(has_key_payload) {
        furi_string_set(instance->code_last_payload, item->item_str);
    }

    instance->last_index_write++;
    return true;
}

bool subghz_history_replace_tpms_payload(
    SubGhzHistory* instance,
    uint16_t idx,
    TPMSBlockGeneric* generic) {
    furi_assert(instance);
    furi_assert(generic);

    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    if(!item || !item->flipper_string) {
        FURI_LOG_E(TAG, "replace: missing item %u", idx);
        return false;
    }

    FlipperFormat* fresh = flipper_format_string_alloc();
    SubGhzProtocolStatus s = tpms_block_generic_serialize(generic, fresh, item->preset);
    if(s != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "replace: serialize failed (%d)", (int)s);
        flipper_format_free(fresh);
        return false;
    }

    flipper_format_free(item->flipper_string);
    item->flipper_string = fresh;

    // Refresh the list label as "<Protocol> <id>" (matches the TPMS branch in
    // subghz_history_add_to_history).
    furi_string_printf(
        item->item_str,
        "%s %08lX",
        generic->protocol_name ? generic->protocol_name : "TPMS",
        (unsigned long)generic->id);

    return true;
}
