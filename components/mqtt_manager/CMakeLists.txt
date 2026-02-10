idf_component_register(
    SRCS "mqtt_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 
        mqtt
        esp_log
        app_storage
        protocol
        blufi_custom
)