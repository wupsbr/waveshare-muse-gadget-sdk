# What this fork adds to Meta's SDK

Baseline: [`facebookincubator/muse-gadget-sdk`](https://github.com/facebookincubator/muse-gadget-sdk)
at `693cde9` ("Support the reTerminal E1002's six-colour e-paper", #35). Everything
below is on top of it. To see it all: `git diff upstream/main`.

The Linux SDK (`linux/`) and the skills (`skills/`) are unchanged.

## At a glance

| # | Change | Type | Default | Files |
|---|---|---|---|---|
| 1 | Waveshare ESP32-S3-Touch-LCD-1.85C | New board | — | `boards/board_waveshare_s3_185c.c`, `devices/sdkconfig.muse-waveshare-s3-185c` |
| 2 | Waveshare ESP32-S3-Touch-AMOLED-1.43C | New board | — | `boards/board_waveshare_s3_143c.c`, `devices/sdkconfig.muse-waveshare-s3-143c` |
| 3 | Waveshare ESP32-S3-Touch-AMOLED-1.8 | New board | — | `boards/board_waveshare_s3_18.c`, `devices/sdkconfig.muse-waveshare-s3-18` |
| 4 | Spoken replies (ElevenLabs) | Feature | On when a key is set | `muse_chat_session.cpp`, `Kconfig` |
| 5 | All messages (pushes) | Feature + setting | **Off** | `muse_chat_session.cpp`, `muse_voice.c`, `muse_chat.h`, `muse_settings.*`, `muse_settings_ui.c` |
| 6 | Touch-drag volume | Feature | On (touch boards) | `muse_ui.c`, `muse_settings_ui.c` |
| 7 | Battery without a PMU | Feature | On (1.85C, 1.43C) | the two board files |
| 8 | Display freeze fix | Fix (from upstream PR #34) | — | `boards/muse_lcd_bands.c` |
| 9 | Watchdogs back on | Robustness | On (these boards) | the three overlays |
| 10 | Cross-signed TLS chains | Fix for #4 | On (these boards) | the three overlays |
| 11 | Secrets outside git | Tooling | — | `secrets/`, `tools/muse/secrets.py`, `tools/muse/board.sh`, `.gitignore` |
| 12 | Claude Code support | Tooling | — | `CLAUDE.md`, `.claude/skills/flash-muse-board/` |

## 1–3. Three Waveshare boards with the full UI

All three use the same approach, with no vendor BSP:
- They drive `esp_lcd` directly, with `esp_lv_adapter` and `muse_lcd_bands`.
- They build the `esp_codec_dev` devices by hand.
- They register the same way upstream boards do:
  - `Kconfig` (`MUSE_BOARD_WAVESHARE_S3_{185C,143C,18}`)
  - `CMakeLists.txt`
  - `idf_component.yml` (panel drivers from the registry)
  - `tools/muse/board.sh` (aliases `s185c`, `s143c`, `s18`)
  - `ports.py` and `avatar.py`
  - the docs tables

| | 1.85C | 1.43C | 1.8 |
|---|---|---|---|
| Panel | ST77916 360×360. There are two revisions: register `0x04 = 00 02 7f 7f` selects Waveshare's vendor init. | SH8601 466×466, mounted 180° (`MADCTL 0xC0`), column gap 8 | SH8601 (original) or CO5300 (V2) 368×448 |
| Touch | CST816 at 0x15, auto-sleep turned off | FocalTech-style registers at 0x15, both axes flipped | FT3168 at 0x38 or CST816 at 0x15. The board file probes for one to pick the revision. |
| Resets | TCA9554 EXIO1 (touch), EXIO2 (panel) | GPIO13 (panel), GPIO16 (touch) | TCA9554 P0–P2, which also switch the panel's power |
| Audio | ES8311 (DAC) + amp on GPIO15, ES7210 (2 mics) | ES8311 + NS4150B on GPIO46, ES7210, LDO enable on GPIO18 | ES8311 for both directions, amp on GPIO46 |
| Power | ADC on GPIO8 ×3 | ADC on GPIO4 ×2, charge status on GPIO7 | AXP2101 (`muse_pmu`) |
| Buttons | BOOT = talk | BOOT = talk | BOOT = talk, PWR = aux (via the PMU) |
| Power off | Deep sleep, BOOT wakes | Deep sleep, BOOT wakes | `muse_pmu_power_off` |
| Flash | 16 MB | 8 MB (`partitions_muse_8mb.csv`) | 16 MB |

Upstream has an open PR for the 1.8 (#34) that uses Waveshare's BSP. Ours
drives both hardware revisions directly, because BSP 2.0.3 only sets up the
CO5300 panel.

## 4. Spoken replies

Upstream asks for text-only replies, since it dropped the server's TTS in
#12. Its `start_tts` is a documented hook that only paces captions with
silence. This fork fills that hook in:

- **Fetching.** A `muse_tts` task (stack in PSRAM) POSTs each reply's text to
  ElevenLabs' streaming endpoint (`mp3_22050_32`) with `esp_http_client` and
  the certificate bundle.
- **Handing it to the session task.** The MP3 goes through a 64 KB PSRAM stream
  buffer. `tts_pump` feeds it to the session task's `tts_data()`, so the turn's
  MP3 state stays on one task.
- **Playback.** The existing `decode()` resamples the MP3 to 16 kHz. Captions
  follow the speech.
- **Cancelling.** Ending or cancelling a turn bumps a job counter, and the fetch
  stops.
- **Failure.** If no speech has played yet, a failure falls back to text at
  reading pace.
- **Speaker setting.** With Speaker off, replies stay text.
- **Kconfig.** `MUSE_ELEVENLABS_API_KEY` (empty disables the feature),
  `MUSE_ELEVENLABS_VOICE_ID` (default Sarah) and `MUSE_ELEVENLABS_MODEL`
  (default `eleven_flash_v2_5`).
- **Latency.** Measured: first audio about 1.0–1.2 s after the text arrives.

## 5. All messages (pushes) — off by default

Upstream drops every subscription event unless a push-to-talk turn is waiting
for a reply. It also closes the session after 10 min idle and reopens it only
on the next press. So messages that Muse sends first, or replies to something
typed in the app, never reach the gadget.

With **Settings › Sound › All messages** on:
- **Showing them.** An assistant message that arrives with no turn pending
  opens a reply-only turn (`push_begin`). The voice task wakes the screen and
  plays it like any reply (`play_push`), spoken if #4 is on.
- **No duplicates.** The IDs of the last messages shown are remembered, so a
  turn's late `message.assistant` isn't played twice.
- **Talk button first.** A press of the talk button cancels a pending push.
  Pushes that arrive while Muse is listening or speaking stay in the app.
- **Staying connected.** The session no longer closes when idle, on battery
  too. If it was already closed, it reconnects.

**Off (the default)** behaves like upstream: only answers to the talk button,
and the idle disconnect stays, which saves battery. The setting is stored in
NVS as `pushes`.

## 6. Touch-drag volume

- **The gesture.** A clearly vertical drag on the avatar tile (|dy| > 2·|dx|,
  with steps of 5% of the screen height) changes the volume by ±5.
- **What still works.** Horizontal swipes still open settings. The touch that
  wakes the screen, the speaker button, pairing cards and images are excluded.
  `lv_indev_wait_release` keeps the drag from also tapping things.
- **What it shows.** A cyan ring on round screens, or a bar on rectangular
  ones, that fades 2 s after the last change. It turns violet when Speaker is
  off.
- **Saving.** The level is saved on release, as the settings slider does. The
  slider now follows changes made elsewhere: a drag, or BLE.

## 7. Battery without a PMU

The upstream Battery page already exists. It just needs `read_power`.

- **1.85C.** The ADC reads GPIO8 through a 200k/100k divider. With no charge
  or VBUS pin, it infers USB from voltage trends.
- **1.43C.** The ADC reads GPIO4 through a 200k/200k divider. The ETA6098
  `STAT` pin on GPIO7 is low while charging.
- **Both.**
  - They use the LiPo curve upstream uses for the StickS3.
  - They err toward `usb = true`, because on battery the firmware dozes Wi-Fi.
  - A reading below 2.5 V means no battery.

## 8. Display freeze fix

This is cherry-picked from upstream PR #34 with its author kept. LVGL's flush
wait used to spin while it held priority-inherited mutexes. That starved the
`lcd_send` task, and the screen froze about 20 min after boot. The fix is a
`flush_wait_cb` that blocks on a semaphore instead. It's shared by every board
that uses `muse_lcd_bands`.

## 9–10. Overlay settings for these boards

- **Watchdogs on.** `sdkconfig.defaults` turns them off. This fork sets
  `ESP_INT_WDT`, and `ESP_TASK_WDT` with panic and a 20 s timeout. A stuck task
  then panics with a backtrace and reboots, instead of hanging until RST.
- **`MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`.** `api.elevenlabs.io`
  presents GTS Root R1 cross-signed by GlobalSign Root CA, which IDF's bundle
  no longer carries.

## 11. Secrets outside git

- **`secrets/`.** It is gitignored, except for its README and the `*.example`
  templates. It holds `muse_sdk_token`, `elevenlabs_api_key` and
  `elevenlabs_voice_id`.
- **`tools/muse/secrets.py`.** It writes them into a build's generated
  `sdkconfig`, from the environment or from the files. It prints only which
  ones it set.
- **`board.sh build`.** It calls the script before every build.

## 12. Claude Code support

- **`CLAUDE.md`** covers the boards, the flow, the secret rules, how to read
  logs without resetting the board, troubleshooting and the hardware traps.
- **The `flash-muse-board` skill** goes from a plugged-in board to a verified
  boot.

## Not changed

- The pairing protocol, the Noise session, the home-network tunnel and OTA.
- Every other board. The only exception is #8, which applies to every board
  that uses `muse_lcd_bands`.
- `linux/` and `skills/`.
