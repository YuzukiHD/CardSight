# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

project(cardsight C ASM)

option(CARDSIGHT_HOST_TESTS "Build the host test programs" OFF)
option(CARDSIGHT_BUILD_EFEX "Build the SyterKit eFEX boot stubs" ON)
option(CARDSIGHT_WITH_APP "Link the CardSight application" ON)
option(CARDSIGHT_WERROR "Treat warnings in our code as errors" OFF)

set(CS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND CMAKE_MODULE_PATH "${CS_ROOT}/cmake")
include(options)
include(warnings)

if(CARDSIGHT_HOST_TESTS)
    enable_testing()
    add_subdirectory(tests/host)
    return()
endif()

if(CARDSIGHT_BUILD_EFEX)
    include(syterkit)
endif()

add_subdirectory(src)
