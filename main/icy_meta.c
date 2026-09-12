#include <string.h>
#include "icy_meta.h"

enum { ICY_STATE_AUDIO = 0, ICY_STATE_META_DATA = 1 };

void icy_meta_init(icy_meta_ctx_t *ctx, int metaint, icy_meta_cb_t cb, void *cb_ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->enabled = (metaint > 0);
    ctx->metaint = metaint;
    ctx->bytes_until_meta = metaint;
    ctx->state = ICY_STATE_AUDIO;
    ctx->cb = cb;
    ctx->cb_ctx = cb_ctx;
}

/* Pulls "StreamTitle='...';" out of the raw metadata block text (which
 * also typically contains StreamUrl and padding nulls) and invokes the
 * callback with just the title. */
static void handle_completed_block(icy_meta_ctx_t *ctx)
{
    if (ctx->meta_buf_len == 0 || ctx->cb == NULL) {
        return;
    }
    ctx->meta_buf[ctx->meta_buf_len < sizeof(ctx->meta_buf) ? ctx->meta_buf_len : sizeof(ctx->meta_buf) - 1] = '\0';

    const char *key = "StreamTitle='";
    char *start = strstr(ctx->meta_buf, key);
    if (start == NULL) {
        return;
    }
    start += strlen(key);
    char *end = strstr(start, "';");
    if (end == NULL) {
        end = strchr(start, '\''); /* tolerate a missing trailing semicolon */
    }
    if (end == NULL || end == start) {
        return;
    }

    char title[256];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(title)) {
        len = sizeof(title) - 1;
    }
    memcpy(title, start, len);
    title[len] = '\0';
    ctx->cb(title, ctx->cb_ctx);
}

void icy_meta_process(icy_meta_ctx_t *ctx, const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t *out_len)
{
    if (!ctx->enabled) {
        if (out != in) {
            memmove(out, in, in_len);
        }
        *out_len = in_len;
        return;
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t b = in[i];

        switch (ctx->state) {
        case ICY_STATE_AUDIO:
            if (ctx->bytes_until_meta > 0) {
                out[out_idx++] = b; /* safe in-place: out_idx <= i always */
                ctx->bytes_until_meta--;
            } else {
                /* This byte is the length byte, not audio - length is
                 * in units of 16 bytes, per the ICY spec. */
                ctx->meta_len = (int)b * 16;
                ctx->meta_read = 0;
                ctx->meta_buf_len = 0;
                ctx->state = (ctx->meta_len > 0) ? ICY_STATE_META_DATA : ICY_STATE_AUDIO;
                if (ctx->meta_len == 0) {
                    ctx->bytes_until_meta = ctx->metaint;
                }
            }
            break;

        case ICY_STATE_META_DATA:
            if (ctx->meta_buf_len < sizeof(ctx->meta_buf) - 1) {
                ctx->meta_buf[ctx->meta_buf_len++] = (char)b;
            }
            ctx->meta_read++;
            if (ctx->meta_read >= ctx->meta_len) {
                handle_completed_block(ctx);
                ctx->state = ICY_STATE_AUDIO;
                ctx->bytes_until_meta = ctx->metaint;
            }
            break;

        default:
            ctx->state = ICY_STATE_AUDIO;
            ctx->bytes_until_meta = ctx->metaint;
            break;
        }
    }

    *out_len = out_idx;
}
