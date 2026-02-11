idf_component_register(
    SRCS "src/time_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 
        lwip    # SNTP 依赖
 
)