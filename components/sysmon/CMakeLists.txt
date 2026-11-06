idf_component_register(SRCS "sysmon.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_state net esp_system esp_hw_support spi_flash esp_app_format heap log esp_timer app_update)
