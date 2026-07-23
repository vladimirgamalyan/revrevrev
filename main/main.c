/*
 * RevRevRev main task: brings up USB HID, WiFi, and the status LED, then loops
 * polling the BOOT button (manual wake test) and the Telegram wake flag. It owns
 * all USB HID access; the Telegram task only sets a flag this loop drains.
 *
 * WiFi runs with WIFI_PS_MIN_MODEM (modem sleep between DTIM beacons), the power
 * profile the long-poll workload uses, keeping idle draw around 20 mA.
 */

#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "config.h"
#include "telegram.h"
#include "usb_status.h"

#define APP_BUTTON (GPIO_NUM_0) // BOOT button
#define LED_GPIO (GPIO_NUM_48)  // Onboard WS2812 RGB LED

// After a remote-wakeup request, watch this long for the USB bus to resume — the
// one device-side signal that the host actually woke. Generous enough to cover a
// slow resume from S3; if it does not resume in time, no confirmation is sent.
#define WAKE_CONFIRM_TIMEOUT_US (12 * 1000000)
static const char *TAG = "revrevrev";

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

const char *hid_string_descriptor[5] = {
    (char[]){0x09, 0x04}, // 0: supported language is English (0x0409)
    "RevRevRev",          // 1: Manufacturer
    "RevRevRev Wake Key", // 2: Product
    "1",                  // 3: Serial
    "Wake Key HID",       // 4: HID interface
};

/**
 * @brief Configuration descriptor
 *
 * One HID interface, with the remote wakeup attribute set — without it the
 * host has no way to know this device is allowed to signal a wake while the
 * bus is suspended.
 */
static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/********* TinyUSB HID callbacks ***************/

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;
}

/********* USB status publication ***************/

// Snapshots of the USB link the main task publishes for the /status diagnostic,
// so the Telegram task reads state instead of touching TinyUSB from its context.
static atomic_bool s_usb_mounted;
static atomic_bool s_usb_suspended;
static atomic_bool s_usb_remote_wakeup_en;

bool usb_is_mounted(void)
{
    return atomic_load(&s_usb_mounted);
}

bool usb_is_suspended(void)
{
    return atomic_load(&s_usb_suspended);
}

bool usb_remote_wakeup_enabled(void)
{
    return atomic_load(&s_usb_remote_wakeup_en);
}

/********* TinyUSB device callbacks ***************/

// Confirms whether the host actually enabled remote wakeup for this device, and
// publishes it for /status — this is the signal that decides whether a /wake can
// wake the host from sleep.
void tud_suspend_cb(bool remote_wakeup_en)
{
    atomic_store(&s_usb_remote_wakeup_en, remote_wakeup_en);
    ESP_LOGI(TAG, "USB suspended, remote wakeup %s", remote_wakeup_en ? "enabled" : "disabled");
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB resumed");
}

/********* Status LED ***************/

// Onboard WS2812 used as a WiFi indicator: red while unconnected, green once
// the device has an IP. Kept dim so it does not glare — and a production build
// should turn it off entirely during host sleep, where every mA counts.
static led_strip_handle_t status_led;

// Serializes LED access: led_set runs from both the main task (init, wake blink)
// and the event-loop task (WiFi status), and the led_strip/RMT backend is not
// safe to drive from two tasks at once.
static SemaphoreHandle_t s_led_mutex;

// Tracks the WiFi indicator state so a wake blink can restore the right color
// afterwards. Written from the event task, read from the main task.
static atomic_bool s_wifi_connected;

static void led_init(void)
{
    s_led_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_led_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &status_led));
}

// Assumes s_led_mutex is already held. Used by led_set and by callers that must
// hold the mutex across more than one step (see led_restore_status).
static void led_set_locked(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(status_led, 0, r, g, b);
    led_strip_refresh(status_led);
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    led_set_locked(r, g, b);
    xSemaphoreGive(s_led_mutex);
}

// Restore the LED to the current WiFi indicator color. The state read and the
// pixel write happen under one mutex hold so a concurrent WiFi event — which
// flips s_wifi_connected and repaints — cannot interleave between them and
// leave the LED stuck on a stale color.
static void led_restore_status(void)
{
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    if (atomic_load(&s_wifi_connected)) {
        led_set_locked(0, 2, 0); // green: connected
    } else {
        led_set_locked(2, 0, 0); // red: not connected
    }
    xSemaphoreGive(s_led_mutex);
}

