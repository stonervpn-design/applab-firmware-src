#include "receiver.h"

#include "registry.h"

#include <furi.h>
#include <m-array.h>

#define TAG "SubGhzReceiver"

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
    SubGhzReceiverSlotArray_init(instance->slots);
    const SubGhzProtocolRegistry* protocol_registry_items =
        subghz_environment_get_protocol_registry(environment);

    size_t total_protocols = subghz_protocol_registry_count(protocol_registry_items);
    size_t loaded_decoders = 0;
    FURI_LOG_I(TAG, "Registry has %u protocols", (unsigned)total_protocols);

    for(size_t i = 0; i < total_protocols; ++i) {
        const SubGhzProtocol* protocol =
            subghz_protocol_registry_get_by_index(protocol_registry_items, i);

        if(protocol->decoder && protocol->decoder->alloc) {
            SubGhzReceiverSlot* slot = SubGhzReceiverSlotArray_push_new(instance->slots);
            slot->base = protocol->decoder->alloc(environment);
            loaded_decoders++;
        }
    }

    FURI_LOG_I(TAG, "Loaded %u decoders", (unsigned)loaded_decoders);
    instance->callback = NULL;
    instance->context = NULL;
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
