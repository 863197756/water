idf_component_register(
    SRCS 
        "main.c" 
        "app_logic.c" 
        "net_manager.c" 
        "protocol.c"
        "mqtt_manager.c"
        "time_manager.c"
    INCLUDE_DIRS 
        "." 
    
    PRIV_REQUIRES 
        esp_wifi
        esp_event
        driver
        esp_driver_gpio
        nvs_flash
        app_storage
        blufi_custom 
        json_parser 
        json
        mqtt
        lwip
        # bsp_driver 
        # modem_4g 
)