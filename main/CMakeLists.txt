idf_component_register(
    SRCS
        "main.c"
        "hub75_driver.c"
        "gfx_font.c"
        "json_min.c"
        "sntp_time.c"
        "settings_store.c"
        "wifi_manager.c"
        "web_portal.c"
        "api_client.c"
        "peripherals.c"
        "dfplayer.c"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "icons/favicon.svg"
        "icons/favicon.ico"
        "icons/apple-touch-icon.png"
        "icons/site.webmanifest"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_pcnt
        esp_driver_uart
        esp_adc
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_http_server
        esp_https_ota
        esp-tls
        app_update
        mbedtls
        lwip
        esp_timer
        nvs_flash
        esp_hw_support
        esp_hal_i2s
        esp_hal_lcd
        soc
        esp_rom
        espressif__mdns
)

# Gzip web_ui.html and remote_ui.html at build time (~225 KB -> ~45 KB flash savings)
set(WEB_UI_GZ "${CMAKE_CURRENT_BINARY_DIR}/web_ui.html.gz")
set(REMOTE_UI_GZ "${CMAKE_CURRENT_BINARY_DIR}/remote_ui.html.gz")

add_custom_command(
    OUTPUT "${WEB_UI_GZ}"
    COMMAND "${PYTHON}" -c "import gzip, pathlib, sys; pathlib.Path(sys.argv[2]).write_bytes(gzip.compress(pathlib.Path(sys.argv[1]).read_bytes(), compresslevel=9, mtime=0))"
            "${CMAKE_CURRENT_SOURCE_DIR}/web_ui.html" "${WEB_UI_GZ}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web_ui.html"
    COMMENT "Gzipping web_ui.html for flash embedding"
)
add_custom_command(
    OUTPUT "${REMOTE_UI_GZ}"
    COMMAND "${PYTHON}" -c "import gzip, pathlib, sys; pathlib.Path(sys.argv[2]).write_bytes(gzip.compress(pathlib.Path(sys.argv[1]).read_bytes(), compresslevel=9, mtime=0))"
            "${CMAKE_CURRENT_SOURCE_DIR}/remote_ui.html" "${REMOTE_UI_GZ}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/remote_ui.html"
    COMMENT "Gzipping remote_ui.html for flash embedding"
)
add_custom_target(cs_gzip_web_ui DEPENDS "${WEB_UI_GZ}" "${REMOTE_UI_GZ}")
add_dependencies(${COMPONENT_LIB} cs_gzip_web_ui)
target_add_binary_data(${COMPONENT_LIB} "${WEB_UI_GZ}" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${REMOTE_UI_GZ}" BINARY)