// Brief blue flashes to confirm a wake command was received. A test aid; runs
// in the main task after the wake is already triggered, then hands the LED back
// to the WiFi status color.
static void wake_blink(void)
{
    for (int i = 0; i < 3; i++) {
        led_set(0, 0, 8); // blue
        vTaskDelay(pdMS_TO_TICKS(80));
        led_set(0, 0, 0); // off
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    led_restore_status();
}

/********* WiFi ***************/

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        atomic_store(&s_wifi_connected, false);
        led_set(2, 0, 0); // red: not connected
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
        atomic_store(&s_wifi_connected, true);
        led_set(0, 2, 0); // green: connected with an IP
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config_wifi_ssid(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, config_wifi_password(), sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Modem sleep between DTIM beacons: the radio powers down when idle and
    // wakes to catch buffered downlink. This is the profile the real long-poll
    // workload would run, so the measured current reflects production idle.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
}

/********* Application ***************/

static void send_wake_keypress(void)
{
    // A lone left-Ctrl with no keycode: enough key activity to wake the host or
    // its display, but it types nothing and toggles no lock state, so it is
    // harmless even if a text field happens to be focused. Report ID 0 — the
    // report descriptor above declares none, so no ID prefix byte.
    tud_hid_keyboard_report(0, KEYBOARD_MODIFIER_LEFTCTRL, NULL);
    vTaskDelay(pdMS_TO_TICKS(20));
    tud_hid_keyboard_report(0, 0, NULL);
}

// The one wake action, driven by either trigger: the BOOT button (manual test)
// or an authorized Telegram command. Runs only in the main task, so all USB
// access stays single-threaded. Returns true when a remote wakeup was actually
// signalled — the only case where a subsequent bus resume confirms the host woke.
static bool trigger_wake(void)
{
    if (tud_suspended()) {
        // The bus is asleep — an ordinary HID report would not be seen. This is
        // the only path that can wake the host. tud_remote_wakeup() returns true
        // only if the host had remote wakeup enabled and the signal was sent, so
        // a resume that follows is attributable to this request.
        ESP_LOGI(TAG, "Bus suspended, requesting remote wakeup");
        return tud_remote_wakeup();
    } else if (tud_mounted()) {
        ESP_LOGI(TAG, "Sending wake keypress");
        send_wake_keypress();
        // Display-wake path: the host is already in S0, so there is no bus
        // resume to observe and no confirmation is possible.
        return false;
    }
    return false;
}

void app_main(void)
{
    const gpio_config_t boot_button_config = {
        .pin_bit_mask = BIT64(APP_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = true,
        .pull_down_en = false,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_button_config));

    // WiFi needs NVS for its calibration data.
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    led_init();
    led_set(2, 0, 0); // red until WiFi connects

    ESP_LOGI(TAG, "USB initialization");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB initialization DONE");

    ESP_LOGI(TAG, "WiFi bring-up");
    wifi_start();

    ESP_LOGI(TAG, "Starting Telegram command path");
    telegram_start();

    bool button_was_up = true;

    // While >0, a Telegram wake requested a remote wakeup and we are watching for
    // the bus to resume so we can confirm to wake_confirm_chat that the host woke.
    int64_t wake_confirm_deadline = 0;
    int64_t wake_confirm_chat = 0;

    while (1) {
        // Publish the current USB link state for the /status diagnostic. Done
        // from this task so the Telegram task never reads TinyUSB itself.
        atomic_store(&s_usb_mounted, tud_mounted());
        atomic_store(&s_usb_suspended, tud_suspended());

        bool button_is_up = gpio_get_level(APP_BUTTON);
        if (button_was_up && !button_is_up) {
            // Falling edge: BOOT button just pressed (manual wake test).
            trigger_wake();
        }
        button_was_up = button_is_up;

        // Wake requested by an authorized Telegram command. Draining it here
        // keeps every USB HID call in this one task. Wake first, then blink the
        // LED as a visible confirmation the command arrived.
        int64_t wake_chat;
        if (telegram_take_wake_request(&wake_chat)) {
            bool may_resume = trigger_wake();
            wake_blink();
            if (may_resume) {
                // A remote wakeup was signalled: start watching for the resume.
                wake_confirm_chat = wake_chat;
                wake_confirm_deadline = esp_timer_get_time() + WAKE_CONFIRM_TIMEOUT_US;
            }
        }

        // If a remote wakeup is outstanding, a bus that is no longer suspended is
        // the host having woken. Confirm it once, then stop watching; give up
        // silently if the deadline passes without a resume.
        if (wake_confirm_deadline != 0) {
            if (!tud_suspended()) {
                telegram_notify_wake_confirmed(wake_confirm_chat);
                wake_confirm_deadline = 0;
            } else if (esp_timer_get_time() >= wake_confirm_deadline) {
                wake_confirm_deadline = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
