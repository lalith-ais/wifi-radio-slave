#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * ICY (Shoutcast/Icecast) in-stream metadata handling.
 *
 * When a station supports it, the server interleaves small text blocks
 * (e.g. "StreamTitle='Artist - Song';StreamUrl='...';") directly into
 * the audio byte stream, once every `icy-metaint` bytes of audio - a
 * value only known from the response headers, not fixed. This module
 * strips those blocks back out in-place, so the decoder only ever sees
 * pure audio, and reports the parsed title via a callback whenever a
 * non-empty metadata block completes.
 */

typedef void (*icy_meta_cb_t)(const char *stream_title, void *user_ctx);

typedef struct {
    bool enabled;           /* false when metaint <= 0 - pass-through, no-op */
    int metaint;
    int bytes_until_meta;   /* countdown of audio bytes remaining before the next block */
    int state;              /* internal - 0=audio, 1=reading metadata block data */
    int meta_len;           /* total bytes in the current metadata block */
    int meta_read;          /* bytes of the current block consumed so far */
    char meta_buf[512];     /* enough for any realistic StreamTitle; longer blocks are still
                              * correctly skipped, just truncated in what gets parsed */
    size_t meta_buf_len;
    icy_meta_cb_t cb;
    void *cb_ctx;
} icy_meta_ctx_t;

/* metaint <= 0 means the station didn't advertise ICY metadata support
 * (no `icy-metaint` header) - the context becomes a harmless pass-through. */
void icy_meta_init(icy_meta_ctx_t *ctx, int metaint, icy_meta_cb_t cb, void *cb_ctx);

/*
 * Filters `in_len` raw stream bytes, writing only the audio bytes to
 * `out` (`*out_len` <= in_len always). `out` may safely alias `in` -
 * the compaction is done left-to-right in place. Invokes the callback
 * synchronously for each completed non-empty metadata block.
 */
void icy_meta_process(icy_meta_ctx_t *ctx, const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t *out_len);
