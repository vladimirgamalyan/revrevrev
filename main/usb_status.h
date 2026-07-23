#pragma once

#include <stdbool.h>

/*
 * USB link state published by the main task — the sole owner of TinyUSB access —
 * for the Telegram /status diagnostic. Reads are lock-free snapshots, at most one
 * main-loop tick (~20 ms) stale, so the polling task never touches TinyUSB.
 */

bool usb_is_mounted(void);
bool usb_is_suspended(void);

// Whether the host enabled USB remote wakeup — the signal that decides whether
// /wake can wake a sleeping host. It is negotiated only when the host suspends,
// so it is UNKNOWN until the host has slept at least once, distinct from a host
// that slept and left it OFF.
typedef enum {
    USB_REMOTE_WAKEUP_UNKNOWN = 0, // host has not suspended yet; not negotiated
    USB_REMOTE_WAKEUP_OFF,
    USB_REMOTE_WAKEUP_ON,
} usb_remote_wakeup_state_t;

usb_remote_wakeup_state_t usb_remote_wakeup_state(void);
