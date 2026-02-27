idf_component_register(
    SRCS "src/modem_4g.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 
        driver 
        # esp_driver_gpio
        lwip 
        freertos 
        nvs_flash
        esp_event
        esp_netif
        espressif__esp_modem
)