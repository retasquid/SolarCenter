cmake_minimum_required(VERSION 3.22)

# Version : version.txt (augmentée à chaque livraison) + commit git, « -dirty » s'il reste des
# modifications non commitées. Le numéro change à chaque commit, ce qui fait recompiler la
# description de l'application : la date de compilation affichée reste juste.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" APP_VERSION_BASE LIMIT_COUNT 1)
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                OUTPUT_VARIABLE APP_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain --untracked-files=no
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                OUTPUT_VARIABLE APP_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(PROJECT_VER "${APP_VERSION_BASE}")
if(APP_GIT_HASH)
    string(APPEND PROJECT_VER "+${APP_GIT_HASH}")
endif()
if(APP_GIT_DIRTY)
    string(APPEND PROJECT_VER "-dirty")
endif()
# Reconfigurer après un commit ou un changement de version.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
foreach(f .git/HEAD .git/logs/HEAD)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${f}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/${f}")
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solar_center)
