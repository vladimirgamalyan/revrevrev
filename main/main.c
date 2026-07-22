/*
 * Firmware spike: wake over USB HID with WiFi up, now trimmed to measure the
 * device's realistic idle draw.
 *
 * The port-stress question is settled (the port sustained ~100 mA of WiFi load
 * overnight in S3). This variant drops the artificial TX load and enables
 * WIFI_PS_MIN_MODEM — the power profile the real long-poll workload would use —
 * so a USB meter reads a number close to production idle. No TLS, no Telegram.
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "led_strip.h"
#include "secrets.h"

#define APP_BUTTON (GPIO_NUM_0) // BOOT button
#define LED_GPIO (GPIO_NUM_48)  // Onboard WS2812 RGB LED
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

/********* TinyUSB device callbacks ***************/

// Confirms whether the host actually enabled remote wakeup for this device.
void tud_suspend_cb(bool remote_wakeup_en)
{
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

static void led_init(void)
{
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

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(status_led, 0, r, g, b);
    led_strip_refresh(status_led);
}

/********* WiFi ***************/

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        led_set(2, 0, 0); // red: not connected
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
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
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
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
    // Report ID 0: the report descriptor above declares no report ID, so the
    // report must go out without an ID prefix byte.
    uint8_t keycode[6] = {HID_KEY_A};
    tud_hid_keyboard_report(0, 0, keycode);
    vTaskDelay(pdMS_TO_TICKS(20));
    tud_hid_keyboard_report(0, 0, NULL);
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

    bool button_was_up = true;
    while (1) {
        bool button_is_up = gpio_get_level(APP_BUTTON);
        if (button_was_up && !button_is_up) {
            // Falling edge: BOOT button just pressed.
            if (tud_suspended()) {
                // The bus is asleep — an ordinary HID report would not be
                // seen. This is the only path that can wake the host.
                ESP_LOGI(TAG, "Bus suspended, requesting remote wakeup");
                tud_remote_wakeup();
            } else if (tud_mounted()) {
                ESP_LOGI(TAG, "Sending wake keypress");
                send_wake_keypress();
            }
        }
        button_was_up = button_is_up;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
