/**
 * @file furi_hal_mic.h
 * Microphone capture HAL. On boards with a full-duplex I2S codec (StickS3:
 * ES8311 ADC + MEMS mic) this captures 16-bit mono PCM at the speaker's I2S
 * sample rate. No-op / returns 0 on boards without a mic.
 *
 * The mic shares the ES8311 + I2S with the speaker (furi_hal_speaker), so do not
 * record and play at the same time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start capture: clock the codec, configure the ADC, enable the RX channel.
 * Returns false if the board has no mic or init failed. */
bool furi_hal_mic_start(void);

/** Read up to `frames` mono int16 samples into `mono_out` (blocking up to
 * timeout_ms per I2S read). Returns the number of frames actually read. */
size_t furi_hal_mic_read(int16_t* mono_out, size_t frames, uint32_t timeout_ms);

/** Stop capture (disable the RX channel). */
void furi_hal_mic_stop(void);

#ifdef __cplusplus
}
#endif
