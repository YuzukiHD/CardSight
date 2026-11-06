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

	/* only hart 0 runs this image; another hart parks */
	csrr	t0, mhartid
	bnez	t0, hart_park

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

	la	sp, __stack_top

	la	t0, early_trap
	csrw	mtvec, t0

	/* float registers usable (initial state); the port saves them per task */
	li	t0, (1 << 13)		/* mstatus.FS = initial */
	csrs	mstatus, t0
	csrw	fcsr, zero

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

hart_park:
	wfi
	j	hart_park

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
