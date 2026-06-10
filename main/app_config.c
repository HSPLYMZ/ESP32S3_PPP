/*
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头；取消 Wi-Fi 自动信道配置，恢复手动信道配置。
 */

#include "app_config.h"

#include <string.h>

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

#define APP_CFG_NAMESPACE "wifi_cfg"
#define APP_KEY_SSID "softap_ssid"
#define APP_KEY_PASS "softap_pass"
#define APP_KEY_CHANNEL "softap_ch"
#define APP_KEY_APN_PROFILE "apn_profile"
#define APP_KEY_APN_CUSTOM "apn_custom"

typedef struct {
    app_apn_profile_t profile;
    const char *label;
    const char *apn;
} apn_profile_desc_t;

static const apn_profile_desc_t s_apn_profiles[] = {
    { APP_APN_PROFILE_MOBILE, "中国移动", "cmnet" },
    { APP_APN_PROFILE_UNICOM, "中国联通", "3gnet" },
    { APP_APN_PROFILE_TELECOM, "中国电信", "ctnet" },
    { APP_APN_PROFILE_CUSTOM, "自定义", "" },
};

static const apn_profile_desc_t *find_apn_profile_desc(app_apn_profile_t profile)
{
    for (size_t i = 0; i < sizeof(s_apn_profiles) / sizeof(s_apn_profiles[0]); ++i) {
        if (s_apn_profiles[i].profile == profile) {
            return &s_apn_profiles[i];
        }
    }
    return NULL;
}

static esp_err_t load_string_or_default(nvs_handle_t handle,
                                        const char *key,
                                        char *dest,
                                        size_t dest_size,
                                        const char *default_value)
{
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);

    if (err == ESP_OK && required_size <= dest_size) {
        return nvs_get_str(handle, key, dest, &required_size);
    }

    if (err == ESP_ERR_NVS_NOT_FOUND || required_size > dest_size) {
        ESP_RETURN_ON_ERROR(nvs_set_str(handle, key, default_value), "app_config", "set default string");
        strlcpy(dest, default_value, dest_size);
        return ESP_OK;
    }

    return err;
}

esp_err_t app_config_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_config_set_defaults(app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    strlcpy(config->ssid, APP_WIFI_DEFAULT_SSID, sizeof(config->ssid));
    strlcpy(config->password, APP_WIFI_DEFAULT_PASS, sizeof(config->password));
    config->channel = APP_WIFI_DEFAULT_CHANNEL;
    config->apn_profile = APP_APN_PROFILE_MOBILE;
}

bool app_config_wifi_channel_is_valid(uint8_t channel)
{
    return channel == 1 || channel == 6 || channel == 11;
}

esp_err_t app_config_validate(const app_config_t *config, const char **error_message)
{
    size_t ssid_len = 0;
    size_t password_len = 0;

    if (config == NULL) {
        if (error_message != NULL) {
            *error_message = "配置对象为空。";
        }
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(config->ssid);
    password_len = strlen(config->password);

    if (ssid_len == 0 || ssid_len > 32) {
        if (error_message != NULL) {
            *error_message = "Wi-Fi 名称长度必须在 1 到 32 个字符之间。";
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (password_len < 8 || password_len > 63) {
        if (error_message != NULL) {
            *error_message = "Wi-Fi 密码长度必须在 8 到 63 个字符之间。";
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (!app_config_wifi_channel_is_valid(config->channel)) {
        if (error_message != NULL) {
            *error_message = "Wi-Fi 信道仅支持 1 / 6 / 11。";
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (config->apn_profile == APP_APN_PROFILE_CUSTOM && config->custom_apn[0] == '\0') {
        if (error_message != NULL) {
            *error_message = "自定义 APN 不能为空。";
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(config->custom_apn) >= sizeof(config->custom_apn)) {
        if (error_message != NULL) {
            *error_message = "自定义 APN 过长。";
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (error_message != NULL) {
        *error_message = NULL;
    }
    return ESP_OK;
}

esp_err_t app_config_load(app_config_t *config)
{
    nvs_handle_t handle;
    uint8_t stored_channel = APP_WIFI_DEFAULT_CHANNEL;
    uint8_t stored_profile = APP_APN_PROFILE_MOBILE;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_set_defaults(config);

    ESP_RETURN_ON_ERROR(nvs_open(APP_CFG_NAMESPACE, NVS_READWRITE, &handle), "app_config", "open nvs");

    err = load_string_or_default(handle, APP_KEY_SSID, config->ssid, sizeof(config->ssid), APP_WIFI_DEFAULT_SSID);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = load_string_or_default(handle, APP_KEY_PASS, config->password, sizeof(config->password), APP_WIFI_DEFAULT_PASS);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_get_u8(handle, APP_KEY_CHANNEL, &stored_channel);
    if (err == ESP_OK && app_config_wifi_channel_is_valid(stored_channel)) {
        config->channel = stored_channel;
    } else if (err == ESP_ERR_NVS_NOT_FOUND || !app_config_wifi_channel_is_valid(stored_channel)) {
        ESP_ERROR_CHECK(nvs_set_u8(handle, APP_KEY_CHANNEL, APP_WIFI_DEFAULT_CHANNEL));
        config->channel = APP_WIFI_DEFAULT_CHANNEL;
    } else {
        nvs_close(handle);
        return err;
    }

    err = nvs_get_u8(handle, APP_KEY_APN_PROFILE, &stored_profile);
    if (err == ESP_OK && find_apn_profile_desc((app_apn_profile_t)stored_profile) != NULL) {
        config->apn_profile = (app_apn_profile_t)stored_profile;
    } else if (err == ESP_ERR_NVS_NOT_FOUND || find_apn_profile_desc((app_apn_profile_t)stored_profile) == NULL) {
        config->apn_profile = APP_APN_PROFILE_MOBILE;
        ESP_ERROR_CHECK(nvs_set_u8(handle, APP_KEY_APN_PROFILE, (uint8_t)config->apn_profile));
    } else {
        nvs_close(handle);
        return err;
    }

    err = load_string_or_default(handle, APP_KEY_APN_CUSTOM, config->custom_apn, sizeof(config->custom_apn), "");
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *config)
{
    nvs_handle_t handle;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(nvs_open(APP_CFG_NAMESPACE, NVS_READWRITE, &handle), "app_config", "open nvs");
    ESP_ERROR_CHECK(nvs_set_str(handle, APP_KEY_SSID, config->ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, APP_KEY_PASS, config->password));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_KEY_CHANNEL, config->channel));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_KEY_APN_PROFILE, (uint8_t)config->apn_profile));
    ESP_ERROR_CHECK(nvs_set_str(handle, APP_KEY_APN_CUSTOM, config->custom_apn));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    return ESP_OK;
}

const char *app_config_get_apn_profile_label(app_apn_profile_t profile)
{
    const apn_profile_desc_t *desc = find_apn_profile_desc(profile);
    return desc != NULL ? desc->label : "未知";
}

const char *app_config_get_effective_apn(const app_config_t *config)
{
    const apn_profile_desc_t *desc;

    if (config == NULL) {
        return "";
    }

    if (config->apn_profile == APP_APN_PROFILE_CUSTOM) {
        return config->custom_apn;
    }

    desc = find_apn_profile_desc(config->apn_profile);
    return desc != NULL ? desc->apn : "cmnet";
}
