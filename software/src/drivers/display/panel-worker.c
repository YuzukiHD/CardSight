/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The mailbox between hart 0 (LVGL) and hart 1 (the panel's pixel pusher). Both harts set the
 * cluster's coherence bit in the start code, so shared lines stay coherent and only fences are
 * needed; an explicit invalidate would be broadcast and throw away the other hart's dirty
 * lines, so none is used.
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

#define MAILBOX_IDLE 0U
#define MAILBOX_SEND 1U
#define MAILBOX_DONE 2U

/* Hart 1 of the V861 is off after reset: the CPU configuration block gives it an entry and the
 * per-hart power controller is asked to switch the core on. */
#define CCU_CFG_GATE	0x050cU /* bus gate of the RISC-V configuration block */
#define RISCV_CFG_BASE	0x08008000U
#define HART1_ENTRY	(RISCV_CFG_BASE + 0x180U)
#define HART1_ENTRY_HI	(RISCV_CFG_BASE + 0x184U)
#define HART1_CORE_CFG	(RISCV_CFG_BASE + 0x188U)
#define CORE_CFG_RV32	(1U << 8)
#define HART1_PMC	0x08001000U /* PMC base of hart 1 */
#define PMC_MODE_MANUAL_ON 0x18U
#define PMC_POWER_ON	   0x11U

struct panel_mailbox {
	volatile uint32_t command;
	volatile uint32_t address;
	volatile uint32_t length;
	volatile int32_t result;
	volatile uint32_t alive;
	volatile uint32_t heartbeat;
};

static struct panel_mailbox *panel_mailbox(void)
{
	return (struct panel_mailbox *)SUNXI_PANEL_MAILBOX_ADDRESS;
}

/* Only store ordering is needed between the mailbox fields and the command word; the full
 * read/write fence would drain the pipeline for no gain. */
static void panel_fence(void)
{
	__asm__ volatile("fence w, w" ::: "memory");
}

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

extern char _start[];

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

void sunxi_panel_worker_status(uint32_t *alive, uint32_t *command, uint32_t *heartbeat)
{
	struct panel_mailbox *mailbox = panel_mailbox();

	*alive = mailbox->alive;
	*command = mailbox->command;
	*heartbeat = mailbox->heartbeat;
}
