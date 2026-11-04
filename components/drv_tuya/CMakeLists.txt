idf_component_register(SRCS "tuya_proto.c" "drv_tuya.c"
                       INCLUDE_DIRS "include"
                       REQUIRES config
                       PRIV_REQUIRES mbedtls lwip esp_hw_support log)
