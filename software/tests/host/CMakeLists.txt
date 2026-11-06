# SPDX-License-Identifier: GPL-2.0-or-later
# The host tests compile the real block layer, file systems and storage proxy against a RAM disk.

set(CS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

file(GLOB LWEXT4_SOURCES CONFIGURE_DEPENDS ${CS_ROOT}/third_party/lwext4/src/*.c)
file(GLOB STORAGE_SOURCES CONFIGURE_DEPENDS ${CS_ROOT}/apps/cardsight/storage/*.c)

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
    ${LWEXT4_SOURCES}
    ${STORAGE_SOURCES}
)

target_include_directories(cs_host_core PUBLIC
    ${CS_ROOT}/include
    ${CS_ROOT}/src
    ${CS_ROOT}/apps/cardsight
    ${FATFS_DIR}
    ${CS_ROOT}/third_party/lwext4/include
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
