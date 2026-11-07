/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file plic.c
 * @brief The PLIC on the machine mode context of hart 0: dispatch, masking and claim/complete.
 *
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

/** @brief Offset of a source's priority register. */
#define PLIC_PRIORITY(source)	   (4U * (source))
/** @brief Offset of the enable word holding one source for a context. */
#define PLIC_ENABLE(source, ctx)   (0x2000U + 0x80U * (ctx) + 4U * ((source) / 32U))
/** @brief Offset of a context's priority threshold. */
#define PLIC_THRESHOLD(ctx)	   (0x200000U + 0x1000U * (ctx))
/** @brief Offset of a context's claim/complete register. */
#define PLIC_CLAIM(ctx)		   (0x200004U + 0x1000U * (ctx))
/** @brief Context index of the machine mode context of hart 0. */
#define PLIC_CONTEXT		   0U
/** @brief Number of source numbers the driver keeps handlers for. */
#define MAX_SOURCES		   256U

/** @brief Handler table indexed by PLIC source number. */
static struct {
	/** @brief Handler for this source, or NULL when nobody listens. */
	cs_irq_handler_t handler;
	/** @brief Cookie passed to @c handler. */
	void *argument;
} plic_vectors[MAX_SOURCES];
/** @brief Set once cs_plic_init() has cleared and configured the context. */
static bool plic_ready;
/** @brief Depth of the handler calls in progress, exposed through cs_in_interrupt(). */
static volatile int plic_nesting;

/**
 * @brief Tell whether a PLIC handler is currently running.
 *
 * @return Non-zero while a handler is executing, zero otherwise.
 */
int cs_in_interrupt(void)
{
	return plic_nesting != 0;
}

/**
 * @brief Reset the machine mode context of hart 0 and mark the driver ready.
 *
 * Clears every enable word covering the SoC's sources, lowers the priority threshold to zero
 * so all priorities pass, then repeatedly claims interrupts and completes them at once to drop
 * whatever the loader left pending. Only after that is the driver marked ready, which gates
 * cs_irq_connect() and the mask helpers.
 */
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

/**
 * @brief Attach a handler to a PLIC source and give the source a priority.
 *
 * Stores the handler and its cookie in the vector table, then raises the source priority to 1
 * if it is still zero, because the PLIC masks sources with priority 0. The source stays masked
 * until cs_irq_enable() is called.
 *
 * @param[in] source   PLIC source number, 1 to the SoC's source count.
 * @param[in] handler  Handler to run when the source fires.
 * @param[in] argument Cookie passed to @p handler.
 * @return 0 on success, or -EINVAL when the driver is not ready or @p source is out of range.
 */
int cs_irq_connect(uint32_t source, cs_irq_handler_t handler, void *argument)
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

/**
 * @brief Unmask one source for the machine mode context of hart 0.
 *
 * Does nothing when the driver is not ready or @p source is out of range.
 *
 * @param[in] source PLIC source number.
 */
void cs_irq_enable(uint32_t source)
{
	if (plic_ready && source != 0U && source <= SUNXI_PLIC_NDEV) {
		setbits32(SUNXI_PLIC_BASE + PLIC_ENABLE(source, PLIC_CONTEXT), 1U << (source % 32U));
	}
}

/**
 * @brief Mask one source for the machine mode context of hart 0.
 *
 * Does nothing when the driver is not ready or @p source is out of range.
 *
 * @param[in] source PLIC source number.
 */
void cs_irq_disable(uint32_t source)
{
	if (plic_ready && source != 0U && source <= SUNXI_PLIC_NDEV) {
		clrbits32(SUNXI_PLIC_BASE + PLIC_ENABLE(source, PLIC_CONTEXT), 1U << (source % 32U));
	}
}

/* Called by the FreeRTOS RISC-V port for every interrupt that is not the machine timer. */
void freertos_risc_v_application_interrupt_handler(uintptr_t cause);

/**
 * @brief Claim and dispatch external interrupts for as long as the PLIC has any pending.
 *
 * Ignores causes other than machine external (with the interrupt bit masked off the cause).
 * Otherwise it claims a source, runs the registered handler with plic_nesting raised so code
 * inside the handler can tell it is in interrupt context, and completes the source so the
 * PLIC can raise it again. A source with no handler is masked to avoid an interrupt storm.
 * The loop repeats until a claim returns 0, which means nothing is left pending.
 *
 * @param[in] cause RISC-V mcause value received by the port's trap handler.
 */
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
