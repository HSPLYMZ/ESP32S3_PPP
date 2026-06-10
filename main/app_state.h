/*
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头，统一状态层版本记录格式。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

typedef struct {
    app_config_t config;
    bool softap_started;
    uint8_t connected_sta_count;
    bool wifi_reconfigure_pending;
    bool device_reboot_pending;
    bool led_enabled;
    size_t active_led_phase;
    uint8_t runtime_channel;
} app_state_snapshot_t;

void app_state_init(void);
void app_state_set_config(const app_config_t *config);
void app_state_get_config(app_config_t *config);
void app_state_set_softap_started(bool started);
void app_state_set_connected_sta_count(uint8_t connected_sta_count);
void app_state_set_wifi_reconfigure_pending(bool pending);
void app_state_set_device_reboot_pending(bool pending);
void app_state_set_led_enabled(bool enabled);
void app_state_set_active_led_phase(size_t active_led_phase);
void app_state_set_runtime_channel(uint8_t runtime_channel);
void app_state_get_snapshot(app_state_snapshot_t *snapshot);
