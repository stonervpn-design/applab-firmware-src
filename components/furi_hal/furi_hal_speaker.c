/**
 * @file furi_hal_speaker.c
 * Speaker HAL — I2S tone generation for boards with speaker hardware,
 * no-op stubs for boards without.
 *
 * Stays close to the STM32 Flipper Zero firmware API:
 *   - Mutex-based exclusive ownership (acquire/release/is_mine)
 *   - start(frequency, volume) / set_volume / stop
 *   - Cubic volume scaling (v^3) for perceptually linear loudness
 *
 * On ESP32 the speaker is driven via I2S (instead of STM32's PWM/TIM16).
 * A background thread continuously writes a sine-wave buffer to the I2S
 * peripheral; frequency and volume changes regenerate the buffer.
 */

#include "furi_hal_speaker.h"
#include "furi_hal_mic.h"
#include "furi_hal.h" /* furi_hal_m5pm1_set_speaker_amp (StickS3 AW8737 amp) */
#include "boards/board.h"
#include <furi.h>

#define TAG "FuriHalSpeaker"

#if BOARD_HAS_SPEAKER

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#if defined(BOARD_HAS_ES8311) && BOARD_HAS_ES8311
#include <driver/i2c.h>
#endif

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Configuration ---- */
#define SPEAKER_SAMPLE_RATE   44100
#define SPEAKER_BITS          16
#define SPEAKER_DMA_DESC_NUM  4
#define SPEAKER_DMA_FRAME_NUM 256
#define SPEAKER_THREAD_STACK  2048

/* Maximum number of samples we pre-compute for one sine cycle.
 * For very low frequencies the cycle is long; we cap it to keep
 * memory usage reasonable (~4 KB for stereo 16-bit). */
#define SPEAKER_MAX_CYCLE_SAMPLES 1024

/* GDO-mirror mode: sample-rate and per-write buffer size.
 * 16 kHz sample rate is plenty for 1-bit OOK/FM audio (typical RF audio
 * bandwidth <5 kHz) and keeps CPU cost reasonable. 64-frame buffer ⇒ ~4 ms
 * per write, which sets the pacing for the I2S writer loop. */
#define MIRROR_SAMPLE_RATE 16000
#define MIRROR_BUF_FRAMES  64

typedef enum {
    SpeakerModeIdle,
    SpeakerModeTone,
    SpeakerModeGdoMirror,
} SpeakerMode;

/* ---- Static state ---- */
static FuriMutex* speaker_mutex = NULL;
static i2s_chan_handle_t i2s_tx_handle = NULL;
#ifdef BOARD_PIN_SPEAKER_DIN
static i2s_chan_handle_t i2s_rx_handle = NULL; /* full-duplex RX (ES8311 ADC / mic) */
static bool i2s_rx_enabled = false;
#endif
static bool i2s_channel_enabled = false;
/* Set true only after I2S init fully succeeds. If audio init fails (e.g. a bad
 * pin/config), it is logged and left false so acquire() no-ops instead of the
 * whole device aborting at boot. */
static bool speaker_available = false;

/* Active mode — read by writer thread, written by API callers under ownership. */
static volatile SpeakerMode speaker_mode = SpeakerModeIdle;

/* Tone-mode state */
static volatile float speaker_frequency = 0.0f;
static volatile float speaker_volume = 0.0f;
static volatile bool speaker_buffer_dirty = true;

/* GDO-mirror-mode state */
static volatile gpio_num_t speaker_mirror_pin = (gpio_num_t)-1;
static volatile float speaker_mirror_volume = 0.0f;

/* Background writer thread */
static FuriThread* speaker_thread = NULL;
static volatile bool speaker_thread_run = false;

/* Pre-computed waveform buffer (stereo interleaved: L, R, L, R, ...) */
static int16_t* wave_buffer = NULL;
static size_t wave_buffer_samples = 0; /* number of stereo frames */
static size_t wave_buffer_bytes = 0;

