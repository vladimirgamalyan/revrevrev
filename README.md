# RevRevRev

A hardware key on ESP32-S3 that wakes a sleeping computer from Telegram.

The device is plugged into a USB port and presents itself to the host as an
ordinary USB keyboard. On command it emits a single keypress — the same event a
real keyboard produces — and the host wakes from sleep. The command arrives over
Telegram, so the computer can be woken from anywhere with network access, with
no agent software installed on the host and nothing running on it while it
sleeps.

## Name

The name comes from the military wake-up call "Reveille! Reveille! Reveille!" —
traditionally announced three times over a ship's or camp's PA system to rouse
everyone. "Rev" is short for reveille, said three times: RevRevRev. Fitting,
since waking a sleeping host is the device's only job. See
[ADR-0005](docs/adr/0005-rename-to-revrevrev.md).

## Status

Early. Design is settled; no firmware yet. Planned work, ordered by risk, is in
[ROADMAP.md](ROADMAP.md).

## How it works

1. The device connects to WiFi and polls the Telegram Bot API (`getUpdates`)
   over HTTPS.
2. An authorized user sends a wake command to the bot.
3. The device emits a USB HID keypress to the host.
4. The host wakes, exactly as it would from a physical keypress.

Because the host sees a plain USB keyboard, waking works regardless of the
operating system and requires no driver, service, or agent on the host side.

## Hardware

- An ESP32-S3 board with native USB (the S3's USB-OTG peripheral, not a
  USB-to-UART bridge).
- A host USB port that keeps power while the machine sleeps.

## Host requirements

Waking over USB must be permitted by the host — the device cannot force it:

- USB wake enabled in BIOS/UEFI (often named *Wake on USB*, *USB Power in S3/S4*,
  or *ErP* must be disabled).
- Windows: Device Manager → the keyboard device → Power Management → *Allow this
  device to wake the computer*.
- The USB port must retain power in the target sleep state. On many desktops
  only a subset of ports does.

## Design decisions

- [ADR-0001](docs/adr/0001-use-usb-hid-for-wake.md) — USB HID rather than BLE
  HID, because Bluetooth wake support is uneven across hosts.
- [ADR-0002](docs/adr/0002-poll-telegram-directly.md) — the device polls Telegram
  directly rather than through a relay server, trading token exposure for having
  no server to run.
- [ADR-0003](docs/adr/0003-use-esp-idf-for-firmware.md) — ESP-IDF (C), for
  direct control over TinyUSB, TLS, and NVS.
- [ADR-0004](docs/adr/0004-compile-time-secrets-header.md) — credentials live in
  an uncommitted header for now; runtime provisioning is the intended successor.
- [ADR-0005](docs/adr/0005-rename-to-revrevrev.md) — renamed from WakeKey to
  RevRevRev.
- [ADR-0006](docs/adr/0006-stay-on-esp32s3-wifi.md) — stay on ESP32-S3 with
  WiFi; the ~20 mA idle draw is not worth switching to a lower-power radio.

Further decisions that shape structure or carry long-lived tradeoffs go in
[`docs/adr/`](docs/adr/README.md).

## Security

Treat the device as physically trusted hardware:

- The WiFi credentials and the Telegram bot token are compiled into the firmware
  ([ADR-0004](docs/adr/0004-compile-time-secrets-header.md)). Anyone holding a
  build of it, or able to dump the flash, can read them — so firmware images
  must not be published or built in CI while this holds.
- The bot must accept commands only from an allowlist of chat IDs. A Telegram
  bot is reachable by anyone who knows its username, so an unrestricted bot is
  an open wake button.
- The device is a keyboard from the host's point of view. That is the same trust
  boundary as leaving a keyboard plugged in.
