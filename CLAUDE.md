# CLAUDE.md

Guidance for Claude Code (and other coding agents) in this repository: Meta's
Muse Gadget SDK, forked to run on three Waveshare ESP32-S3 boards with spoken
replies, pushes, touch volume and battery reading. Humans: start with
`README.md`.

The SDK's own agent guide still applies — read it for the build system, board
contract and conventions: @esp32/AGENTS.md. Adding a board: @esp32/devices/AGENTS.md.

## Rules that matter most

1. **Secrets never leave `secrets/`.** The Muse SDK token (`mgst_…`) and the
   ElevenLabs key (`sk_…`) are one-line files in `secrets/` (gitignored).
   - Never print them, paste them into chat, write them into tracked files or
     put them on a command line that gets echoed. Read them from the file inside
     a script and report only "set" / "missing" and the length or prefix.
   - `esp32/tools/muse/board.sh build` injects them through
     `tools/muse/secrets.py`. Don't edit `build-*/sdkconfig` by hand unless you
     must, and then without echoing the value.
   - Every `build-*/` directory and every flashed image contains them: never
     commit, upload or attach either.
   - The firmware itself logs a truncated token (`SDK token: mgst_XXXX…`).
     Mask `(mgst|sk)_[A-Za-z0-9_-]+` when you quote logs.
   - **Before any push**, scan the files to be committed **and** `git log -p
     upstream/main..HEAD` for the real values (whole and in pieces) and for
     `mgst_[A-Za-z0-9]{12,}` / `sk_[a-f0-9]{24,}`. The only expected hit is the
     fake `mgst_AAAA…` in `esp32/tests/link_pairing_handshake_harness.c`.
2. **Back up before the first flash.** `python -m esptool -p PORT read-flash 0
   0x1000000 backup/<board>-<mac>-factory.bin` (0x800000 for 8 MB parts).
   `backup/` is not in this repo; keep dumps out of git (vendor firmware).
3. **Identify the board; don't assume.** A wrong profile writes the wrong pin
   map. See "Identify the board" below.
4. **Don't delete or overwrite** the user's files, backups or branches without
   asking. `erase-flash` wipes pairing and Wi-Fi: only on first install or when
   asked.

## Boards

| Alias | Board | Chip / flash / PSRAM | Panel · touch | Audio | Power | Buttons |
|---|---|---|---|---|---|---|
| `s185c` | Waveshare ESP32-S3-Touch-LCD-1.85C | S3 QFN56 · 16 MB · 8 MB octal | ST77916 360×360 round (QSPI) · CST816 0x15 | ES8311 + amp GPIO15, ES7210 | no PMU; battery ADC GPIO8 ×3 | BOOT = talk; RST; slide switch cuts battery |
| `s143c` | Waveshare ESP32-S3-Touch-AMOLED-1.43C | S3-PICO-1-N8R8 · 8 MB · 8 MB octal | SH8601 466×466 round, mounted 180° · FT-style 0x15 | ES8311 + NS4150B GPIO46, ES7210, LDO EN GPIO18 | no PMU; battery ADC GPIO4 ×2, charge GPIO7 (low = charging) | BOOT = talk; PWR is a power latch |
| `s18` | Waveshare ESP32-S3-Touch-AMOLED-1.8 | S3 QFN56 · 16 MB · 8 MB octal | SH8601 or CO5300 368×448 · FT3168 0x38 or CST816 0x15 | ES8311 (speaker + mic), amp GPIO46 | AXP2101 0x34 | BOOT = talk; PWR = aux via PMU |

Board files: `esp32/components/muse/boards/board_waveshare_s3_{185c,143c,18}.c`.
Overlays: `esp32/devices/sdkconfig.muse-waveshare-s3-{185c,143c,18}`. All three
drive `esp_lcd` directly (no vendor BSP) with `esp_lv_adapter` +
`muse_lcd_bands`, and re-enable the watchdogs that `sdkconfig.defaults` turns off.

## Identify the board

All three enumerate as Espressif `303a:1001` "USB JTAG/serial debug unit"
(`/dev/cu.usbmodem*` on macOS). The model isn't in the descriptor, but the
**MAC is its USB serial number**: `ioreg -p IOUSB -l -w0 | grep '"USB Serial Number"'`
tells boards apart **without touching them**.

