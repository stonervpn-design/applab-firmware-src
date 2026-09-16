/**
 * @file furi_hal_infrared.c
 * ESP32 Infrared HAL implementation using RMT peripheral.
 *
 * RX: RMT receiver captures edges, a GPTimer provides silence timeout.
 * TX: RMT transmitter with carrier modulation, fed via encode callback.
 */

#include <furi_hal_infrared.h>
#include <furi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/idf_additions.h> /* xTaskCreateWithCaps / vTaskDeleteWithCaps (PSRAM task stacks) */

#include <driver/rmt_tx.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_encoder.h>
#include <driver/gptimer.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <furi_hal_power.h>
#include <esp_rom_sys.h> /* esp_rom_delay_us() for the raw IR LED self-test */

#include BOARD_INCLUDE

/* Pin CPU at max frequency while IR RX/TX is active. RMT is APB-clocked (DFS-safe)
 * but this keeps the refill/timeout ISR latency tight. Separate guards for RX and TX
 * because the underlying perf lock is counting and the two can be independent. */
static bool furi_hal_ir_tx_perf_held = false;
static bool furi_hal_ir_rx_perf_held = false;
static void furi_hal_ir_tx_perf(bool on) {
    if(on && !furi_hal_ir_tx_perf_held) {
        furi_hal_power_perf_lock_acquire();
        furi_hal_ir_tx_perf_held = true;
    } else if(!on && furi_hal_ir_tx_perf_held) {
        furi_hal_power_perf_lock_release();
        furi_hal_ir_tx_perf_held = false;
    }
}
static void furi_hal_ir_rx_perf(bool on) {
    if(on && !furi_hal_ir_rx_perf_held) {
        furi_hal_power_perf_lock_acquire();
        furi_hal_ir_rx_perf_held = true;
    } else if(!on && furi_hal_ir_rx_perf_held) {
        furi_hal_power_perf_lock_release();
        furi_hal_ir_rx_perf_held = false;
    }
}

/* ---- Configuration ---- */

#if BOARD_HAS_IR
#define IR_TX_GPIO  BOARD_PIN_IR_TX
#define IR_RX_GPIO  BOARD_PIN_IR_RX
#else
#define IR_TX_GPIO  GPIO_NUM_NC
#define IR_RX_GPIO  GPIO_NUM_NC
#endif

/* Active internal IR pins. Default to the board pins; the Pin Config app can
 * remap them at boot via furi_hal_infrared_set_pins(). */
static gpio_num_t s_ir_tx_pin = IR_TX_GPIO;
static gpio_num_t s_ir_rx_pin = IR_RX_GPIO;

void furi_hal_infrared_set_pins(uint16_t tx_pin, uint16_t rx_pin) {
    s_ir_tx_pin = (gpio_num_t)tx_pin;
    s_ir_rx_pin = (gpio_num_t)rx_pin;
}

/* Grove / Qwiic connector pins for external IR unit (M5Unit IR) auto-detection.
 * On the Cardputer-ADV the Grove RX line is the same pin as the internal IR RX
 * (both GPIO1 = Grove SCL); the internal LED is on G44, the Grove IR TX on G2. */
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
#define IR_GROVE_TX_GPIO ((gpio_num_t)BOARD_PIN_QWIIC_SDA) /* M5Unit IR TX line */
#define IR_GROVE_RX_GPIO ((gpio_num_t)BOARD_PIN_QWIIC_SCL) /* M5Unit IR TSOP RX */
#else
#define IR_GROVE_TX_GPIO GPIO_NUM_NC
#define IR_GROVE_RX_GPIO GPIO_NUM_NC
#endif

/* Which physical TX output the IR HAL currently drives. Selected by the IR app
 * via furi_hal_infrared_set_tx_output() (usually from the auto-detect probe). */
static FuriHalInfraredTxPin active_tx_pin = FuriHalInfraredTxPinInternal;

#define IR_RMT_RX_MEM_BLOCK_SYMBOLS 128
#define IR_RMT_RX_RESOLUTION_HZ     1000000 /* 1 MHz = 1 us per tick */
#define IR_RMT_TX_RESOLUTION_HZ     1000000 /* 1 MHz = 1 us per tick */
#define IR_RMT_RX_MAX_SYMBOLS       1024
/* Byte length of the RX symbol buffer. The buffer is now heap-allocated (a
 * pointer), so sizeof(ir_rx.symbols) no longer gives the buffer size — pass
 * this to the rmt_receive() calls that formerly used sizeof(ir_rx.symbols). */
