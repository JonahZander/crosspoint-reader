---
name: flash-device
description: Build and flash CrossPoint firmware to the Xteink X4 e-reader over USB, monitor serial output, and update the SD card. Use when the user asks to flash, upload, deploy firmware, or copy files to the reader's SD card.
---

# Flashing the Xteink X4

## Prerequisites

- PlatformIO CLI (`pio`). If missing, install into a venv in the session
  scratchpad (never into the repo):

  ```bash
  python3 -m venv "$SCRATCHPAD/pio-venv" && "$SCRATCHPAD/pio-venv/bin/pip" install platformio
  ```

- The reader shows up as `/dev/ttyACM0` (Linux). **It drops off USB when it
  goes to deep sleep** — if the port is missing, ask the user to wake the
  device (press a button) or replug it. Never assume it's connected;
  check first:

  ```bash
  ls /dev/ttyACM*
  ```

## Build and flash

```bash
pio run -e default                                     # build only
pio run -e default -t upload --upload-port /dev/ttyACM0  # build + flash (~1 min)
```

Environments (`platformio.ini`): `default` = development (serial logging),
`gh_release` = production, `gh_release_rc` = release candidate. Flash
`default` during development.

Sanity gates before flashing: host tests green
(`cmake -S test -B build/test && cmake --build build/test -j && ctest --test-dir build/test -j`),
build reports RAM well under the 380KB ceiling (healthy: ~31%).

## Serial monitoring

```bash
python3 scripts/debugging_monitor.py   # color-coded, preferred
pio device monitor                     # plain fallback
```

USB CDC is on from boot (`ARDUINO_USB_CDC_ON_BOOT=1`), so early logs are
visible. A monitor session holds the port — close it before uploading.

## SD card updates

The SD card mounts as a USB block device (`/dev/mmcblk0p1`) when inserted
in the host's reader (not through the e-reader):

```bash
udisksctl mount -b /dev/mmcblk0p1     # → /run/media/<user>/XXXX-XXXX
# ... copy files (books to /, tactics levels to /tactics/) ...
sync && udisksctl unmount -b /dev/mmcblk0p1
```

Rules:
- **Never touch the user's books** (`*.epub` in the root) unless asked.
- `.crosspoint/` holds caches, saves and reading progress — deleting it
  loses progress; only clear it to force a re-parse after EPUB-pipeline
  changes.
- Always `sync` + unmount before telling the user to pull the card.

## Troubleshooting

- **No `/dev/ttyACM0`**: device asleep or unplugged — wake/replug. Do not
  retry-loop; report and wait for the user.
- **Permission denied on port**: user needs to be in the `dialout`/`uucp`
  group (distro-dependent) — suggest, don't fix silently.
- **Upload succeeds but behavior unchanged**: confirm the right env was
  flashed and that the change isn't in SD-card data that also needs
  copying.
- **i18n build error (`bad format`)**: `lib/I18n/translations/*.yaml`
  lines must be exactly `KEY: "value"` — no trailing comments.
