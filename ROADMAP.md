# Roadmap

Ordered by risk rather than by convenience. Waking a sleeping host over USB is
the assumption the whole product rests on, and it depends on the host and the
board — not on us. Nothing involving Telegram gets built until it is proven,
because if the host will not wake, every line of it is wasted.

## 1. Prove the host wakes at all — no code

**Done, 2026-07-16.** A plain USB keyboard wakes the target host from sleep, in
any port, with nothing changed in BIOS or Device Manager — the settings were
already permissive by default. No port restriction to design around, so the
device can live wherever it is convenient.

The procedure is kept below, because other hosts will not necessarily be this
accommodating: plug an ordinary USB keyboard into the port the device will
occupy, sleep the machine, press a key. If it does not wake, the settings to
work through are `Wake on USB` / `USB Power in S3/S4` in BIOS/UEFI, `ErP` or
`Deep Sleep` (must be off), and the per-device *Allow this device to wake the
computer* checkbox in Windows Device Manager.

What this establishes: wake over USB is permitted by the host firmware, and the
ports keep power in the sleep state used. What it does not establish is covered
by step 2 — a keyboard and this board are not the same load, and not the same
device asking to wake.

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
- **Does the host sleep in S3, or in Modern Standby (S0ix)?** `powercfg /a`
  answers it. Waking from every port with no BIOS change fits either — a desktop
  that powers all ports in S3, or a machine that never truly leaves S0 — but the
  answer changes what step 2 faces. Under Modern Standby the ports generally
  stay powered and the suspend-current concern largely evaporates; under S3 it is
  live.