#define IR_RMT_RX_SYMBOLS_BYTES     (IR_RMT_RX_MAX_SYMBOLS * sizeof(rmt_symbol_word_t))

/* When the RX line returns to idle, RMT ends the burst and records the
 * terminating space as a zero-duration "end marker" — so the long inter-frame
 * space that trails every burst is never delivered to the decoder. The STM32
 * capture path delivers that space naturally (when the next burst's leading
 * edge arrives), and min_split_time protocols (NEC, Samsung, RC5/6, SIRC,
 * Kaseikyo, RCA, Pioneer) rely on it to finish decoding mid-stream. Without it
 * they can only complete via the silence timeout — which never fires while a
 * button is HELD, because the ~110 ms repeat bursts keep restarting it, so a
 * held remote is never learned. We therefore synthesise the trailing space at
 * the end of each idle-terminated burst. Its duration must exceed the largest
 * protocol min_split_time (Pioneer = 26000 us) yet stay below NEC's repeat
 * pause max (150000 us) and the silence timeout, so 50 ms is a safe choice. */
#define IR_RX_FRAME_END_SPACE_US 50000

/* ---- State ---- */

typedef enum {
    InfraredStateIdle,
    InfraredStateAsyncRx,
    InfraredStateAsyncTx,
    InfraredStateAsyncTxStopReq,
    InfraredStateAsyncTxStopped,
    InfraredStateMAX,
} InfraredState;

/* RX state */
static struct {
    rmt_channel_handle_t channel;
    rmt_receive_config_t rx_config;
    rmt_symbol_word_t* symbols; /* on-demand IR_RMT_RX_SYMBOLS_BYTES: alloc in async_rx_start, free in async_rx_stop */
    FuriHalInfraredRxCaptureCallback capture_callback;
    void* capture_context;
    FuriHalInfraredRxTimeoutCallback timeout_callback;
    void* timeout_context;
    gptimer_handle_t timeout_timer;
    uint32_t timeout_us;
} ir_rx;
static volatile uint32_t s_ir_rx_diag_syms;
static volatile uint32_t s_ir_rx_diag_cbs;
static volatile uint32_t s_ir_diag_d0, s_ir_diag_d1;
static volatile int s_ir_diag_l0, s_ir_diag_l1;

/* TX state */
static struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t copy_encoder;
    FuriHalInfraredTxGetDataISRCallback data_callback;
    void* data_context;
    FuriHalInfraredTxSignalSentISRCallback signal_sent_callback;
    void* signal_sent_context;
    SemaphoreHandle_t done_semaphore;
    uint32_t carrier_freq;
    float duty_cycle;
} ir_tx;

static volatile InfraredState ir_state = InfraredStateIdle;

/* ---- RX Implementation ---- */

static void ir_rx_restart_timeout(void) {
    if(ir_rx.timeout_timer) {
        gptimer_stop(ir_rx.timeout_timer);
        gptimer_set_raw_count(ir_rx.timeout_timer, 0);
        gptimer_start(ir_rx.timeout_timer);
    }
}

static bool IRAM_ATTR ir_rx_timeout_isr(gptimer_handle_t timer,
                                         const gptimer_alarm_event_data_t* edata,
                                         void* user_ctx) {
    (void)timer;
    (void)edata;
    (void)user_ctx;
    gptimer_stop(timer);
    if(ir_rx.timeout_callback) {
        ir_rx.timeout_callback(ir_rx.timeout_context);
    }
    return false; /* no high-priority task woken */
}

