idf_component_register(SRCS "control.c" "act.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES algo
                       PRIV_REQUIRES app_state config history drv_zendure drv_tongou sysmon nvs_flash esp_system log)
