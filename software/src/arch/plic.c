/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The PLIC, machine mode context of hart 0:
 *
 *	0x000000 + 4 * source		priority
 *	0x002000 + 0x80 * context	enable bits
 *	0x200000 + 0x1000 * context	threshold
 *	0x200004 + 0x1000 * context	claim / complete
 *
 * A source's number here is the PLIC source; the SoC's peripheral nodes number them with the
 * CLIC id of the small core, 16 higher. The board file gives the PLIC source directly.
 */

#include <errno.h>
#include <stdbool.h>

#include <hal/io.h>
#include <log.h>

#include "arch/arch.h"
#include "board/cardsight_board.h"

#define PLIC_PRIORITY(source)	   (4U * (source))
#define PLIC_ENABLE(source, ctx)   (0x2000U + 0x80U * (ctx) + 4U * ((source) / 32U))
#define PLIC_THRESHOLD(ctx)	   (0x200000U + 0x1000U * (ctx))
#define PLIC_CLAIM(ctx)		   (0x200004U + 0x1000U * (ctx))
#define PLIC_CONTEXT		   0U
#define MAX_SOURCES		   256U

static struct {
	cs_irq_handler_t handler;
	void *argument;
} plic_vectors[MAX_SOURCES];
static bool plic_ready;
static volatile int plic_nesting;

int cs_in_interrupt(void)
{
	return plic_nesting != 0;
}

void cs_plic_init(void)
{
	uintptr_t base = SUNXI_PLIC_BASE;
	uint32_t source;

	for (source = 0U; source < (SUNXI_PLIC_NDEV + 31U) / 32U; source++) {
		writel(0U, base + PLIC_ENABLE(32U * source, PLIC_CONTEXT));
	}
	writel(0U, base + PLIC_THRESHOLD(PLIC_CONTEXT));
	/* drop whatever the loader left pending */
	while ((source = readl(base + PLIC_CLAIM(PLIC_CONTEXT))) != 0U) {
		writel(source, base + PLIC_CLAIM(PLIC_CONTEXT));
	}
	plic_ready = true;
}

int cs_irq_connect(unsigned int source, cs_irq_handler_t handler, void *argument)
{
	if (!plic_ready || source == 0U || source > SUNXI_PLIC_NDEV || source >= MAX_SOURCES) {
		return -EINVAL;
	}
	plic_vectors[source].handler = handler;
	plic_vectors[source].argument = argument;
	if (readl(SUNXI_PLIC_BASE + PLIC_PRIORITY(source)) == 0U) {
		writel(1U, SUNXI_PLIC_BASE + PLIC_PRIORITY(source));
	}
	return 0;
}

void cs_irq_enable(unsigned int source)
{
	if (plic_ready && source != 0U && source <= SUNXI_PLIC_NDEV) {
		setbits32(SUNXI_PLIC_BASE + PLIC_ENABLE(source, PLIC_CONTEXT), 1U << (source % 32U));
	}
}

void cs_irq_disable(unsigned int source)
{
	if (plic_ready && source != 0U && source <= SUNXI_PLIC_NDEV) {
		clrbits32(SUNXI_PLIC_BASE + PLIC_ENABLE(source, PLIC_CONTEXT), 1U << (source % 32U));
	}
}

/* Called by the FreeRTOS RISC-V port for every interrupt that is not the machine timer. */
void freertos_risc_v_application_interrupt_handler(uintptr_t cause);

void freertos_risc_v_application_interrupt_handler(uintptr_t cause)
{
	uintptr_t base = SUNXI_PLIC_BASE;
	uint32_t source;

	if ((cause & 0x7fffffffU) != 11U) { /* machine external */
		return;
	}
	for (;;) {
		source = readl(base + PLIC_CLAIM(PLIC_CONTEXT));
		if (source == 0U) {
			break;
		}
		if (source < MAX_SOURCES && plic_vectors[source].handler != NULL) {
			plic_nesting++;
			plic_vectors[source].handler(plic_vectors[source].argument);
			plic_nesting--;
		} else {
			cs_irq_disable(source); /* nobody listens: do not storm */
		}
		writel(source, base + PLIC_CLAIM(PLIC_CONTEXT));
	}
}
