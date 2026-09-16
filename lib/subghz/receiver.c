#include "receiver.h"

#include "registry.h"

#include <furi.h>
#include <m-array.h>
#include <esp_heap_caps.h>
#include <stdio.h>

#define TAG "SubGhzReceiver"

// TEMP DIAGNOSTIC globals (remove after RCA): last receiver's decoder load counts,
// surfaced on the SubGhz "Cannot parse" dialog to reveal OOM decoder-skip on device.
uint16_t g_subghz_dbg_loaded = 0;
uint16_t g_subghz_dbg_decodable = 0;

typedef struct {
    SubGhzProtocolEncoderBase* base;
} SubGhzReceiverSlot;

ARRAY_DEF(SubGhzReceiverSlotArray, SubGhzReceiverSlot, M_POD_OPLIST); //-V658
#define M_OPL_SubGhzReceiverSlotArray_t() ARRAY_OPLIST(SubGhzReceiverSlotArray, M_POD_OPLIST)

struct SubGhzReceiver {
    SubGhzReceiverSlotArray_t slots;
    SubGhzProtocolFlag filter;

    SubGhzReceiverCallback callback;
    void* context;
};

SubGhzReceiver* subghz_receiver_alloc_init(SubGhzEnvironment* environment) {
    SubGhzReceiver* instance = malloc(sizeof(SubGhzReceiver));
    furi_check(instance); // tiny struct; a clean named abort beats a NULL deref on total OOM
    SubGhzReceiverSlotArray_init(instance->slots);
    const SubGhzProtocolRegistry* protocol_registry_items =
        subghz_environment_get_protocol_registry(environment);

    size_t total_protocols = subghz_protocol_registry_count(protocol_registry_items);
    size_t loaded_decoders = 0;
    size_t decodable = 0;
    FURI_LOG_I(TAG, "Registry has %u protocols", (unsigned)total_protocols);

    // The internal SRAM heap on this board cannot reliably hold every decoder struct
    // (they total ~20-25 KB and the internal heap is tight/fragmented once the services
    // and app views are up). A dropped decoder makes its protocol undecodable — the
    // "cannot parse" / "decode reaches 100% but nothing decodes" symptom. Decoders are
    // plain data touched only by the SubGhz worker task (never from an ISR, never across
    // a flash/NVS write), so route their plain-malloc allocations to PSRAM for the
    // duration of the load, then restore the default internal-first threshold. Explicit
    // heap_caps(MALLOC_CAP_INTERNAL/DMA) allocations are unaffected by this.
    heap_caps_malloc_extmem_enable(128);

    for(size_t i = 0; i < total_protocols; ++i) {
        const SubGhzProtocol* protocol =
            subghz_protocol_registry_get_by_index(protocol_registry_items, i);

        if(protocol->decoder && protocol->decoder->alloc) {
            decodable++;
            // Alloc first; only create a slot if it succeeded. A failed decoder alloc
            // must NOT leave a NULL-base slot, or the decode/free loops below would
            // dereference NULL and crash on the first RF edge.
            void* base = protocol->decoder->alloc(environment);
            if(base) {
                SubGhzReceiverSlot* slot = SubGhzReceiverSlotArray_push_new(instance->slots);
                slot->base = base;
                loaded_decoders++;
            }
        }
    }

    // Restore the default: allocations < 16384 B go to internal SRAM again.
    heap_caps_malloc_extmem_enable(16384);

    FURI_LOG_I(TAG, "Loaded %u decoders", (unsigned)loaded_decoders);
    g_subghz_dbg_loaded = (uint16_t)loaded_decoders;
    g_subghz_dbg_decodable = (uint16_t)decodable;
    // Degrade gracefully instead of crashing when not every decoder fits (a no-PSRAM
    // board with tight/fragmented internal RAM can't always hold all ~113 decoder
    // structs). A missing decoder just makes THAT protocol undecodable this session;
    // the decode/free loops only ever touch loaded slots, so this is safe. Previously
    // this path called furi_crash("DECSKIP ...") — a leftover diagnostic that turned a
    // recoverable low-memory condition into a reboot on the Cardputer.
    if(loaded_decoders != decodable) {
        FURI_LOG_W(
            TAG,
            "low RAM: loaded %u/%u decoders (free=%u largest=%u) - some protocols "
            "undecodable this session",
            (unsigned)loaded_decoders,
            (unsigned)decodable,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    instance->callback = NULL;
    instance->context = NULL;
    return instance;
}

SubGhzReceiver*
    subghz_receiver_alloc_init_single(SubGhzEnvironment* environment, const char* only_name) {
    // Load a receiver with ONLY the decoder named `only_name` (plus nothing else).
    // Used when re-parsing a SAVED file whose protocol is already known: on a no-PSRAM
    // board the full ~113-decoder set costs ~20-25 KB of internal RAM, but displaying one
    // saved signal needs exactly one decoder. Falls back to the full set if `only_name` is
    // NULL or is not a registry protocol (e.g. a cross-firmware alias), so callers that
    // rely on the alias search still work.
    if(!only_name) return subghz_receiver_alloc_init(environment);

    SubGhzReceiver* instance = malloc(sizeof(SubGhzReceiver));
    furi_check(instance);
    SubGhzReceiverSlotArray_init(instance->slots);
    const SubGhzProtocolRegistry* reg = subghz_environment_get_protocol_registry(environment);
    size_t total = subghz_protocol_registry_count(reg);
    size_t loaded = 0;

    for(size_t i = 0; i < total; ++i) {
        const SubGhzProtocol* p = subghz_protocol_registry_get_by_index(reg, i);
        if(p && p->decoder && p->decoder->alloc && p->name && strcmp(p->name, only_name) == 0) {
            void* base = p->decoder->alloc(environment);
            if(base) {
                SubGhzReceiverSlot* slot = SubGhzReceiverSlotArray_push_new(instance->slots);
                slot->base = base;
                loaded++;
            }
            break;
        }
    }

    instance->callback = NULL;
    instance->context = NULL;

    if(loaded == 0) {
        // Not a registry name (alias / RAW / typo) — fall back to the full set so the
        // caller's normal search + alias path still resolves it.
        subghz_receiver_free(instance);
        return subghz_receiver_alloc_init(environment);
    }
    FURI_LOG_I(TAG, "single-decoder receiver: %s", only_name);
    return instance;
}

void subghz_receiver_free(SubGhzReceiver* instance) {
    furi_check(instance);

    instance->callback = NULL;
    instance->context = NULL;

    // Release allocated slots
    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            slot->base->protocol->decoder->free(slot->base);
            slot->base = NULL;
        }
    SubGhzReceiverSlotArray_clear(instance->slots);

    free(instance);
}

void subghz_receiver_decode(SubGhzReceiver* instance, bool level, uint32_t duration) {
    furi_check(instance);
    furi_check(instance->slots);

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            if((slot->base->protocol->flag & instance->filter) != 0) {
                slot->base->protocol->decoder->feed(slot->base, level, duration);
            }
        }
}

