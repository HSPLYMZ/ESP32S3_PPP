/*
 * Version V1.2, last modified: 2026.06.08, update: pin USB Host, CDC driver, and cellular PPP tasks to Core0.
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头，保留 EC200A AT/PPP 双接口与 APN 配置逻辑。
 * 版本V1.1，最后修改时间：2026.06.05 11.23，更新内容：增强 PPP 拨号诊断日志；分离 MI_03 与 MI_04 的预拨号接收流；补充拨号失败恢复流程。
 */

#include "cellular_ppp.h"

#include <stdio.h>
#include <string.h>

#include "app_tasks.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "sdkconfig.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "wifi_ap.h"

#define EC200A_USB_VID 0x2C7C
#define EC200A_USB_PID 0x6005
#define EC200A_AT_INTERFACE 3
#define EC200A_PPP_INTERFACE 4

#define CDC_IN_BUFFER_SIZE 1024
#define CDC_OUT_BUFFER_SIZE 1024
#define AT_RX_BUFFER_SIZE 2048
#define PPP_RX_BUFFER_SIZE 2048
#define AT_TX_TIMEOUT_MS 1000
#define AT_RESPONSE_TIMEOUT_MS 3000
#define DIAL_RESPONSE_TIMEOUT_MS 15000
#define REDIAL_POLL_MS 200
#define RECOVER_DELAY_MS 3000
#define AT_READY_RETRY_COUNT 3
#define AT_RECOVERY_DELAY_MS 500
#define PPP_PORT_PROBE_RETRY_DELAY_MS 1200
#define ESCAPE_GUARD_MS 1200
#define CGATT_WAIT_RETRY_COUNT 20
#define CGATT_WAIT_INTERVAL_MS 1000
#define PRE_DIAL_SETTLE_MS 1000

static const char *TAG = "cellular_ppp";

typedef enum {
    CDC_PATH_AT = 0,
    CDC_PATH_PPP = 1,
} cdc_path_t;

typedef struct {
    cdc_path_t path;
} cdc_user_ctx_t;

static cdc_acm_dev_hdl_t s_at_cdc_dev = NULL;
static cdc_acm_dev_hdl_t s_ppp_cdc_dev = NULL;
static esp_netif_t *s_ppp_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static StreamBufferHandle_t s_at_rx_stream = NULL;
static StreamBufferHandle_t s_ppp_rx_stream = NULL;
static SemaphoreHandle_t s_disconnect_sem = NULL;
static bool s_ppp_mode = false;
static bool s_started = false;
static cdc_path_t s_ppp_data_path = CDC_PATH_PPP;
static volatile bool s_redial_requested = false;
static volatile uint8_t s_usb_dev_addr = CDC_HOST_ANY_DEV_ADDR;
static cdc_user_ctx_t s_at_user_ctx = { .path = CDC_PATH_AT };
static cdc_user_ctx_t s_ppp_user_ctx = { .path = CDC_PATH_PPP };
static app_config_t s_runtime_config = { 0 };
static cellular_status_t s_status = { 0 };
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static void status_set_defaults(void)
{
    portENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.last_error,
            "Waiting for EC200A USB wiring and USB Host bring-up.",
            sizeof(s_status.last_error));
    strlcpy(s_status.dial_status, "等待 4G 模组接入", sizeof(s_status.dial_status));
    strlcpy(s_status.sim_status, "--", sizeof(s_status.sim_status));
    strlcpy(s_status.signal_csq, "--", sizeof(s_status.signal_csq));
    strlcpy(s_status.cereg_status, "--", sizeof(s_status.cereg_status));
    strlcpy(s_status.network_info, "--", sizeof(s_status.network_info));
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_error(const char *message)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.last_error, message != NULL ? message : "", sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_status_lock);
    if (message != NULL && message[0] != '\0') {
        ESP_LOGW(TAG, "%s", message);
    }
}

