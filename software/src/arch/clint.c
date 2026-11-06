/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The machine timer of the C907 cluster: mtime at CLINT + 0xbff8 counts at 24 MHz, the compare
 * register of hart n is CLINT + 0x4000 + 8n. FreeRTOS keeps the compare register and the next
 * value in port.c's globals when configMTIME_BASE_ADDRESS is 0, so the port's trap handler
 * services the tick and this file only starts it.
 */

#include <stddef.h>

#include <hal/io.h>

#include "arch/arch.h"
#include "board/cardsight_board.h"

#define CLINT_MTIME	 (SUNXI_CLINT_BASE + 0xbff8U)
#define CLINT_MTIMECMP(hart) (SUNXI_CLINT_BASE + 0x4000U + 8U * (hart))

/* Defined in port.c; the trap handler updates them for every tick. */
extern uint64_t ullNextTime;
extern const size_t uxTimerIncrementsForOneTick;
extern volatile uint64_t *pullMachineTimerCompareRegister;

static uint64_t boot_ticks;

void cs_clint_init(void)
{
	boot_ticks = *(volatile uint64_t *)CLINT_MTIME;
}

uint64_t cs_clint_ticks(void)
{
	return *(volatile uint64_t *)CLINT_MTIME;
}

uint64_t cs_clint_frequency(void)
{
	return SUNXI_TIMEBASE_HZ;
}

/* Called by xPortStartScheduler() (the port declares it weak when the machine timer is external). */
void vPortSetupTimerInterrupt(void);

void vPortSetupTimerInterrupt(void)
{
	uint64_t now = cs_clint_ticks();

	pullMachineTimerCompareRegister = (volatile uint64_t *)CLINT_MTIMECMP(0U);
	ullNextTime = now + uxTimerIncrementsForOneTick;
	*pullMachineTimerCompareRegister = ullNextTime;
	ullNextTime += uxTimerIncrementsForOneTick;
}

void cs_udelay(uint32_t microseconds)
{
	uint64_t target = cs_clint_ticks() + (uint64_t)microseconds * SUNXI_TIMEBASE_HZ / 1000000U;

	while (cs_clint_ticks() < target) {
	}
}

/* ktime.c: the wall clock from the same counter. */
uint64_t ktime_get_us(void)
{
	return (cs_clint_ticks() - boot_ticks) * 1000000U / SUNXI_TIMEBASE_HZ;
}

uint64_t ktime_get_ms(void)
{
	return ktime_get_us() / 1000U;
}
