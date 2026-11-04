idf_component_register(SRCS "forecast.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_state config http_util algo esp_system log)