static void status_set_error_response(const char *prefix, const char *response)
{
    char message[sizeof(s_status.last_error)];

    if (response != NULL && response[0] != '\0') {
        snprintf(message, sizeof(message), "%s %s", prefix, response);
    } else {
        strlcpy(message, prefix, sizeof(message));
    }

    status_set_error(message);
}

static void status_set_usb_connected(bool usb_connected)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.usb_connected = usb_connected;
    if (!usb_connected) {
        s_status.at_ready = false;
        s_status.ppp_connected = false;
        s_status.napt_enabled = false;
        s_status.redial_pending = false;
        s_status.ppp_ip[0] = '\0';
        s_status.dns[0] = '\0';
        strlcpy(s_status.dial_status, "等待 4G 模组接入", sizeof(s_status.dial_status));
        strlcpy(s_status.sim_status, "--", sizeof(s_status.sim_status));
        strlcpy(s_status.signal_csq, "--", sizeof(s_status.signal_csq));
        strlcpy(s_status.cereg_status, "--", sizeof(s_status.cereg_status));
        strlcpy(s_status.network_info, "--", sizeof(s_status.network_info));
    }
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_at_ready(bool at_ready)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.at_ready = at_ready;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_ppp_connected(bool ppp_connected)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.ppp_connected = ppp_connected;
    if (!ppp_connected) {
        s_status.napt_enabled = false;
        s_status.redial_pending = false;
        s_status.ppp_ip[0] = '\0';
        s_status.dns[0] = '\0';
        strlcpy(s_status.dial_status, "PPP 未连接", sizeof(s_status.dial_status));
    }
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_ip_dns(const char *ip, const char *dns)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.ppp_ip, ip, sizeof(s_status.ppp_ip));
    strlcpy(s_status.dns, dns, sizeof(s_status.dns));
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_napt_enabled(bool napt_enabled)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.napt_enabled = napt_enabled;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_redial_pending(bool redial_pending)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.redial_pending = redial_pending;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_text_field(char *field, size_t field_len, const char *value)
{
    if (field == NULL || field_len == 0) {
        return;
    }

    if (value == NULL || value[0] == '\0') {
        strlcpy(field, "--", field_len);
    } else {
        strlcpy(field, value, field_len);
    }
}

