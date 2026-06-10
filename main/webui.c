/*
 * Version V1.2, last modified: 2026.06.08, update: pin HTTP server and WebUI helper tasks to Core1.
 * 版本V1.0，最后修改时间：2026.06.05 11.14，更新内容：建立文件版本注释头；WebUI 标题改为 S3控制台；新增版本号显示；移除自动信道配置项。
 */

#include "webui.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "app_tasks.h"
#include "cellular_ppp.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "system_monitor.h"
#include "wifi_ap.h"

#define WEB_FORM_BUFFER_SIZE 512
#define WEB_HTML_BUFFER_SIZE 2048

static const char *TAG = "webui";
static httpd_handle_t s_http_server = NULL;

const char *app_get_version(void);

static void device_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    webui_stop();
    wifi_ap_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static bool hex_value(char value, uint8_t *decoded)
{
    if (value >= '0' && value <= '9') {
        *decoded = (uint8_t)(value - '0');
        return true;
    }
    if (value >= 'a' && value <= 'f') {
        *decoded = (uint8_t)(value - 'a' + 10);
        return true;
    }
    if (value >= 'A' && value <= 'F') {
        *decoded = (uint8_t)(value - 'A' + 10);
        return true;
    }
    return false;
}

static void url_decode(char *dest, size_t dest_size, const char *src)
{
    size_t write_pos = 0;

    if (dest_size == 0) {
        return;
    }

    while (*src != '\0' && write_pos + 1 < dest_size) {
        if (*src == '+') {
            dest[write_pos++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            uint8_t high = 0;
            uint8_t low = 0;
            if (hex_value(src[1], &high) && hex_value(src[2], &low)) {
                dest[write_pos++] = (char)((high << 4) | low);
                src += 3;
            } else {
                dest[write_pos++] = *src++;
            }
        } else {
            dest[write_pos++] = *src++;
        }
    }

    dest[write_pos] = '\0';
}

static void parse_form_field(const char *form_data, const char *target_key, char *value, size_t value_size)
{
    char work_buffer[WEB_FORM_BUFFER_SIZE];
    char *save_ptr = NULL;
    char *token = NULL;

    value[0] = '\0';
    strlcpy(work_buffer, form_data, sizeof(work_buffer));

    for (token = strtok_r(work_buffer, "&", &save_ptr);
         token != NULL;
         token = strtok_r(NULL, "&", &save_ptr)) {
        char *separator = strchr(token, '=');
        char decoded_key[32];

        if (separator == NULL) {
            continue;
        }

        *separator = '\0';
        url_decode(decoded_key, sizeof(decoded_key), token);
        if (strcmp(decoded_key, target_key) == 0) {
            url_decode(value, value_size, separator + 1);
            return;
        }
    }
}

static uint32_t webui_clamp_refresh_seconds(uint32_t refresh_seconds)
{
    if (refresh_seconds == 10 || refresh_seconds == 100) {
        return refresh_seconds;
    }
    return 5;
}

static uint32_t webui_parse_refresh_value(const char *text)
{
    uint32_t refresh_seconds = 5;

    if (text != NULL && text[0] != '\0') {
        unsigned long parsed = strtoul(text, NULL, 10);
        refresh_seconds = (uint32_t)parsed;
    }

    return webui_clamp_refresh_seconds(refresh_seconds);
}

static uint32_t webui_get_refresh_from_query(httpd_req_t *req)
{
    char query[64];
    char value[16];

    if (httpd_req_get_url_query_len(req) <= 0) {
        return 5;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return 5;
    }
    if (httpd_query_key_value(query, "refresh", value, sizeof(value)) != ESP_OK) {
        return 5;
    }

    return webui_parse_refresh_value(value);
}

static uint32_t webui_get_refresh_from_form(const char *form_data)
{
    char refresh_value[16];

    parse_form_field(form_data, "refresh", refresh_value, sizeof(refresh_value));
    return webui_parse_refresh_value(refresh_value);
}

static esp_err_t webui_redirect_root(httpd_req_t *req, uint32_t refresh_seconds)
{
    char location[32];

    refresh_seconds = webui_clamp_refresh_seconds(refresh_seconds);
    snprintf(location, sizeof(location), "/?refresh=%u", (unsigned)refresh_seconds);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t webui_send_chunk(httpd_req_t *req, const char *text)
{
    return httpd_resp_sendstr_chunk(req, text);
}

static esp_err_t webui_send_chunkf(httpd_req_t *req, const char *format, ...)
{
    char buffer[WEB_HTML_BUFFER_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return httpd_resp_sendstr_chunk(req, buffer);
}

static void format_uptime_string(char *uptime_string, size_t uptime_string_size)
{
    uint64_t uptime_seconds = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    uint64_t days = uptime_seconds / 86400ULL;
    uint64_t hours = (uptime_seconds % 86400ULL) / 3600ULL;
    uint64_t minutes = (uptime_seconds % 3600ULL) / 60ULL;
    uint64_t seconds = uptime_seconds % 60ULL;

    if (days > 0) {
        snprintf(uptime_string,
                 uptime_string_size,
                 "%llu天 %02llu时 %02llu分 %02llu秒",
                 days, hours, minutes, seconds);
    } else {
        snprintf(uptime_string,
                 uptime_string_size,
                 "%02llu时 %02llu分 %02llu秒",
                 hours, minutes, seconds);
    }
}

static void webui_format_heap_text(char *buffer, size_t buffer_size, size_t bytes, size_t total_bytes)
{
    unsigned whole_kb = (unsigned)(bytes / 1024U);
    unsigned tenth_kb = (unsigned)(((bytes % 1024U) * 10U) / 1024U);
    unsigned used_percent = 0;

    if (total_bytes > 0 && bytes <= total_bytes) {
        used_percent = (unsigned)(((total_bytes - bytes) * 100U) / total_bytes);
    }

    snprintf(buffer, buffer_size, "%u.%u KB / 占用 %u%%", whole_kb, tenth_kb, used_percent);
}

static void get_ap_ip_string(char *ip_string, size_t ip_string_size)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_t *ap_netif = wifi_ap_get_ap_netif();

    if (ap_netif != NULL && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        snprintf(ip_string, ip_string_size, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strlcpy(ip_string, "192.168.4.1", ip_string_size);
    }
}

static void get_ap_mac_string(char *mac_string, size_t mac_string_size)
{
    uint8_t mac[6] = { 0 };

    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        snprintf(mac_string,
                 mac_string_size,
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strlcpy(mac_string, "--", mac_string_size);
    }
}

static void webui_format_network_info(char *buffer, size_t buffer_size, const char *raw_info)
{
    if (raw_info == NULL || raw_info[0] == '\0' || strcmp(raw_info, "--") == 0) {
        strlcpy(buffer, "未读取", buffer_size);
        return;
    }

    if (strstr(raw_info, "46011") != NULL) {
        strlcpy(buffer, "中国移动-FDD LTE B8", buffer_size);
        return;
    }

    strlcpy(buffer, raw_info, buffer_size);
}

static void webui_format_cereg_text(char *buffer, size_t buffer_size, const char *raw_cereg)
{
    if (raw_cereg == NULL || raw_cereg[0] == '\0' || strcmp(raw_cereg, "--") == 0) {
        strlcpy(buffer, "未读取", buffer_size);
        return;
    }

    if (strcmp(raw_cereg, "0,1") == 0) {
        strlcpy(buffer, "+CEREG:0,1  0 = 禁用主动上报，1 = 本地小区已注册正常", buffer_size);
        return;
    }

    snprintf(buffer, buffer_size, "+CEREG:%s", raw_cereg);
}

static const char *webui_modem_model_text(const cellular_status_t *cellular_status)
{
    if (cellular_status->usb_connected && cellular_status->at_ready) {
        return "模组已就绪 EC200A";
    }
    return "未读取";
}

static const char *webui_dial_status_text(const cellular_status_t *cellular_status)
{
    if (cellular_status->dial_status[0] != '\0') {
        return cellular_status->dial_status;
    }
    return "未拨号";
}

static esp_err_t webui_send_root_page(httpd_req_t *req,
                                      const char *notice,
                                      bool is_success,
                                      uint32_t refresh_seconds)
{
    app_state_snapshot_t state_snapshot;
    cellular_status_t cellular_status;
    system_monitor_snapshot_t monitor_snapshot;
    char ap_ip[16];
    char ap_mac[18];
    char uptime[32];
    char free_heap_text[48];
    char min_heap_text[48];
    char network_info_text[96];
    char cereg_text[128];
    char temperature_text[48];
    const char *notice_class = is_success ? "ok" : "warn";
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);

    app_state_get_snapshot(&state_snapshot);
    cellular_ppp_get_status(&cellular_status);
    system_monitor_get_snapshot(&monitor_snapshot);
    get_ap_ip_string(ap_ip, sizeof(ap_ip));
    get_ap_mac_string(ap_mac, sizeof(ap_mac));
    format_uptime_string(uptime, sizeof(uptime));
    webui_format_heap_text(free_heap_text, sizeof(free_heap_text), free_heap, total_heap);
    webui_format_heap_text(min_heap_text, sizeof(min_heap_text), min_free_heap, total_heap);
    webui_format_network_info(network_info_text, sizeof(network_info_text), cellular_status.network_info);
    webui_format_cereg_text(cereg_text, sizeof(cereg_text), cellular_status.cereg_status);
    refresh_seconds = webui_clamp_refresh_seconds(refresh_seconds);

    if (monitor_snapshot.sensor_ready) {
        snprintf(temperature_text,
                 sizeof(temperature_text),
                 "%.1f C%s",
                 (double)monitor_snapshot.temperature_c,
                 monitor_snapshot.overheat ? " / 过温告警" : "");
    } else {
        strlcpy(temperature_text, "未就绪", sizeof(temperature_text));
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    webui_send_chunk(req,
                     "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                     "<meta http-equiv=\"refresh\" content=\"");
    webui_send_chunkf(req, "%u", (unsigned)refresh_seconds);
    webui_send_chunk(req,
                     "\">"
                     "<title>S3控制台</title>"
                     "<style>"
                     "body{font-family:\"Microsoft YaHei\",Arial,sans-serif;margin:0;background:#eef2f7;color:#1f2937;}"
                     ".wrap{max-width:1040px;margin:0 auto;padding:24px;}"
                     ".panel{background:#ffffff;border:1px solid #d7deea;padding:20px;margin-bottom:16px;border-radius:8px;}"
                     ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}"
                     ".item{border:1px solid #d7deea;border-radius:6px;padding:12px;background:#f8fafc;}"
                     ".title{font-size:13px;color:#64748b;margin-bottom:6px;}"
                     ".value{font-size:16px;font-weight:600;word-break:break-word;}"
                     "h1,h2{margin:0 0 12px 0;}"
                     ".row{margin:8px 0;font-size:15px;}"
                     "label{display:block;margin:12px 0 6px 0;font-weight:600;}"
                     "input,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #b9c4d3;border-radius:6px;font-size:15px;background:#fff;}"
                     ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:16px;}"
                     "button{padding:10px 14px;border:0;border-radius:6px;background:#2563eb;color:#ffffff;font-size:15px;}"
                     ".secondary{background:#475569;}"
                     ".danger{background:#b91c1c;}"
                     ".ok{background:#e7f7ed;border:1px solid #9ed6ae;padding:12px;border-radius:6px;margin-bottom:16px;}"
                     ".warn{background:#fff3d9;border:1px solid #e1bf68;padding:12px;border-radius:6px;margin-bottom:16px;}"
                     ".hint{font-size:13px;color:#475569;margin-top:10px;line-height:1.5;}"
                     ".toolbar{display:flex;flex-wrap:wrap;gap:12px;align-items:end;}"
                     ".toolbar .field{min-width:160px;flex:1;}"
                     "</style></head><body><div class=\"wrap\">");

    webui_send_chunk(req,
                     "<div class=\"panel\"><h1>S3控制台</h1>"
                     "<div class=\"row\">查看系统状态、4G 状态，并调整热点与 APN 参数。</div>"
                     "</div>");

    if (notice != NULL && notice[0] != '\0') {
        webui_send_chunkf(req, "<div class=\"%s\">%s</div>", notice_class, notice);
    }

    webui_send_chunk(req, "<div class=\"panel\"><h2>系统状态</h2><div class=\"grid\">");
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">运行时间</div><div class=\"value\">%s</div></div>", uptime);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">软件版本</div><div class=\"value\">%s</div></div>", app_get_version());
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">当前空闲内存</div><div class=\"value\">%s</div></div>", free_heap_text);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">历史最小空闲内存</div><div class=\"value\">%s</div></div>", min_heap_text);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">热点 IP</div><div class=\"value\">%s</div></div>", ap_ip);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">热点 MAC</div><div class=\"value\">%s</div></div>", ap_mac);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">芯片温度</div><div class=\"value\">%s</div></div>", temperature_text);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">CPU 主频</div><div class=\"value\">%u MHz</div></div>", (unsigned)monitor_snapshot.cpu_freq_mhz);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">双核任务分布</div><div class=\"value\">Core0: %u / Core1: %u / 总任务: %u</div></div>",
                      (unsigned)monitor_snapshot.core0_tasks,
                      (unsigned)monitor_snapshot.core1_tasks,
                      (unsigned)monitor_snapshot.total_tasks);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">LED status</div><div class=\"value\">%s</div></div>",
                      state_snapshot.led_enabled ? "Enabled" : "Disabled");
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">刷新周期</div><div class=\"value\">%u 秒</div></div>", (unsigned)refresh_seconds);
    webui_send_chunk(req, "</div></div>");

    webui_send_chunk(req, "<div class=\"panel\"><h2>4G 模组状态</h2><div class=\"grid\">");
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">4G 模组型号</div><div class=\"value\">%s</div></div>", webui_modem_model_text(&cellular_status));
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">拨号状态</div><div class=\"value\">%s</div></div>", webui_dial_status_text(&cellular_status));
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">PPP 地址</div><div class=\"value\">%s</div></div>",
                      cellular_status.ppp_ip[0] != '\0' ? cellular_status.ppp_ip : "-");
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">DNS</div><div class=\"value\">%s</div></div>",
                      cellular_status.dns[0] != '\0' ? cellular_status.dns : "-");
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">SIM 状态</div><div class=\"value\">%s</div></div>", cellular_status.sim_status);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">信号强度</div><div class=\"value\">%s</div></div>", cellular_status.signal_csq);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">驻网状态</div><div class=\"value\">%s</div></div>", cereg_text);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">运营商 / 制式</div><div class=\"value\">%s</div></div>", network_info_text);
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">当前 APN</div><div class=\"value\">%s</div></div>",
                      app_config_get_effective_apn(&state_snapshot.config));
    webui_send_chunkf(req, "<div class=\"item\"><div class=\"title\">最近错误</div><div class=\"value\">%s</div></div>",
                      cellular_status.last_error[0] != '\0' ? cellular_status.last_error : "-");
    webui_send_chunk(req, "</div></div>");

    webui_send_chunk(req,
                     "<div class=\"panel\"><h2>Wi-Fi 热点配置</h2>"
                     "<form method=\"post\" action=\"/wifi\">"
                     "<input type=\"hidden\" name=\"refresh\" value=\"");
    webui_send_chunkf(req, "%u", (unsigned)refresh_seconds);
    webui_send_chunk(req, "\">");
    webui_send_chunkf(req, "<label for=\"ssid\">Wi-Fi 名称</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" value=\"%s\">", state_snapshot.config.ssid);
    webui_send_chunkf(req, "<label for=\"password\">Wi-Fi 密码</label><input id=\"password\" name=\"password\" maxlength=\"63\" value=\"%s\">", state_snapshot.config.password);
    webui_send_chunk(req, "<div class=\"toolbar\">");
    webui_send_chunk(req, "<div class=\"field\"><label for=\"channel\">手动信道</label><select id=\"channel\" name=\"channel\">");
    webui_send_chunkf(req, "<option value=\"1\" %s>1</option>", state_snapshot.config.channel == 1 ? "selected" : "");
    webui_send_chunkf(req, "<option value=\"6\" %s>6</option>", state_snapshot.config.channel == 6 ? "selected" : "");
    webui_send_chunkf(req, "<option value=\"11\" %s>11</option>", state_snapshot.config.channel == 11 ? "selected" : "");
    webui_send_chunk(req, "</select></div>");
    webui_send_chunk(req, "<div class=\"field\"><label for=\"refresh\">页面刷新时间</label><select id=\"refresh\" name=\"refresh\">");
    webui_send_chunkf(req, "<option value=\"5\" %s>5S</option>", refresh_seconds == 5 ? "selected" : "");
    webui_send_chunkf(req, "<option value=\"10\" %s>10S</option>", refresh_seconds == 10 ? "selected" : "");
    webui_send_chunkf(req, "<option value=\"100\" %s>100S</option>", refresh_seconds == 100 ? "selected" : "");
    webui_send_chunk(req, "</select></div></div>");
    webui_send_chunk(req, "<div class=\"actions\"><button type=\"submit\">保存并重启热点</button></div></form>");
    webui_send_chunk(req, "<div class=\"hint\">保存后会写入 NVS，并在热点重启后生效。</div></div>");

    webui_send_chunk(req,
                     "<div class=\"panel\"><h2>APN 配置</h2>"
                     "<form method=\"post\" action=\"/apn\">"
                     "<input type=\"hidden\" name=\"refresh\" value=\"");
    webui_send_chunkf(req, "%u", (unsigned)refresh_seconds);
    webui_send_chunk(req, "\">");
    webui_send_chunk(req, "<label for=\"apn_profile\">运营商 APN</label><select id=\"apn_profile\" name=\"apn_profile\">");
    webui_send_chunkf(req, "<option value=\"0\" %s>中国移动</option>", state_snapshot.config.apn_profile == APP_APN_PROFILE_MOBILE ? "selected" : "");
    webui_send_chunkf(req, "<option value=\"1\" %s>中国联通</option>", state_snapshot.config.apn_profile == APP_APN_PROFILE_UNICOM ? "selected" : "");
    webui_send_chunkf(req, "<option value=\"2\" %s>中国电信</option>", state_snapshot.config.apn_profile == APP_APN_PROFILE_TELECOM ? "selected" : "");
    webui_send_chunkf(req, "<option value=\"3\" %s>自定义</option>", state_snapshot.config.apn_profile == APP_APN_PROFILE_CUSTOM ? "selected" : "");
    webui_send_chunk(req, "</select>");
    webui_send_chunkf(req, "<label for=\"custom_apn\">自定义 APN</label><input id=\"custom_apn\" name=\"custom_apn\" maxlength=\"31\" value=\"%s\">", state_snapshot.config.custom_apn);
    webui_send_chunk(req, "<div class=\"actions\"><button type=\"submit\">保存 APN 并准备重拨</button></div></form>");
    webui_send_chunk(req, "<div class=\"hint\">保存后写入 NVS；若 4G 已在线，可再点一次“4G 重拨”让新 APN 生效。</div></div>");

    webui_send_chunk(req,
                     "<div class=\"panel\"><h2>设备操作</h2><div class=\"actions\">"
                     "<form method=\"post\" action=\"/wifi/default\"><input type=\"hidden\" name=\"refresh\" value=\"");
    webui_send_chunkf(req, "%u", (unsigned)refresh_seconds);
    webui_send_chunk(req,
                     "\"><button class=\"secondary\" type=\"submit\">恢复默认 Wi-Fi 参数</button></form>"
                     "<form method=\"post\" action=\"/cellular/redial\"><input type=\"hidden\" name=\"refresh\" value=\"");
    webui_send_chunkf(req, "%u", (unsigned)refresh_seconds);
    webui_send_chunk(req,
                     "\"><button class=\"secondary\" type=\"submit\">4G 重拨</button></form>"
                     "<form method=\"post\" action=\"/led/toggle\"><input type=\"hidden\" name=\"refresh\" value=\"");
    webui_send_chunkf(req, "%u", (unsigned)refresh_seconds);
    webui_send_chunkf(req,
                      "\"><button class=\"secondary\" type=\"submit\">%s LED status</button></form>"
                      "<form method=\"post\" action=\"/device/reboot\"><input type=\"hidden\" name=\"refresh\" value=\"",
                      state_snapshot.led_enabled ? "Disable" : "Enable");
    webui_send_chunkf(req, "%u", (unsigned)refresh_seconds);
    webui_send_chunk(req,
                     "\"><button class=\"danger\" type=\"submit\">重启设备</button></form>"
                     "</div></div>");

    webui_send_chunk(req, "</div></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static bool webui_read_form(httpd_req_t *req, char *form_data, size_t form_size, uint32_t *refresh_seconds)
{
    int total_received = 0;

    if (refresh_seconds != NULL) {
        *refresh_seconds = 5;
    }

    if (req->content_len <= 0 || req->content_len >= (int)form_size) {
        return false;
    }

    while (total_received < req->content_len) {
        int received = httpd_req_recv(req, form_data + total_received, req->content_len - total_received);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        total_received += received;
    }

    form_data[total_received] = '\0';
    if (refresh_seconds != NULL) {
        *refresh_seconds = webui_get_refresh_from_form(form_data);
    }
    return true;
}

static esp_err_t schedule_device_restart(void)
{
    BaseType_t create_result;
    app_state_snapshot_t snapshot;

    app_state_get_snapshot(&snapshot);
    if (snapshot.device_reboot_pending) {
        return ESP_OK;
    }

    app_state_set_device_reboot_pending(true);
    create_result = xTaskCreatePinnedToCore(device_restart_task,
                                            "device_reboot",
                                            3072,
                                            NULL,
                                            APP_TASK_PRIO_WEB_AUX,
                                            NULL,
                                            APP_CORE_APP);
    if (create_result != pdPASS) {
        app_state_set_device_reboot_pending(false);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t webui_root_get_handler(httpd_req_t *req)
{
    return webui_send_root_page(req, NULL, true, webui_get_refresh_from_query(req));
}

static esp_err_t webui_wifi_post_handler(httpd_req_t *req)
{
    char form_data[WEB_FORM_BUFFER_SIZE];
    char ssid[33];
    char password[65];
    char channel_text[8];
    app_config_t next_config;
    const char *error_message = NULL;
    uint32_t refresh_seconds = 5;
    esp_err_t err;

    if (!webui_read_form(req, form_data, sizeof(form_data), &refresh_seconds)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return webui_send_root_page(req, "表单数据无效。", false, refresh_seconds);
    }

    app_state_get_config(&next_config);
    parse_form_field(form_data, "ssid", ssid, sizeof(ssid));
    parse_form_field(form_data, "password", password, sizeof(password));
    parse_form_field(form_data, "channel", channel_text, sizeof(channel_text));

    strlcpy(next_config.ssid, ssid, sizeof(next_config.ssid));
    strlcpy(next_config.password, password, sizeof(next_config.password));
    next_config.channel = (uint8_t)strtoul(channel_text, NULL, 10);

    if (app_config_validate(&next_config, &error_message) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return webui_send_root_page(req, error_message, false, refresh_seconds);
    }

    err = app_config_save(&next_config);
    if (err != ESP_OK) {
        return webui_send_root_page(req, "写入 NVS 失败，未能保存 Wi-Fi 参数。", false, refresh_seconds);
    }

    app_state_set_config(&next_config);
    cellular_ppp_apply_config(&next_config);
    err = wifi_ap_schedule_reconfigure();
    if (err != ESP_OK) {
        return webui_send_root_page(req, "无法启动 Wi-Fi 重启任务。", false, refresh_seconds);
    }

    return webui_send_root_page(req, "Wi-Fi 参数已保存到 NVS，热点将在约 1 秒后重启。", true, refresh_seconds);
}

static esp_err_t webui_apn_post_handler(httpd_req_t *req)
{
    char form_data[WEB_FORM_BUFFER_SIZE];
    char apn_profile_text[8];
    char custom_apn[32];
    app_config_t next_config;
    const char *error_message = NULL;
    uint32_t refresh_seconds = 5;
    esp_err_t err;

    if (!webui_read_form(req, form_data, sizeof(form_data), &refresh_seconds)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return webui_send_root_page(req, "表单数据无效。", false, refresh_seconds);
    }

    app_state_get_config(&next_config);
    parse_form_field(form_data, "apn_profile", apn_profile_text, sizeof(apn_profile_text));
    parse_form_field(form_data, "custom_apn", custom_apn, sizeof(custom_apn));

    next_config.apn_profile = (app_apn_profile_t)strtoul(apn_profile_text, NULL, 10);
    strlcpy(next_config.custom_apn, custom_apn, sizeof(next_config.custom_apn));

    if (app_config_validate(&next_config, &error_message) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return webui_send_root_page(req, error_message, false, refresh_seconds);
    }

    err = app_config_save(&next_config);
    if (err != ESP_OK) {
        return webui_send_root_page(req, "写入 NVS 失败，未能保存 APN 参数。", false, refresh_seconds);
    }

    app_state_set_config(&next_config);
    cellular_ppp_apply_config(&next_config);
    return webui_send_root_page(req, "APN 参数已保存到 NVS。需要时可点击 4G 重拨使新配置生效。", true, refresh_seconds);
}

static esp_err_t webui_wifi_default_post_handler(httpd_req_t *req)
{
    app_config_t default_config;
    esp_err_t err;
    char form_data[WEB_FORM_BUFFER_SIZE];
    uint32_t refresh_seconds = 5;

    if (req->content_len > 0) {
        if (!webui_read_form(req, form_data, sizeof(form_data), &refresh_seconds)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return webui_send_root_page(req, "表单数据无效。", false, refresh_seconds);
        }
    }

    app_config_set_defaults(&default_config);
    err = app_config_save(&default_config);
    if (err != ESP_OK) {
        return webui_send_root_page(req, "恢复默认 Wi-Fi 参数失败。", false, refresh_seconds);
    }

    app_state_set_config(&default_config);
    cellular_ppp_apply_config(&default_config);
    err = wifi_ap_schedule_reconfigure();
    if (err != ESP_OK) {
        return webui_send_root_page(req, "默认参数已写入，但无法启动热点重启任务。", false, refresh_seconds);
    }

    return webui_send_root_page(req, "默认 Wi-Fi 参数已恢复，热点将在约 1 秒后重启。", true, refresh_seconds);
}

static esp_err_t webui_cellular_redial_post_handler(httpd_req_t *req)
{
    char form_data[WEB_FORM_BUFFER_SIZE];
    uint32_t refresh_seconds = 5;
    esp_err_t err = cellular_ppp_request_redial();

    if (req->content_len > 0) {
        if (!webui_read_form(req, form_data, sizeof(form_data), &refresh_seconds)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return webui_send_root_page(req, "表单数据无效。", false, refresh_seconds);
        }
    }

    if (err != ESP_OK) {
        return webui_send_root_page(req, "当前无法发起 4G 重拨。", false, refresh_seconds);
    }

    return webui_send_root_page(req, "已发起 4G 重拨，请等待 PPP 重新上线。", true, refresh_seconds);
}

static esp_err_t webui_led_toggle_post_handler(httpd_req_t *req)
{
    app_state_snapshot_t snapshot;
    char form_data[WEB_FORM_BUFFER_SIZE];
    uint32_t refresh_seconds = 5;
    bool next_enabled;

    if (req->content_len > 0) {
        if (!webui_read_form(req, form_data, sizeof(form_data), &refresh_seconds)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return webui_send_root_page(req, "Invalid form data.", false, refresh_seconds);
        }
    }

    app_state_get_snapshot(&snapshot);
    next_enabled = !snapshot.led_enabled;
    app_state_set_led_enabled(next_enabled);

    return webui_redirect_root(req, refresh_seconds);
}

static esp_err_t webui_device_reboot_post_handler(httpd_req_t *req)
{
    char form_data[WEB_FORM_BUFFER_SIZE];
    uint32_t refresh_seconds = 5;
    esp_err_t err;

    if (req->content_len > 0) {
        if (!webui_read_form(req, form_data, sizeof(form_data), &refresh_seconds)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return webui_send_root_page(req, "表单数据无效。", false, refresh_seconds);
        }
    }

    err = schedule_device_restart();
    if (err != ESP_OK) {
        return webui_send_root_page(req, "无法启动设备重启任务。", false, refresh_seconds);
    }

    return webui_send_root_page(req, "设备已进入重启流程，请等待约 2 秒重新连接。", true, refresh_seconds);
}

esp_err_t webui_start(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.core_id = APP_CORE_APP;
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_http_server, &config));

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = webui_root_get_handler, .user_ctx = NULL };
    httpd_uri_t wifi_post_uri = { .uri = "/wifi", .method = HTTP_POST, .handler = webui_wifi_post_handler, .user_ctx = NULL };
    httpd_uri_t apn_post_uri = { .uri = "/apn", .method = HTTP_POST, .handler = webui_apn_post_handler, .user_ctx = NULL };
    httpd_uri_t wifi_default_post_uri = { .uri = "/wifi/default", .method = HTTP_POST, .handler = webui_wifi_default_post_handler, .user_ctx = NULL };
    httpd_uri_t led_toggle_post_uri = { .uri = "/led/toggle", .method = HTTP_POST, .handler = webui_led_toggle_post_handler, .user_ctx = NULL };
    httpd_uri_t reboot_post_uri = { .uri = "/device/reboot", .method = HTTP_POST, .handler = webui_device_reboot_post_handler, .user_ctx = NULL };
    httpd_uri_t cellular_redial_post_uri = { .uri = "/cellular/redial", .method = HTTP_POST, .handler = webui_cellular_redial_post_handler, .user_ctx = NULL };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &wifi_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &apn_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &wifi_default_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &led_toggle_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &reboot_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &cellular_redial_post_uri));

    ESP_LOGI(TAG, "WebUI started: http://192.168.4.1/");
    return ESP_OK;
}

void webui_stop(void)
{
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}