static bool IRAM_ATTR ir_rmt_rx_done_callback(rmt_channel_handle_t channel,
                                                const rmt_rx_done_event_data_t* edata,
                                                void* user_ctx) {
    (void)channel;
    (void)user_ctx;

    bool last_level_mark = false; /* level of the last edge we delivered */
    bool any_edge = false;

    /* Process received symbols and call capture callback for each edge */
    s_ir_rx_diag_cbs++;
    s_ir_rx_diag_syms += edata->num_symbols;
    if(s_ir_rx_diag_cbs == 1 && edata->num_symbols > 0) {
        s_ir_diag_d0 = edata->received_symbols[0].duration0; s_ir_diag_l0 = edata->received_symbols[0].level0;
        s_ir_diag_d1 = edata->received_symbols[0].duration1; s_ir_diag_l1 = edata->received_symbols[0].level1;
    }
    for(size_t i = 0; i < edata->num_symbols; i++) {
        const rmt_symbol_word_t* sym = &edata->received_symbols[i];

        /* Each RMT symbol has two phases: duration0/level0 then duration1/level1.
         * invert_in=true on the RX channel already converts the active-low IR
         * receiver output to logical mark=1/space=0, so pass sym->level through
         * unchanged. Negating here would drop the leader mark via the worker's
         * "skip first space" rule. */
        if(sym->duration0 > 0 && ir_rx.capture_callback) {
            ir_rx.capture_callback(
                ir_rx.capture_context, sym->level0, sym->duration0);
            ir_rx_restart_timeout();
            last_level_mark = sym->level0;
            any_edge = true;
        }
        if(sym->duration1 > 0 && ir_rx.capture_callback) {
            ir_rx.capture_callback(
                ir_rx.capture_context, sym->level1, sym->duration1);
            ir_rx_restart_timeout();
            last_level_mark = sym->level1;
            any_edge = true;
        }
    }

    /* A burst always ends on a Mark (the trailing Space is the idle that
     * terminated reception, dropped above as a zero-duration end marker).
     * Deliver that trailing Space synthetically so min_split_time protocols
     * decode immediately, instead of waiting for a silence timeout that never
     * arrives while a held remote keeps repeating. See IR_RX_FRAME_END_SPACE_US.
     * Decoded protocols consume it (the worker resets its timing count, so no
     * stray raw timing); raw signals just gain a trailing gap. */
    if(any_edge && last_level_mark && ir_rx.capture_callback) {
        ir_rx.capture_callback(ir_rx.capture_context, false, IR_RX_FRAME_END_SPACE_US);
        ir_rx_restart_timeout();
    }

    /* Re-start receiving for next burst. The buffer is always non-NULL while
     * state==AsyncRx (alloc precedes the AsyncRx transition, free follows the
     * Idle transition + channel delete), so the NULL check is purely defensive. */
    if(ir_state == InfraredStateAsyncRx && ir_rx.symbols) {
        rmt_receive(channel, ir_rx.symbols, IR_RMT_RX_SYMBOLS_BYTES, &ir_rx.rx_config);
    }

    return false;
}

void furi_hal_infrared_async_rx_start(void) {
    furi_check(ir_state == InfraredStateIdle);
#if !BOARD_HAS_IR
    FURI_LOG_E("IR", "Board has no IR support");
    return;
#endif
    furi_hal_ir_rx_perf(true); /* keep CPU at max while capturing IR edges */

    /* Allocate the RX symbol buffer on demand so its ~4 KB of internal RAM is
     * reclaimed whenever IR RX is idle (e.g. during Sub-GHz). Internal RAM is
     * fine here (the RX channel uses with_dma=false). Allocated before the RMT
     * channel is created and before the AsyncRx transition, so no ISR can run
     * against it yet; fail hard on OOM, mirroring the TX path's furi_check().
     * Freed in furi_hal_infrared_async_rx_stop(). */
    ir_rx.symbols =
        heap_caps_malloc(IR_RMT_RX_SYMBOLS_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    furi_check(ir_rx.symbols);

    /* Route RX to the active input: the Grove M5Unit TSOP when an external
     * module is selected, otherwise the board's internal IR RX pin. */
    gpio_num_t rx_gpio = s_ir_rx_pin;
    if(active_tx_pin == FuriHalInfraredTxPinExtPA7) {
        rx_gpio = IR_GROVE_RX_GPIO;
    }

    s_ir_rx_diag_syms = 0; s_ir_rx_diag_cbs = 0;
    FURI_LOG_I("IRDIAG", "rx_start gpio=%d ext=%d", (int)rx_gpio, (int)(active_tx_pin == FuriHalInfraredTxPinExtPA7));
    /* Configure RMT RX channel */
    rmt_rx_channel_config_t rx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_RX_RESOLUTION_HZ,
        .mem_block_symbols = IR_RMT_RX_MEM_BLOCK_SYMBOLS,
        .gpio_num = rx_gpio,
        .flags = {
            .invert_in = true, /* IR receiver module output is active-low */
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_chan_config, &ir_rx.channel));
    gpio_pullup_en(rx_gpio); /* IR receiver output can float weak; internal pull-up steadies marginal RX */

    /* Register RX done callback */
    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = ir_rmt_rx_done_callback,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(ir_rx.channel, &rx_cbs, NULL));

    /* Enable channel */
    ESP_ERROR_CHECK(rmt_enable(ir_rx.channel));

    /* Configure receive parameters */
    ir_rx.rx_config.signal_range_min_ns = 1250;  /* glitch filter: ignore < 1.25us */
    /* RMT idle threshold: silence longer than this ends the frame.
       Max at 1 MHz resolution is ~32.7 ms (RMT_LL_MAX_IDLE_VALUE=32767).
       Use 20 ms default which covers all common IR protocols. */
    uint32_t max_ns = ir_rx.timeout_us > 0 ? ir_rx.timeout_us * 1000 : 20000 * 1000;
    if(max_ns > 32000 * 1000) max_ns = 32000 * 1000; /* clamp to RMT HW limit */
    ir_rx.rx_config.signal_range_max_ns = max_ns;
    ir_rx.rx_config.flags.en_partial_rx = false;

    ir_state = InfraredStateAsyncRx;

    /* Start receiving */
    ESP_ERROR_CHECK(rmt_receive(ir_rx.channel, ir_rx.symbols, IR_RMT_RX_SYMBOLS_BYTES, &ir_rx.rx_config));
}

