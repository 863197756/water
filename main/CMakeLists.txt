idf_component_register(
    SRCS 
        "main.c" 
        "app_logic.c" 
    INCLUDE_DIRS 
        "." 
    
    PRIV_REQUIRES 
        nvs_flash
        esp_event
        driver
        esp_wifi
        app_update
        esp_https_ota
        esp-tls

        app_storage
        blufi_custom 
        app_fsm
        net_manager
        mqtt_manager
        time_manager
        
        protocol
        bsp_driver 
        modem_4g 
)
