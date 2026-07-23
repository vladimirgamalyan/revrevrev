#include "telegram.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "driver/temperature_sensor.h"
#include "cJSON.h"

#include "config.h"
#include "watchdog.h"
#include "usb_status.h"

static const char *TAG = "telegram";

// Long-poll: getUpdates is held open server-side for this long when idle, so
// the socket timeout must exceed it. limit=1 keeps each response to a single
// update, so the receive buffer stays small and the parse trivial.
#define TG_LONG_POLL_SECONDS 50
#define TG_HTTP_TIMEOUT_MS ((TG_LONG_POLL_SECONDS + 10) * 1000)
#define TG_BACKOFF_MS 5000

// After an authorized /wake, poll on this short timeout for a brief window so a
// "host woke" confirmation from the main task can be delivered within seconds
// instead of waiting out the next full long-poll. The window bounds how long we
// keep short-polling if the host never resumes.
#define TG_CONFIRM_POLL_SECONDS 2
#define TG_CONFIRM_WINDOW_SECONDS 15

// A Telegram text message tops out at 4096 characters; 8 KiB leaves ample room
// for the surrounding JSON of one update.
#define TG_RX_CAPACITY 8192
#define TG_URL_CAPACITY 512

// Longest reply this firmware formats (tg_send_status). Sizes msg[] and the
// send-side encode buffer so a fully percent-encoded reply is never truncated.
#define TG_MSG_CAPACITY 128

// The Telegram task runs the TLS handshake, cert-bundle verification, and the
// cJSON parse on its own stack, so it needs generous headroom.
#define TG_TASK_STACK_SIZE 16384

// UTF-8 for U+00B0; a separate "C" literal follows so the escape does not
// swallow it as another hex digit. url_encode percent-encodes it on the wire.
#define DEGREE_SIGN "\xC2\xB0"

// Set by the polling task on an authorized /wake, drained by the main task,
// which owns all USB access. s_wake_chat_id carries which chat asked, so the
// main task can address the later "host woke" confirmation back to it.
static atomic_bool s_wake_requested;
static atomic_int_least64_t s_wake_chat_id;

// Set by the main task once it observes the host actually resume after a wake
// (USB bus resume), drained by the polling task, which replies to the chat.
static atomic_bool s_wake_confirmed;
static atomic_int_least64_t s_wake_confirm_chat_id;

// Task-local to the polling task: raised by tg_handle_command on a /wake so the
// loop shortens its polling and watches for the confirmation above.
static bool s_wake_pending_confirm;

// Internal die-temperature sensor, installed once at task start and read on
// /status. NULL if installation failed; /status then reports temperature as
// unavailable rather than failing.
static temperature_sensor_handle_t s_temp_sensor;

// Response accumulator filled by the HTTP event handler across ON_DATA chunks.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} tg_rx_t;

static esp_err_t tg_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    tg_rx_t *rx = evt->user_data;
    if (rx->overflow) {
        return ESP_OK;
    }
    // Keep one byte for a NUL terminator so the buffer parses as a C string.
    if (rx->len + (size_t)evt->data_len >= rx->cap) {
        rx->overflow = true;
        return ESP_OK;
    }
    memcpy(rx->buf + rx->len, evt->data, evt->data_len);
    rx->len += (size_t)evt->data_len;
    return ESP_OK;
}

// Percent-encode src into dst for use in a URL query value. Unreserved
// characters (RFC 3986) pass through; everything else becomes %XX.
static void url_encode(const char *src, char *dst, size_t dst_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 4 < dst_cap; ++si) {
        unsigned char c = (unsigned char)src[si];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
}

// Fire-and-forget reply to the chat. The response is ignored; failures are
// logged and swallowed, since a lost acknowledgement must not stall polling.
static void tg_send_message(esp_http_client_handle_t client, tg_rx_t *rx, int64_t chat_id, const char *text)
{
    char encoded[3 * TG_MSG_CAPACITY + 1];
    char url[TG_URL_CAPACITY];
    url_encode(text, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage?chat_id=%lld&text=%s",
             config_telegram_token(), (long long)chat_id, encoded);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    // The reply body is ignored, but the event handler accumulates into the
    // shared rx buffer via the client's user_data. Reset it so the discarded
    // response starts clean instead of piling onto the just-parsed getUpdates
    // buffer and spuriously tripping the overflow guard.
    rx->len = 0;
    rx->overflow = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sendMessage failed: %s", esp_err_to_name(err));
    }
}

