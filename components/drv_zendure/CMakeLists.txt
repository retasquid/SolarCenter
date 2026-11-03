idf_component_register(SRCS "drv_zendure.c"
                       INCLUDE_DIRS "include"
                       REQUIRES config
                       PRIV_REQUIRES http_util log)