void furi_hal_infrared_async_rx_stop(void) {
    if(ir_state != InfraredStateAsyncRx) {
        FURI_LOG_W("IR", "async_rx_stop called but RX was not running");
        return;
    }

    ir_state = InfraredStateIdle;

    rmt_disable(ir_rx.channel);
    rmt_del_channel(ir_rx.channel);
    ir_rx.channel = NULL;

    /* The channel is deleted above (after ir_state was set to Idle), so the
     * RX-done ISR can no longer fire or re-arm against this buffer — free it
     * here to reclaim ~4 KB of internal RAM until the next async_rx_start().
     * Safe on every teardown: the early-return above skips this when RX was
     * never started (buffer stays NULL), and NULLing guards against double-free. */
    heap_caps_free(ir_rx.symbols);
    ir_rx.symbols = NULL;

    /* Stop and delete timeout timer if active */
    if(ir_rx.timeout_timer) {
        gptimer_stop(ir_rx.timeout_timer);
        gptimer_disable(ir_rx.timeout_timer);
        gptimer_del_timer(ir_rx.timeout_timer);
        ir_rx.timeout_timer = NULL;
    }

    FURI_LOG_I("IRDIAG", "rx_stop cbs=%lu syms=%lu sym0: d0=%lu l%d d1=%lu l%d", (unsigned long)s_ir_rx_diag_cbs, (unsigned long)s_ir_rx_diag_syms, (unsigned long)s_ir_diag_d0, s_ir_diag_l0, (unsigned long)s_ir_diag_d1, s_ir_diag_l1);
    furi_hal_ir_rx_perf(false); /* RX stopped → allow CPU to scale down */
}

void furi_hal_infrared_async_rx_set_timeout(uint32_t timeout_us) {
    ir_rx.timeout_us = timeout_us;

    /* If RX is already running, create/update the GPTimer for timeout */
    if(ir_state == InfraredStateAsyncRx || ir_state == InfraredStateIdle) {
        /* Delete existing timer */
        if(ir_rx.timeout_timer) {
            gptimer_stop(ir_rx.timeout_timer);
            gptimer_disable(ir_rx.timeout_timer);
            gptimer_del_timer(ir_rx.timeout_timer);
            ir_rx.timeout_timer = NULL;
        }

        if(timeout_us == 0) return;

        /* Create a one-shot alarm timer */
        gptimer_config_t timer_config = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000, /* 1 MHz = 1us */
        };
        ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &ir_rx.timeout_timer));

        gptimer_alarm_config_t alarm_config = {
            .alarm_count = timeout_us,
            .reload_count = 0,
            .flags = {
                .auto_reload_on_alarm = false,
            },
        };
        ESP_ERROR_CHECK(gptimer_set_alarm_action(ir_rx.timeout_timer, &alarm_config));

        gptimer_event_callbacks_t timer_cbs = {
            .on_alarm = ir_rx_timeout_isr,
        };
        ESP_ERROR_CHECK(gptimer_register_event_callbacks(ir_rx.timeout_timer, &timer_cbs, NULL));
        ESP_ERROR_CHECK(gptimer_enable(ir_rx.timeout_timer));
        /* Timer will be started when first edge is received */
    }
}

