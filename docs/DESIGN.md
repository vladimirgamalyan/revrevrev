# RevRevRev — Design and Technical Details

How the firmware works, in depth. For the short version see the
[README](../README.md); for the reasoning behind specific choices see the
[ADRs](adr/); for what is done and what is next see the [ROADMAP](../ROADMAP.md).

RevRevRev is ESP-IDF (C) firmware for the ESP32-S3. The device is plugged into a
USB port of the host it wakes, presents itself as a USB HID keyboard, and emits a
keypress when an authorized Telegram command arrives — so the host wakes exactly
as it would from a physical key, with no host-side software.

## Firmware architecture

Three concurrent contexts run:

- **The main task** (`app_main` in `main/main.c`) initializes USB, WiFi, and the
  LED, then loops every 20 ms polling the BOOT button and a wake flag. It owns
  **all** USB HID calls.
- **The Telegram task** (`main/telegram.c`) long-polls the Bot API over HTTPS and,
  on an authorized `/wake`, sets an atomic flag.
- **The default event-loop task** handles WiFi/IP events and drives the status
  LED.

The Telegram task never calls into USB. It hands the request off through atomic
flags that the main loop drains via `telegram_take_wake_request()`, so every HID
report is issued from one task and no locking around TinyUSB is needed. Once the
main task sees the host resume it signals back through
`telegram_notify_wake_confirmed()`, and the Telegram task sends the confirmation:

```
Telegram task                        main task
-------------                        ---------
/wake received
  store(flag, chat_id) ----------->  telegram_take_wake_request()
                                       trigger_wake()  --> USB HID
                                       bus resumes = host woke
  send "Host woke up."  <---------  telegram_notify_wake_confirmed()
```

## USB HID wake

The device enumerates as a single HID keyboard interface via TinyUSB
(VID `0x303A`, PID `0x4004`). Two details make the wake work:

- **Remote-wakeup attribute.** The configuration descriptor sets
  `TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP`. Without it the host has no way to know the
  device is allowed to signal a wake while the bus is suspended.
- **Two wake paths**, chosen in `trigger_wake()` by bus state:
  - *Bus suspended* (host asleep): `tud_remote_wakeup()`. This is a no-op unless
    the host enabled remote wakeup, which is observable in `tud_suspend_cb`.
  - *Bus mounted and awake*: a keypress via `tud_hid_keyboard_report()`. This
    covers waking a display from DPMS/monitor sleep while the system itself is
    still in S0.

The keypress is a **lone left-Ctrl with no keycode**, so it registers as keyboard
activity but types nothing and toggles no lock state — harmless even if a text
field happens to be focused.

The device draws far more than the ~2.5 mA a USB host budgets for a suspended
device, so it is knowingly out of spec on suspend current. Whether a given host
tolerates that is a per-host question; see [ADR-0001](adr/0001-use-usb-hid-for-wake.md),
[ADR-0006](adr/0006-stay-on-esp32s3-wifi.md), and roadmap steps 1–2 for the
findings on the reference host.

## WiFi

Station mode, credentials via the config accessor (below). The radio runs with
`WIFI_PS_MIN_MODEM` — modem sleep between DTIM beacons — which is the profile the
long-poll workload uses and keeps idle draw around 20 mA. The event handler
reconnects on disconnect and reflects link state on the LED.

## Telegram command path

- **Long polling.** `GET /bot<token>/getUpdates?timeout=50&limit=1&offset=<n>`.
  The request is held open server-side for up to 50 s, so the socket timeout is
  set above that. Outbound HTTPS works from behind home NAT with no inbound
  reachability, which is why the device polls rather than using a webhook or a
  relay ([ADR-0002](adr/0002-poll-telegram-directly.md)).
- **TLS.** The server certificate is verified against the ESP-IDF mbedTLS
  certificate bundle via `esp_crt_bundle_attach`, not a pinned certificate, so
  Telegram can rotate certificates freely ([ADR-0007](adr/0007-verify-telegram-tls-with-cert-bundle.md)).
- **Parsing.** cJSON reads `update_id`, `message.chat.id`, and `message.text`.
  `offset` advances past every update seen so it is not redelivered. `limit=1`
  keeps each response to a single update; the receive buffer is 8 KiB, ample for
  one message (Telegram caps text at 4096 characters).
- **Backlog drain.** On boot the first request uses `offset=-1`, which returns
  only the latest update; the task fast-forwards `offset` past it **without
  acting**. This keeps a stale command sent before a reboot from waking the host.
- **Commands.** `/wake` triggers a wake and replies `Sending wake key to the
  host.` — the action taken, not a claim that the host woke; `/status` replies
  with uptime since boot, the chip's internal die temperature, and the USB link
  state — `mounted`/`suspended` and whether the host enabled `remote-wake` — as a
  wake diagnostic readable over Telegram without the UART console; `/start`
  replies with a short help line; anything else from an authorized chat is
  ignored. The wake flag is set **before** the reply is sent, so the reply's
  round trip cannot delay the wake.
- **Wake confirmation.** When the host was asleep (USB bus suspended) and the
  wake was signalled via remote wakeup, the main task watches for the bus to
  resume — the one device-side signal that the host actually woke, needing no
  host software — and a second reply, `Host woke up.`, is sent. A display-only
  wake (host already in S0) produces no observable resume, so no confirmation is
  sent there. To deliver the confirmation within seconds, the poll loop briefly
  shortens its long-poll after a `/wake`
  ([ADR-0009](adr/0009-confirm-host-wake-on-bus-resume.md)).
