# SPDX-License-Identifier: GPL-2.0-or-later
# The interactive WebAssembly build of the real CardSight UI.
#
# It compiles LVGL, the pages, the widgets and the glass material exactly as the
# board does, then renders into a memory framebuffer that the browser copies to
# a canvas. The fixture model and the request emulation live in ui_web.c; there
# is no RTOS and no card driver here.
#
#   emcmake cmake -S software/tests/wasm -B software/build/wasm -DCMAKE_BUILD_TYPE=Release
#   cmake --build software/build/wasm
#
cmake_minimum_required(VERSION 3.20)
project(cardsight-ui C)

set(CS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

file(GLOB_RECURSE CS_LVGL_WEB_SOURCES CONFIGURE_DEPENDS ${CS_ROOT}/third_party/LVGL/src/*.c)
list(FILTER CS_LVGL_WEB_SOURCES EXCLUDE REGEX "/(libs|others|drivers)/")
file(GLOB CS_UI_WEB_SOURCES CONFIGURE_DEPENDS
    ${CS_ROOT}/apps/cardsight/ui/*.c
    ${CS_ROOT}/apps/cardsight/ui/core/*.c
    ${CS_ROOT}/apps/cardsight/ui/widgets/*.c
    ${CS_ROOT}/apps/cardsight/ui/pages/*.c
    ${CS_ROOT}/apps/cardsight/fonts/*.c
    ${CS_ROOT}/apps/cardsight/assets/*.c
)
list(REMOVE_ITEM CS_UI_WEB_SOURCES ${CS_ROOT}/apps/cardsight/ui/screenshot.c)

add_executable(cardsight-ui
    ${CS_LVGL_WEB_SOURCES}
    ${CS_UI_WEB_SOURCES}
    ${CS_ROOT}/src/kernel/size.c
    ui_web.c
)

target_include_directories(cardsight-ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../host/stubs
    ${CS_ROOT}/include
    ${CS_ROOT}/src
    ${CS_ROOT}/src/gui
    ${CS_ROOT}/apps/cardsight
    ${CS_ROOT}/third_party/LVGL
)
target_compile_definitions(cardsight-ui PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
target_compile_options(cardsight-ui PRIVATE -O2 -w)

target_link_options(cardsight-ui PRIVATE
    --no-entry
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createCardSightUI
    -sENVIRONMENT=web
    -sFILESYSTEM=0
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=33554432
    -sSTACK_SIZE=2097152
    -sNO_EXIT_RUNTIME=1
    -sEXPORTED_FUNCTIONS=['_cardsight_web_init','_cardsight_web_tick','_cardsight_web_width','_cardsight_web_height','_cardsight_web_framebuffer','_cardsight_web_dirty','_cardsight_web_pointer','_cardsight_web_page','_cardsight_web_mode','_cardsight_web_action']
    -sEXPORTED_RUNTIME_METHODS=['HEAPU8']
)