void furi_hal_infrared_async_rx_set_capture_isr_callback(
    FuriHalInfraredRxCaptureCallback callback,
    void* ctx) {
    ir_rx.capture_callback = callback;
    ir_rx.capture_context = ctx;
}

void furi_hal_infrared_async_rx_set_timeout_isr_callback(
    FuriHalInfraredRxTimeoutCallback callback,
    void* ctx) {
    ir_rx.timeout_callback = callback;
    ir_rx.timeout_context = ctx;
}

/* ---- TX Implementation ---- */

/**
 * TX strategy: We use a simple loop in a FreeRTOS task context.
 * The data_callback provides (duration_us, level) pairs.
 * We build RMT symbols and transmit them in batches.
 *
 * The STM32 uses DMA double-buffering with ISR callbacks;
 * on ESP32 we use the RMT encoder infrastructure with a custom
 * "IR TX encoder" that pulls data from the callback.
 */

/* A raw signal can hold up to MAX_TIMINGS_AMOUNT (1024) timings = 512 RMT
 * symbols. The whole packet MUST be sent in a single rmt_transmit() call:
 * splitting it into several transactions makes the RMT channel go idle
 * between them, inserting timing gaps mid-frame that corrupt the waveform so
 * the receiver no longer recognises it (raw/learned signals only — decoded
 * protocol packets are short). The RMT driver does internal ping-pong refill
 * from this buffer, so one transaction is gapless even though the buffer is
 * larger than mem_block_symbols. */
#define IR_TX_MAX_SYMBOLS 600

static void ir_tx_task(void* arg) {
    (void)arg;

    rmt_symbol_word_t* symbols =
        heap_caps_malloc(IR_TX_MAX_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_8BIT);
    furi_check(symbols);
    rmt_transmit_config_t tx_config = {
        .loop_count = 0, /* no loop, single shot */
        .flags = {
            .eot_level = 0, /* idle low after transmission */
        },
    };

    bool running = true;
    while(running) {
        size_t sym_count = 0;
        bool packet_end = false;
        bool last_packet = false;

        /* Accumulate one full packet of symbols from the callback */
        while(sym_count < IR_TX_MAX_SYMBOLS && !packet_end) {
            uint32_t duration_mark = 0;
            uint32_t duration_space = 0;
            bool level_mark = false;
            bool level_space = false;

            /* Get mark timing */
            FuriHalInfraredTxGetDataState state_mark =
                ir_tx.data_callback(ir_tx.data_context, &duration_mark, &level_mark);

            if(state_mark == FuriHalInfraredTxGetDataStateLastDone && duration_mark == 0) {
                last_packet = true;
                break;
            }

            /* Get space timing */
            FuriHalInfraredTxGetDataState state_space = FuriHalInfraredTxGetDataStateOk;
            if(state_mark == FuriHalInfraredTxGetDataStateOk) {
                state_space =
                    ir_tx.data_callback(ir_tx.data_context, &duration_space, &level_space);
            } else {
                /* Mark was the last in this packet, add a trailing space */
                duration_space = 0;
                level_space = false;
                if(state_mark == FuriHalInfraredTxGetDataStateDone) {
                    packet_end = true;
                } else {
                    last_packet = true;
                    packet_end = true;
                }
            }

            /* Build RMT symbol: duration0=mark, duration1=space */
            symbols[sym_count].duration0 = duration_mark;
            symbols[sym_count].level0 = level_mark ? 1 : 0;
            symbols[sym_count].duration1 = duration_space > 0 ? duration_space : 1;
            symbols[sym_count].level1 = level_space ? 1 : 0;
            sym_count++;

            if(state_space == FuriHalInfraredTxGetDataStateDone) {
                packet_end = true;
            } else if(state_space == FuriHalInfraredTxGetDataStateLastDone) {
                packet_end = true;
                last_packet = true;
            }
        }

        if(sym_count > 0) {
            /* Transmit the whole packet in a single (gapless) transaction. Handle
             * errors gracefully instead of ESP_ERROR_CHECK: rmt_transmit can fail
             * (e.g. transient OOM) and must NOT abort()/reboot the device. Use a
             * finite wait (an IR burst is well under 1 s) so a stalled channel can
             * never hang the task at portMAX_DELAY. */
            esp_err_t terr = rmt_transmit(
                ir_tx.channel,
                ir_tx.copy_encoder,
                symbols,
                sym_count * sizeof(rmt_symbol_word_t),
                &tx_config);
            if(terr == ESP_OK) {
                terr = rmt_tx_wait_all_done(ir_tx.channel, 1000);
            }
            if(terr != ESP_OK) {
                FURI_LOG_E("IR", "rmt tx failed (%s) - ending burst", esp_err_to_name(terr));
                packet_end = true; /* fire signal-sent so the app UI does not hang */
                last_packet = true; /* stop after this iteration */
            }
        }

        if(packet_end) {
            /* Signal sent callback */
            if(ir_tx.signal_sent_callback) {
                ir_tx.signal_sent_callback(ir_tx.signal_sent_context);
            }
        }

        /* Check stop conditions */
        if(last_packet || ir_state == InfraredStateAsyncTxStopReq) {
            running = false;
        }
    }

    heap_caps_free(symbols);

    ir_state = InfraredStateAsyncTxStopped;
    if(ir_tx.done_semaphore) {
        xSemaphoreGive(ir_tx.done_semaphore);
    }

#if defined(CONFIG_SPIRAM)
    vTaskDeleteWithCaps(NULL); /* stack was allocated from PSRAM (see async_tx_start) */
#else
    vTaskDelete(NULL);
#endif
}

