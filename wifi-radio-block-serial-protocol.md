# WiFi radio block — serial CLI protocol reference

Textual, line-oriented protocol between the front panel (ESP32-S3 + TFT + EC11) and the WiFi radio block (ESP32-S3, WiFi + esp_audio_codec + I2S), modeled on the SMTP/IMAP style of request/reply plus unsolicited push.

## Framing

- All lines are 7-bit ASCII, terminated with CRLF (`\r\n`).
- **Command** (front panel → radio block): `VERB [ARG1] [ARG2] ...\r\n`
- **Reply** (radio block → front panel, solicited — always follows a command):
  - Final/only line: `CCC<sp>text\r\n`
  - Continuation line (more to follow): `CCC-text\r\n`
- **Event** (radio block → front panel, unsolicited): `*CCC text\r\n`
  - The leading `*` never appears on a reply, so the front panel can dispatch on the first byte with no state tracking.

## Response code ranges

| Range | Meaning |
|---|---|
| 2xx | Success |
| 4xx | Client error — bad command, bad argument, out-of-range index |
| 5xx | Server error — SPIFFS write failed, internal fault |
| 6xx | Reserved exclusively for `*`-prefixed async events (never a plain reply) |

## Design decisions

- **WiFi credentials**: single active SSID/password, stored in SPIFFS. No multi-network list.
- **Playlist**: stored in SPIFFS, indexed list of stream URLs with optional friendly names.
- **Status model**: event-driven. The radio block pushes unsolicited `*6xx` events for state changes (connection, playback, faults) rather than requiring the front panel to poll.
- **Front panel concurrency model**: the front panel never blocks waiting on a reply. Every incoming line — solicited reply or unsolicited `*6xx` event alike — lands in the same inbound queue and is serviced reactively by the WiFi Radio Actor. A command is fire-and-forget from the actor's perspective; its eventual reply is just another queued message, handled the same way an unsolicited event is.
- **Failure/reason fields**: every `<reason>` placeholder below is a single-byte numeric status code, not free text (see Reason codes table).
- **`FACTORY CONFIRM`**: requires a confirmation timeout — if `FACTORY CONFIRM` doesn't arrive within the timeout window, the pending factory-reset request lapses and a fresh `FACTORY` must be sent to restart the sequence. (Timeout duration TBD — 10s is a reasonable starting point.)

---

## Commands

### WiFi

| Command | Reply | Events fired |
|---|---|---|
| `WIFI.SET <ssid> <pass>` | `250 stored` / `501 bad syntax` | — |
| `WIFI.CONNECT` | `220 connecting` | `*601 wifi connected <ip>` or `*602 wifi failed <rc>` |
| `WIFI.DISCONNECT` | `250 ok` | `*603 wifi disconnected` |
| `WIFI.STATUS` | `211 <ssid> <ip> <rssi>` | — |

### Playlist (SPIFFS-backed)

| Command | Reply |
|---|---|
| `PL.ADD <url> [name]` | `250 added <index>` |
| `PL.INS <index> <url> [name]` | `250 ok` / `404 bad index` |
| `PL.DEL <index>` | `250 ok` / `404 bad index` |
| `PL.LIST` | `210-<index> <name> <url>` (one line per entry) ... `210 end` |
| `PL.CLEAR` | `250 ok` |
| `PL.RENAME <index> <name>` | `250 ok` / `404 bad index` |
| `PL.SAVE` | `250 ok` / `500 save failed` |

### Playback

| Command | Reply | Events fired |
|---|---|---|
| `PLAY [index]` | `220 accepted` | `*610 track start <index> <name>` → `*611 buffering <pct>` → `*612 playing` (or `*613 track error <rc>`) |
| `STOP` | `250 ok` | `*614 stopped` |
| `PAUSE` | `250 ok` | `*615 paused` |
| `NEXT` | `220 accepted` | same chain as `PLAY` |
| `PREV` | `220 accepted` | same chain as `PLAY` |
| `VOL <0-100>` | `250 ok` | — |
| `VOL?` | `211 <value>` | — |

### Status

| Command | Reply |
|---|---|
| `STATUS` | `211 <state> <index> <codec> <bitrate>` |
| `STATS` | `212-buffer <pct>` / `212-cpu <pct>` / `212-underruns <n>` / `212 uptime <s>` |
| `INFO` | `213-fw <version>` / `213-heap <bytes>` / `213 spiffs_free <bytes>` |
| `STREAMINFO` | `214-name <name>` / `214-genre <genre>` / `214-bitrate <kbps>` / `214 now_playing <title>` |

### ICY (in-stream) metadata event

Fires whenever the station embeds a new `StreamTitle` in the stream (i.e. on a song/segment change), for stations that support ICY metadata:

`*620 now playing <title>`

### Format event

Fires exactly once per track, as soon as the decoder confirms the real stream format - useful for a front end to display without scraping console logs:

`*616 format <profile> <rate>hz <channels>ch <bits>bit`

`<profile>` is a free-text classification such as `MP3`, `AAC-LC (or explicit-signalled HE-AAC)`, or `HE-AAC / HE-AACv1 (SBR)` / `HE-AACv2 (SBR+PS)`.

### System

| Command | Reply |
|---|---|
| `SAVE` | `250 ok` |
| `RESET` | `221 bye` (then reboots) |
| `FACTORY` | `450 confirm required` → `FACTORY CONFIRM` → `250 ok` (then reboots) |

### Fault event

Can fire at any time, unsolicited:

`*690 sys error <rc>` — e.g. SPIFFS write failure, low heap.

### Reason codes (`<rc>`)

Single-byte numeric code, not free text. Placeholder table — exact enum to be finalized:

| Code | Meaning |
|---|---|
| 0 | Unknown / unspecified |
| 1 | Timeout |
| 2 | Auth failure (WiFi) |
| 3 | Not found (stream URL / SSID) |
| 4 | SPIFFS write failure |
| 5 | Low heap / out of memory |
| 6 | Codec/stream decode error |

---

## Open questions for the next design pass

- Final reason code enum (`<rc>` table above is a starting placeholder).
- Exact `FACTORY CONFIRM` timeout duration.
