idf_component_register(SRCS "main.c" "camera.c" "storage.c" "flash_led.c" "net.c" "web.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "index.html"
                       REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_ledc
                                esp_timer esp_psram esp_wifi esp_netif esp_event nvs_flash
                                esp_http_server)
