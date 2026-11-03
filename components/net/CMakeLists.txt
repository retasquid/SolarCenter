idf_component_register(SRCS "net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES config
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer app_state log)
