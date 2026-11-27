# SPDX-License-Identifier: GPL-2.0-or-later
# The host tests compile the real block layer, file systems and storage proxy against a RAM disk.

set(CS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

include(lwext4)
file(GLOB STORAGE_SOURCES CONFIGURE_DEPENDS
    ${CS_ROOT}/apps/cardsight/storage/*.c
    ${CS_ROOT}/apps/cardsight/storage/lazy/*.c
    ${CS_ROOT}/apps/cardsight/storage/eager/*.c
)

# FatFs sources are compiled from a copy: our ffconf.h has to win over the submodule's.
set(FATFS_DIR ${CMAKE_BINARY_DIR}/fatfs)
file(MAKE_DIRECTORY ${FATFS_DIR})
foreach(FATFS_FILE ff.c ff.h ffunicode.c ffsystem.c diskio.h)
    configure_file(${CS_ROOT}/third_party/fatfs/source/${FATFS_FILE} ${FATFS_DIR}/${FATFS_FILE} COPYONLY)
endforeach()
configure_file(${CS_ROOT}/src/fs/fatfs/ffconf.h ${FATFS_DIR}/ffconf.h COPYONLY)

add_library(cs_host_core STATIC
    ${CS_ROOT}/src/block/genhd.c
    ${CS_ROOT}/src/block/bdev.c
    ${CS_ROOT}/src/block/partitions.c
    ${CS_ROOT}/src/block/layout.c
    ${CS_ROOT}/src/fs/vfs.c
    ${CS_ROOT}/src/fs/vfs_fat.c
    ${CS_ROOT}/src/fs/vfs_ext.c
    ${CS_ROOT}/src/kernel/size.c
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffunicode.c
    ${FATFS_DIR}/ffsystem.c
    ${CS_LWEXT4_SOURCES}
    ${STORAGE_SOURCES}
)

target_include_directories(cs_host_core PUBLIC
    ${CS_ROOT}/include
    ${CS_ROOT}/src
    ${CS_ROOT}/apps/cardsight
    ${FATFS_DIR}
    ${CS_LWEXT4_ROOT}/include
)

target_compile_definitions(cs_host_core PUBLIC
    CARDSIGHT_HOST_TEST=1
    STORAGE_PROXY_HOST_TEST=1
    CONFIG_LOG_LEVEL=4
    CONFIG_STORAGE_PROXY_SOURCE_DISK="mmcblk0"
    CONFIG_STORAGE_PROXY_MAX_ENTRIES=512
    CONFIG_STORAGE_PROXY_STRING_BYTES=65536
    CONFIG_STORAGE_PROXY_SHELL=1
    CONFIG_SHELL=1
    CONFIG_VFS_MAX_MOUNTS=8
    CONFIG_VFS_MAX_FILES=16
    CONFIG_VFS_MAX_FS_TYPES=8
    CONFIG_EXT_FS_MAX_MOUNTS=3
    CONFIG_USE_DEFAULT_CFG=1
    CONFIG_DEBUG_PRINTF=0
    CONFIG_DEBUG_ASSERT=0
    CONFIG_HAVE_OWN_ERRNO=0
    CONFIG_HAVE_OWN_OFLAGS=1
    CONFIG_JOURNALING_ENABLE=1
    CONFIG_EXT4_BLOCKDEVS_COUNT=3
    CONFIG_EXT4_MOUNTPOINTS_COUNT=3
)

target_compile_options(cs_host_core PRIVATE -w)
target_compile_options(cs_host_core PUBLIC ${CARDSIGHT_HOST_WARNINGS})

add_executable(test_storage_proxy_matrix storage_proxy_matrix_test.c)
target_link_libraries(test_storage_proxy_matrix PRIVATE cs_host_core)
target_compile_options(test_storage_proxy_matrix PRIVATE ${CARDSIGHT_HOST_WARNINGS})

add_test(NAME storage_proxy_matrix COMMAND test_storage_proxy_matrix)

add_executable(test_lazy_proxy lazy_proxy_test.c)
target_link_libraries(test_lazy_proxy PRIVATE cs_host_core)
target_compile_options(test_lazy_proxy PRIVATE ${CARDSIGHT_HOST_WARNINGS})

add_test(NAME lazy_proxy COMMAND test_lazy_proxy)

# The UI simulator builds the real LVGL, pages and glass material against host stubs and writes
# one PPM per page to /tmp; it is a development tool, not a test.
file(GLOB_RECURSE CS_LVGL_SIM_SOURCES CONFIGURE_DEPENDS ${CS_ROOT}/third_party/LVGL/src/*.c)
list(FILTER CS_LVGL_SIM_SOURCES EXCLUDE REGEX "/(libs|others|drivers)/")
file(GLOB CS_UI_SIM_SOURCES CONFIGURE_DEPENDS
    ${CS_ROOT}/apps/cardsight/ui/*.c
    ${CS_ROOT}/apps/cardsight/ui/core/*.c
    ${CS_ROOT}/apps/cardsight/ui/widgets/*.c
    ${CS_ROOT}/apps/cardsight/ui/pages/*.c
    ${CS_ROOT}/apps/cardsight/fonts/*.c
    ${CS_ROOT}/apps/cardsight/assets/*.c
)
list(REMOVE_ITEM CS_UI_SIM_SOURCES ${CS_ROOT}/apps/cardsight/ui/screenshot.c)
add_executable(ui_sim
    ${CS_LVGL_SIM_SOURCES}
    ${CS_UI_SIM_SOURCES}
    ${CS_ROOT}/src/kernel/size.c
    ui_sim.c
)
target_include_directories(ui_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CS_ROOT}/include
    ${CS_ROOT}/src
    ${CS_ROOT}/src/gui
    ${CS_ROOT}/apps/cardsight
    ${CS_ROOT}/third_party/LVGL
)
target_compile_definitions(ui_sim PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
target_compile_options(ui_sim PRIVATE -w)
target_link_libraries(ui_sim PRIVATE m)
