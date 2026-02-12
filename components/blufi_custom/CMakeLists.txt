idf_component_register(
    SRCS 
        "src/blufi_custom.c" 
        "src/blufi_security.c"
    INCLUDE_DIRS 
        "include"
    PRIV_REQUIRES 
        nvs_flash 
        esp_wifi 
        esp_event 
        mbedtls 
        bt 
        json_parser 
        esp_timer
        app_storage
        app_events
)

