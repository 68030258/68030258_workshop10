idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_timer driver freertos esp_wifi esp_netif esp_event esp_sntp
)

