/*
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头；取消自动信道接口，仅保留手动热点配置。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "esp_netif.h"

typedef struct {
    void (*on_reconfigure_done)(esp_err_t err);
} wifi_ap_callbacks_t;

esp_err_t wifi_ap_init(const wifi_ap_callbacks_t *callbacks);
esp_netif_t *wifi_ap_get_ap_netif(void);
esp_err_t wifi_ap_apply_config(const app_config_t *config);
esp_err_t wifi_ap_schedule_reconfigure(void);
esp_err_t wifi_ap_set_dns_server(const esp_ip4_addr_t *dns_addr);
void wifi_ap_stop(void);
uint8_t wifi_ap_get_runtime_channel(void);
