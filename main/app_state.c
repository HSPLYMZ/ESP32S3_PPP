/*
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头，统一状态层版本记录格式。
 */

#include "app_state.h"

#include <assert.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_state_mutex = NULL;
static app_state_snapshot_t s_state = { 0 };

static void app_state_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void app_state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

void app_state_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    assert(s_state_mutex != NULL);

    app_config_set_defaults(&s_state.config);
    s_state.runtime_channel = APP_WIFI_DEFAULT_CHANNEL;
}

void app_state_set_config(const app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    app_state_lock();
    s_state.config = *config;
    app_state_unlock();
}

void app_state_get_config(app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    app_state_lock();
    *config = s_state.config;
    app_state_unlock();
}

void app_state_set_softap_started(bool started)
{
    app_state_lock();
    s_state.softap_started = started;
    app_state_unlock();
}

void app_state_set_connected_sta_count(uint8_t connected_sta_count)
{
    app_state_lock();
    s_state.connected_sta_count = connected_sta_count;
    app_state_unlock();
}

void app_state_set_wifi_reconfigure_pending(bool pending)
{
    app_state_lock();
    s_state.wifi_reconfigure_pending = pending;
    app_state_unlock();
}

void app_state_set_device_reboot_pending(bool pending)
{
    app_state_lock();
    s_state.device_reboot_pending = pending;
    app_state_unlock();
}

void app_state_set_led_enabled(bool enabled)
{
    app_state_lock();
    s_state.led_enabled = enabled;
    app_state_unlock();
}

void app_state_set_active_led_phase(size_t active_led_phase)
{
    app_state_lock();
    s_state.active_led_phase = active_led_phase;
    app_state_unlock();
}

void app_state_set_runtime_channel(uint8_t runtime_channel)
{
    app_state_lock();
    s_state.runtime_channel = runtime_channel;
    app_state_unlock();
}

void app_state_get_snapshot(app_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    app_state_lock();
    *snapshot = s_state;
    app_state_unlock();
}
