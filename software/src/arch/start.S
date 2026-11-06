/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Entry point of the C907 image: machine mode, PSRAM already initialised by the loader.
 *
 * The XuanTie core needs its ISA extension switch, the hardware misaligned access handler, the
 * cluster cache coherency setting and the branch/cache reset before anything else runs.
 */

	.section .text.start, "ax"
	.global _start
	.type _start, @function
_start:
	/* no interrupt may fire before the kernel is ready */
	csrw	mie, zero
	csrw	mip, zero
	li	t0, 0x8			/* mstatus.MIE */
	csrc	mstatus, t0

	/* These run on both harts: each one needs its own ISA switch, coherent cluster
	 * behaviour and caches. Hart 0 clears bss and runs the kernel setup; hart 1 joins
	 * the scheduler when hart 0 powers it on. */
	/* mxstatus: THEADISAEE (22), MAEE (21), CLINTEE (17), UCME (16), MM (15) */
	csrr	t0, 0x7c0
	li	t1, ((1 << 22) | (1 << 21) | (1 << 17) | (1 << 16) | (1 << 15))
	or	t0, t0, t1
	csrw	0x7c0, t0

	/* cache coherency unit of the cluster */
	li	t0, 1
	csrw	0x7f3, t0

	/* invalidate I/D cache, BHT and BTB, then switch them on */
	li	t0, 0x70013
	csrw	0x7c2, t0
	fence
	fence.i
	li	t0, 0x11ff
	csrw	0x7c1, t0
	fence
	li	t0, 0x16e30c
	csrw	0x7c5, t0
	fence

	/* float and vector registers usable (initial state); the port saves them per task.
	 * Both harts need this before any C code can use the vector unit. */
	li	t0, ((1 << 13) | (1 << 9)) /* mstatus.FS = initial, mstatus.VS = initial */
	csrs	mstatus, t0
	csrw	fcsr, zero

	csrr	t0, mhartid
	bnez	t0, hart_secondary

	la	sp, __stack_top

	la	t0, early_trap
	csrw	mtvec, t0

	/* clear bss */
	la	t0, __bss_start
	la	t1, __bss_end
1:
	bgeu	t0, t1, 2f
	sw	zero, 0(t0)
	addi	t0, t0, 4
	j	1b
2:
	call	main

hart_secondary:
	/* Hart 1 boots here after hart 0 powers it on (panel-worker.c). It runs no kernel: it
	 * only pushes the panel's pixels from the mailbox. Its boot stack is the top of the
	 * boot SRAM. */
	li	sp, 0x00120000
	call	sunxi_panel_worker_main
1:	wfi
	j	1b

	/* first trap vector: report and halt */
	.section .text
	.global early_trap
	.align	4
early_trap:
	csrr	a0, mcause
	csrr	a1, mepc
	call	early_trap_report
1:
	wfi
	j	1b
