# SPDX-License-Identifier: GPL-2.0-or-later
# The two E907 boot stubs come from the SyterKit submodule: the PSRAM init the FEL stage runs
# first and the C907 start that points the core at our image.

set(CS_SYTERKIT ${CS_ROOT}/third_party/SyterKit)

if(NOT EXISTS ${CS_SYTERKIT}/boards/yuzuki-cardsight/configs/efex_defconfig)
    message(FATAL_ERROR
        "third_party/SyterKit is not checked out; run: git submodule update --init --recursive")
endif()

find_program(CS_MAKE make REQUIRED)

set(CS_SYTERKIT_BUILD ${CMAKE_BINARY_DIR}/syterkit)
set(CS_EFEX_INIT ${CS_SYTERKIT_BUILD}/build/soc/sun252iw1/app_efex/init_psram/init_psram_efex.bin)
set(CS_EFEX_BOOT ${CS_SYTERKIT_BUILD}/build/soc/sun252iw1/app_efex/c907_boot/c907_boot_efex.bin)
set(CS_EFEX_ENV ${CMAKE_COMMAND} -E env CROSS_COMPILE=${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-)

add_custom_command(
    OUTPUT ${CS_EFEX_INIT} ${CS_EFEX_BOOT}
    COMMAND ${CS_EFEX_ENV} ${CS_MAKE} -C ${CS_SYTERKIT} O=${CS_SYTERKIT_BUILD} yuzuki-cardsight_efex_defconfig
    COMMAND ${CS_EFEX_ENV} ${CS_MAKE} -C ${CS_SYTERKIT} O=${CS_SYTERKIT_BUILD} -j8
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/images
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CS_EFEX_INIT} ${CMAKE_BINARY_DIR}/images/init_psram_efex.bin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CS_EFEX_BOOT} ${CMAKE_BINARY_DIR}/images/c907_boot_efex.bin
    DEPENDS ${CS_SYTERKIT}/boards/yuzuki-cardsight/configs/efex_defconfig
    BYPRODUCTS ${CMAKE_BINARY_DIR}/images/init_psram_efex.bin ${CMAKE_BINARY_DIR}/images/c907_boot_efex.bin
    COMMENT "SyterKit eFEX stubs"
    VERBATIM
)

add_custom_target(efex_stubs ALL DEPENDS ${CS_EFEX_INIT} ${CS_EFEX_BOOT})
