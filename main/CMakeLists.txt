idf_component_register(
    SRCS "main.c" "rtc_pcf85063.c" "battery_axp2101.c" "wifi_update.c" "dlc.c"
    INCLUDE_DIRS "."
    REQUIRES tama_core fatfs esp_timer esp_partition nvs_flash esp_driver_gpio perfmon esp_pm esp_driver_usb_serial_jtag app_update esp_wifi esp_netif esp_event esp_http_client mbedtls
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -Wno-attributes
)
