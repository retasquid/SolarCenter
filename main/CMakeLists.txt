idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES nvs_flash esp_netif esp_event app_state config net web sysmon
                                     http_util poll forecast history drv_tuya drv_tongou control)

# Image LittleFS (interface web) générée par tools/build_web.py, flashée seulement par 'idf.py storage-flash'
# (efface l'historique). Mise à jour sans perte : python tools/ota.py --www.
set(LITTLEFS_IMAGE_DIR "${PROJECT_DIR}/littlefs_image")
if(EXISTS "${LITTLEFS_IMAGE_DIR}")
    littlefs_create_partition_image(storage "${LITTLEFS_IMAGE_DIR}")
else()
    message(WARNING "littlefs_image/ absent : lancer 'python tools/build_web.py' pour inclure l'interface web")
endif()
