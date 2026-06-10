/*
 * Version V1.2, last modified: 2026.06.08, update: pin LED status task to Core1.
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头，统一 LED 状态机文件版本记录格式。
 */

#include "led_status.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_state.h"
#include "app_tasks.h"
#include "cellular_ppp.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO_NUM 48
#define LED_COUNT 1
#define RMT_RESOLUTION_HZ 10000000
#define FRAME_PERIOD_MS 20
#define BREATH_STEPS 100
#define MAX_BRIGHTNESS 80
#define LED_PHASE_STATUS_MS 5000
#define LED_PHASE_SEPARATOR_MS 2000
#define LED_DISABLED_POLL_MS 250

typedef struct {
    const char *name;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint32_t duration_ms;
    bool separator;
} led_phase_desc_t;

enum {
    LED_PHASE_WIFI_AP = 0,
    LED_PHASE_WIFI_STA,
    LED_PHASE_EC200A_HW,
    LED_PHASE_PPP_LINK,
    LED_PHASE_SEPARATOR,
    LED_PHASE_COUNT,
};

static const char *TAG = "led_status";
static uint8_t s_led_pixels[LED_COUNT * 3];
static size_t s_led_phase_index = LED_PHASE_WIFI_AP;
static uint64_t s_led_phase_started_ms = 0;
static int s_breath_step = 0;
static int s_breath_direction = 1;
static bool s_led_output_off = false;

static const led_phase_desc_t s_led_phase_table[LED_PHASE_COUNT] = {
    [LED_PHASE_WIFI_AP] = { "wifi_ap", 255, 0, 0, LED_PHASE_STATUS_MS, false },
    [LED_PHASE_WIFI_STA] = { "wifi_sta", 0, 0, 255, LED_PHASE_STATUS_MS, false },
    [LED_PHASE_EC200A_HW] = { "ec200a_hw", 0, 255, 0, LED_PHASE_STATUS_MS, false },
    [LED_PHASE_PPP_LINK] = { "ppp_link", 255, 160, 0, LED_PHASE_STATUS_MS, false },
    [LED_PHASE_SEPARATOR] = { "separator", 255, 255, 255, LED_PHASE_SEPARATOR_MS, true },
};

static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 0.3 * RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.9 * RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 0.9 * RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.3 * RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
    .level1 = 0,
    .duration1 = RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
};

static size_t ws2812_encoder_callback(const void *data,
                                      size_t data_size,
                                      size_t symbols_written,
                                      size_t symbols_free,
                                      rmt_symbol_word_t *symbols,
                                      bool *done,
                                      void *arg)
{
    (void)arg;

    if (symbols_free < 8) {
        return 0;
    }

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = (const uint8_t *)data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (uint8_t bit = 0x80; bit != 0; bit >>= 1) {
            symbols[symbol_pos++] = (bytes[data_pos] & bit) ? ws2812_one : ws2812_zero;
        }
        return symbol_pos;
    }

    symbols[0] = ws2812_reset;
    *done = true;
    return 1;
}

static void set_pixel_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    s_led_pixels[0] = green;
    s_led_pixels[1] = red;
    s_led_pixels[2] = blue;
}

static void show_pixel(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder)
{
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    ESP_ERROR_CHECK(rmt_transmit(channel, encoder, s_led_pixels, sizeof(s_led_pixels), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, portMAX_DELAY));
}

static uint8_t scale_color(uint8_t color, uint8_t brightness)
{
    return (uint8_t)((uint16_t)color * brightness / 255);
}

static bool led_phase_is_healthy(const app_state_snapshot_t *snapshot,
                                 const cellular_status_t *cellular_status,
                                 size_t phase_index)
{
    if (phase_index == LED_PHASE_WIFI_AP) {
        return snapshot->softap_started;
    }
    if (phase_index == LED_PHASE_WIFI_STA) {
        return snapshot->connected_sta_count > 0;
    }
    if (phase_index == LED_PHASE_EC200A_HW) {
        return cellular_status->usb_connected && cellular_status->at_ready;
    }
    if (phase_index == LED_PHASE_PPP_LINK) {
        return cellular_status->ppp_connected && cellular_status->napt_enabled;
    }
    return true;
}

