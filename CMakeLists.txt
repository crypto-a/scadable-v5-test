cmake_minimum_required(VERSION 3.16)

# Bake the firmware version into the binary at configure time. Prefers the
# short git SHA so the dashboard's graph can show the exact commit each
# device is running. Falls back to a UTC timestamp when no .git is around.
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

# Compile-time wiring. Build orchestrator injects these via env vars; never
# checked into git.
if(DEFINED ENV{SCADABLE_API_BASE})
    add_compile_definitions(SCADABLE_API_BASE="$ENV{SCADABLE_API_BASE}")
endif()
if(DEFINED ENV{SCADABLE_API_TOKEN})
    add_compile_definitions(SCADABLE_API_TOKEN="$ENV{SCADABLE_API_TOKEN}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(scadable_firmware)
