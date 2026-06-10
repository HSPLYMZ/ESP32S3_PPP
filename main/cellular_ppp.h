/*
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头，保留 4G 双接口与 APN 配置接口。
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "app_config.h"

typedef struct {
    bool usb_connected;
    bool at_ready;
    bool ppp_connected;
    bool napt_enabled;
    bool redial_pending;
    char ppp_ip[16];
    char dns[16];
    char dial_status[48];
    char sim_status[32];
    char signal_csq[32];
    char cereg_status[64];
    char network_info[96];
    char last_error[64];
} cellular_status_t;

esp_err_t cellular_ppp_start(esp_netif_t *ap_netif);
void cellular_ppp_get_status(cellular_status_t *status);
esp_err_t cellular_ppp_request_redial(void);
esp_err_t cellular_ppp_apply_config(const app_config_t *config);
