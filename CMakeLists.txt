cmake_minimum_required(VERSION 3.16)

# Bake firmware_version into the binary at configure time.
# Prefer the git short SHA; fall back to a timestamp if git is unavailable.
find_package(Git QUIET)
set(FW_VERSION "")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE FW_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT FW_VERSION)
    string(TIMESTAMP FW_VERSION "%Y%m%d-%H%M%S" UTC)
endif()
add_compile_definitions(SCADABLE_FW_VERSION="${FW_VERSION}")
message(STATUS "SCADABLE firmware_version: ${FW_VERSION}")

# Pin a broker host at build time. When set, firmware skips mDNS and connects
# directly. Useful for talking to a remote VM. Pass via env or -D.
if(DEFINED ENV{SCADABLE_BROKER_HOST})
    add_compile_definitions(SCADABLE_BROKER_HOST="$ENV{SCADABLE_BROKER_HOST}")
    message(STATUS "SCADABLE broker host (build-time pin): $ENV{SCADABLE_BROKER_HOST}")
endif()
if(DEFINED ENV{SCADABLE_BROKER_PORT})
    add_compile_definitions(SCADABLE_BROKER_PORT=$ENV{SCADABLE_BROKER_PORT})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(scadable_firmware)
