
idf_component_register(
    SRCS "blink_example_main.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "emqxsl_ca.pem"
    REQUIRES
        esp_driver_gpio
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        mqtt
)
