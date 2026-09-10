#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * I2S output to the ES9023 DAC. No control bus on this DAC - it just
 * streams whatever's clocked into it, so this module only ever touches
 * BCLK/WCLK/DATA (see config.h for the pin assignment).
 */

/* Sets up the I2S channel at a sensible default (44.1kHz/16-bit/stereo)
 * and enables it. Call once at startup, before any playback. */
esp_err_t i2s_out_init(void);

/*
 * Reconfigures the I2S clock/slot to match the decoder's actual output
 * format, if different from the current configuration. Cheap to call
 * on every decoded frame - it's a no-op when nothing has changed.
 *
 * This matters more than it looks: HE-AAC's SBR doubles the sample
 * rate relative to the ADTS-signalled core rate (see the codec
 * characterization notes), so the I2S clock has to track the decoded
 * rate, not whatever the stream nominally advertises, or HE-AAC
 * stations play back at half speed.
 */
esp_err_t i2s_out_configure(int sample_rate, int channels, int bits_per_sample);

/*
 * Blocking write of one decoded PCM frame. Naturally paces playback to
 * real time, since the DMA buffer only accepts data as fast as the DAC
 * clocks it out.
 */
esp_err_t i2s_out_write(const uint8_t *pcm, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/*
 * Discards whatever's still queued in the DMA buffer without changing
 * the current clock/slot configuration. Call this once at the start of
 * a new track.
 *
 * Why this matters: the DMA buffer holds ~185ms of audio for jitter
 * tolerance. If nothing gets written for a stretch - e.g. while the new
 * stream's HTTP connect is still in flight - the driver underruns and
 * just keeps looping whatever's still sitting in its last-filled
 * descriptor, which is a leftover chunk of the *previous* track. That's
 * the "old stream repeats a segment" symptom. Pausing/resuming the
 * channel does NOT clear this - only overwriting the buffer's actual
 * contents does, which is what this function does: it blocking-writes
 * enough silence to cover the full DMA capacity, so whatever the
 * hardware might still be looping on becomes zero.
 */
esp_err_t i2s_out_flush(void);
