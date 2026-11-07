/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file clint.c
 * @brief The C907 machine timer: tick start-up, delay loops and the ktime wall clock.
 *
 * The machine timer of the C907 cluster: mtime at CLINT + 0xbff8 counts at 24 MHz, the compare
 * register of hart n is CLINT + 0x4000 + 8n. FreeRTOS keeps the compare register and the next
 * value in port.c's globals when configMTIME_BASE_ADDRESS is 0, so the port's trap handler
 * services the tick and this file only starts it.
 */

#include <stddef.h>

#include <hal/io.h>

#include "arch/arch.h"
#include "board/cardsight_board.h"

/** @brief Address of the 64-bit free-running machine timer. */
#define CLINT_MTIME	 (SUNXI_CLINT_BASE + 0xbff8U)
/** @brief Address of the timer compare register of one hart. */
#define CLINT_MTIMECMP(hart) (SUNXI_CLINT_BASE + 0x4000U + 8U * (hart))

/* Defined in port.c; the trap handler updates them for every tick. */
/** @brief Next mtime value at which the FreeRTOS port raises the next tick. */
extern uint64_t ullNextTime;
/** @brief Timer counts in one FreeRTOS tick. */
extern const size_t uxTimerIncrementsForOneTick;
/** @brief Compare register the port's tick handler programs. */
extern volatile uint64_t *pullMachineTimerCompareRegister;

/** @brief mtime value sampled during cs_clint_init(), the origin of ktime. */
static uint64_t boot_ticks;

/**
 * @brief Record the machine timer value at boot as the origin of the ktime clock.
 */
void cs_clint_init(void)
{
	boot_ticks = *(volatile uint64_t *)CLINT_MTIME;
}

/**
 * @brief Read the free-running machine timer.
 *
 * @return Current mtime value in timer ticks.
 */
uint64_t cs_clint_ticks(void)
{
	return *(volatile uint64_t *)CLINT_MTIME;
}

/**
 * @brief Report the machine timer rate.
 *
 * @return SUNXI_TIMEBASE_HZ, the 24 MHz timer clock.
 */
uint64_t cs_clint_frequency(void)
{
	return SUNXI_TIMEBASE_HZ;
}

/* Called by xPortStartScheduler() (the port declares it weak when the machine timer is external). */
void vPortSetupTimerInterrupt(void);

/**
 * @brief Arm hart 0's compare register and prime the port's tick globals.
 *
 * Points the port at hart 0's compare register, programs the next tick one increment after
 * now, and stores the following deadline in ullNextTime so the trap handler only has to write
 * it back for each subsequent tick.
 */
void vPortSetupTimerInterrupt(void)
{
	uint64_t now = cs_clint_ticks();

	pullMachineTimerCompareRegister = (volatile uint64_t *)CLINT_MTIMECMP(0U);
	ullNextTime = now + uxTimerIncrementsForOneTick;
	*pullMachineTimerCompareRegister = ullNextTime;
	ullNextTime += uxTimerIncrementsForOneTick;
}

/**
 * @brief Busy-wait for a number of microseconds on the machine timer.
 *
 * Computes the deadline in timer ticks from the requested microseconds and the timer rate,
 * then spins until mtime reaches it. The loop does not yield, so it is usable before the
 * scheduler and inside interrupt context, but it burns CPU time.
 *
 * @param[in] microseconds Delay length in microseconds.
 */
void cs_udelay(uint32_t microseconds)
{
	uint64_t target = cs_clint_ticks() + (uint64_t)microseconds * SUNXI_TIMEBASE_HZ / 1000000U;

	while (cs_clint_ticks() < target) {
	}
}

/* ktime.c: the wall clock from the same counter. */
/**
 * @brief Return the wall clock in microseconds since cs_clint_init().
 *
 * Subtracts the boot mtime value and converts the tick difference to microseconds with the
 * timer rate.
 *
 * @return Microseconds since boot.
 */
uint64_t ktime_get_us(void)
{
	return (cs_clint_ticks() - boot_ticks) * 1000000U / SUNXI_TIMEBASE_HZ;
}

/**
 * @brief Return the wall clock in milliseconds since cs_clint_init().
 *
 * @return Milliseconds since boot.
 */
uint64_t ktime_get_ms(void)
{
	return ktime_get_us() / 1000U;
}
