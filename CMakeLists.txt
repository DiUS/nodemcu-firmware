cmake_minimum_required(VERSION 3.5)
set(CONFIG_NVS_ENCRYPTION 1)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nodemcu)

idf_build_set_property(COMPILE_OPTIONS "-DCONFIG_NVS_ENCRYPTION" APPEND)