#if defined(BOARD_HAS_ES8311) && BOARD_HAS_ES8311
/* ---- ES8311 codec (Cardputer-ADV) ----
 * Unlike the standard Cardputer's dumb NS4168 I2S amp, the ADV routes audio
 * through an ES8311 codec that must be configured over I2C before it will pass
 * any I2S data to the speaker. Init runs lazily on the first speaker acquire
 * (by then the keyboard has installed the shared I2C bus and the I2S channel —
 * hence MCLK — is enabled). */
static bool es8311_inited = false;

static esp_err_t es8311_write(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_ES8311_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_ES8311_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}


/* Register init for DAC/speaker playback on the Cardputer-ADV. Values follow
 * M5Unified's `_speaker_enabled_cb_cardputer_adv` (m5stack/M5Unified), which is
 * the known-good sequence for THIS board. The critical bit is reg 0x01=0xB5:
 * the ADV routes NO dedicated MCLK to the ES8311, so the codec must derive its
 * master clock from BCLK/SCLK. (The previous 0x01=0x30 made the codec wait for
 * an MCLK pin that doesn't exist here → DAC never clocked → silence.)
 * es8311_seq[0] is a reset with a 20 ms settle before the rest is written. */
static const uint8_t es8311_seq[][2] = {
    {0x00, 0x1F}, /* reset digital + analog (20 ms settle follows) */
#ifdef BOARD_PIN_SPEAKER_DIN
    {0x00, 0x00}, /* deassert reset (full-duplex/mic path) */
#endif
    {0x00, 0x80}, /* CSM power on */
#ifdef BOARD_PIN_SPEAKER_DIN
    /* --- StickS3 full-duplex (speaker + mic) --- */
    {0x45, 0x00}, /* clear GP */
    {0x44, 0x00}, /* SDOUT source = ADC */
    {0x01, 0xBF}, /* clock: MCLK from BCLK + BOTH DAC and ADC clocks (0xB5=DAC-only,
                     0xBA=ADC-only, 0xBF=both — needed to play AND capture at once) */
#else
    /* --- Cardputer-ADV: playback only, no MCLK pin (proven value) --- */
    {0x01, 0xB5}, /* clock manager: MCLK sourced from BCLK/SCLK */
#endif
    {0x02, 0x18}, /* clock manager: MULT_PRE=3 */
    {0x0D, 0x01}, /* power up analog circuitry */
    {0x12, 0x00}, /* power up DAC */
    {0x13, 0x10}, /* enable output drive (to speaker/HP) */
    {0x32, 0xBF}, /* DAC volume ~0 dB */
    {0x37, 0x08}, /* bypass DAC equalizer */
#ifdef BOARD_PIN_SPEAKER_DIN
    /* ADC / microphone path — EXACTLY the M5 StickS3 mic-enable registers. */
    {0x0E, 0x02}, /* power up analog PGA + ADC modulator */
    {0x14, 0x10}, /* mic input select (M5 StickS3 value) */
    {0x17, 0xFF}, /* ADC volume max */
    {0x1C, 0x6A}, /* ADC EQ bypass + DC offset cancellation */
#endif
};

static void es8311_ensure_init(void) {
    if(es8311_inited) return;
    es8311_inited = true;

    esp_err_t first = es8311_write(es8311_seq[0][0], es8311_seq[0][1]); /* reset */
    furi_delay_ms(20);
    unsigned nacks = (first != ESP_OK) ? 1 : 0;
    for(size_t i = 1; i < sizeof(es8311_seq) / sizeof(es8311_seq[0]); i++) {
        if(es8311_write(es8311_seq[i][0], es8311_seq[i][1]) != ESP_OK) nacks++;
    }
    FURI_LOG_I(TAG, "ES8311 init done (addr 0x%02X, %u write errors)",
               BOARD_ES8311_I2C_ADDR, nacks);
}

void furi_hal_speaker_es8311_set_volume_reg(float volume) {
    if(!es8311_inited) return;
    if(volume < 0.0f) volume = 0.0f;
    if(volume > 1.0f) volume = 1.0f;
    es8311_write(0x32, (uint8_t)(volume * 255.0f)); /* 0x00 mute .. 0xFF max */
}
#endif

