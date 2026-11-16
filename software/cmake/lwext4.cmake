# SPDX-License-Identifier: GPL-2.0-or-later
# lwext4: the upstream submodule plus the CardSight patch series, applied to a
# copy in the build directory so the submodule itself stays untouched. The
# patches are applied in file-name order, so a new change is one more numbered
# file in patches/lwext4/.

set(CS_LWEXT4_ROOT ${CMAKE_BINARY_DIR}/lwext4)

file(REMOVE_RECURSE ${CS_LWEXT4_ROOT})
file(COPY ${CS_ROOT}/third_party/lwext4/ DESTINATION ${CS_LWEXT4_ROOT})
file(GLOB CS_LWEXT4_PATCHES ${CS_ROOT}/patches/lwext4/*.patch)
list(SORT CS_LWEXT4_PATCHES)
foreach(CS_LWEXT4_PATCH IN LISTS CS_LWEXT4_PATCHES)
    execute_process(
        COMMAND patch -p1 -s -d ${CS_LWEXT4_ROOT} -i ${CS_LWEXT4_PATCH}
        RESULT_VARIABLE CS_LWEXT4_PATCH_RESULT)
    if(NOT CS_LWEXT4_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "the lwext4 patch ${CS_LWEXT4_PATCH} does not apply (${CS_LWEXT4_PATCH_RESULT})")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CS_LWEXT4_PATCH})
endforeach()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CS_LWEXT4_PATCHES})
# The default block cache is eight 4 KiB blocks, too small for a Debian
# directory with more than a thousand entries: the directory blocks and the
# inode-table blocks evict each other while readdir walks the list. 64 blocks
# cost 256 KiB per mounted ext volume and stay well inside the heap.
add_compile_definitions(CONFIG_BLOCK_DEV_CACHE_SIZE=64)
file(GLOB CS_LWEXT4_SOURCES ${CS_LWEXT4_ROOT}/src/*.c)
