idf_component_register(
    SRCS "src/net_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 
        esp_wifi
        esp_event
        app_events
        app_storage
        modem_4g  # 待 4G 模组实现后开启
)
