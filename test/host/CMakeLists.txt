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

# Tout algo/ en une bibliothèque : la règle d'or (C pur) est vérifiée à la compilation sur PC.
file(GLOB ALGO_SRCS "${COMP}/algo/*.c")
# Garde-fou : l'extension CMake Tools de VS Code ajoute d'elle-même les nouveaux test_*.c ici.
list(FILTER ALGO_SRCS EXCLUDE REGEX "/test_[^/]*\\.c$")
add_library(algo STATIC ${ALGO_SRCS})
target_include_directories(algo PUBLIC "${COMP}/algo/include")
if(UNIX)
    target_link_libraries(algo PUBLIC m)
endif()

foreach(t flows tariff rls stats models)
    add_executable(test_${t} test_${t}.c)
    target_link_libraries(test_${t} algo unity)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

add_executable(test_jwt test_jwt.c "${COMP}/drv_envoy/jwt_exp.c")
target_include_directories(test_jwt PRIVATE "${COMP}/drv_envoy/include")
target_link_libraries(test_jwt unity)
add_test(NAME jwt COMMAND test_jwt)