/* ---- Camera-verifiable IR LED self-test ---------------------------------
 * Raw-drives the internal IR TX GPIO with a ~38 kHz square wave in short bursts
 * for `ms` total, BYPASSING the RMT/carrier/encoder path entirely. Point a phone
 * camera at the LED: if it flickers, the GPIO->LED hardware is good and any
 * "nothing transmitted" fault is in the RMT/app data path; if it stays dark, the
 * pin mapping or emitter itself is the problem. Runs in its own task so it never
 * blocks the UI, and yields between bursts so the task watchdog stays fed. */
static void ir_led_selftest_task(void* arg) {
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    gpio_num_t pin = s_ir_tx_pin;
    if(active_tx_pin == FuriHalInfraredTxPinExtPA7) pin = IR_GROVE_TX_GPIO;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    furi_hal_ir_tx_perf(true);
    FURI_LOG_I("IRDIAG", "LED self-test: 38kHz on GPIO%d for %lu ms", (int)pin, (unsigned long)ms);

    uint32_t done = 0;
    while(done < ms) {
        /* ~90 ms of 38 kHz carrier (half-period ~13 us) */
        for(int i = 0; i < 3400; i++) {
            gpio_set_level(pin, 1);
            esp_rom_delay_us(13);
            gpio_set_level(pin, 0);
            esp_rom_delay_us(13);
        }
        done += 90;
        /* ~60 ms gap: LED off + yield so the watchdog is fed */
        gpio_set_level(pin, 0);
        vTaskDelay(pdMS_TO_TICKS(60));
        done += 60;
    }
    gpio_set_level(pin, 0);
    furi_hal_ir_tx_perf(false);
    FURI_LOG_I("IRDIAG", "LED self-test done on GPIO%d", (int)pin);
    vTaskDelete(NULL);
}

void furi_hal_infrared_led_selftest(uint32_t ms) {
    if(ir_state != InfraredStateIdle) return; /* don't fight an active TX/RX */
    if(ms == 0) ms = 3000;
    xTaskCreate(ir_led_selftest_task, "ir_selftest", 2560, (void*)(uintptr_t)ms, 10, NULL);
}

