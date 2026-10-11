# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
set(PARTITION_TABLE_CSV "${CMAKE_SOURCE_DIR}/partitions.csv")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION OR GIT_VERSION STREQUAL "")
    set(GIT_VERSION "dev")
endif()
add_compile_definitions(APP_GIT_VERSION="${GIT_VERSION}")

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
project(RadioSonicFirmware)

# Drift guard: sdkconfig is untracked and only seeded from sdkconfig.defaults on
# first configure, so a pulled defaults change silently does nothing on existing
# checkouts. This ALL target runs on EVERY build and fails it if any option
# pinned in the defaults files disagrees with the live sdkconfig — a drifted
# config cannot produce a binary. (See scripts/check_config_drift.py.)
idf_build_get_property(python PYTHON)
add_custom_target(check_config_drift ALL
    COMMAND ${python} ${CMAKE_SOURCE_DIR}/scripts/check_config_drift.py ${CMAKE_SOURCE_DIR}
    COMMENT "Checking sdkconfig against sdkconfig.defaults (drift guard)"
    VERBATIM
)
