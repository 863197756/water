idf_component_register(
    SRCS "protocol.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 
        json            # cJSON 依赖
        json_parser     # jsmn 依赖
)