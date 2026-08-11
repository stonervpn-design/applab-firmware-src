/*
 * IR transmitter — ESP32 port.
 *
 * TagTinker's ESL waveform is OOK pulse-position (PP4): a fixed ~40 us carrier
 * burst followed by one of four gaps encodes two bits. The original hardware
 * drove a ~1.255 MHz carrier on STM32 TIM1 with cycle-accurate bit-banging; on
 * ESP32 we feed the same marks/gaps to the RMT-based furi_hal_infrared async TX,
 * which applies the carrier in hardware. RMT runs at 1 us resolution and carries
 * a ~1.25 MHz carrier (80 MHz / 64), well within OOK tolerance. Because the RMT
 * is hardware-timed, no critical section is needed.
 */

#include "tagtinker_ir.h"

#include <furi.h>
#include <furi_hal_infrared.h>

/* ~1.255 MHz OOK carrier (STM32 used 64 MHz/51); RMT gives 80 MHz/64 = 1.25 MHz. */
#define TT_CARRIER_HZ   1250000u
#define TT_CARRIER_DUTY 0.49f

/* PP4 symbol timings in microseconds (original STM32 cycle counts / 64 MHz). */
#define TT_BURST_US 40u
static const uint16_t tt_gap_us[4] = {
    60, /* value 0 (3871 cyc  ~60 us)  */
    242, /* value 1 (15483 cyc ~242 us) */
    121, /* value 2 (7741 cyc  ~121 us) */
    181, /* value 3 (11612 cyc ~181 us) */
};

/*
 * One frame's flat timing stream: mark, space, mark, space, ..., mark (closing).
 * Even index = mark (carrier on), odd index = space. A frame is at most
 * TAGTINKER_MAX_FRAME_SIZE (96) bytes -> 8*96+1 = 769 timings, comfortably under
 * the HAL's ~1200-timing / 600-symbol single-packet limit, so each frame is
 * transmitted gaplessly in one RMT transaction.
 */
#define TT_MAX_FRAME_BYTES 96u
#define TT_MAX_TIMINGS     (8u * TT_MAX_FRAME_BYTES + 1u)
/* On-demand: allocated in tagtinker_ir_init(), freed in tagtinker_ir_deinit(),
 * so this ~1.5 KB is resident only during an active IR TX session and is
 * reclaimable (e.g. for Sub-GHz) whenever IR is idle. */
static uint16_t* tt_stream = NULL;
static volatile size_t tt_stream_len = 0;
static volatile size_t tt_stream_idx = 0;

static bool ir_initialized = false;
static volatile bool ir_busy = false;
static volatile bool ir_stop_requested = false;

/* Build the mark/space stream for one PP4 frame. Returns the timing count. */
static size_t tt_build_stream(const uint8_t* data, size_t len) {
    if(!tt_stream) return 0;
    size_t n = 0;
    for(size_t b = 0; b < len; b++) {
        uint8_t byte = data[b];
        for(int s = 0; s < 4; s++) {
            uint8_t sym = byte & 0x03u;
            byte >>= 2;
            tt_stream[n++] = TT_BURST_US; /* mark  */
            tt_stream[n++] = tt_gap_us[sym]; /* space */
        }
    }
    tt_stream[n++] = TT_BURST_US; /* closing mark */
    return n;
}

/* async TX data callback: yields the next (duration, level) pair from tt_stream. */
static FuriHalInfraredTxGetDataState
    tt_ir_tx_cb(void* ctx, uint32_t* duration, bool* level) {
    (void)ctx;
    if(!tt_stream) {
        *duration = 0;
        *level = false;
        return FuriHalInfraredTxGetDataStateLastDone;
    }
    size_t i = tt_stream_idx;
    if(i >= tt_stream_len) {
        *duration = 0;
        *level = false;
        return FuriHalInfraredTxGetDataStateLastDone;
    }
    *duration = tt_stream[i];
    *level = ((i & 1u) == 0u); /* even = mark (carrier on) */
    tt_stream_idx = i + 1;
    return (i + 1 >= tt_stream_len) ? FuriHalInfraredTxGetDataStateLastDone :
                                      FuriHalInfraredTxGetDataStateOk;
}

void tagtinker_ir_init(void) {
    if(ir_initialized) return;
    /* Allocate the timing stream on demand. Internal RAM is fine (no PSRAM on
     * this board, and this buffer is read in task context, not DMA'd). On
     * failure leave ir_initialized=false so tagtinker_ir_transmit() aborts. */
    tt_stream = malloc(TT_MAX_TIMINGS * sizeof(uint16_t));
    if(!tt_stream) return;
    ir_stop_requested = false;
    ir_busy = false;
    ir_initialized = true;
}

void tagtinker_ir_deinit(void) {
    if(!ir_initialized) return;
    tagtinker_ir_stop();
    ir_initialized = false;
    free(tt_stream);
    tt_stream = NULL;
}

bool tagtinker_ir_is_busy(void) {
    return ir_busy;
}

bool tagtinker_ir_transmit(const uint8_t* data, size_t len, uint16_t repeats_raw, uint8_t delay) {
    if(!ir_initialized) return false;
    if(!data || len == 0 || len > TT_MAX_FRAME_BYTES) return false;

    ir_stop_requested = false;
    ir_busy = true;
    uint32_t repeats = repeats_raw & 0x7FFFu;

    tt_stream_len = tt_build_stream(data, len);
    furi_hal_infrared_async_tx_set_data_isr_callback(tt_ir_tx_cb, NULL);

    bool ok = true;
    for(uint32_t rep = 0; rep <= repeats; rep++) {
        if(ir_stop_requested) {
            ok = false;
            break;
        }

        tt_stream_idx = 0;
        furi_hal_infrared_async_tx_start(TT_CARRIER_HZ, TT_CARRIER_DUTY);
        furi_hal_infrared_async_tx_wait_termination();
        furi_hal_infrared_async_tx_stop();

        if(rep < repeats) {
            /* delay parameter = inter-repeat gap in units of 500 us. */
            if(delay > 0) furi_delay_us((uint32_t)delay * 500u);
            /* Yield to the OS periodically so long repeat runs don't starve it. */
            if((rep % 5u) == 4u) furi_delay_ms(1);
        }
    }

    ir_busy = false;
    return ok;
}

void tagtinker_ir_stop(void) {
    ir_stop_requested = true;
}
