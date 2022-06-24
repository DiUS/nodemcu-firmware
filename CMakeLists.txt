cmake_minimum_required(VERSION 3.5)
set(CONFIG_NVS_ENCRYPTION 1)
set(PROJECT_VER "$ENV{PROJECT_VER}")

if(DEFINED ENV{EXTRA_COMPONENT_DIRS})
set(EXTRA_COMPONENT_DIRS "$ENV{EXTRA_COMPONENT_DIRS}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nodemcu)

# Useful when tidying up code to catch all the warnings.
#idf_build_set_property(COMPILE_OPTIONS "-Werror" "-Werror=deprecated-declarations" APPEND)

idf_build_set_property(COMPILE_OPTIONS "-DCONFIG_NVS_ENCRYPTION" APPEND)
