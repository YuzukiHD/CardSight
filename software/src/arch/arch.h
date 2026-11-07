/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file arch.h
 * @brief What the arch layer offers: the timer, the interrupt controller and the trap report.
 *
 * Declares the machine timer accessors from clint.c, the PLIC driver from plic.c and the
 * fatal-trap reporting from trap.c.
 */

#ifndef CARDSIGHT_ARCH_H
#define CARDSIGHT_ARCH_H

#include <stdint.h>

/**
 * @brief Interrupt handler installed for one PLIC source.
 *
 * The handler runs in the context of the FreeRTOS trap handler, so it must not block and must
 * not call scheduler functions that require a task context.
 *
 * @param[in] argument Cookie given to cs_irq_connect().
 */
typedef void (*cs_irq_handler_t)(void *argument);

/* plic.c */
/**
 * @brief Reset the PLIC context of this hart and mark the driver ready.
 *
 * Disables every enable word, clears the priority threshold, drops interrupts left pending by
 * the loader and enables the driver.
 */
void cs_plic_init(void);
/**
 * @brief Attach a handler to a PLIC source and give the source a non-zero priority.
 *
 * @param[in] source   PLIC source number, 1 to the number of sources.
 * @param[in] handler  Handler to run when the source fires.
 * @param[in] argument Cookie passed to @p handler.
 * @return 0 on success, or -EINVAL when the driver is not ready or @p source is out of range.
 */
int cs_irq_connect(uint32_t source, cs_irq_handler_t handler, void *argument);
/**
 * @brief Unmask one source in the PLIC of this hart.
 *
 * @param[in] source PLIC source number; out-of-range values are ignored.
 */
void cs_irq_enable(uint32_t source);
/**
 * @brief Mask one source in the PLIC of this hart.
 *
 * @param[in] source PLIC source number; out-of-range values are ignored.
 */
void cs_irq_disable(uint32_t source);

/* clint.c */
/**
 * @brief Record the machine timer value at boot.
 *
 * The recorded value is the origin of the ktime wall clock maintained in clint.c.
 */
void cs_clint_init(void);
/**
 * @brief Read the free-running machine timer (mtime).
 *
 * @return Current mtime value in timer ticks.
 */
uint64_t cs_clint_ticks(void);
/**
 * @brief Report the rate of the machine timer.
 *
 * @return Timer frequency in hertz.
 */
uint64_t cs_clint_frequency(void);
/**
 * @brief Busy-wait for a number of microseconds on the machine timer.
 *
 * @param[in] microseconds Delay length in microseconds.
 *
 * @note Blocks the calling hart; usable before the scheduler and inside interrupt context.
 */
void cs_udelay(uint32_t microseconds);

/* plic.c: whether a handler is running, for code that must not block there */
/**
 * @brief Tell whether an interrupt handler is currently running.
 *
 * @return Non-zero inside a PLIC handler, zero otherwise.
 */
int cs_in_interrupt(void);

/* trap.c */
/**
 * @brief Print a panic message and stop the hart for good.
 *
 * Disables the machine interrupts and parks the hart in a wait-for-interrupt loop.
 *
 * @param[in] message Text to print; must not be NULL.
 */
void cs_panic(const char *message) __attribute__((noreturn));

#endif /* CARDSIGHT_ARCH_H */
