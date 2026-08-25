cmake_minimum_required(VERSION 3.16)

# Firmware's own idea of its version, for comparing against GitHub's latest
# release tag before attempting an OTA update. Falls back to "dev" outside
# a git checkout (or if no tags are reachable) rather than failing the
# build. Must run *before* project(), not after -- ESP-IDF's project()
# call (via the include() below) is itself what walks and registers every
# component, so a set() placed after it runs too late for main/CMakeLists.txt
# to see the value.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE FIRMWARE_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_DESCRIBE_RESULT
)
if(NOT GIT_DESCRIBE_RESULT EQUAL 0 OR NOT FIRMWARE_VERSION)
    set(FIRMWARE_VERSION "dev")
endif()
message(STATUS "FIRMWARE_VERSION=${FIRMWARE_VERSION}")
# add_compile_definitions() doesn't reach component sources -- ESP-IDF's
# component build isolation doesn't inherit plain top-level CMake compile
# definitions. main/CMakeLists.txt reads this cache variable directly and
# applies it to its own target instead.
set(FIRMWARE_VERSION "${FIRMWARE_VERSION}" CACHE INTERNAL "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(imagejockey)
