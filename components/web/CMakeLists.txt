idf_component_register(SRCS "web.c" "api.c" "api_state.c" "api_hist.c" "api_ctl.c" "jsonw.c" "cfg_json.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_http_server esp_timer esp_app_format esp_hw_support spi_flash
                                     app_state config net sysmon algo drv_envoy forecast history control log
                       EMBED_TXTFILES "setup.html")