// Send a compile-time string literal, asserting it fits the reply budget so a
// future longer literal fails the build instead of being silently truncated by
// tg_send_message's fixed-size encode buffer.
#define TG_SEND_LITERAL(client, rx, chat_id, lit)                \
    do {                                                         \
        _Static_assert(sizeof(lit) <= TG_MSG_CAPACITY,           \
                       "reply literal exceeds TG_MSG_CAPACITY"); \
        tg_send_message((client), (rx), (chat_id), (lit));       \
    } while (0)

// Match a bot command at the start of text, allowing the "/cmd@botname" form
// and a trailing argument, so "/wakeup" does not match "/wake".
static bool is_command(const char *text, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(text, cmd, n) != 0) {
        return false;
    }
    char after = text[n];
    return after == '\0' || after == ' ' || after == '@';
}

// Install and enable the internal temperature sensor. Best effort: on failure
// s_temp_sensor is left NULL and /status reports temperature as unavailable.
static void temp_sensor_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "temp sensor install failed: %s", esp_err_to_name(err));
        s_temp_sensor = NULL;
        return;
    }
    err = temperature_sensor_enable(s_temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "temp sensor enable failed: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = NULL;
    }
}

static bool read_chip_temp(float *out)
{
    return s_temp_sensor != NULL &&
           temperature_sensor_get_celsius(s_temp_sensor, out) == ESP_OK;
}

// Format time since boot as "Dd HHh MMm SSs".
static void format_uptime(char *buf, size_t cap)
{
    int64_t total = esp_timer_get_time() / 1000000; // microseconds -> seconds
    int days = (int)(total / 86400);
    int hours = (int)((total % 86400) / 3600);
    int mins = (int)((total % 3600) / 60);
    int secs = (int)(total % 60);
    snprintf(buf, cap, "%dd %02dh %02dm %02ds", days, hours, mins, secs);
}

static void tg_send_status(esp_http_client_handle_t client, tg_rx_t *rx, int64_t chat_id)
{
    char uptime[32]; // "Dd HHh MMm SSs" — ample even for an absurd day count
    format_uptime(uptime, sizeof(uptime));

    char temp_str[16];
    float temp;
    if (read_chip_temp(&temp)) {
        snprintf(temp_str, sizeof(temp_str), "%.1f" DEGREE_SIGN "C", temp);
    } else {
        snprintf(temp_str, sizeof(temp_str), "n/a");
    }

    // USB diagnostics: whether the host sees the device, whether the bus is
    // suspended (host asleep), and whether the host enabled remote wakeup — the
    // signal that decides whether /wake can wake a sleeping host.
    const char *link = usb_is_mounted() ? "mounted" : "not mounted";
    const char *bus = usb_is_suspended() ? "suspended" : "awake";
    const char *rwake = usb_remote_wakeup_enabled() ? "on" : "off";

    char msg[TG_MSG_CAPACITY];
    snprintf(msg, sizeof(msg),
             "RevRevRev\nuptime: %s\nchip temp: %s\nUSB: %s, %s, remote-wake %s",
             uptime, temp_str, link, bus, rwake);
    tg_send_message(client, rx, chat_id, msg);
}

static void tg_handle_command(esp_http_client_handle_t client, tg_rx_t *rx, int64_t chat_id, const char *text)
{
    if (is_command(text, "/wake")) {
        ESP_LOGI(TAG, "Authorized wake from chat %lld", (long long)chat_id);
        // Flag the wake before the reply: the host waking must not wait on the
        // acknowledgement's round trip. The reply reports the action taken — a
        // wake key was sent — not that the host woke: the main task issues the
        // wake asynchronously and only it can observe whether the host resumed.
        // Record the chat first so it is visible once the flag is seen, then ask
        // the loop to watch for the main task's follow-up confirmation.
        atomic_store(&s_wake_chat_id, chat_id);
        atomic_store(&s_wake_requested, true);
        s_wake_pending_confirm = true;
        TG_SEND_LITERAL(client, rx, chat_id, "Sending wake key to the host.");
    } else if (is_command(text, "/status")) {
        tg_send_status(client, rx, chat_id);
    } else if (is_command(text, "/start")) {
        TG_SEND_LITERAL(client, rx, chat_id,
                        "RevRevRev online. /wake wakes the host, /status shows uptime and temperature.");
    }
    // Any other text from an authorized chat is intentionally ignored.
}

