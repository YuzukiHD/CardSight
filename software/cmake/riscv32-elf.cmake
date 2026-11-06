# SPDX-License-Identifier: GPL-2.0-or-later
# Cross toolchain for the C907: XuanTie GCC, rv32imafdc with the Zicsr/Zifencei extensions.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED XUANTIE_TOOLCHAIN)
    if(DEFINED ENV{XUANTIE_TOOLCHAIN})
        set(XUANTIE_TOOLCHAIN $ENV{XUANTIE_TOOLCHAIN})
    else()
        message(FATAL_ERROR
            "XUANTIE_TOOLCHAIN is not set; point it at the XuanTie toolchain directory "
            "(environment variable, or pass -DXUANTIE_TOOLCHAIN=<path>).")
    endif()
endif()

set(CMAKE_C_COMPILER ${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-gcc)
set(CMAKE_ASM_COMPILER ${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-gcc)
set(CMAKE_AR ${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-ar CACHE FILEPATH "")
set(CMAKE_OBJCOPY ${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-objcopy CACHE FILEPATH "")
set(CMAKE_OBJDUMP ${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-objdump CACHE FILEPATH "")
set(CMAKE_SIZE ${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-size CACHE FILEPATH "")
set(CMAKE_RANLIB ${XUANTIE_TOOLCHAIN}/bin/riscv64-unknown-elf-ranlib CACHE FILEPATH "")
set(CMAKE_C_COMPILER_TARGET riscv64-unknown-elf)
set(CMAKE_ASM_COMPILER_TARGET riscv64-unknown-elf)

set(CS_RISCV_ARCH "-march=rv32imafdc_zicsr_zifencei -mabi=ilp32d -mcmodel=medany -mno-relax")

set(CMAKE_C_FLAGS_INIT "${CS_RISCV_ARCH} -O2 -g -ffreestanding -fno-common -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${CS_RISCV_ARCH} -g")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostartfiles -Wl,--gc-sections -Wl,--build-id=none")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")