static void status_set_dial_status(const char *value)
{
    portENTER_CRITICAL(&s_status_lock);
    status_set_text_field(s_status.dial_status, sizeof(s_status.dial_status), value);
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_sim_status(const char *value)
{
    portENTER_CRITICAL(&s_status_lock);
    status_set_text_field(s_status.sim_status, sizeof(s_status.sim_status), value);
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_signal_csq(const char *value)
{
    portENTER_CRITICAL(&s_status_lock);
    status_set_text_field(s_status.signal_csq, sizeof(s_status.signal_csq), value);
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_cereg_status(const char *value)
{
    portENTER_CRITICAL(&s_status_lock);
    status_set_text_field(s_status.cereg_status, sizeof(s_status.cereg_status), value);
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_network_info(const char *value)
{
    portENTER_CRITICAL(&s_status_lock);
    status_set_text_field(s_status.network_info, sizeof(s_status.network_info), value);
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t enable_softap_napt(void)
{
#if IP_NAPT
    if (s_ap_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_netif_napt_enable(s_ap_netif);
    if (err == ESP_OK) {
        status_set_napt_enabled(true);
        ESP_LOGI(TAG, "NAPT enabled on SoftAP");
    } else {
        status_set_napt_enabled(false);
        ESP_LOGE(TAG, "Failed to enable NAPT: %s", esp_err_to_name(err));
    }
    return err;
#else
    status_set_napt_enabled(false);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t ppp_transmit(void *handle, void *buffer, size_t len)
{
    (void)handle;

    cdc_acm_dev_hdl_t active_ppp_dev = (s_ppp_data_path == CDC_PATH_AT) ? s_at_cdc_dev : s_ppp_cdc_dev;

    if (active_ppp_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return cdc_acm_host_data_tx_blocking(active_ppp_dev, (const uint8_t *)buffer, len, AT_TX_TIMEOUT_MS);
}

static const esp_netif_driver_ifconfig_t s_ppp_driver_cfg = {
    .handle = (void *)1,
    .transmit = ppp_transmit,
};

static bool cdc_rx_callback(const uint8_t *data, size_t data_len, void *arg)
{
    cdc_user_ctx_t *ctx = (cdc_user_ctx_t *)arg;

    if (ctx == NULL) {
        return true;
    }

    if (ctx->path == CDC_PATH_PPP) {
        if (s_ppp_mode && s_ppp_netif != NULL && s_ppp_data_path == CDC_PATH_PPP) {
            esp_netif_receive(s_ppp_netif, (void *)data, data_len, NULL);
        } else if (s_ppp_rx_stream != NULL) {
            xStreamBufferSend(s_ppp_rx_stream, data, data_len, 0);
        }
    } else if (s_ppp_mode && s_ppp_netif != NULL && s_ppp_data_path == CDC_PATH_AT) {
        esp_netif_receive(s_ppp_netif, (void *)data, data_len, NULL);
    } else if (s_at_rx_stream != NULL) {
        xStreamBufferSend(s_at_rx_stream, data, data_len, 0);
    }

    return true;
}

static void close_modem_handles(void)
{
    if (s_at_cdc_dev != NULL) {
        cdc_acm_host_close(s_at_cdc_dev);
        s_at_cdc_dev = NULL;
    }

    if (s_ppp_cdc_dev != NULL) {
        cdc_acm_host_close(s_ppp_cdc_dev);
        s_ppp_cdc_dev = NULL;
    }
}

static void reset_rx_streams(void)
{
    if (s_at_rx_stream != NULL) {
        xStreamBufferReset(s_at_rx_stream);
    }
    if (s_ppp_rx_stream != NULL) {
        xStreamBufferReset(s_ppp_rx_stream);
    }
}

static void cdc_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    cdc_user_ctx_t *ctx = (cdc_user_ctx_t *)user_ctx;

    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
        status_set_error("CDC-ACM host error.");
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "EC200A USB disconnected on path %d", ctx != NULL ? ctx->path : -1);
        s_ppp_mode = false;
        status_set_usb_connected(false);
        status_set_error("EC200A USB disconnected.");
        close_modem_handles();
        xSemaphoreGive(s_disconnect_sem);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "Serial state: 0x%04X", event->data.serial_state.val);
        break;
    default:
        ESP_LOGD(TAG, "CDC event: %d", event->type);
        break;
    }
}

static void usb_new_device_callback(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *dev_desc = NULL;
    usb_device_info_t dev_info;

    if (usb_host_get_device_descriptor(usb_dev, &dev_desc) != ESP_OK || dev_desc == NULL) {
        return;
    }

    if (dev_desc->idVendor != EC200A_USB_VID || dev_desc->idProduct != EC200A_USB_PID) {
        return;
    }

    if (usb_host_device_info(usb_dev, &dev_info) == ESP_OK) {
        s_usb_dev_addr = dev_info.dev_addr;
        ESP_LOGI(TAG, "Detected EC200A USB device at address %u", s_usb_dev_addr);
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static bool wait_for_text_in_stream(StreamBufferHandle_t stream,
                                    const char *needle,
                                    uint32_t timeout_ms,
                                    char *capture,
                                    size_t capture_len)
{
    int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    size_t used = 0;

    if (capture != NULL && capture_len > 0) {
        capture[0] = '\0';
    }

    while (esp_timer_get_time() < deadline) {
        uint8_t chunk[128];
        size_t got = xStreamBufferReceive(stream, chunk, sizeof(chunk) - 1, pdMS_TO_TICKS(100));
        if (got == 0) {
            continue;
        }

        chunk[got] = '\0';
        if (capture != NULL && capture_len > 1) {
            size_t copy = got;
            if (copy > capture_len - used - 1) {
                copy = capture_len - used - 1;
            }
            if (copy > 0) {
                memcpy(capture + used, chunk, copy);
                used += copy;
                capture[used] = '\0';
            }
            if (strstr(capture, needle) != NULL) {
                return true;
            }
        } else if (strstr((const char *)chunk, needle) != NULL) {
            return true;
        }
    }

    return false;
}

static esp_err_t send_wait_on_port(const char *port_name,
                                   cdc_acm_dev_hdl_t dev,
                                   StreamBufferHandle_t stream,
                                   const char *command,
                                   const char *expected,
                                   uint32_t timeout_ms,
                                   char *response,
                                   size_t response_len)
{
    if (dev == NULL || stream == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xStreamBufferReset(stream);
    if (response != NULL && response_len > 0) {
        response[0] = '\0';
    }

    ESP_LOGI(TAG, "%s> %s", port_name, command);
    ESP_RETURN_ON_ERROR(cdc_acm_host_data_tx_blocking(dev,
                                                      (const uint8_t *)command,
                                                      strlen(command),
                                                      AT_TX_TIMEOUT_MS),
                        TAG,
                        "Port tx failed");

    if (!wait_for_text_in_stream(stream, expected, timeout_ms, response, response_len)) {
        ESP_LOGE(TAG,
                 "%s timeout, expected '%s', got: %s",
                 port_name,
                 expected,
                 response != NULL ? response : "");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "%s response matched '%s'", port_name, expected);
    return ESP_OK;
}

static esp_err_t send_at_wait(const char *command, const char *expected, uint32_t timeout_ms)
{
    char response[512];
    return send_wait_on_port("AT", s_at_cdc_dev, s_at_rx_stream, command, expected, timeout_ms, response, sizeof(response));
}

static esp_err_t send_path_wait(cdc_path_t path,
                                const char *command,
                                const char *expected,
                                uint32_t timeout_ms,
                                char *response,
                                size_t response_len)
{
    if (path == CDC_PATH_AT) {
        return send_wait_on_port("AT", s_at_cdc_dev, s_at_rx_stream, command, expected, timeout_ms, response, response_len);
    }

    return send_wait_on_port("PPP", s_ppp_cdc_dev, s_ppp_rx_stream, command, expected, timeout_ms, response, response_len);
}

static esp_err_t send_ppp_wait(const char *command,
                               const char *expected,
                               uint32_t timeout_ms,
                               char *response,
                               size_t response_len)
{
    return send_wait_on_port("PPP", s_ppp_cdc_dev, s_ppp_rx_stream, command, expected, timeout_ms, response, response_len);
}

static esp_err_t send_at_capture(const char *command,
                                 const char *expected,
                                 uint32_t timeout_ms,
                                 char *response,
                                 size_t response_len)
{
    return send_wait_on_port("AT", s_at_cdc_dev, s_at_rx_stream, command, expected, timeout_ms, response, response_len);
}

static esp_err_t transmit_raw(cdc_acm_dev_hdl_t dev, const char *data)
{
    if (dev == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)data, strlen(data), AT_TX_TIMEOUT_MS);
}

static esp_err_t ensure_at_ready(void)
{
    char response[256];
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < AT_READY_RETRY_COUNT; ++attempt) {
        err = send_at_wait("AT\r", "OK", AT_RESPONSE_TIMEOUT_MS);
        if (err == ESP_OK) {
            return send_at_wait("ATE0\r", "OK", AT_RESPONSE_TIMEOUT_MS);
        }

        ESP_LOGW(TAG, "AT handshake attempt %d/%d failed", attempt + 1, AT_READY_RETRY_COUNT);
        if (s_at_cdc_dev != NULL) {
            (void)send_at_capture("ATH\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response));
        }
        vTaskDelay(pdMS_TO_TICKS(AT_RECOVERY_DELAY_MS));
    }

    return err;
}

static void escape_data_mode(cdc_acm_dev_hdl_t dev,
                             StreamBufferHandle_t stream,
                             const char *port_name)
{
    char response[256];

    if (dev == NULL || stream == NULL) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(ESCAPE_GUARD_MS));
    xStreamBufferReset(stream);
    if (transmit_raw(dev, "+++") == ESP_OK) {
        ESP_LOGI(TAG, "%s escape sequence sent", port_name);
    }
    vTaskDelay(pdMS_TO_TICKS(ESCAPE_GUARD_MS));
    (void)send_wait_on_port(port_name, dev, stream, "ATH\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response));
}

static esp_err_t wait_for_packet_attach(void)
{
    char response[256];

    for (int attempt = 0; attempt < CGATT_WAIT_RETRY_COUNT; ++attempt) {
        if (send_at_capture("AT+CGATT?\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response)) == ESP_OK) {
            if (strstr(response, "+CGATT: 1") != NULL) {
                vTaskDelay(pdMS_TO_TICKS(PRE_DIAL_SETTLE_MS));
                return ESP_OK;
            }
        }

        ESP_LOGW(TAG, "Packet attach not ready yet, attempt %d/%d", attempt + 1, CGATT_WAIT_RETRY_COUNT);
        vTaskDelay(pdMS_TO_TICKS(CGATT_WAIT_INTERVAL_MS));
    }

    status_set_error("Packet domain attach timeout.");
    return ESP_ERR_TIMEOUT;
}

static void compact_spaces(char *text)
{
    char *read_ptr = text;
    char *write_ptr = text;
    bool last_space = false;

    while (read_ptr != NULL && *read_ptr != '\0') {
        char current = *read_ptr++;
        bool is_space = (current == '\r' || current == '\n' || current == '\t' || current == ' ');

        if (is_space) {
            if (!last_space && write_ptr != text) {
                *write_ptr++ = ' ';
            }
            last_space = true;
        } else {
            *write_ptr++ = current;
            last_space = false;
        }
    }

    if (write_ptr > text && write_ptr[-1] == ' ') {
        write_ptr--;
    }
    *write_ptr = '\0';
}

static bool extract_prefixed_value(const char *response,
                                   const char *prefix,
                                   char *value,
                                   size_t value_len)
{
    const char *begin = strstr(response, prefix);
    const char *end = NULL;
    size_t copy_len = 0;

    if (begin == NULL || value == NULL || value_len == 0) {
        return false;
    }

    begin += strlen(prefix);
    while (*begin == ' ' || *begin == '\t') {
        begin++;
    }

    end = strpbrk(begin, "\r\n");
    if (end == NULL) {
        end = begin + strlen(begin);
    }

    copy_len = (size_t)(end - begin);
    if (copy_len >= value_len) {
        copy_len = value_len - 1;
    }

    memcpy(value, begin, copy_len);
    value[copy_len] = '\0';
    compact_spaces(value);
    return value[0] != '\0';
}

static void update_radio_status_snapshot(void)
{
    char response[512];
    char parsed[128];

    if (send_at_capture("AT+CPIN?\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response)) == ESP_OK &&
        extract_prefixed_value(response, "+CPIN:", parsed, sizeof(parsed))) {
        status_set_sim_status(parsed);
    } else {
        status_set_sim_status("读取失败");
    }

    if (send_at_capture("AT+CSQ\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response)) == ESP_OK &&
        extract_prefixed_value(response, "+CSQ:", parsed, sizeof(parsed))) {
        status_set_signal_csq(parsed);
    } else {
        status_set_signal_csq("读取失败");
    }

    if (send_at_capture("AT+CEREG?\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response)) == ESP_OK &&
        extract_prefixed_value(response, "+CEREG:", parsed, sizeof(parsed))) {
        status_set_cereg_status(parsed);
    } else {
        status_set_cereg_status("读取失败");
    }

    if (send_at_capture("AT+QNWINFO\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response)) == ESP_OK &&
        extract_prefixed_value(response, "+QNWINFO:", parsed, sizeof(parsed))) {
        status_set_network_info(parsed);
    } else {
        status_set_network_info("读取失败");
    }
}

static void request_current_link_redial(const char *reason)
{
    status_set_dial_status("正在断开 PPP");
    status_set_redial_pending(true);
    s_redial_requested = true;
    status_set_error(reason);
}

static void ppp_status_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case NETIF_PPP_ERRORAUTHFAIL:
        status_set_dial_status("PPP authentication failed");
        status_set_error("PPP authentication failed.");
        break;
    case NETIF_PPP_ERRORPROTOCOL:
        status_set_dial_status("PPP protocol failed");
        status_set_error("PPP protocol negotiation failed.");
        break;
    case NETIF_PPP_ERRORPEERDEAD:
        status_set_dial_status("PPP peer timeout");
        status_set_error("PPP peer timeout.");
        break;
    case NETIF_PPP_ERRORCONNECT:
        status_set_dial_status("PPP link lost");
        status_set_error("PPP connection lost.");
        break;
    case NETIF_PPP_CONNECT_FAILED:
        status_set_dial_status("PPP stack start failed");
        status_set_error("PPP stack failed to start.");
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_dns_info_t dns_info = { 0 };
        char ip[16];
        char dns[16];

        esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(dns, sizeof(dns), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));

        status_set_ppp_connected(true);
        status_set_ip_dns(ip, dns);
        status_set_redial_pending(false);
        status_set_dial_status("拨号成功");
        status_set_error("");

        ESP_LOGI(TAG, "PPP online, ip=%s dns=%s", ip, dns);
        esp_netif_set_default_netif(event->esp_netif);
        if (wifi_ap_set_dns_server(&dns_info.ip.u_addr.ip4) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to update SoftAP DNS server");
        }
        enable_softap_napt();
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        status_set_ppp_connected(false);
        status_set_dial_status("PPP 已断开");
        status_set_error("PPP lost IP.");
        ESP_LOGW(TAG, "PPP lost IP");
    }
}

static esp_err_t create_ppp_netif(void)
{
    esp_netif_ppp_config_t ppp_config = {
        .ppp_phase_event_enabled = false,
        .ppp_error_event_enabled = true,
    };

    if (s_ppp_netif != NULL) {
        return ESP_OK;
    }

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base_cfg.if_desc = "ec200a_ppp";

    esp_netif_config_t netif_cfg = {
        .base = &base_cfg,
        .driver = &s_ppp_driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP,
    };

    s_ppp_netif = esp_netif_new(&netif_cfg);
    if (s_ppp_netif == NULL) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_params(s_ppp_netif, &ppp_config), TAG, "Failed to set PPP params");
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_auth(s_ppp_netif, NETIF_PPP_AUTHTYPE_NONE, NULL, NULL), TAG, "Failed to set PPP auth");
    return ESP_OK;
}

static esp_err_t open_modem_interfaces(void)
{
    cdc_acm_host_open_config_t at_open_cfg = {
        .vid = EC200A_USB_VID,
        .pid = EC200A_USB_PID,
        .interface_idx = EC200A_AT_INTERFACE,
        .dev_addr = s_usb_dev_addr,
        .connection_timeout_ms = 5000,
        .out_buffer_size = CDC_OUT_BUFFER_SIZE,
        .in_buffer_size = CDC_IN_BUFFER_SIZE,
        .event_cb = cdc_event_callback,
        .data_cb = cdc_rx_callback,
        .user_arg = &s_at_user_ctx,
    };
    cdc_acm_host_open_config_t ppp_open_cfg = {
        .vid = EC200A_USB_VID,
        .pid = EC200A_USB_PID,
        .interface_idx = EC200A_PPP_INTERFACE,
        .dev_addr = s_usb_dev_addr,
        .connection_timeout_ms = 5000,
        .out_buffer_size = CDC_OUT_BUFFER_SIZE,
        .in_buffer_size = CDC_IN_BUFFER_SIZE,
        .event_cb = cdc_event_callback,
        .data_cb = cdc_rx_callback,
        .user_arg = &s_ppp_user_ctx,
    };
    esp_err_t err;

    err = cdc_acm_host_open(&at_open_cfg, &s_at_cdc_dev);
    if (err != ESP_OK) {
        return err;
    }

    err = cdc_acm_host_open(&ppp_open_cfg, &s_ppp_cdc_dev);
    if (err != ESP_OK) {
        cdc_acm_host_close(s_at_cdc_dev);
        s_at_cdc_dev = NULL;
        return err;
    }

    cdc_acm_host_desc_print(s_at_cdc_dev);
    cdc_acm_host_set_control_line_state(s_at_cdc_dev, true, true);
    cdc_acm_host_set_control_line_state(s_ppp_cdc_dev, true, true);
    return ESP_OK;
}

static void recover_after_dial_failure(void)
{
    if (s_ppp_data_path == CDC_PATH_AT) {
        escape_data_mode(s_at_cdc_dev, s_at_rx_stream, "AT");
    } else {
        if (s_at_cdc_dev != NULL) {
            char response[256];
            (void)send_at_capture("ATH\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response));
        }
        escape_data_mode(s_ppp_cdc_dev, s_ppp_rx_stream, "PPP");
    }

    s_ppp_data_path = CDC_PATH_PPP;
    close_modem_handles();
    reset_rx_streams();
}

static esp_err_t probe_ppp_command_port(void)
{
    char response[256];
    esp_err_t err;

    err = send_ppp_wait("AT\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PPP command port probe failed, response=%s", response);
        status_set_error_response("PPP port probe failed.", response);
        return err;
    }

    err = send_ppp_wait("ATE0\r", "OK", AT_RESPONSE_TIMEOUT_MS, response, sizeof(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PPP command port ATE0 failed, response=%s", response);
        status_set_error_response("PPP port ATE0 failed.", response);
    }

    return err;
}

static esp_err_t dial_ppp_data_mode(cdc_path_t path)
{
    char response[512];
    esp_err_t err = send_path_wait(path, "ATD*99***1#\r", "CONNECT", DIAL_RESPONSE_TIMEOUT_MS, response, sizeof(response));

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s dial timeout or failure, response=%s", path == CDC_PATH_AT ? "AT" : "PPP", response);
        status_set_error_response(path == CDC_PATH_AT ? "AT dial failed." : "PPP dial failed.", response);
    } else {
        ESP_LOGI(TAG, "%s dial CONNECT response=%s", path == CDC_PATH_AT ? "AT" : "PPP", response);
    }

    return err;
}

static esp_err_t try_dial_sequence(void)
{
    esp_err_t err;

    s_ppp_data_path = CDC_PATH_PPP;
    status_set_dial_status("Validating PPP port");
    err = probe_ppp_command_port();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Retrying PPP port probe after settle delay");
        vTaskDelay(pdMS_TO_TICKS(PPP_PORT_PROBE_RETRY_DELAY_MS));
        err = probe_ppp_command_port();
    }
    if (err == ESP_OK) {
        status_set_dial_status("Dialing on PPP port");
        status_set_error("AT ready, dialing PPP on data port.");
        err = dial_ppp_data_mode(CDC_PATH_PPP);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Falling back to AT port for PPP data session");
    s_ppp_data_path = CDC_PATH_AT;
    status_set_dial_status("Fallback to AT port");
    status_set_error("Falling back to AT port for PPP dial.");
    err = send_at_wait("ATE0\r", "OK", AT_RESPONSE_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    status_set_dial_status("Dialing on AT port");
    status_set_error("AT ready, dialing PPP on AT port.");
    return dial_ppp_data_mode(CDC_PATH_AT);
}

static void cellular_task(void *arg)
{
    (void)arg;

    while (true) {
        status_set_usb_connected(false);
        status_set_at_ready(false);
        status_set_ppp_connected(false);
        status_set_napt_enabled(false);
        status_set_dial_status("等待 4G 模组接入");
        status_set_error("Waiting for EC200A USB device.");

        ESP_LOGI(TAG, "Opening EC200A 0x%04X:0x%04X AT=%d PPP=%d addr=%u",
                 EC200A_USB_VID, EC200A_USB_PID, EC200A_AT_INTERFACE, EC200A_PPP_INTERFACE, s_usb_dev_addr);

        esp_err_t err = open_modem_interfaces();
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        reset_rx_streams();
        status_set_usb_connected(true);
        status_set_dial_status("正在建立 AT 链路");
        status_set_error("");

        err = ensure_at_ready();
        if (err == ESP_OK) {
            char apn_cmd[96];
            snprintf(apn_cmd,
                     sizeof(apn_cmd),
                     "AT+CGDCONT=1,\"IP\",\"%s\"\r",
                     app_config_get_effective_apn(&s_runtime_config));
            err = send_at_wait(apn_cmd, "OK", AT_RESPONSE_TIMEOUT_MS);
        }
        if (err == ESP_OK) {
            err = wait_for_packet_attach();
        }
        if (err == ESP_OK) {
            status_set_at_ready(true);
            status_set_dial_status("AT 已就绪");
            update_radio_status_snapshot();
            err = create_ppp_netif();
        }
        if (err == ESP_OK) {
            status_set_dial_status("正在验证 PPP 口");
            err = try_dial_sequence();
        }
        if (err == ESP_OK) {
            s_ppp_mode = true;
            esp_netif_action_start(s_ppp_netif, 0, 0, 0);
            esp_netif_action_connected(s_ppp_netif, 0, 0, 0);

            while (true) {
                if (xSemaphoreTake(s_disconnect_sem, pdMS_TO_TICKS(REDIAL_POLL_MS)) == pdTRUE) {
                    break;
                }

                if (s_redial_requested) {
                    ESP_LOGW(TAG, "4G redial requested, closing current data session");
                    s_ppp_mode = false;
                    status_set_ppp_connected(false);
                    status_set_napt_enabled(false);
                    status_set_dial_status("正在断开 PPP");
                    recover_after_dial_failure();
                    xSemaphoreTake(s_disconnect_sem, pdMS_TO_TICKS(1000));
                    break;
                }
            }
        } else {
            status_set_dial_status("拨号失败");
            status_set_error(esp_err_to_name(err));
            recover_after_dial_failure();
            vTaskDelay(pdMS_TO_TICKS(RECOVER_DELAY_MS));
        }

        s_redial_requested = false;
        status_set_redial_pending(false);
        s_ppp_mode = false;
    }
}

esp_err_t cellular_ppp_start(esp_netif_t *ap_netif)
{
    BaseType_t ok;

    if (s_started) {
        return ESP_OK;
    }

    s_ap_netif = ap_netif;
    app_config_set_defaults(&s_runtime_config);
    status_set_defaults();

    s_at_rx_stream = xStreamBufferCreate(AT_RX_BUFFER_SIZE, 1);
    s_ppp_rx_stream = xStreamBufferCreate(PPP_RX_BUFFER_SIZE, 1);
    s_disconnect_sem = xSemaphoreCreateBinary();
    if (s_at_rx_stream == NULL || s_ppp_rx_stream == NULL || s_disconnect_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, ppp_status_event_handler, NULL));

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    ok = xTaskCreatePinnedToCore(usb_lib_task,
                                 "usb_lib",
                                 4096,
                                 NULL,
                                 APP_TASK_PRIO_USB_HOST,
                                 NULL,
                                 APP_CORE_NETWORK);
    if (ok != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    const cdc_acm_host_driver_config_t cdc_driver_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = APP_TASK_PRIO_USB_CDC,
        .xCoreID = APP_CORE_NETWORK,
        .new_dev_cb = usb_new_device_callback,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&cdc_driver_cfg));

    ok = xTaskCreatePinnedToCore(cellular_task,
                                 "cellular_ppp",
                                 7168,
                                 NULL,
                                 APP_TASK_PRIO_CELLULAR,
                                 NULL,
                                 APP_CORE_NETWORK);
    if (ok != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

void cellular_ppp_get_status(cellular_status_t *status)
{
    if (status == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

esp_err_t cellular_ppp_request_redial(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    request_current_link_redial("4G redial requested from WebUI.");
    return ESP_OK;
}

esp_err_t cellular_ppp_apply_config(const app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_runtime_config = *config;
    return ESP_OK;
}