// Fast-forward *offset past the current backlog without acting on it, so a
// stale command from before boot cannot wake the host. getUpdates with
// offset=-1 returns only the most recent update; starting one past it discards
// everything older. Best effort: on any failure *offset is left untouched.
static void tg_prime_offset(tg_rx_t *rx, int64_t *offset)
{
    rx->buf[rx->len] = '\0';
    cJSON *root = cJSON_Parse(rx->buf);
    if (root == NULL) {
        return;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *last = cJSON_IsArray(result) ? cJSON_GetArrayItem(result, 0) : NULL;
    cJSON *update_id = (last != NULL) ? cJSON_GetObjectItem(last, "update_id") : NULL;
    if (cJSON_IsNumber(update_id)) {
        *offset = (int64_t)update_id->valuedouble + 1;
    }
    cJSON_Delete(root);
}

// Parse one getUpdates response, advancing *offset past every update seen so
// the same updates are not redelivered on the next poll. Returns true when the
// body was a well-formed getUpdates payload (an empty result included), false
// when it could not be parsed or had an unexpected shape.
static bool tg_handle_response(esp_http_client_handle_t client, tg_rx_t *rx, int64_t *offset)
{
    rx->buf[rx->len] = '\0';
    cJSON *root = cJSON_Parse(rx->buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "getUpdates JSON parse failed");
        return false;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsTrue(ok) || !cJSON_IsArray(result)) {
        ESP_LOGW(TAG, "Unexpected getUpdates payload");
        cJSON_Delete(root);
        return false;
    }

    cJSON *update;
    cJSON_ArrayForEach(update, result) {
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        if (cJSON_IsNumber(update_id)) {
            int64_t id = (int64_t)update_id->valuedouble;
            if (id >= *offset) {
                *offset = id + 1;
            }
        }

        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!cJSON_IsObject(message)) {
            continue;
        }
        cJSON *chat = cJSON_GetObjectItem(message, "chat");
        cJSON *text = cJSON_GetObjectItem(message, "text");
        if (!cJSON_IsObject(chat) || !cJSON_IsString(text)) {
            continue;
        }
        cJSON *chat_id_item = cJSON_GetObjectItem(chat, "id");
        if (!cJSON_IsNumber(chat_id_item)) {
            continue;
        }
        // cJSON holds every number as a double; Telegram chat IDs stay well
        // within 2^53, so this conversion is exact for all IDs the API issues.
        int64_t chat_id = (int64_t)chat_id_item->valuedouble;

        if (!config_chat_allowed(chat_id)) {
            ESP_LOGW(TAG, "Ignoring command from unauthorized chat %lld", (long long)chat_id);
            continue;
        }
        tg_handle_command(client, rx, chat_id, text->valuestring);
    }

    cJSON_Delete(root);
    return true;
}

// Liveness watchdog for the poll loop (ADR-0008). The device is unattended and
// remote, so a silent wedge in the network stack or this loop is unrecoverable
// without a reboot. Contact with Telegram — a getUpdates that returns HTTP 200 —
// is the liveness signal; losing it for too long triggers esp_restart().
typedef struct {
    int consecutive_failures;
    int64_t last_success_us;
} tg_watchdog_t;

static void tg_watchdog_note_success(tg_watchdog_t *wd)
{
    wd->consecutive_failures = 0;
    wd->last_success_us = esp_timer_get_time();
}

static void tg_watchdog_note_failure(tg_watchdog_t *wd)
{
    wd->consecutive_failures++;
    int64_t silence_us = esp_timer_get_time() - wd->last_success_us;
    bool too_many = wd->consecutive_failures >= TG_WATCHDOG_MAX_CONSECUTIVE_FAILURES;
    bool too_long = silence_us >= (int64_t)TG_WATCHDOG_MAX_SILENCE_SECONDS * 1000000;
    if (too_many || too_long) {
        ESP_LOGE(TAG, "Watchdog: no Telegram contact for %llds over %d consecutive failures; restarting",
                 (long long)(silence_us / 1000000), wd->consecutive_failures);
        esp_restart();
    }
}

// Consume a pending "host woke" confirmation from the main task, if any. Returns
// true once per confirmation, writing the chat to reply to into *chat_id.
static bool tg_take_wake_confirmation(int64_t *chat_id)
{
    if (atomic_exchange(&s_wake_confirmed, false)) {
        *chat_id = atomic_load(&s_wake_confirm_chat_id);
        return true;
    }
    return false;
}

