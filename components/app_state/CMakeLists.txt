idf_component_register(SRCS "app_state.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos algo
                       PRIV_REQUIRES esp_timer log)
