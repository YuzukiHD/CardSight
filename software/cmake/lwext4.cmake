# SPDX-License-Identifier: GPL-2.0-or-later
# lwext4: the upstream submodule plus the CardSight patch series, applied to a
# copy in the build directory so the submodule itself stays untouched.

set(CS_LWEXT4_ROOT ${CMAKE_BINARY_DIR}/lwext4)
set(CS_LWEXT4_PATCH ${CS_ROOT}/patches/lwext4/0001-csum-seed.patch)

file(REMOVE_RECURSE ${CS_LWEXT4_ROOT})
file(COPY ${CS_ROOT}/third_party/lwext4/ DESTINATION ${CS_LWEXT4_ROOT})
execute_process(
    COMMAND patch -p1 -s -d ${CS_LWEXT4_ROOT} -i ${CS_LWEXT4_PATCH}
    RESULT_VARIABLE CS_LWEXT4_PATCH_RESULT)
if(NOT CS_LWEXT4_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "the lwext4 checksum-seed patch does not apply (${CS_LWEXT4_PATCH_RESULT})")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CS_LWEXT4_PATCH})
file(GLOB CS_LWEXT4_SOURCES ${CS_LWEXT4_ROOT}/src/*.c)
