/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The interrupt helpers the device stack's FreeRTOS glue expects. This kernel runs one hart, so
 * "inside an interrupt" is the state the PLIC dispatcher publishes while it calls a handler.
 */

#ifndef CARDSIGHT_IRQ_H
#define CARDSIGHT_IRQ_H

#include "FreeRTOS.h"
#include "task.h"

#include "arch/arch.h"

static inline int in_interrupt(void)
{
	return cs_in_interrupt();
}

/* the critical-section names the device stack's glue uses */
static inline void os_crit_enter(void)
{
	taskENTER_CRITICAL();
}

static inline void os_crit_exit(void)
{
	taskEXIT_CRITICAL();
}

#endif /* CARDSIGHT_IRQ_H */
