/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What the arch layer offers: the timer, the interrupt controller and the trap report. */

#ifndef CARDSIGHT_ARCH_H
#define CARDSIGHT_ARCH_H

#include <stdint.h>

typedef void (*cs_irq_handler_t)(void *argument);

/* plic.c */
void cs_plic_init(void);
int cs_irq_connect(uint32_t source, cs_irq_handler_t handler, void *argument);
void cs_irq_enable(uint32_t source);
void cs_irq_disable(uint32_t source);

/* clint.c */
void cs_clint_init(void);
uint64_t cs_clint_ticks(void);
uint64_t cs_clint_frequency(void);
void cs_udelay(uint32_t microseconds);

/* plic.c: whether a handler is running, for code that must not block there */
int cs_in_interrupt(void);

/* trap.c */
void cs_panic(const char *message) __attribute__((noreturn));

#endif /* CARDSIGHT_ARCH_H */
