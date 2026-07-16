# Roadmap

Ordered by risk rather than by convenience. Waking a sleeping host over USB is
the assumption the whole product rests on, and it depends on the host and the
board — not on us. Nothing involving Telegram gets built until it is proven,
because if the host will not wake, every line of it is wasted.

## 1. Prove the host wakes at all — no code

Plug an ordinary USB keyboard into the port the device will occupy, sleep the
machine, press a key.

If it does not wake, the settings to work through are `Wake on USB` / `USB Power
in S3/S4` in BIOS/UEFI, `ErP` or `Deep Sleep` (must be off), and the per-device
*Allow this device to wake the computer* checkbox in Windows Device Manager.

Answers three things at once: whether wake is permitted in firmware, whether the
port keeps power in the target sleep state, and which port qualifies — on many
desktops only a subset does.

**Done when:** a known-good port is identified and the premise is confirmed.

## 2. Firmware spike — wake over USB HID

An ESP32-S3 that enumerates as an HID keyboard and triggers a wake on a BOOT
button press. No WiFi, no TLS, no Telegram.

A real keyboard and this board fail in different ways, so step 1 does not cover
this:

- **Remote wakeup is its own mechanism, not just HID.** The configuration
  descriptor must carry the remote wakeup attribute, the host must enable it
  (observable in `tud_suspend_cb`), and the wake must go through
  `tud_remote_wakeup()` — the bus is suspended, so an ordinary HID report will
  not do.
- **Suspend current.** USB budgets a suspended device roughly 2.5 mA. An
  ESP32-S3 with WiFi up draws orders of magnitude more, so the design is
  knowingly out of spec. Most hosts ignore this; some cut port power or report
  over-current. Only the actual hardware can answer which kind we have.

**Done when:** this board wakes this host from sleep.

## 3. Telegram command path

WiFi, then HTTPS long polling of `getUpdates` ([ADR-0002](docs/adr/0002-poll-telegram-directly.md)).

The chat ID allowlist belongs here and is not optional — the bot is reachable by
anyone who knows its username, so the allowlist is the only access control there
is. Credentials go behind a single narrow accessor, per
[ADR-0004](docs/adr/0004-compile-time-secrets-header.md).

**Done when:** a message from an authorized chat wakes the machine, and one from
any other chat does not.

## 4. Runtime provisioning

Replaces the compile-time header with SoftAP/BLE provisioning, superseding
[ADR-0004](docs/adr/0004-compile-time-secrets-header.md) via a new ADR.

Until this lands, credentials are compiled into the firmware, which means
firmware images must never be published or built in CI. The repository is
public, so that constraint is live and easy to trip over — which is the argument
for not deferring this indefinitely.

## Open questions

- Which ESP32-S3 module.
- Which host is the target.
