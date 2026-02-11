idf_component_register(
    SRCS "src/mqtt_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        protocol
    PRIV_REQUIRES 
        mqtt  #esp官方MQTT库
        esp_event
        esp_netif
        app_storage
        blufi_custom
)