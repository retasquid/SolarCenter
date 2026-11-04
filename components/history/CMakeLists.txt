idf_component_register(SRCS "history.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_state algo
                       PRIV_REQUIRES config forecast nvs_flash esp_system log)
