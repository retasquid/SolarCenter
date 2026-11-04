idf_component_register(SRCS "poll.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_state config algo drv_shelly drv_envoy drv_zendure drv_tongou history
                                     esp_system log)
