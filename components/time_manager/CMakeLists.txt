idf_component_register(
    SRCS "time_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 
        lwip    # SNTP 依赖
        esp_log
)