#pragma once

/*
 * Serial CLI implementing the WiFi Radio Block protocol: CRLF-terminated
 * ASCII lines, SMTP-style numeric-code replies to commands, and
 * `*`-prefixed unsolicited async events in the 6xx range. See the
 * "WiFi radio block - serial CLI protocol reference" doc for the full
 * command/reply/event table this implements.
 *
 * Runs over a dedicated UART1 (external USB-UART bridge, see config.h
 * for pins), kept deliberately separate from the internal-USB console
 * so protocol traffic is never interleaved with ESP_LOGx output.
 * Starts immediately in app_main() and never blocks on WiFi state, per
 * the "front panel never waits" design decision: every reply and event
 * is just a line written when ready, and the reader on the other end
 * services them off a queue.
 */

/* Starts the CLI task. Call once, as early as possible in app_main(). */
void cli_start_task(void);

/*
 * Emits an unsolicited `*CCC text\r\n` event line, e.g.
 * cli_event(610, "track start %d %s", index, name). Thread-safe -
 * callable from the player task while the CLI task is mid-reply.
 */
void cli_event(int code, const char *fmt, ...);
