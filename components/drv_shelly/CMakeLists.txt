idf_component_register(SRCS "drv_shelly.c"
                       INCLUDE_DIRS "include"
                       REQUIRES config
                       PRIV_REQUIRES http_util log)
