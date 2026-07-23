#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Telegram command path (ADR-0002): a background task long-polls the Bot API
 * over HTTPS and flags a wake request when an authorized chat sends /wake.
 *
 * The task never touches USB itself. All USB access stays in the main task, so
 * the task signals through a flag that the main loop drains and acts on.
 */

// Start the background polling task. Call once, after WiFi has been brought up.
void telegram_start(void);

// Return true at most once per authorized /wake, clearing the request and
// writing the requesting chat's ID to *chat_id. Polled by the main loop, which
// owns the USB HID wake and later confirms the wake to that chat.
bool telegram_take_wake_request(int64_t *chat_id);

// Called by the main task once it has observed the host actually resume after a
// wake (USB bus resume). Hands the confirmation back to the polling task, which
// replies "Host woke up." to the chat that issued /wake.
void telegram_notify_wake_confirmed(int64_t chat_id);
