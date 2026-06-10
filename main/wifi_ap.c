/*
 * Version V1.2, last modified: 2026.06.08, update: pin Wi-Fi reconfiguration helper task to Core1.
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头；取消自动信道扫描逻辑，恢复手动信道配置。
 */

#include "wifi_ap.h"

#include <string.h>

#include "app_state.h"
#include "app_tasks.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_ap";
static const esp_ip4_addr_t s_default_dns = { .addr = ESP_IP4TOADDR(223, 5, 5, 5) };

static bool s_wifi_stack_initialized = false;
static esp_netif_t *s_ap_netif = NULL;
static wifi_ap_callbacks_t s_callbacks = { 0 };

static void wifi_refresh_station_count(void)
{
    wifi_sta_list_t station_list = { 0 };
    esp_err_t err = esp_wifi_ap_get_sta_list(&station_list);

    if (err == ESP_OK) {
        app_state_set_connected_sta_count((uint8_t)station_list.num);
    } else {
        app_state_set_connected_sta_count(0);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_AP_START) {
        app_state_set_softap_started(true);
        app_state_set_connected_sta_count(0);
        ESP_LOGI(TAG, "Wi-Fi SoftAP started");
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        app_state_set_softap_started(false);
        app_state_set_connected_sta_count(0);
        ESP_LOGI(TAG, "Wi-Fi SoftAP stopped");
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        wifi_refresh_station_count();
        ESP_LOGI(TAG, "Station connected, aid=%d", event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_refresh_station_count();
        ESP_LOGI(TAG, "Station disconnected");
    }
}

static void wifi_build_ap_config(const app_config_t *config, wifi_config_t *wifi_ap_config, uint8_t actual_channel)
{
    size_t ssid_len = strlen(config->ssid);
    size_t password_len = strlen(config->password);

    memset(wifi_ap_config, 0, sizeof(*wifi_ap_config));
    memcpy(wifi_ap_config->ap.ssid, config->ssid, ssid_len);
    memcpy(wifi_ap_config->ap.password, config->password, password_len);
    wifi_ap_config->ap.ssid_len = ssid_len;
    wifi_ap_config->ap.max_connection = APP_WIFI_MAX_STA;
    wifi_ap_config->ap.channel = actual_channel;
    wifi_ap_config->ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_ap_config->ap.pmf_cfg.required = false;
}

static esp_err_t wifi_ap_apply_internal(const app_config_t *config)
{
    wifi_config_t wifi_ap_config;
    uint8_t actual_channel = config->channel;
    uint8_t offer_dns = 1;
    esp_err_t err;

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return err;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_build_ap_config(config, &wifi_ap_config, actual_channel);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap_netif,
                                           ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns,
                                           sizeof(offer_dns)));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(wifi_ap_set_dns_server(&s_default_dns));

    app_state_set_runtime_channel(actual_channel);
    app_state_set_softap_started(true);
    return ESP_OK;
}

static void wifi_reconfigure_task(void *arg)
{
    app_config_t config;
    esp_err_t err;

    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(1000));
    app_state_get_config(&config);
    err = wifi_ap_apply_internal(&config);
    app_state_set_wifi_reconfigure_pending(false);

    if (s_callbacks.on_reconfigure_done != NULL) {
        s_callbacks.on_reconfigure_done(err);
    }

    vTaskDelete(NULL);
}

esp_err_t wifi_ap_init(const wifi_ap_callbacks_t *callbacks)
{
    if (s_wifi_stack_initialized) {
        return ESP_OK;
    }

    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    s_wifi_stack_initialized = true;
    return ESP_OK;
}

esp_netif_t *wifi_ap_get_ap_netif(void)
{
    return s_ap_netif;
}

esp_err_t wifi_ap_apply_config(const app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return wifi_ap_apply_internal(config);
}

esp_err_t wifi_ap_schedule_reconfigure(void)
{
    BaseType_t create_result;
    app_state_snapshot_t snapshot;

    app_state_get_snapshot(&snapshot);
    if (snapshot.wifi_reconfigure_pending) {
        return ESP_OK;
    }

    app_state_set_wifi_reconfigure_pending(true);
    create_result = xTaskCreatePinnedToCore(wifi_reconfigure_task,
                                            "wifi_reconfig",
                                            4096,
                                            NULL,
                                            APP_TASK_PRIO_WEB_AUX,
                                            NULL,
                                            APP_CORE_APP);
    if (create_result != pdPASS) {
        app_state_set_wifi_reconfigure_pending(false);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t wifi_ap_set_dns_server(const esp_ip4_addr_t *dns_addr)
{
    esp_netif_dns_info_t dns_info = { 0 };

    if (s_ap_netif == NULL || dns_addr == NULL || dns_addr->addr == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4 = *dns_addr;
    return esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
}

void wifi_ap_stop(void)
{
    if (s_wifi_stack_initialized) {
        (void)esp_wifi_stop();
    }
}

uint8_t wifi_ap_get_runtime_channel(void)
{
    app_state_snapshot_t snapshot;
    app_state_get_snapshot(&snapshot);
    return snapshot.runtime_channel;
}
