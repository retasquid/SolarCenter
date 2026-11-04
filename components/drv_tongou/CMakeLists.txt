idf_component_register(SRCS "drv_tongou.c"
                       INCLUDE_DIRS "include"
                       REQUIRES config
                       PRIV_REQUIRES http_util drv_tuya log)