/* ---- Helpers ---- */

/** Apply cubic volume scaling (matches STM32 firmware behaviour). */
static inline float speaker_volume_curve(float v) {
    if(v < 0.0f) v = 0.0f;
    if(v > 1.0f) v = 1.0f;
    return v * v * v;
}

/** (Re-)generate the sine-wave buffer for the current frequency/volume. */
static void speaker_generate_buffer(void) {
    float freq = speaker_frequency;
    float vol = speaker_volume_curve(speaker_volume);

    if(freq < 1.0f) freq = 1.0f;

    /* Compute how many samples make up one full cycle */
    uint32_t samples_per_cycle = (uint32_t)(SPEAKER_SAMPLE_RATE / freq);
    if(samples_per_cycle < 2) samples_per_cycle = 2;
    if(samples_per_cycle > SPEAKER_MAX_CYCLE_SAMPLES) samples_per_cycle = SPEAKER_MAX_CYCLE_SAMPLES;

    /* Allocate / reallocate if needed */
    size_t needed = samples_per_cycle * 2 * sizeof(int16_t); /* stereo */
    if(wave_buffer == NULL || wave_buffer_samples != samples_per_cycle) {
        if(wave_buffer) free(wave_buffer);
        wave_buffer = malloc(needed);
        if(!wave_buffer) {
            wave_buffer_samples = 0;
            wave_buffer_bytes = 0;
            speaker_buffer_dirty = true;
            return;
        }
        wave_buffer_samples = samples_per_cycle;
    }
    wave_buffer_bytes = needed;

    /* Fill with sine wave */
    float amplitude = vol * 32767.0f;
    for(uint32_t i = 0; i < samples_per_cycle; i++) {
        int16_t sample = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * (float)i / (float)samples_per_cycle));
        wave_buffer[i * 2] = sample;     /* Left */
        wave_buffer[i * 2 + 1] = sample; /* Right */
    }

    speaker_buffer_dirty = false;
}

/** Background thread: continuously writes audio to I2S in the active mode. */
static int32_t speaker_writer_thread(void* context) {
    UNUSED(context);

    /* Stack-allocated mirror buffer (stereo int16). Kept here, not on the
     * thread stack of every iteration, so its lifetime spans the whole loop. */
    static int16_t mirror_buf[MIRROR_BUF_FRAMES * 2];

    while(speaker_thread_run) {
        SpeakerMode mode = speaker_mode;

        if(mode == SpeakerModeTone) {
            if(speaker_buffer_dirty) {
                speaker_generate_buffer();
            }
            if(wave_buffer && wave_buffer_bytes > 0) {
                size_t bytes_written = 0;
                i2s_channel_write(i2s_tx_handle, wave_buffer, wave_buffer_bytes, &bytes_written, 100);
            }
            continue;
        }

        if(mode == SpeakerModeGdoMirror) {
            const gpio_num_t pin = speaker_mirror_pin;
            const float vol = speaker_volume_curve(speaker_mirror_volume);
            const int16_t amp = (int16_t)(vol * 32767.0f);
            const int64_t period_us = 1000000 / MIRROR_SAMPLE_RATE;

            /* Sample the GPIO at MIRROR_SAMPLE_RATE, busy-waiting between
             * samples so each sample reflects the GDO0 level at the right
             * moment. The i2s_channel_write below provides the longer-term
             * pacing (DMA back-pressure) — busy-wait only handles intra-buffer
             * spacing (~62 µs at 16 kHz). */
            int64_t next_us = esp_timer_get_time();
            for(int i = 0; i < MIRROR_BUF_FRAMES; i++) {
                while(esp_timer_get_time() < next_us) { /* spin */ }
                next_us += period_us;
                int16_t s = gpio_get_level(pin) ? amp : (int16_t)-amp;
                mirror_buf[i * 2] = s;
                mirror_buf[i * 2 + 1] = s;
            }

            size_t bytes_written = 0;
            i2s_channel_write(i2s_tx_handle, mirror_buf, sizeof(mirror_buf), &bytes_written, 100);
            continue;
        }

        /* Idle */
        furi_delay_ms(5);
    }

    return 0;
}

