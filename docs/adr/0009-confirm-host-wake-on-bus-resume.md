# 0009. Confirm host wake by observing USB bus resume

- Status: Accepted
- Date: 2026-07-22

## Context

On `/wake` the firmware replied `Waking the host.` immediately — before the wake
was even attempted, and regardless of whether it worked. Two problems:

- The reply overpromises. It reads as "the host is awake" when all that happened
  is a command was accepted. The wake is issued asynchronously by the main task,
  which owns USB; the Telegram task that sends the reply cannot know the outcome.
- There was no signal back to the user that the machine actually came up, which
  is the thing they care about when waking a remote host they cannot see.

The constraint on any confirmation: it must be determinable **on the device,
without host-side software**. The device is a plain USB HID keyboard to the host
(ADR-0001); anything requiring an agent on the host is out of scope.

The one host-software-free signal available is the USB bus itself. When the host
is asleep the bus is suspended; a wake goes out via `tud_remote_wakeup()`, and if
the host wakes it drives a bus **resume** (`tud_resume_cb`, `tud_suspended()`
turns false). That resume is a genuine, standard indication the host woke. It
only exists for the asleep-host path, though: a display-only wake (host already
in S0, monitor asleep via DPMS) sends an ordinary keypress and produces no bus
transition the device can observe.

## Decision

Split the acknowledgement into an honest action report plus an optional, genuine
confirmation:

- The first reply reports the action, `Sending wake key to the host.`, still set
  before the wake so the reply never delays it.
- `trigger_wake()` returns whether a remote wakeup was actually signalled
  (`tud_remote_wakeup()` sends only if the host enabled it). When it was, the
  main task watches `tud_suspended()` for up to `WAKE_CONFIRM_TIMEOUT_US` (12 s).
  On resume it calls `telegram_notify_wake_confirmed(chat_id)`; the poll loop
  then sends a second reply, `Host woke up.`. If the bus never resumes in the
  window, nothing is sent.
- Display-only wakes are deliberately left unconfirmed — there is no on-device
  signal to key on, and inventing one would need host software.
- To deliver the confirmation promptly rather than after the next 50 s
  long-poll, the poll loop drops `getUpdates` to a `TG_CONFIRM_POLL_SECONDS`
  (2 s) timeout for a `TG_CONFIRM_WINDOW_SECONDS` (15 s) window after a `/wake`.
- The cross-task handoff, previously a lone `atomic_bool`, now also carries the
  requesting chat id (so the confirmation is addressed correctly) and a
  confirmation flag plus chat id on the way back. All still lock-free atomics;
  USB access stays single-threaded in the main task.

## Consequences

Easier:

- The acknowledgement is truthful, and for the primary asleep→awake case the
  user gets a real "it came up" confirmation of the remote machine.

Harder:

- Only the suspend→resume path is confirmable. A display-only wake succeeds but
  is silently unconfirmed, which the reply wording ("Sending wake key…") does not
  distinguish.
- The resume is attributed to our request within the watch window; an unrelated
  host wake inside that window would produce a false `Host woke up.`. The window
  is short and the harm is low.
- Confirmation latency is bounded by the short poll (~1–5 s typical), at the cost
  of a handful of extra `getUpdates` requests per `/wake`.
- The task handoff carries more shared state than a single bool, though it
  remains lock-free and keeps every HID call in one task.