void furi_hal_infrared_async_tx_start(uint32_t freq, float duty_cycle) {
    furi_check(ir_state == InfraredStateIdle);
    furi_check(ir_tx.data_callback != NULL);
    furi_check(freq >= INFRARED_MIN_FREQUENCY && freq <= INFRARED_MAX_FREQUENCY);
    furi_check(duty_cycle > 0.0f && duty_cycle <= 1.0f);
#if !BOARD_HAS_IR
    FURI_LOG_E("IR", "Board has no IR support");
    return;
#endif
    furi_hal_ir_tx_perf(true); /* keep CPU at max during IR transmit */

    ir_tx.carrier_freq = freq;
    ir_tx.duty_cycle = duty_cycle;

    /* Route TX to the active output: the Grove M5Unit IR LED (G2) when an
     * external module is selected, otherwise the board's internal IR LED (G44). */
    gpio_num_t tx_gpio = s_ir_tx_pin;
    if(active_tx_pin == FuriHalInfraredTxPinExtPA7) {
        tx_gpio = IR_GROVE_TX_GPIO;
    }
    FURI_LOG_I(
        "IRDIAG",
        "tx_start gpio=%d freq=%lu duty=%d%% ext=%d",
        (int)tx_gpio,
        (unsigned long)freq,
        (int)(duty_cycle * 100.0f),
        (int)(active_tx_pin == FuriHalInfraredTxPinExtPA7));

    /* Create RMT TX channel with carrier modulation. trans_queue_depth=1 and a
     * single 48-symbol mem block keep the INTERNAL-RAM allocation minimal: we
     * serialize with rmt_tx_wait_all_done() and never queue more than one
     * transaction, so depth 1 is sufficient. Depth 4 used to abort the device with
     * "rmt_tx_create_trans_queue: no mem for queues" when internal RAM was low. */
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_TX_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
        .gpio_num = tx_gpio,
        .flags = {
            .invert_out = false,
            .with_dma = false,
        },
    };

    ir_tx.channel = NULL;
    ir_tx.copy_encoder = NULL;
    ir_tx.done_semaphore = NULL;

    /* Bring the channel up with GRACEFUL failure instead of ESP_ERROR_CHECK: a
     * failed allocation under memory pressure must never abort()/reboot. */
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &ir_tx.channel);
    if(err == ESP_OK) {
        rmt_carrier_config_t carrier_config = {
            .frequency_hz = freq,
            .duty_cycle = duty_cycle,
            .flags = {
                .polarity_active_low = false,
                .always_on = false, /* Only modulate during mark (level=1) */
            },
        };
        err = rmt_apply_carrier(ir_tx.channel, &carrier_config);
    }
    if(err == ESP_OK) {
        rmt_copy_encoder_config_t copy_enc_config = {};
        err = rmt_new_copy_encoder(&copy_enc_config, &ir_tx.copy_encoder);
    }
    if(err == ESP_OK) {
        err = rmt_enable(ir_tx.channel);
    }

    ir_tx.done_semaphore = xSemaphoreCreateBinary();
    ir_state = InfraredStateAsyncTx;

    /* Spawn the TX task only if the channel + semaphore came up. On PSRAM boards
     * put the task STACK in PSRAM: internal SRAM is often exhausted (this is the
     * "task/sem alloc" OOM that silently blocked TX), and the task never runs during
     * a flash-cache-disable window, so a PSRAM stack is safe here. No-PSRAM boards
     * (Cardputer) use a modest internal stack. */
    BaseType_t task_ok = pdFAIL;
    if(err == ESP_OK && ir_tx.done_semaphore != NULL) {
#if defined(CONFIG_SPIRAM)
        task_ok = xTaskCreateWithCaps(
            ir_tx_task, "ir_tx", 4096, NULL, 15, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        task_ok = xTaskCreate(ir_tx_task, "ir_tx", 3072, NULL, 15, NULL);
#endif
    }

    /* On any failure (typically low internal RAM), fail the transmit cleanly: tear
     * down what was built, mark the burst finished, and let async_tx_wait_termination()
     * return without hanging or crashing. */
    if(err != ESP_OK || ir_tx.done_semaphore == NULL || task_ok != pdPASS) {
        FURI_LOG_E(
            "IR",
            "async_tx_start failed (%s) - out of RAM? IR not sent",
            (err != ESP_OK) ? esp_err_to_name(err) : "task/sem alloc");
        if(ir_tx.channel) rmt_disable(ir_tx.channel); /* no-op/err if never enabled */
        if(ir_tx.copy_encoder) {
            rmt_del_encoder(ir_tx.copy_encoder);
            ir_tx.copy_encoder = NULL;
        }
        if(ir_tx.channel) {
            rmt_del_channel(ir_tx.channel);
            ir_tx.channel = NULL;
        }
        ir_state = InfraredStateAsyncTxStopped; /* "already finished" */
        if(ir_tx.done_semaphore) xSemaphoreGive(ir_tx.done_semaphore);
    }
}

