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
    # A release package without git history carries its version in VERSION.
    if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" GIT_VERSION LIMIT_COUNT 1)
    else()
        set(GIT_VERSION "dev")
    endif()
endif()
add_compile_definitions(APP_GIT_VERSION="${GIT_VERSION}")

# Components kept outside this tree (a folder or repository of their own):
# RS_EXTRA_COMPONENT_DIRS lists the folders that hold them, separated by
# semicolons. RS_DEPENDENCIES_LOCK moves the component manager's lock file out
# of this tree too, so a build with them leaves this checkout unchanged.
# docs/ADDING_EFFECTS.md, "Components outside the tree".
if(DEFINED ENV{RS_EXTRA_COMPONENT_DIRS})
    list(APPEND EXTRA_COMPONENT_DIRS $ENV{RS_EXTRA_COMPONENT_DIRS})
endif()
if(DEFINED ENV{RS_DEPENDENCIES_LOCK})
    idf_build_set_property(DEPENDENCIES_LOCK "$ENV{RS_DEPENDENCIES_LOCK}")
endif()

# What this configure baked in: the version string and the out-of-tree
# component settings. None of them is a file CMake watches, so flash.ps1
# compares this record with the current values before it builds and
# reconfigures when they differ (a new commit or tag, or a switch between a
# build with and without out-of-tree components).
file(WRITE "${CMAKE_BINARY_DIR}/rs_configured.txt"
     "${GIT_VERSION}|$ENV{RS_EXTRA_COMPONENT_DIRS}|$ENV{RS_DEPENDENCIES_LOCK}")

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
