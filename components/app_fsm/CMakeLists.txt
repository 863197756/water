idf_component_register(
    SRCS "src/app_fsm.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_event
        app_events
        app_storage
        net_manager
        mqtt_manager
        bsp_driver
)
