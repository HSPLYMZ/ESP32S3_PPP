/*
 * Version V1.2, last modified: 2026.06.08, update: add task/core affinity policy and background system monitor for dual-core optimization.
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头；新增统一版本号出口；取消 Wi-Fi 自动信道并恢复手动信道。
 * 版本V1.1，最后修改时间：2026.06.05 11.23，更新内容：同步系统版本号到 PPP 诊断增强版。
 */

#include "app_config.h"
#include "app_state.h"
#include "cellular_ppp.h"
#include "esp_log.h"
#include "led_status.h"
#include "system_monitor.h"
#include "webui.h"
#include "wifi_ap.h"

static const char *TAG = "ESP32S3_PPP_V1";

const char *app_get_version(void)
{
    // WebUI version display uses this single application version export.
    return "V1.1";
}

void app_main(void)
{
    app_config_t config;

    ESP_LOGI(TAG, "ESP32S3_PPP_V1 start, version=%s", app_get_version());

    app_state_init();
    ESP_ERROR_CHECK(app_config_nvs_init());
    ESP_ERROR_CHECK(app_config_load(&config));
    app_state_set_config(&config);

    ESP_ERROR_CHECK(system_monitor_init());
    ESP_ERROR_CHECK(wifi_ap_init(NULL));
    ESP_ERROR_CHECK(wifi_ap_apply_config(&config));
    ESP_ERROR_CHECK(cellular_ppp_start(wifi_ap_get_ap_netif()));
    ESP_ERROR_CHECK(cellular_ppp_apply_config(&config));
    ESP_ERROR_CHECK(webui_start());
    ESP_ERROR_CHECK(led_status_start());
}
