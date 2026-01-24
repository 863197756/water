idf_component_register(
    SRCS 
        "main.c" 
        "app_logic.c" 
        "net_manager.c" 
    INCLUDE_DIRS 
        "." 
    
    PRIV_REQUIRES 
        # bsp_driver 
        # modem_4g 
        blufi_custom 
        app_storage 
        json_parser 
        esp_wifi
)