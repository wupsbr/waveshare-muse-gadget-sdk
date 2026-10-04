---
name: flash-muse-board
description: Identify a connected Waveshare ESP32-S3 board, back up its factory firmware, build Muse for it with the user's secrets and flash it, then check the boot log. Use when the user says "flash my board", "put Muse on this board", "I plugged in a board", or asks which board is connected.
---

# Flash Muse onto a Waveshare board

Follow CLAUDE.md's rules throughout: never print the SDK token or API keys,
and mask `(mgst|sk)_…` in any log you quote.

## 1. Find the board

```sh
ls /dev/cu.usbmodem*        # none? check the cable (charge-only cables are common) and skip docks/hubs
```

If no port appears, run `system_profiler SPUSBHostDataType`. When nothing
shows on the bus, it's the cable or the connection, not the firmware. Ask
the user to try a data cable straight into the computer.

For each port:

```sh
python -m esptool -p PORT chip-id     # chip, PSRAM, MAC
python -m esptool -p PORT flash-id    # flash size
```

## 2. Back it up (first time only)

If `backup/` has no dump with this MAC, read the whole flash before writing
anything:

```sh
mkdir -p ../backup
python -m esptool -p PORT read-flash 0 0x1000000 ../backup/board-<mac>-factory.bin   # 0x800000 for 8 MB
```

Use the default baud: higher rates fail on these boards' USB-Serial/JTAG.

## 3. Identify the model

Use CLAUDE.md › "Identify the board". Firmware already running Muse names
itself in its boot log. For vendor firmware, run `strings` on the dump. Tell the
user which board it is and what told you. Map it to an alias:

| Board | Alias |
|---|---|
| ESP32-S3-Touch-LCD-1.85C | `s185c` |
| ESP32-S3-Touch-AMOLED-1.43C | `s143c` |
| ESP32-S3-Touch-AMOLED-1.8 | `s18` |
| anything else | check `esp32/AGENTS.md`'s board table; if it isn't there, it needs a port (`esp32/devices/AGENTS.md`) |

## 4. Check the secrets

Look in `secrets/` for `muse_sdk_token` (required) and `elevenlabs_api_key`
(optional; without it replies are text only). Report only whether each is
present, plus its prefix and length. If the token is missing, ask the user to
create it from `secrets/muse_sdk_token.example`.

## 5. Build and flash

```sh
cd esp32
tools/muse/board.sh build <alias>                     # prints which secrets it set
grep -E "error:|binary size" /tmp/muse_build_<alias>.log
```

For a first install, or when the user asks for a clean slate,
`python -m esptool -p PORT erase-flash` first. This wipes pairing. Then:

```sh
cd build-muse-<profile> && python -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

Re-check the MAC right before writing, in case the user swapped boards.

## 6. Verify

Capture about 15 s from boot with `python3 tools/muse/monitor.py PORT 15`.
Expect these lines:

- `muse: board: <name>`
- `UI up: WxH`
- `audio ready` and the mic `self-test`
- `advertising as MuseGadget-XXXXXX` (or `setup=done … ws=up` if already paired)

Report any `E (`, `W (`, panic or reboot loop. Then give the user the pairing
steps from CLAUDE.md, with the `MuseGadget-XXXXXX` name you saw.
