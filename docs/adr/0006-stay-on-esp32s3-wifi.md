# 0006. Stay on ESP32-S3 with WiFi rather than switch radios for lower power

- Status: Accepted
- Date: 2026-07-22

## Context

The suspend-current concern from [ADR-0001](0001-use-usb-hid-for-wake.md) and
step 2 of the roadmap raised a question: the device draws far more than USB
budgets for a suspended device, so should it move to a lower-power radio to cut
what it draws while the host sleeps? The concrete alternative considered was an
ESP32-C6 using BLE (or Zigbee) to reach an always-on BLE↔WiFi bridge already
running on the network, offloading connectivity and dropping the device's radio
current by an order of magnitude.

Two things had to be weighed: whether the draw is actually a problem, and
whether the alternative is even compatible with how the device wakes the host.

Measurements settled the first:

- The host sleeps in S3 (`powercfg /a`), so the ~2.5 mA suspend budget is the
  live kind, not the sort that evaporates under Modern Standby. The device is
  knowingly out of spec.
- The port sustained ~100 mA (worst case: WiFi radio always on plus forced TX)
  through an overnight S3 sleep with no brownout, no reset, and no loss of port
  power — and BOOT still woke the host.
- Realistic idle draw with WiFi modem sleep (`WIFI_PS_MIN_MODEM`) measured
  ~20 mA (~18 mA without the status LED). From 5 V that is 0.1 W, roughly
  0.9 kWh per year.

A hardware fact settled the second: the ESP32-C6 has no USB-OTG peripheral, only
a fixed-function USB Serial/JTAG controller, so it cannot enumerate as a USB HID
keyboard. USB-HID wake ([ADR-0001](0001-use-usb-hid-for-wake.md)) requires
USB-OTG, which among common ESP32 parts exists only on the S2 and S3 (and P4).
Moving to a C6 therefore forces abandoning USB-HID wake for BLE-HID wake — the
exact transport ADR-0001 rejected for its uneven support on desktops.

## Decision

Stay on the ESP32-S3 with WiFi as the command transport, using
`WIFI_PS_MIN_MODEM`. Do not switch to an ESP32-C6, and do not offload
connectivity to the always-on BLE↔WiFi bridge.
[ADR-0001](0001-use-usb-hid-for-wake.md) (USB HID) and
[ADR-0002](0002-poll-telegram-directly.md) (poll Telegram directly) stand.

The device is permanently USB-powered, not battery-powered, so ~20 mA is
negligible in both energy and cost, the port tolerates it with wide margin, and
there is no thermal constraint. The ~18 mA that BLE would save buys nothing that
matters here, while its cost is real: reversing ADR-0002 to reintroduce a relay
in the wake path — a new runtime dependency and a new ADR — and, for the C6,
giving up the reliable USB-HID wake the product rests on.

## Consequences

Easier:

- No hardware change; the proven S3 board and the single WiFi code path continue.
- The wake path stays USB-HID — the most broadly supported transport, already
  proven on this host.
- No relay or bridge in the wake path (ADR-0002 intact); fewer things that can
  break at the moment the device is needed.

Harder:

- The device draws ~20 mA continuously while idle — roughly 8× the USB suspend
  budget. Tolerated here, but a host with a strict, enforcing port could refuse
  it, and this decision would then need revisiting (a lower-power transport, or a
  powered hub).
- Battery operation stays off the table; WiFi's draw rules it out without a
  redesign.
- Networks that a bridge could route around — those blocking Telegram, per
  ADR-0002 — remain unreachable.