void subghz_receiver_reset(SubGhzReceiver* instance) {
    furi_check(instance);
    furi_check(instance->slots);

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            slot->base->protocol->decoder->reset(slot->base);
        }
}

static void subghz_receiver_rx_callback(SubGhzProtocolDecoderBase* decoder_base, void* context) {
    SubGhzReceiver* instance = context;
    FURI_LOG_I(
        TAG,
        "DECODE SUCCESS: protocol='%s' type=%d",
        decoder_base->protocol->name,
        decoder_base->protocol->type);
    if(instance->callback) {
        instance->callback(instance, decoder_base, instance->context);
    } else {
        FURI_LOG_W(TAG, "No callback set!");
    }

    // Step 4 (dispatch integrity net): after ANY decoder emits on a burst, reset
    // the SIBLING decoders so a single RF burst cannot fan out to multiple
    // protocols each writing its own row. The emitting decoder (decoder_base)
    // resets itself in its own state machine, so it is skipped here. This bounds
    // the blast radius of fieldless (no-CRC) decoders that grab a foreign signal:
    // once a genuine keyed frame is decoded, the loose ones can't also complete on
    // the same burst tail. Resetting only clears partial state (the slots array is
    // not modified), so this is safe re-entrant from a decoder feed() call.
    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            if((SubGhzProtocolDecoderBase*)slot->base != decoder_base) {
                slot->base->protocol->decoder->reset(slot->base);
            }
        }
}

void subghz_receiver_set_rx_callback(
    SubGhzReceiver* instance,
    SubGhzReceiverCallback callback,
    void* context) {
    furi_check(instance);

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            subghz_protocol_decoder_base_set_decoder_callback(
                (SubGhzProtocolDecoderBase*)slot->base, subghz_receiver_rx_callback, instance);
        }

    instance->callback = callback;
    instance->context = context;
}

void subghz_receiver_set_filter(SubGhzReceiver* instance, SubGhzProtocolFlag filter) {
    furi_check(instance);
    instance->filter = filter;
}

SubGhzProtocolDecoderBase* subghz_receiver_search_decoder_base_by_name(
    SubGhzReceiver* instance,
    const char* decoder_name) {
    furi_check(instance);

    SubGhzProtocolDecoderBase* result = NULL;

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            if(strcmp(slot->base->protocol->name, decoder_name) == 0) {
                result = (SubGhzProtocolDecoderBase*)slot->base;
                break;
            }
        }
    return result;
}
