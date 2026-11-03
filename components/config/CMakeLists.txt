idf_component_register(SRCS "config.c" "cfg_defaults.c" "cfg_validate.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash esp_rom log freertos)
