idf_component_register(
    SRCS 
        "main.c" 
        "app_logic.c" 
    INCLUDE_DIRS 
        "." 
    
    PRIV_REQUIRES 
        nvs_flash
        app_storage
        blufi_custom 
        net_manager
        mqtt_manager
        protocol
        time_manager
        bsp_driver 
        modem_4g 
)