static void telegram_task(void *arg)
{
    (void)arg;

    temp_sensor_init();

    static char rx_buffer[TG_RX_CAPACITY];
    tg_rx_t rx = {
        .buf = rx_buffer,
        .cap = sizeof(rx_buffer),
    };

    esp_http_client_config_t cfg = {
        .url = "https://api.telegram.org",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TG_HTTP_TIMEOUT_MS,
        .event_handler = tg_http_event,
        .user_data = &rx,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        vTaskDelete(NULL);
        return;
    }

    char url[TG_URL_CAPACITY];
    int64_t offset = 0;

    // Drained once, before the first real poll, to skip any pre-boot backlog.
    // The same request path retries through the backoff until WiFi is up.
    bool primed = false;

    // Seed last-success at boot so the silence window counts from now, not from 0.
    tg_watchdog_t wd = { .last_success_us = esp_timer_get_time() };

    // While >0, a /wake is awaiting the main task's resume confirmation: poll on
    // the short timeout until it arrives or this deadline passes.
    int64_t confirm_deadline_us = 0;

    ESP_LOGI(TAG, "Polling Telegram getUpdates");
    while (1) {
        if (primed) {
            int poll_timeout = (confirm_deadline_us != 0) ? TG_CONFIRM_POLL_SECONDS : TG_LONG_POLL_SECONDS;
            snprintf(url, sizeof(url),
                     "https://api.telegram.org/bot%s/getUpdates?timeout=%d&limit=1&offset=%lld",
                     config_telegram_token(), poll_timeout, (long long)offset);
        } else {
            snprintf(url, sizeof(url),
                     "https://api.telegram.org/bot%s/getUpdates?timeout=0&limit=1&offset=-1",
                     config_telegram_token());
        }
        esp_http_client_set_url(client, url);
        esp_http_client_set_method(client, HTTP_METHOD_GET);

        rx.len = 0;
        rx.overflow = false;
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            // Also the expected path until WiFi has an IP: back off and retry.
            ESP_LOGW(TAG, "getUpdates failed: %s (backing off)", esp_err_to_name(err));
            tg_watchdog_note_failure(&wd);
            vTaskDelay(pdMS_TO_TICKS(TG_BACKOFF_MS));
            continue;
        }
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "getUpdates HTTP %d (backing off)", status);
            tg_watchdog_note_failure(&wd);
            vTaskDelay(pdMS_TO_TICKS(TG_BACKOFF_MS));
            continue;
        }
        // A 200 is a completed round trip to Telegram: the loop is alive.
        tg_watchdog_note_success(&wd);
        if (rx.overflow) {
            // A truncated body cannot be parsed to learn its update_id, so
            // advance past the requested offset to avoid stalling on it. With
            // limit=1 and an 8 KiB buffer this is effectively unreachable.
            ESP_LOGW(TAG, "Response exceeded %u bytes, skipping update", (unsigned)rx.cap);
            offset += 1;
            continue;
        }
        if (!primed) {
            tg_prime_offset(&rx, &offset);
            primed = true;
            continue;
        }
        if (!tg_handle_response(client, &rx, &offset)) {
            // A 200 whose body we could not parse or make sense of. We cannot
            // learn its update_id to advance past it, and getUpdates would keep
            // returning the same pending update immediately, so back off instead
            // of spinning — the liveness watchdog counts any 200 as contact and
            // would not catch this loop.
            vTaskDelay(pdMS_TO_TICKS(TG_BACKOFF_MS));
        }

        // A just-handled /wake opens a window during which we poll fast so the
        // main task's resume confirmation reaches the user within seconds.
        if (s_wake_pending_confirm) {
            s_wake_pending_confirm = false;
            confirm_deadline_us = esp_timer_get_time() + (int64_t)TG_CONFIRM_WINDOW_SECONDS * 1000000;
        }
        if (confirm_deadline_us != 0) {
            int64_t confirm_chat;
            if (tg_take_wake_confirmation(&confirm_chat)) {
                TG_SEND_LITERAL(client, &rx, confirm_chat, "Host woke up.");
                confirm_deadline_us = 0;
            } else if (esp_timer_get_time() >= confirm_deadline_us) {
                // Host did not resume in time (asleep-wake failed, or this was a
                // display-only wake the device cannot observe): stay silent.
                confirm_deadline_us = 0;
            }
        }
    }
}

void telegram_start(void)
{
    xTaskCreate(telegram_task, "telegram", TG_TASK_STACK_SIZE, NULL, 5, NULL);
}

bool telegram_take_wake_request(int64_t *chat_id)
{
    if (atomic_exchange(&s_wake_requested, false)) {
        *chat_id = atomic_load(&s_wake_chat_id);
        return true;
    }
    return false;
}

void telegram_notify_wake_confirmed(int64_t chat_id)
{
    atomic_store(&s_wake_confirm_chat_id, chat_id);
    atomic_store(&s_wake_confirmed, true);
}
