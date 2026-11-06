/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The image: one flat load at 0x12000000, executed from PSRAM. The .bss and the stack are not
 * part of the file; the loader writes the file and jumps to _start. The newlib heap is whatever
 * is left between the image and the end of PSRAM.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
	psram (rwx) : ORIGIN = 0x12000000, LENGTH = 0x00800000
}

SECTIONS
{
	.text : ALIGN(4) {
		__image_start = .;
		KEEP(*(.text.start))
		*(.text .text.*)
		. = ALIGN(16);
	} > psram

	.rodata : ALIGN(16) {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
		. = ALIGN(16);
	} > psram

	.initcall : ALIGN(4) {
		__initcall_start = .;
		KEEP(*(.initcall))
		__initcall_end = .;
	} > psram

	.shell_cmd : ALIGN(4) {
		__shell_cmd_start = .;
		KEEP(*(.shell_cmd))
		__shell_cmd_end = .;
	} > psram

	.data : ALIGN(16) {
		*(.data .data.*)
		*(.sdata .sdata.*)
		. = ALIGN(16);
	} > psram

	.bss (NOLOAD) : ALIGN(16) {
		__bss_start = .;
		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end = .;
	} > psram

	.stack (NOLOAD) : ALIGN(16) {
		. += 0x2000;
		__stack_top = .;
	} > psram

	__image_end = .;
	__heap_start = ALIGN(__image_end, 16);
	__heap_end = ORIGIN(psram) + LENGTH(psram);

	ASSERT(__heap_start < __heap_end, "the image does not fit in PSRAM")
	/DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
