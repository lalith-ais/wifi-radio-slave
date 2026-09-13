# WiFi Radio Block — project summary

Status: **production-grade, working end to end on real hardware.** Started as a decode-only codec characterization bench harness (see the earlier AAC/HE-AAC CPU/heap spec sheet), evolved this session into a standalone, drop-in WiFi internet radio subsystem.

## Hardware

- **MCU**: ESP32-S3
- **Audio out**: ES9023 DAC over I2S — DATA=GPIO41, WCLK=GPIO40, BCLK=GPIO39
- **Control**: dedicated UART1 via an external USB-UART bridge — TX=GPIO17, RX=GPIO18, 115200-8-N-1 (kept separate from the internal-USB console, which stays as the flash/log interface only)
- **Flash**: 16MB — custom partition table: `factory` app 3MB, `storage` SPIFFS 1MB, rest unpartitioned

## Firmware architecture

Modular C, ESP-IDF v5.5.2:

| Module | Responsibility |
|---|---|
| `main.c` | Boot orchestration — CLI starts first (never blocks on WiFi), WiFi+player start in a background task |
| `wifi_mgr.c` | STA connect/retry, power-save disabled, periodic RSSI reporting |
| `stream_player.c` | HTTP fetch, redirect/playlist-wrapper following, format sniffing (MP3/AAC), decode loop, decode-error resync/recovery |
| `icy_meta.c` | Strips ICY in-stream metadata (song titles) from the audio stream before decode |
| `i2s_out.c` | I2S output to the DAC, sample-rate reconfiguration (handles HE-AAC's SBR rate-doubling), DMA-buffer silence flush on track switch |
| `cli.c` | Serial command dispatcher, playlist management, all CLI commands |
| `storage.c` | SPIFFS-backed playlist + last-played-station persistence |

## Serial CLI protocol

Textual, SMTP-style framing (locked spec, see the separate protocol reference doc):
- Commands: `VERB [args]\r\n`
- Replies: `CCC text` (final) / `CCC-text` (continuation)
- Async events: `*CCC text` (never confused with a reply — front panel design is fully event-driven, never blocks)

Full command set: WiFi status, playlist management (`PL.ADD/DEL/LIST/CLEAR/RENAME/SAVE`), playback (`PLAY/STOP/NEXT/PREV`), status/stats/info/stream-info, system (`RESET/FACTORY`). Async events cover connection state, playback state, format detection, ICY now-playing, and periodic RSSI.

## Key features delivered this session

- Real-time MP3/AAC-LC/HE-AAC/HE-AACv2 decode → I2S playback, clean and stable (proven over a 900s soak test with zero drift)
- Clean track switching (no audio-tail bleed between stations)
- ICY metadata (song titles, station name/genre/bitrate) surfaced live over the CLI
- SPIFFS persistence: playlist survives reboot (`PL.SAVE`), last-played station auto-resumes at boot with zero commands needed
- Resilient decode-error handling (tolerates and resyncs from transient bad frames instead of killing the track)
- Live RSSI reporting for a future signal-strength UI element

## Notable bugs found & fixed (real debugging wins, not guesses)

1. **WiFi power-save** causing periodic audio crackle — fixed by disabling modem sleep.
2. **I2S DMA buffer only pausing, not clearing**, on track switch — caused old-track audio to repeat; fixed by overwriting the buffer with silence.
3. **`esp_http_client_get_header()` silently failing** in the open/fetch-headers/read usage pattern — caused ICY metadata to leak unfiltered into the decoder, corrupting playback. Found via direct evidence (a raw-header event dump) after ruling out RSSI/timing as red herrings. Fixed by capturing headers directly in the HTTP event callback instead.
4. **Stack overflow** from ~9.7KB playlist-copy arrays declared on-stack instead of `static`, only surfaced once a real (non-empty) playlist was saved.
5. **Stale `current_index`** causing NEXT/PREV to get "stuck" on rapid presses — fixed by updating it synchronously at track-request time instead of waiting for the periodic status snapshot.

## Not yet done

- WiFi credential persistence (`WIFI.SET` is still a stub — SSID/password remain Kconfig-only)
- Volume control / `PAUSE` (no I2S mute/gain path yet)
- `FACTORY` reset doesn't yet wipe anything real
- CLI/transport-framing/domain layering for cross-project reuse (discussed, not started)

## Next step

Build the LVGL front-panel UI (separate board/thread) — a WiFi Radio Actor on the front panel that speaks this exact serial protocol, mirroring how the existing T4B DAB module is already integrated.
