idf_component_register(SRCS "http_util.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES esp_http_client mbedtls log)
