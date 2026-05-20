cmake_minimum_required(VERSION 3.16)

# Bake the firmware version into the binary at configure time.
find_package(Git QUIET)
set(FW_VERSION "")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE FW_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT FW_VERSION)
    string(TIMESTAMP FW_VERSION "%Y%m%d-%H%M%S" UTC)
endif()
add_compile_definitions(SCADABLE_FW_VERSION="${FW_VERSION}")
message(STATUS "SCADABLE firmware_version: ${FW_VERSION}")

# Override the broker URI at build time via env var. Default is the
# Toronto regional broker.
if(DEFINED ENV{SCADABLE_MQTT_URI})
    add_compile_definitions(SCADABLE_MQTT_URI="$ENV{SCADABLE_MQTT_URI}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(scadable_firmware)
