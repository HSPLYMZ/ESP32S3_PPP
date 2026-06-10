/*
 * Version V1.2, last modified: 2026.06.08, update: add Core1 background monitor task and FreeRTOS task distribution snapshots.
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头，统一系统监控文件版本记录格式。
 */

#include "system_monitor.h"

#include <string.h>

#include "app_tasks.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "system_monitor";
static const float SYSTEM_TEMP_WARN_C = 70.0f;
// Reduce background polling pressure so monitoring stays informative
// without competing as often with the PPP/USB data path.
static const uint32_t SYSTEM_MONITOR_POLL_MS = 5000;

static SemaphoreHandle_t s_monitor_mutex = NULL;
static temperature_sensor_handle_t s_temp_sensor = NULL;
static system_monitor_snapshot_t s_snapshot = { 0 };

static void system_monitor_task(void *arg)
{
    (void)arg;

    while (true) {
        system_monitor_poll();
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_MONITOR_POLL_MS));
    }
}

static void monitor_lock(void)
{
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
}

static void monitor_unlock(void)
{
    xSemaphoreGive(s_monitor_mutex);
}

static void update_task_distribution(system_monitor_snapshot_t *snapshot)
{
    snapshot->total_tasks = uxTaskGetNumberOfTasks();
    snapshot->core0_tasks = 0;
    snapshot->core1_tasks = 0;

#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t task_count = snapshot->total_tasks;
    TaskStatus_t *task_array = NULL;

    if (task_count > 0) {
        task_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
        if (task_array != NULL) {
            task_count = uxTaskGetSystemState(task_array, task_count, NULL);
            snapshot->total_tasks = task_count;

            for (UBaseType_t i = 0; i < task_count; ++i) {
                BaseType_t core_id = xTaskGetCoreID(task_array[i].xHandle);

                if (core_id == 0) {
                    snapshot->core0_tasks++;
                } else if (core_id == 1) {
                    snapshot->core1_tasks++;
                } else {
                    snapshot->core0_tasks++;
                    snapshot->core1_tasks++;
                }
            }

            vPortFree(task_array);
        }
    }
#endif
}

esp_err_t system_monitor_init(void)
{
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    esp_chip_info_t chip_info;
    esp_err_t err;
    BaseType_t task_ok;

    s_monitor_mutex = xSemaphoreCreateMutex();
    if (s_monitor_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));

    esp_chip_info(&chip_info);
    s_snapshot.core_count = chip_info.cores;
    s_snapshot.cpu_freq_mhz = (uint32_t)(esp_clk_cpu_freq() / 1000000U);

    err = temperature_sensor_install(&temp_cfg, &s_temp_sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_temp_sensor);
    }

    if (err == ESP_OK) {
        s_snapshot.sensor_ready = true;
        ESP_LOGI(TAG, "Internal temperature sensor enabled");
    } else {
        s_snapshot.sensor_ready = false;
        ESP_LOGW(TAG, "Internal temperature sensor unavailable: %s", esp_err_to_name(err));
    }

    system_monitor_poll();
    task_ok = xTaskCreatePinnedToCore(system_monitor_task,
                                      "sys_monitor",
                                      4096,
                                      NULL,
                                      APP_TASK_PRIO_MONITOR,
                                      NULL,
                                      APP_CORE_APP);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void system_monitor_poll(void)
{
    system_monitor_snapshot_t next = { 0 };
    esp_chip_info_t chip_info;

    esp_chip_info(&chip_info);
    next.sensor_ready = s_snapshot.sensor_ready;
    next.cpu_freq_mhz = (uint32_t)(esp_clk_cpu_freq() / 1000000U);
    next.core_count = chip_info.cores;

    if (s_temp_sensor != NULL && temperature_sensor_get_celsius(s_temp_sensor, &next.temperature_c) == ESP_OK) {
        next.overheat = next.temperature_c >= SYSTEM_TEMP_WARN_C;
        next.sensor_ready = true;
    } else {
        next.temperature_c = 0.0f;
        next.overheat = false;
    }

    update_task_distribution(&next);

    monitor_lock();
    s_snapshot = next;
    monitor_unlock();
}

void system_monitor_get_snapshot(system_monitor_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    monitor_lock();
    *snapshot = s_snapshot;
    monitor_unlock();
}
