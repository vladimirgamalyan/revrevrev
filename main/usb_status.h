#pragma once

#include <stdbool.h>

/*
 * USB link state published by the main task — the sole owner of TinyUSB access —
 * for the Telegram /status diagnostic. Reads are lock-free snapshots, at most one
 * main-loop tick (~20 ms) stale, so the polling task never touches TinyUSB.
 */

bool usb_is_mounted(void);
bool usb_is_suspended(void);

// Whether the host enabled USB remote wakeup, as negotiated at the last bus
// suspend — the signal that decides whether /wake can wake a sleeping host. It is
// only known once the host has suspended at least once; before that it reads
// false.
bool usb_remote_wakeup_enabled(void);