static void led_report_reset_breath(void)
{
    s_breath_step = 0;
    s_breath_direction = 1;
}

static void led_report_advance_phase(void)
{
    s_led_phase_index = (s_led_phase_index + 1) % LED_PHASE_COUNT;
    s_led_phase_started_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    led_report_reset_breath();
    app_state_set_active_led_phase(s_led_phase_index);
    ESP_LOGI(TAG, "LED phase -> %s", s_led_phase_table[s_led_phase_index].name);
}

static uint8_t led_report_get_brightness(bool breathing)
{
    if (!breathing) {
        return MAX_BRIGHTNESS;
    }

    float phase = (float)s_breath_step / BREATH_STEPS;
    uint8_t brightness = (uint8_t)((1.0f - cosf(phase * (float)M_PI)) * 0.5f * MAX_BRIGHTNESS);

    s_breath_step += s_breath_direction;
    if (s_breath_step >= BREATH_STEPS) {
        s_breath_step = BREATH_STEPS;
        s_breath_direction = -1;
    } else if (s_breath_step <= 0) {
        s_breath_step = 0;
        s_breath_direction = 1;
    }

    return brightness;
}

static void led_report_step(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder)
{
    app_state_snapshot_t snapshot;
    cellular_status_t cellular_status;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    const led_phase_desc_t *phase_desc = NULL;
    bool breathing = false;
    uint8_t brightness = 0;

    app_state_get_snapshot(&snapshot);
    if (!snapshot.led_enabled) {
        if (!s_led_output_off) {
            set_pixel_rgb(0, 0, 0);
            show_pixel(channel, encoder);
            s_led_output_off = true;
            ESP_LOGI(TAG, "LED status output disabled");
        }
        return;
    }

    if (s_led_output_off) {
        s_led_output_off = false;
        s_led_phase_started_ms = 0;
        s_led_phase_index = LED_PHASE_WIFI_AP;
        led_report_reset_breath();
        ESP_LOGI(TAG, "LED status output enabled");
    }

    if (s_led_phase_started_ms == 0) {
        s_led_phase_started_ms = now_ms;
        app_state_set_active_led_phase(s_led_phase_index);
        ESP_LOGI(TAG, "LED phase -> %s", s_led_phase_table[s_led_phase_index].name);
    }

    phase_desc = &s_led_phase_table[s_led_phase_index];
    while (now_ms - s_led_phase_started_ms >= phase_desc->duration_ms) {
        led_report_advance_phase();
        phase_desc = &s_led_phase_table[s_led_phase_index];
    }

    cellular_ppp_get_status(&cellular_status);
    breathing = !phase_desc->separator && led_phase_is_healthy(&snapshot, &cellular_status, s_led_phase_index);
    brightness = led_report_get_brightness(breathing);

    set_pixel_rgb(scale_color(phase_desc->red, brightness),
                  scale_color(phase_desc->green, brightness),
                  scale_color(phase_desc->blue, brightness));
    show_pixel(channel, encoder);
}

static void led_status_task(void *arg)
{
    (void)arg;

    rmt_channel_handle_t led_channel = NULL;
    rmt_tx_channel_config_t tx_channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    rmt_encoder_handle_t ws2812_encoder = NULL;
    rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encoder_callback,
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_config, &led_channel));
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&encoder_config, &ws2812_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_channel));

    while (true) {
        app_state_snapshot_t snapshot;

        led_report_step(led_channel, ws2812_encoder);
        app_state_get_snapshot(&snapshot);
        vTaskDelay(pdMS_TO_TICKS(snapshot.led_enabled ? FRAME_PERIOD_MS : LED_DISABLED_POLL_MS));
    }
}

esp_err_t led_status_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(led_status_task,
                                            "led_status",
                                            4096,
                                            NULL,
                                            APP_TASK_PRIO_LED,
                                            NULL,
                                            APP_CORE_APP);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