/* ---- Public API ---- */

void furi_hal_speaker_init(void) {
    furi_assert(speaker_mutex == NULL);
    speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* Release pin 40 from LCD RST usage — the display driver only pulses it
     * during init and does not need it afterwards. */
    gpio_reset_pin((gpio_num_t)BOARD_PIN_SPEAKER_WCLK);

    /* Configure I2S standard mode */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = SPEAKER_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = SPEAKER_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true; /* send silence when no data */
#ifdef BOARD_PIN_SPEAKER_DIN
    esp_err_t nc_err = i2s_new_channel(&chan_cfg, &i2s_tx_handle, &i2s_rx_handle);
#else
    esp_err_t nc_err = i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL);
#endif
    if(nc_err != ESP_OK) {
        FURI_LOG_E(TAG, "i2s_new_channel failed: %s — speaker disabled", esp_err_to_name(nc_err));
        i2s_tx_handle = NULL;
        return; /* speaker_available stays false; acquire() no-ops */
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            /* Cardputer-ADV/standard Cardputer: no MCLK pin (derive from BCLK).
             * StickS3: route MCLK (GPIO18) — its ES8311 ADC needs a real MCLK. */
#ifdef BOARD_PIN_SPEAKER_MCLK
            .mclk = (gpio_num_t)BOARD_PIN_SPEAKER_MCLK,
#else
            .mclk = I2S_GPIO_UNUSED,
#endif
            .bclk = (gpio_num_t)BOARD_PIN_SPEAKER_BCLK,
            .ws = (gpio_num_t)BOARD_PIN_SPEAKER_WCLK,
            .dout = (gpio_num_t)BOARD_PIN_SPEAKER_DOUT,
#ifdef BOARD_PIN_SPEAKER_DIN
            .din = (gpio_num_t)BOARD_PIN_SPEAKER_DIN,
#else
            .din = I2S_GPIO_UNUSED,
#endif
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    esp_err_t std_err = i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg);
    if(std_err != ESP_OK) {
        FURI_LOG_E(
            TAG,
            "i2s_channel_init_std_mode failed: %s (BCLK=%d WS=%d DOUT=%d) — speaker disabled",
            esp_err_to_name(std_err), BOARD_PIN_SPEAKER_BCLK, BOARD_PIN_SPEAKER_WCLK,
            BOARD_PIN_SPEAKER_DOUT);
        i2s_del_channel(i2s_tx_handle);
        i2s_tx_handle = NULL;
        return; /* speaker_available stays false; acquire() no-ops */
    }

#ifdef BOARD_PIN_SPEAKER_DIN
    /* Init the RX (mic) side of the full-duplex channel. If it fails, keep the
     * speaker working and just disable the mic. */
    esp_err_t rx_err = i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
    if(rx_err != ESP_OK) {
        FURI_LOG_E(TAG, "i2s rx (mic) init failed: %s — mic disabled", esp_err_to_name(rx_err));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = NULL;
    }
#endif

    /* Start the writer thread (it idles until speaker_mode != Idle) */
    speaker_thread = furi_thread_alloc_ex("SpeakerWorker", SPEAKER_THREAD_STACK, speaker_writer_thread, NULL);
    speaker_thread_run = true;
    furi_thread_start(speaker_thread);

    speaker_available = true;
    FURI_LOG_I(TAG, "Init OK (I2S, BCLK=%d WS=%d DOUT=%d)",
               BOARD_PIN_SPEAKER_BCLK, BOARD_PIN_SPEAKER_WCLK, BOARD_PIN_SPEAKER_DOUT);
}

