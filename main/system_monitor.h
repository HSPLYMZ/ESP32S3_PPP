/*
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头，统一系统监控文件版本记录格式。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool sensor_ready;
    bool overheat;
    float temperature_c;
    uint32_t cpu_freq_mhz;
    uint32_t core_count;
    UBaseType_t total_tasks;
    UBaseType_t core0_tasks;
    UBaseType_t core1_tasks;
} system_monitor_snapshot_t;

esp_err_t system_monitor_init(void);
void system_monitor_poll(void);
void system_monitor_get_snapshot(system_monitor_snapshot_t *snapshot);