**Never poll esptool to watch for a board.** Every `esptool` call resets the
chip; a loop calling `read-mac` every few seconds boot-loops whatever is
plugged in. Use the USB serial number (or `tools/muse/ports.py`) to wait for a
board, and run esptool once you've decided to act.

To find the model:

1. Running this firmware: the boot log says `muse: board: <name>`, and
   `python3 esp32/tools/muse/chat.py --status` returns `"board"`.
2. `python -m esptool -p PORT chip-id` and `flash-id`: `ESP32-S3-PICO-1` with
   8 MB embedded flash is the 1.43C; QFN56 with 16 MB is the 1.85C or the 1.8.
3. Vendor firmware: back it up, then `strings backup.bin | grep -iE
   "board|1\.85|1\.43|amoled|st77916|sh8601|cst816|ft5x06|axp2101"`. The 1.85C's
   demo names `ST77916`/`CST816` and `ES7210`; the 1.43C's says `Board:
   S3_AMOLED_1_43C`; the 1.8 often ships xiaozhi (`esp32-s3-touch-amoled-1.8`).

Say which board you found and what told you before flashing.

## Build, flash, monitor

```sh
# once: ESP-IDF v6.0.1 at ~/esp/esp-idf-v6 (board.sh finds it), cmake, ninja
cd esp32
tools/muse/board.sh build s185c        # → build-muse-waveshare-s3-185c/, log /tmp/muse_build_s185c.log
tools/muse/board.sh flash s185c        # finds the port by USB device
```

- `board.sh build` wipes and refetches `managed_components/` each time, so run
  builds **one at a time**. For parallel work, use separate copies of the repo.
- Flash keeps NVS (pairing, Wi-Fi). Fresh install: `python -m esptool -p PORT
  erase-flash` first.
- Errors: `grep -E "error:|warning:" /tmp/muse_build_<board>.log`. Size: the
  `check_sizes` line (4 MB slots; 3.9 MB on the 8 MB 1.43C).
- Host tests need `managed_components/`, which `board.sh` deletes. Run
  `idf.py ... reconfigure` for one board, then `python3 -m unittest discover -s
  tests -p 'test_*.py'` (156 tests), then delete `managed_components/` and
  `dependencies.lock` again.

**Logs without resetting the board** (an agent has no TTY for `idf.py monitor`;
opening the port normally can reset it):

```python
import os, termios, time, re
fd = os.open(PORT, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[2] &= ~termios.HUPCL; termios.tcsetattr(fd, termios.TCSANOW, a)
# read for N seconds, strip ANSI with re.sub(r'\x1b\[[0-9;]*m', '', text), mask secrets
```

`tools/muse/monitor.py PORT SECS` captures from boot (it resets). A healthy
boot shows `muse: board: …`, `UI up: WxH`, `audio ready`, a mic `self-test`
line and `advertising as MuseGadget-XXXXXX` (unpaired) or `setup=done wifi=up
ws=up` (paired).

The serial console accepts bench keys: `z`/`w` sleep/wake the screen, `m` plays
a test MP3, `d`/`u` press/release talk, and `>status\n` prints a JSON status
(mask the tokens in it).

## Pairing

Muse app › Settings › Devices › Developer mode on › **+** › `MuseGadget-XXXXXX`
(shown on screen) › press **BOOT** when asked › send Wi-Fi from the app. Holding
BOOT 5 s resets setup.

## Features added in this fork

- **Spoken replies:** `components/muse/muse_chat_session.cpp`.
  - `start_tts` → `tts_start` queues the text to the `muse_tts` task.
  - `tts_fetch` POSTs to ElevenLabs `…/stream?output_format=mp3_22050_32` with `esp_http_client`.
  - `tts_pump` feeds `tts_data()` on the session task, and `decode()` resamples to 16 kHz.
  - Kconfig: `MUSE_ELEVENLABS_API_KEY`, `_VOICE_ID`, `_MODEL` (`eleven_flash_v2_5`).
  - Needs `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`: the ElevenLabs chain ends at a GlobalSign root that isn't in IDF's bundle.
  - Upstream removed server TTS (PR #12), so without a key replies are text.
- **All messages (pushes):** off by default; Settings › All messages (`muse_settings_pushes_on`, NVS key `pushes`). When on, `on_event` → `push_begin`.
  - Assistant messages that arrive with no turn pending open a reply-only turn.
  - `muse_voice.c` `play_push` plays it.
  - IDs already shown are remembered (`s_shown_ids`).
  - The idle disconnect is skipped and a closed session reconnects. Off behaves like upstream.
- **Touch volume:** `muse_ui.c` `volume_drag`.
  - A vertical drag on the face tile; horizontal stays the settings swipe.
  - The level is saved on release.
- **Avatar reactions:** `muse_pose_t`'s `dizzy`, `sleepy`, `waking` and `tickle` (`muse_pixel.h`), timed in `muse_state.c`.
  - Dizzy: shaking. `muse_imu.c` (QMI8658, probed at 0x6B/0x6A from each board's `init()`) feeds `muse_input.c` `check_shake`.
  - Sleepy: `check_sleep` drowses for `MUSE_SLEEPY_S` before auto-sleep, and plays two soft snores (`muse_voice_request_snore`).
  - Waking: plays when the screen wakes, or when drowsing is interrupted.
  - Tickle: rubbing (4 reversals in 1 s within 15% of the screen) or 4 quick taps in 1.2 s on the face tile, while idle. `muse_ui.c` `touch_read` wraps the board's touch read to sample every 15 ms; `on_touch` counts taps; `tickle_poll` takes the press from LVGL and undoes a volume drag or a slide to settings. Progress waits below `MUSE_TICKLE_HOLD` while it goes on (`muse_state_tickle_hold`).
  - Jollybot (`avatar/muse_pixel.c`) draws them in this fork (`react_setup` and what it drives). Those additions, like Jollybot, are **not** under the Apache License: keep Meta's copyright line and the notice under it, never add an Apache header, and keep them in their own commit so they can be removed on Meta's request.
- **Battery:** each board's `read_power`.
  - The 1.85C infers USB from voltage trends, because it has no VBUS or charge pin.
  - Errs toward `usb = true`: on battery the firmware dozes Wi-Fi and closes idle sessions.

## Troubleshooting

- **Port missing or "Device not configured"**: check the cable before the firmware. Charge-only USB-C cables and docks/hubs dropping out look exactly like a hang. `system_profiler SPUSBHostDataType` shows what macOS really sees.
- **Screen frozen, firmware still running**: LVGL starving the band sender. This is fixed in `muse_lcd_bands.c` (upstream PR #34, cherry-picked).
- **Board dead until RST**: shouldn't happen any more, because the watchdogs panic with a backtrace. Capture the panic text from the console.
- **"Failed to verify certificate" for ElevenLabs**: the cross-signed option above is off.
- **ElevenLabs 401 `missing_permissions`**: the key lacks Text to Speech.
- **ElevenLabs 404 `voice_not_found` / 400 `voice_not_fine_tuned`**: pick another voice ID. A cloned voice must finish training first.
- **Wrong colours on a new panel**: byte order. **Image shifted**: column gap (`set_gap`). **Touch mirrored**: flip the axes in `tp_read`.

## Hardware gotchas (1.85C)

- Side header `E5`–`E8` are TCA9554 pins, not GPIOs. `19`/`20` are USB D−/D+.
- The header's SDA/SCL are the internal I2C bus (GPIO 11/10), shared with touch 0x15, ES8311 0x18, TCA9554 0x20, ES7210 0x40 and PCF85063 0x51.
- The label lists a QMI8658 IMU, but the V2 schematic doesn't show one. Scan I2C before relying on it.

## Upstream

`origin` is this fork; `upstream` is `facebookincubator/muse-gadget-sdk`. Check
what's new with `git fetch upstream && git log --oneline HEAD..upstream/main`,
and `gh pr list -R facebookincubator/muse-gadget-sdk` for open PRs (PR #34 adds
the AMOLED-1.8 via the vendor BSP; ours probes both revisions directly). Keep
upstream conventions: Apache headers, "Say Muse, never Hatch", one overlay per
board, docs updated in `esp32/README.md`, `esp32/AGENTS.md` and `esp32/devices/`.