void furi_hal_speaker_deinit(void) {
    furi_check(speaker_mutex != NULL);

    /* Stop writer thread (only started when init fully succeeded) */
    if(speaker_thread) {
        speaker_thread_run = false;
        furi_thread_join(speaker_thread);
        furi_thread_free(speaker_thread);
        speaker_thread = NULL;
    }

    /* Tear down I2S (handle is NULL if init failed) */
    if(i2s_tx_handle) {
        if(i2s_channel_enabled) {
            i2s_channel_disable(i2s_tx_handle);
            i2s_channel_enabled = false;
        }
        i2s_del_channel(i2s_tx_handle);
        i2s_tx_handle = NULL;
    }
    speaker_available = false;

    /* Free buffer */
    if(wave_buffer) {
        free(wave_buffer);
        wave_buffer = NULL;
    }

    furi_mutex_free(speaker_mutex);
    speaker_mutex = NULL;
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    furi_check(!FURI_IS_IRQ_MODE());

    if(!speaker_available) return false; /* init failed/disabled: no-op, never crash */

    if(furi_mutex_acquire(speaker_mutex, timeout) == FuriStatusOk) {
        /* Enable I2S channel on first acquire */
        if(!i2s_channel_enabled) {
            ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle));
            i2s_channel_enabled = true;
        }
#if defined(BOARD_HAS_ES8311) && BOARD_HAS_ES8311
        /* MCLK is now running (channel enabled) and the keyboard's shared I2C
         * bus is up by first speaker use — configure the codec once. */
        es8311_ensure_init();
#endif
        /* Power on the output amplifier (StickS3 AW8737 via M5PM1 GPIO3, off at
         * boot). No-op on boards without the M5PM1 PMIC. */
        furi_hal_m5pm1_set_speaker_amp(true);
        return true;
    }
    return false;
}

void furi_hal_speaker_release(void) {
    furi_check(!FURI_IS_IRQ_MODE());
    furi_check(furi_hal_speaker_is_mine());

    furi_hal_speaker_stop();
    /* Power the output amp back down (StickS3 AW8737; no-op elsewhere). Also frees
     * the IR receiver, which cannot run while the amp is on. */
    furi_hal_m5pm1_set_speaker_amp(false);
    furi_check(furi_mutex_release(speaker_mutex) == FuriStatusOk);
}

void furi_hal_speaker_play_pcm(const int16_t* mono, size_t count, uint32_t sample_rate) {
    /* Blocking playback of a mono 16-bit clip while the speaker is acquired and
     * idle (the writer thread does not touch I2S in Idle mode). Nearest-neighbour
     * resample to SPEAKER_SAMPLE_RATE and duplicate mono -> stereo. */
    if(!speaker_available || !i2s_tx_handle || !mono || count == 0 || sample_rate == 0) return;

    enum { CHUNK_FRAMES = 256 };
    static int16_t chunk[CHUNK_FRAMES * 2];
    const uint64_t step = ((uint64_t)sample_rate << 16) / SPEAKER_SAMPLE_RATE; /* 16.16 */
    uint64_t pos = 0;

    while((size_t)(pos >> 16) < count) {
        size_t f = 0;
        for(; f < CHUNK_FRAMES; f++) {
            size_t si = (size_t)(pos >> 16);
            if(si >= count) break;
            chunk[f * 2] = mono[si];
            chunk[f * 2 + 1] = mono[si];
            pos += step;
        }
        if(f == 0) break;
        size_t bytes_written = 0;
        i2s_channel_write(i2s_tx_handle, chunk, f * 2 * sizeof(int16_t), &bytes_written, 1000);
    }
}

#ifdef BOARD_PIN_SPEAKER_DIN
bool furi_hal_mic_start(void) {
    if(!speaker_available || !i2s_rx_handle) return false;
    /* The I2S clocks are generated by the (master) TX channel — enable it so the
     * ES8311 is clocked, configure the codec (DAC+ADC), then enable RX capture. */
    if(!i2s_channel_enabled) {
        if(i2s_channel_enable(i2s_tx_handle) != ESP_OK) return false;
        i2s_channel_enabled = true;
    }
#if defined(BOARD_HAS_ES8311) && BOARD_HAS_ES8311
    es8311_ensure_init();
#endif
    if(!i2s_rx_enabled) {
        if(i2s_channel_enable(i2s_rx_handle) != ESP_OK) return false;
        i2s_rx_enabled = true;
    }
    return true;
}

