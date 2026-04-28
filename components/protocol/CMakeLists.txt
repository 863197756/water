idf_component_register(
    SRCS "src/protocol.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 
        json            # cJSON 依赖
        efuse
        esp_event
        app_storage
)
