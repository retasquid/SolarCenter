cmake_minimum_required(VERSION 3.16)
project(solar_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# Unity : copie fournie par ESP-IDF (aucune dépendance supplémentaire à installer).
if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH non défini : activer l'environnement ESP-IDF (pour Unity)")
endif()
set(UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity/src")
add_library(unity STATIC "${UNITY_DIR}/unity.c")
target_include_directories(unity PUBLIC "${UNITY_DIR}")
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64)
target_compile_options(unity PRIVATE -w)

set(COMP "${CMAKE_CURRENT_SOURCE_DIR}/../../components")

enable_testing()

add_executable(test_config test_config.c
               "${COMP}/config/cfg_defaults.c"
               "${COMP}/config/cfg_validate.c")
target_include_directories(test_config PRIVATE "${COMP}/config/include")
target_link_libraries(test_config unity)
if(UNIX)
    target_link_libraries(test_config m)
endif()
add_test(NAME config COMMAND test_config)

add_executable(test_flows test_flows.c
               "${COMP}/algo/flows.c"
               "${COMP}/algo/sanity.c")
target_include_directories(test_flows PRIVATE "${COMP}/algo/include")
target_link_libraries(test_flows unity)
if(UNIX)
    target_link_libraries(test_flows m)
endif()
add_test(NAME flows COMMAND test_flows)

add_executable(test_jwt test_jwt.c "${COMP}/drv_envoy/jwt_exp.c")
target_include_directories(test_jwt PRIVATE "${COMP}/drv_envoy/include")
target_link_libraries(test_jwt unity)
add_test(NAME jwt COMMAND test_jwt)
