# 0004. Keep credentials in an uncommitted compile-time header

- Status: Accepted
- Date: 2026-07-16

## Context

The device needs WiFi credentials, the Telegram bot token, and the allowlist of
permitted chat IDs (ADR-0002). They have to reach the device somehow, and
whatever we choose must not put the token into this repository.

Three options were weighed:

1. **A compile-time header**, excluded from version control.
2. **A separate NVS partition image**, generated from a CSV and flashed apart
   from the firmware.
3. **Runtime provisioning** over SoftAP or BLE, via the `wifi_provisioning`
   component.

One fact reshapes the usual tradeoff: WakeKey is permanently plugged into a USB
port of the very host it wakes, on the same port used to flash it. Reflashing is
therefore nearly free, which removes most of the value that (2) and (3) buy back
in exchange for their extra machinery.

There is no firmware yet. The immediate goal is a working device, and (1) is the
shortest path to one. Option (3) remains the intended end state — it is the only
option that keeps secrets out of the build entirely and lets the device be
reconfigured without a toolchain — but it costs a provisioning state machine, a
portal or app, a reset-to-provision path, and a new attack surface to secure.
That is not work worth doing before the wake path itself exists.

Note that the header must sit outside Kconfig. `sdkconfig` is committed and is
generated from defaults plus local edits, so it holds the resolved values —
routing secrets through `menuconfig` would place them in a tracked file, which
is the exact outcome being avoided.

## Decision

Store credentials as constants in a header that is listed in `.gitignore`, not
in Kconfig and not in `sdkconfig`.

This is deliberately interim. Runtime provisioning (option 3) is the intended
successor, and a later ADR is expected to supersede this one.

## Consequences

Easier:

- No credential tooling, no extra flashing step, no provisioning code — the
  shortest route to a device that wakes a computer.

Harder:

- Credentials are compiled into the firmware image. The image must never be
  published, attached to a release, or built in CI, and anyone who dumps the
  flash reads them in plaintext.
- Rotating the token or changing the WiFi password means a rebuild and a
  reflash, not a config change.
- The exclusion is one `git add -f` away from failing. The header being
  untracked is the only thing keeping the token out of history.
- Migrating to provisioning will change where credentials come from. Keep reads
  behind a single narrow accessor so option 3 replaces its body rather than
  spreading through the codebase.
