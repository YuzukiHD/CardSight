# SPDX-License-Identifier: GPL-2.0-or-later

set(CS_KERNEL_ROOT ${CS_ROOT}/third_party/FreeRTOS-Kernel)

set(CS_KERNEL_SOURCES
    ${CS_KERNEL_ROOT}/tasks.c
    ${CS_KERNEL_ROOT}/queue.c
    ${CS_KERNEL_ROOT}/list.c
    ${CS_KERNEL_ROOT}/timers.c
    ${CS_KERNEL_ROOT}/event_groups.c
    ${CS_KERNEL_ROOT}/stream_buffer.c
    ${CS_KERNEL_ROOT}/portable/MemMang/heap_4.c
    ${CS_KERNEL_ROOT}/portable/GCC/RISC-V/port.c
    ${CS_KERNEL_ROOT}/portable/GCC/RISC-V/portASM.S
)

set(CS_PLATFORM_SOURCES
    ${CS_ROOT}/src/arch/start.S
    ${CS_ROOT}/src/arch/trap.c
    ${CS_ROOT}/src/arch/clint.c
    ${CS_ROOT}/src/arch/plic.c
    ${CS_ROOT}/src/arch/cache.c
    ${CS_ROOT}/src/libc/syscalls.c
    ${CS_ROOT}/src/libc/newlib_locks.c
    ${CS_ROOT}/src/kernel/main.c
    ${CS_ROOT}/src/kernel/kernel_glue.c
    ${CS_ROOT}/src/kernel/initcall.c
    ${CS_ROOT}/src/kernel/console.c
    ${CS_ROOT}/src/kernel/log.c
    ${CS_ROOT}/src/kernel/shell.c
    ${CS_ROOT}/src/kernel/size.c
    ${CS_ROOT}/src/drivers/clk/sunxi-ccu.c
    ${CS_ROOT}/src/drivers/pinctrl/pinctrl-sunxi.c
    ${CS_ROOT}/src/drivers/tty/serial/uart-dw.c
    ${CS_ROOT}/src/drivers/mmc/host/sunxi-mmc.c
    ${CS_ROOT}/src/drivers/mmc/core/sd.c
    ${CS_ROOT}/src/drivers/mmc/card/mmcblk.c
    ${CS_ROOT}/src/drivers/spi/spi-sunxi.c
    ${CS_ROOT}/src/drivers/i2c/i2c-sunxi.c
    ${CS_ROOT}/src/drivers/display/panel-st7789.c
    ${CS_ROOT}/src/drivers/display/display.c
    ${CS_ROOT}/src/drivers/input/touchscreen/cst816.c
    ${CS_ROOT}/src/gui/lvgl_port.c
    ${CS_ROOT}/src/usb/usb_mass_storage.c
    ${CS_ROOT}/src/usb/usb_platform.c
    ${CS_ROOT}/src/block/genhd.c
    ${CS_ROOT}/src/block/bdev.c
    ${CS_ROOT}/src/block/partitions.c
    ${CS_ROOT}/src/block/layout.c
    ${CS_ROOT}/src/fs/vfs.c
    ${CS_ROOT}/src/fs/vfs_os.c
    ${CS_ROOT}/src/fs/vfs_fat.c
    ${CS_ROOT}/src/fs/vfs_ext.c
    ${CS_ROOT}/src/fs/vfs_shell.c
    ${CS_ROOT}/board/cardsight_board.c
)

# FatFs: the submodule is the upstream mirror (source/); our ffconf.h is kept in the tree and
# the sources are compiled from a copy in the build directory, because ff.c includes its
# configuration with a quoted name and would otherwise take the submodule's default.
set(CS_FATFS_DIR ${CMAKE_BINARY_DIR}/fatfs)
file(MAKE_DIRECTORY ${CS_FATFS_DIR})
foreach(CS_FATFS_FILE ff.c ff.h ffunicode.c ffsystem.c diskio.h)
    configure_file(${CS_ROOT}/third_party/fatfs/source/${CS_FATFS_FILE}
        ${CS_FATFS_DIR}/${CS_FATFS_FILE} COPYONLY)
endforeach()
configure_file(${CS_ROOT}/src/fs/fatfs/ffconf.h ${CS_FATFS_DIR}/ffconf.h COPYONLY)
list(APPEND CS_PLATFORM_SOURCES
    ${CS_FATFS_DIR}/ff.c
    ${CS_FATFS_DIR}/ffunicode.c
    ${CS_FATFS_DIR}/ffsystem.c
)