size_t furi_hal_mic_read(int16_t* mono_out, size_t frames, uint32_t timeout_ms) {
    if(!i2s_rx_handle || !i2s_rx_enabled || !mono_out || frames == 0) return 0;
    /* The slot is stereo 16-bit; the ES8311 is mono so its sample is on the left
     * channel. Read stereo in blocks and extract the left channel to mono_out. */
    static int16_t stereo[256 * 2];
    size_t done = 0;
    while(done < frames) {
        size_t want = frames - done;
        if(want > 256) want = 256;
        size_t br = 0;
        if(i2s_channel_read(i2s_rx_handle, stereo, want * 2 * sizeof(int16_t), &br, timeout_ms) !=
           ESP_OK)
            break;
        size_t got = br / (2 * sizeof(int16_t));
        if(got == 0) break;
        for(size_t i = 0; i < got; i++) mono_out[done + i] = stereo[i * 2];
        done += got;
    }
    return done;
}

void furi_hal_mic_stop(void) {
    if(i2s_rx_handle && i2s_rx_enabled) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_rx_enabled = false;
    }
}
#else
bool furi_hal_mic_start(void) {
    return false;
}
size_t furi_hal_mic_read(int16_t* mono_out, size_t frames, uint32_t timeout_ms) {
    (void)mono_out;
    (void)frames;
    (void)timeout_ms;
    return 0;
}
void furi_hal_mic_stop(void) {
}
#endif

bool furi_hal_speaker_is_mine(void) {
    return (FURI_IS_IRQ_MODE()) ||
           (furi_mutex_get_owner(speaker_mutex) == furi_thread_get_current_id());
}

void furi_hal_speaker_start(float frequency, float volume) {
    furi_check(furi_hal_speaker_is_mine());

    if(volume <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    speaker_frequency = frequency;
    speaker_volume = volume;
    speaker_buffer_dirty = true;
    speaker_mode = SpeakerModeTone;
}

void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    furi_check(gdo_pin);
    furi_check(gdo_pin->pin < GPIO_NUM_MAX);

    if(volume <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    speaker_mirror_pin = (gpio_num_t)gdo_pin->pin;
    speaker_mirror_volume = volume;
    speaker_mode = SpeakerModeGdoMirror;
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());

    if(volume <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    /* Update whichever mode is active */
    speaker_volume = volume;
    speaker_mirror_volume = volume;
    speaker_buffer_dirty = true;
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());
    speaker_mode = SpeakerModeIdle;
}

#else /* !BOARD_HAS_SPEAKER */

/* ---- No-op stubs for boards without speaker hardware ---- */

void furi_hal_speaker_init(void) {
}

void furi_hal_speaker_deinit(void) {
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    (void)timeout;
    return true;
}

void furi_hal_speaker_release(void) {
}

bool furi_hal_speaker_is_mine(void) {
    return true;
}

void furi_hal_speaker_start(float frequency, float volume) {
    (void)frequency;
    (void)volume;
}

void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) {
    (void)gdo_pin;
    (void)volume;
}

void furi_hal_speaker_set_volume(float volume) {
    (void)volume;
}

void furi_hal_speaker_stop(void) {
}

void furi_hal_speaker_play_pcm(const int16_t* mono, size_t count, uint32_t sample_rate) {
    (void)mono;
    (void)count;
    (void)sample_rate;
}

bool furi_hal_mic_start(void) {
    return false;
}
size_t furi_hal_mic_read(int16_t* mono_out, size_t frames, uint32_t timeout_ms) {
    (void)mono_out;
    (void)frames;
    (void)timeout_ms;
    return 0;
}
void furi_hal_mic_stop(void) {
}

#endif /* BOARD_HAS_SPEAKER */
