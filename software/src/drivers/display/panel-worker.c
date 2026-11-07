/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file panel-worker.c
 * @brief The mailbox between hart 0 (LVGL) and hart 1 (the panel's pixel pusher).
 *
 * Both harts set the cluster's coherence bit in the start code, so shared lines stay coherent
 * and only fences are needed; an explicit invalidate would be broadcast and throw away the
 * other hart's dirty lines, so none is used.
 */

#include "drivers/display/panel-worker.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <board/cardsight_board.h>
#include <drm/sunxi_display.h>
#include <hal/io.h>
#include <init.h>

#include "drivers/display/panel-st7789.h"

/** @brief Command word value: the worker waits for work. */
#define MAILBOX_IDLE 0U
/** @brief Command word value: hart 0 handed a buffer over. */
#define MAILBOX_SEND 1U
/** @brief Command word value: the worker finished; the result is valid. */
#define MAILBOX_DONE 2U

/* Hart 1 of the V861 is off after reset: the CPU configuration block gives it an entry and the
 * per-hart power controller is asked to switch the core on. */
/** @brief CCU register: bus gate of the RISC-V configuration block. */
#define CCU_CFG_GATE	0x050cU
/** @brief Base of the RISC-V CPU configuration block. */
#define RISCV_CFG_BASE	0x08008000U
/** @brief Low word of hart 1's reset vector. */
#define HART1_ENTRY	(RISCV_CFG_BASE + 0x180U)
/** @brief High word of hart 1's reset vector. */
#define HART1_ENTRY_HI	(RISCV_CFG_BASE + 0x184U)
/** @brief Hart 1's core configuration word (ISA and mode). */
#define HART1_CORE_CFG	(RISCV_CFG_BASE + 0x188U)
/** @brief Core configuration bit: run hart 1 as RV32. */
#define CORE_CFG_RV32	(1U << 8)
/** @brief PMC base of hart 1. */
#define HART1_PMC	0x08001000U
/** @brief PMC mode value: power the hart on under manual control. */
#define PMC_MODE_MANUAL_ON 0x18U
/** @brief PMC power value: switch the core on. */
#define PMC_POWER_ON	   0x11U

/**
 * @brief Work hand-over block shared by the two harts.
 *
 * @details All fields are volatile because each hart polls them without a lock. The command
 * word is the state machine; the other fields are only meaningful around it.
 */
struct panel_mailbox {
	/** @brief State: idle, send or done. */
	volatile uint32_t command;
	/** @brief Address of the buffer to send, set before SEND. */
	volatile uint32_t address;
	/** @brief Length of the buffer in bytes, set before SEND. */
	volatile uint32_t length;
	/** @brief Result of the last transfer, set before DONE. */
	volatile int32_t result;
	/** @brief Non-zero once the worker's loop has started. */
	volatile uint32_t alive;
	/** @brief Bumped on every pass of the worker's loop. */
	volatile uint32_t heartbeat;
};

/**
 * @brief Map the mailbox at its fixed shared address.
 *
 * @return The mailbox.
 */
static struct panel_mailbox *panel_mailbox(void)
{
	return (struct panel_mailbox *)SUNXI_PANEL_MAILBOX_ADDRESS;
}

/**
 * @brief Order the mailbox stores before the command word.
 *
 * @details Only store ordering is needed between the mailbox fields and the command word; the
 * full read/write fence would drain the pipeline for no gain.
 */
static void panel_fence(void)
{
	__asm__ volatile("fence w, w" ::: "memory");
}

/**
 * @brief The worker's loop on hart 1.
 *
 * @details Marks the worker alive and clears any stale command, then polls the mailbox. When a
 * buffer is handed over it sends the bytes through the panel's SPI path, records the result and
 * publishes DONE behind a store fence, so hart 0 sees the payload before the state. It never
 * returns.
 */
void sunxi_panel_worker_main(void)
{
	struct panel_mailbox *mailbox = panel_mailbox();

	mailbox->alive = 1U;
	mailbox->command = MAILBOX_IDLE;
	for (;;) {
		int result;

		mailbox->heartbeat++;
		if (mailbox->command != MAILBOX_SEND) {
			continue;
		}
		if (!sunxi_display_ready()) {
			result = -ENODEV;
		} else {
			result = st7789_write_pixels((const void *)(uintptr_t)mailbox->address,
				mailbox->length);
		}
		mailbox->result = result;
		panel_fence();
		mailbox->command = MAILBOX_DONE;
	}
}

/** @brief Linker symbol: the reset vector hart 1 starts at. */
extern char _start[];

/**
 * @brief Switch hart 1 on and point it at the worker entry.
 *
 * @details Ungates the configuration block, selects RV32 for the core, writes the reset vector
 * from @c _start, then asks the PMC to power the hart on in manual mode. A full fence keeps the
 * vector visible before the power request.
 *
 * @return Always 0.
 */
static int panel_worker_init(void)
{
	setbits32(SUNXI_CCU_BASE + CCU_CFG_GATE, (1U << 16) | 1U);
	clrsetbits32(HART1_CORE_CFG, 0x300U, CORE_CFG_RV32);
	writel((uint32_t)(uintptr_t)_start, HART1_ENTRY);
	writel(0U, HART1_ENTRY_HI);
	__asm__ volatile("fence rw, rw" ::: "memory");
	writel(PMC_MODE_MANUAL_ON, HART1_PMC + 0x100U);
	writel(PMC_POWER_ON, HART1_PMC + 0x104U);
	return 0;
}
INITCALL(3, 10, panel_worker_init);

/**
 * @brief Hand a finished buffer over to the worker.
 *
 * @details Refuses an empty request, a worker that never started, and a worker that is still
 * busy. The payload fields are written first and a store fence separates them from the SEND
 * state, so hart 1 cannot observe a half-written request.
 *
 * @param[in] buffer Pixel bytes to send.
 * @param[in] length Size of @p buffer in bytes.
 * @return 0 on success, -EINVAL, -ENODEV or -EBUSY as described above.
 */
int sunxi_panel_worker_start(const void *buffer, size_t length)
{
	struct panel_mailbox *mailbox = panel_mailbox();

	if (buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	if (mailbox->alive == 0U) {
		return -ENODEV;
	}
	if (mailbox->command != MAILBOX_IDLE) {
		return -EBUSY;
	}
	mailbox->address = (uint32_t)(uintptr_t)buffer;
	mailbox->length = (uint32_t)length;
	mailbox->result = 0;
	panel_fence();
	mailbox->command = MAILBOX_SEND;
	return 0;
}

/**
 * @brief Collect the result of the last hand-over.
 *
 * @details Consumes the DONE state: the mailbox goes back to idle, so a second call reports
 * false until another transfer finishes.
 *
 * @param[out] result Result of the SPI transfer.
 * @return true when a transfer finished since the last call.
 */
bool sunxi_panel_worker_done(int *result)
{
	struct panel_mailbox *mailbox = panel_mailbox();

	if (mailbox->command != MAILBOX_DONE) {
		return false;
	}
	mailbox->command = MAILBOX_IDLE;
	*result = mailbox->result;
	return true;
}

/**
 * @brief Block until the worker is idle again.
 *
 * @details Spins on the command word; the mailbox is in normal cacheable memory, so the loop
 * sees the worker's store on the other hart. DONE is consumed on the way out, so a later start
 * finds the mailbox idle.
 */
void sunxi_panel_worker_wait(void)
{
	struct panel_mailbox *mailbox = panel_mailbox();

	while (mailbox->command == MAILBOX_SEND) {
		__asm__ volatile("nop");
	}
	if (mailbox->command == MAILBOX_DONE) {
		mailbox->command = MAILBOX_IDLE;
	}
}

/**
 * @brief Report the worker's state for the shell.
 *
 * @param[out] alive     Non-zero once the worker's loop has started.
 * @param[out] command   Current command word.
 * @param[out] heartbeat How often the worker's loop came around.
 */
void sunxi_panel_worker_status(uint32_t *alive, uint32_t *command, uint32_t *heartbeat)
{
	struct panel_mailbox *mailbox = panel_mailbox();

	*alive = mailbox->alive;
	*command = mailbox->command;
	*heartbeat = mailbox->heartbeat;
}
