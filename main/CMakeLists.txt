idf_component_register(
    SRCS
        "app_config.c"
        "app_state.c"
        "cellular_ppp.c"
        "led_status.c"
        "main.c"
        "system_monitor.c"
        "webui.c"
        "wifi_ap.c"
    INCLUDE_DIRS "."
    REQUIRES usb
    PRIV_REQUIRES esp_driver_rmt esp_driver_tsens esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_timer esp_hw_support
)

idf_component_optional_requires(PRIVATE usb_host_cdc_acm espressif__usb_host_cdc_acm)
