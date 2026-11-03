idf_component_register(SRCS "drv_envoy.c" "jwt_exp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES config
                       PRIV_REQUIRES http_util log)