file(GLOB CS_LWEXT4_SOURCES CONFIGURE_DEPENDS ${CS_ROOT}/third_party/lwext4/src/*.c)
list(APPEND CS_PLATFORM_SOURCES ${CS_LWEXT4_SOURCES})

# CherryUSB: the upstream submodule plus the board patch series (the MUSB glue and the mass
# storage changes this board needs), applied to a copy in the build directory so the
# submodule itself stays untouched.
set(CS_USB_ROOT ${CMAKE_BINARY_DIR}/cherryusb)
file(REMOVE_RECURSE ${CS_USB_ROOT})
file(COPY ${CS_ROOT}/third_party/CherryUSB/ DESTINATION ${CS_USB_ROOT})
execute_process(
    COMMAND patch -p1 -s -d ${CS_USB_ROOT} -i ${CS_ROOT}/patches/cherryusb/0001-board-support.patch
    RESULT_VARIABLE CS_PATCH_RESULT)
if(NOT CS_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "the CherryUSB board patch does not apply (${CS_PATCH_RESULT})")
endif()
set(CS_USB_SOURCES
    ${CS_USB_ROOT}/osal/usb_osal_freertos.c
    ${CS_USB_ROOT}/core/usbd_core.c
    ${CS_USB_ROOT}/class/msc/usbd_msc.c
    ${CS_USB_ROOT}/port/musb/usb_dc_musb.c
    ${CS_USB_ROOT}/port/musb/usb_glue_sunxi.c
)
list(APPEND CS_PLATFORM_SOURCES ${CS_USB_SOURCES})

# LVGL: the core, the widgets and the fonts; the image coders, the other display drivers and
# the extras are not linked into this image.
file(GLOB_RECURSE CS_LVGL_SOURCES CONFIGURE_DEPENDS ${CS_ROOT}/third_party/LVGL/src/*.c)
list(FILTER CS_LVGL_SOURCES EXCLUDE REGEX "/(libs|others|drivers)/")
list(APPEND CS_PLATFORM_SOURCES ${CS_LVGL_SOURCES})

set(CS_APP_SOURCES "")
if(CARDSIGHT_WITH_APP)
    file(GLOB CS_APP_SOURCES CONFIGURE_DEPENDS
        ${CS_ROOT}/apps/cardsight/main.c
        ${CS_ROOT}/apps/cardsight/app/*.c
        ${CS_ROOT}/apps/cardsight/storage/*.c
        ${CS_ROOT}/apps/cardsight/ui/*.c
        ${CS_ROOT}/apps/cardsight/ui/pages/*.c
        ${CS_ROOT}/apps/cardsight/fonts/*.c
    )
    # the application was built with this flag on SyterOS: the C library of this
    # toolchain declares uint32_t as unsigned long, which upsets the format checker
    set(CS_APP_FORMAT_SOURCES ${CS_APP_SOURCES})
    list(FILTER CS_APP_FORMAT_SOURCES EXCLUDE REGEX "/fonts/")
    set_source_files_properties(${CS_APP_FORMAT_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-format")
endif()

add_executable(cardsight ${CS_KERNEL_SOURCES} ${CS_PLATFORM_SOURCES} ${CS_APP_SOURCES})

target_include_directories(cardsight PRIVATE
    ${CS_ROOT}
    ${CS_ROOT}/include
    ${CS_ROOT}/src
    ${CS_ROOT}/board
    ${CS_ROOT}/apps/cardsight
    ${CS_ROOT}/third_party/FreeRTOS-Kernel/include
    ${CS_ROOT}/third_party/FreeRTOS-Kernel/portable/GCC/RISC-V
    ${CS_ROOT}/third_party/FreeRTOS-Kernel/portable/GCC/RISC-V/chip_specific_extensions/RISCV_MTIME_CLINT_no_extensions
    ${CS_ROOT}/src/kernel
    ${CS_ROOT}/src/gui
    ${CS_ROOT}/src/usb
    ${CS_FATFS_DIR}
    ${CS_ROOT}/third_party/lwext4/include
    ${CS_ROOT}/third_party/LVGL
    ${CS_USB_ROOT}
    ${CS_USB_ROOT}/class/msc
    ${CS_USB_ROOT}/common
    ${CS_USB_ROOT}/core
    ${CS_USB_ROOT}/osal
    ${CS_USB_ROOT}/port/musb
)

file(GLOB CS_FONT_SOURCES CONFIGURE_DEPENDS ${CS_ROOT}/apps/cardsight/fonts/*.c)
set_source_files_properties(${CS_LWEXT4_SOURCES} ${CS_LVGL_SOURCES} ${CS_FONT_SOURCES}
    ${CS_USB_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
# the T-Head cache maintenance instructions are an extension of their own
set_source_files_properties(${CS_ROOT}/src/arch/cache.c
    PROPERTIES COMPILE_OPTIONS "-march=rv32imafdc_zicsr_zifencei_xtheadcmo")
set_source_files_properties(
    ${CS_FATFS_DIR}/ff.c
    ${CS_FATFS_DIR}/ffunicode.c
    ${CS_FATFS_DIR}/ffsystem.c
    PROPERTIES COMPILE_OPTIONS "-w")

target_compile_options(cardsight PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
    -Werror=implicit-function-declaration -Werror=return-type
)
if(CARDSIGHT_WERROR)
    target_compile_options(cardsight PRIVATE -Werror)
endif()

target_compile_definitions(cardsight PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    CONFIG_STORAGE_PROXY_SOURCE_DISK="mmcblk0"
    CONFIG_STORAGE_PROXY_MAX_ENTRIES=8192
    CONFIG_STORAGE_PROXY_STRING_BYTES=1048576
    CONFIG_STORAGE_PROXY_SHELL=1
    CONFIG_SHELL=1
    CONFIG_VFS_MAX_MOUNTS=8
    CONFIG_VFS_MAX_FILES=16
    CONFIG_VFS_MAX_FS_TYPES=8
    CONFIG_EXT_FS_MAX_MOUNTS=3
    CONFIG_USE_DEFAULT_CFG=1
    CONFIG_HAVE_OWN_ERRNO=0
    CONFIG_HAVE_OWN_OFLAGS=1
    CONFIG_JOURNALING_ENABLE=1
    CONFIG_EXT4_BLOCKDEVS_COUNT=3
    CONFIG_EXT4_MOUNTPOINTS_COUNT=3
    CONFIG_LOG_LEVEL=4
)

target_link_options(cardsight PRIVATE
    -T ${CS_ROOT}/board/cardsight.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/cardsight.map
    -Wl,--print-memory-usage
)

target_link_libraries(cardsight PRIVATE m c gcc)

add_custom_command(TARGET cardsight POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/images
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:cardsight> ${CMAKE_BINARY_DIR}/images/cardsight.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:cardsight>
    COMMENT "cardsight.bin"
)