- **Liveness watchdog.** A completed `getUpdates` (HTTP 200) is the device's
  liveness signal. If contact with Telegram is lost for too long — a wall-clock
  silence window, or enough back-to-back failures — the poll loop calls
  `esp_restart()`, since an unattended remote device cannot be power-cycled by
  hand ([ADR-0008](adr/0008-reboot-on-lost-telegram-contact.md)). Thresholds are
  in `main/watchdog.h`.

## Access control

The bot is reachable by anyone who knows its username, so the allowlist of chat
IDs is the only access control there is ([ADR-0002](adr/0002-poll-telegram-directly.md)).
`config_chat_allowed()` checks each command's `chat.id`; commands from any other
chat are logged and dropped. IDs are 64-bit — positive for users, negative
(with a `-100…` prefix) for supergroups and channels.

## Credentials

`main/secrets.h` (gitignored) holds the WiFi credentials, the bot token, and the
allowlist. Every read goes through the narrow accessor in `main/config.c`
(`config_wifi_ssid`, `config_wifi_password`, `config_telegram_token`,
`config_chat_allowed`). Routing all reads through one place is what lets runtime
provisioning — the intended successor to the compile-time header
([ADR-0004](adr/0004-compile-time-secrets-header.md)) — replace those bodies
without changes rippling through the codebase.

To configure a checkout, copy the template and fill it in:

```sh
cp main/secrets.h.example main/secrets.h
```

## Flash and partition layout

The board has 2 MB of flash. The TLS stack (mbedTLS) plus the full certificate
bundle grew the app binary to ~1 MB, nearly filling the default `single_app`
partition. A custom [`partitions.csv`](../partitions.csv) grows the `factory`
partition to `0x1F0000` (~1.94 MB), claiming the ~1 MB of flash that otherwise
sat unused; there is no OTA, so one app partition is all that is needed. It is
selected with `CONFIG_PARTITION_TABLE_CUSTOM` in both `sdkconfig` and
`sdkconfig.defaults`. The `nvs` partition keeps its offset, so the WiFi
calibration data survives a reflash.

## Status LED

The onboard WS2812 (GPIO 48) is a status indicator: **red** while WiFi is
unconnected, **green** once the device has an IP, and a **triple blue blink** on a
Telegram wake as a local confirmation that the command arrived. It is kept dim; a
production build should turn it off during host sleep, where every mA counts.

## Host requirements

Waking over USB must be permitted by the host — the device cannot force it:

- **BIOS/UEFI:** USB wake enabled (often *Wake on USB* or *USB Power in S3/S4*;
  *ErP* / *Deep Sleep* must be off).
- **Windows:** Device Manager → the keyboard device → Power Management → *Allow
  this device to wake the computer*.
- **Port power:** the USB port must retain power in the target sleep state. On
  many desktops only a subset of ports does.

Roadmap step 1 is the procedure for proving this on a new host.

## Building and flashing

Requires ESP-IDF v5.x.

```sh
idf.py build
idf.py -p PORT flash
```

Two quirks of the native-USB S3 are worth knowing:

- **Download mode.** To flash, put the board in download mode: hold BOOT, tap
  RESET, then release BOOT. This exposes the USB-Serial-JTAG COM port. After
  flashing the board hard-resets; if BOOT is still held it boots straight back
  into download mode, so release BOOT and tap RESET (or replug) to run the app.
- **No serial console while running.** With USB-OTG driving the HID interface,
  the shared USB-Serial-JTAG console is unavailable. The console is routed to
  UART0, so reading logs needs the UART pins or a USB-TTL adapter.

## Power

Idle draw is ~20 mA with modem sleep (~18 mA with the LED off) — about 0.1 W. The
device is permanently USB-powered, so this is negligible in both energy and cost;
battery operation is out of scope. See [ADR-0006](adr/0006-stay-on-esp32s3-wifi.md).

## Security

- Credentials are compiled into the firmware ([ADR-0004](adr/0004-compile-time-secrets-header.md)).
  Anyone holding a build, or able to dump the flash, can read them — so firmware
  images must not be published or built in CI while this holds.
- The chat allowlist is the only thing between a stranger and the wake button.
  Keep it tight.
- To the host, the device is a keyboard: the same trust boundary as leaving a
  keyboard plugged in. Treat the device as physically trusted hardware.

## Decisions

The reasoning behind these choices lives in the ADRs:

- [ADR-0001](adr/0001-use-usb-hid-for-wake.md) — USB HID rather than BLE HID.
- [ADR-0002](adr/0002-poll-telegram-directly.md) — poll Telegram directly, no relay.
- [ADR-0003](adr/0003-use-esp-idf-for-firmware.md) — ESP-IDF (C).
- [ADR-0004](adr/0004-compile-time-secrets-header.md) — compile-time secrets header, for now.
- [ADR-0005](adr/0005-rename-to-revrevrev.md) — the RevRevRev name.
- [ADR-0006](adr/0006-stay-on-esp32s3-wifi.md) — stay on ESP32-S3 with WiFi.
- [ADR-0007](adr/0007-verify-telegram-tls-with-cert-bundle.md) — verify TLS with the certificate bundle.
- [ADR-0008](adr/0008-reboot-on-lost-telegram-contact.md) — reboot on lost Telegram contact.
- [ADR-0009](adr/0009-confirm-host-wake-on-bus-resume.md) — confirm host wake on USB bus resume.