void furi_hal_infrared_async_tx_wait_termination(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    /* Wait for TX task to finish */
    if(ir_tx.done_semaphore) {
        xSemaphoreTake(ir_tx.done_semaphore, portMAX_DELAY);
    }

    /* Cleanup — guarded: async_tx_start may have failed to create these under low RAM. */
    if(ir_tx.channel) rmt_disable(ir_tx.channel);
    if(ir_tx.copy_encoder) {
        rmt_del_encoder(ir_tx.copy_encoder);
        ir_tx.copy_encoder = NULL;
    }
    if(ir_tx.channel) {
        rmt_del_channel(ir_tx.channel);
        ir_tx.channel = NULL;
    }
    if(ir_tx.done_semaphore) {
        vSemaphoreDelete(ir_tx.done_semaphore);
        ir_tx.done_semaphore = NULL;
    }

    ir_state = InfraredStateIdle;
}

void furi_hal_infrared_async_tx_stop(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    if(ir_state == InfraredStateAsyncTx) {
        ir_state = InfraredStateAsyncTxStopReq;
    }

    furi_hal_infrared_async_tx_wait_termination();

    furi_hal_ir_tx_perf(false); /* TX finished → allow CPU to scale down */
}

void furi_hal_infrared_async_tx_set_data_isr_callback(
    FuriHalInfraredTxGetDataISRCallback callback,
    void* context) {
    furi_check(ir_state == InfraredStateIdle);
    ir_tx.data_callback = callback;
    ir_tx.data_context = context;
}

void furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
    FuriHalInfraredTxSignalSentISRCallback callback,
    void* context) {
    ir_tx.signal_sent_callback = callback;
    ir_tx.signal_sent_context = context;
}

bool furi_hal_infrared_is_busy(void) {
    return ir_state != InfraredStateIdle;
}

FuriHalInfraredTxPin furi_hal_infrared_detect_tx_output(void) {
    /*
     * Hardware loopback probe for the M5Unit IR module on the Grove connector.
     * Grove SDA (G2) feeds the module's IR LED; Grove SCL (G1) is the TSOP
     * receiver output (active-LOW when it sees a 38 kHz carrier). Emit ~20
     * cycles of 38 kHz on G2 and watch G1: if it pulls LOW the module is
     * present -> use the Grove/external TX; otherwise fall back to the internal
     * LED (G44). No module -> behaves exactly like the old stub (Internal).
     */
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
    gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << IR_GROVE_TX_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx_cfg);

    gpio_config_t rx_cfg = {
        .pin_bit_mask = (1ULL << IR_GROVE_RX_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rx_cfg);

    gpio_set_level(IR_GROVE_TX_GPIO, 0);
    furi_delay_ms(2);

    bool module_detected = false;
    for(int i = 0; i < 20 && !module_detected; i++) {
        gpio_set_level(IR_GROVE_TX_GPIO, 1);
        furi_delay_us(13); /* ~38 kHz half-period */
        gpio_set_level(IR_GROVE_TX_GPIO, 0);
        furi_delay_us(13);
        if(gpio_get_level(IR_GROVE_RX_GPIO) == 0) {
            module_detected = true;
        }
    }

    /* Restore both pins to Hi-Z so the RMT RX/TX HAL can reclaim them. */
    gpio_config_t restore_cfg = {
        .pin_bit_mask = (1ULL << IR_GROVE_TX_GPIO) | (1ULL << IR_GROVE_RX_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&restore_cfg);

    if(module_detected) {
        FURI_LOG_I("IR", "M5Unit IR module detected on Grove (G2/G1) - using external TX");
        return FuriHalInfraredTxPinExtPA7;
    }
    FURI_LOG_I("IR", "No IR module on Grove - using internal LED (G44)");
#endif
    return FuriHalInfraredTxPinInternal;
}

void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin tx_pin) {
    if(tx_pin < FuriHalInfraredTxPinMax) {
        active_tx_pin = tx_pin;
    }
}
