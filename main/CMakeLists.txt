idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES nvs_flash esp_netif esp_event app_state config net web sysmon)

# Image LittleFS (interface web) générée par tools/build_web.py, flashée avec l'application.
set(LITTLEFS_IMAGE_DIR "${PROJECT_DIR}/littlefs_image")
if(EXISTS "${LITTLEFS_IMAGE_DIR}")
    littlefs_create_partition_image(storage "${LITTLEFS_IMAGE_DIR}" FLASH_IN_PROJECT)
else()
    message(WARNING "littlefs_image/ absent : lancer 'python tools/build_web.py' pour inclure l'interface web")
endif()
