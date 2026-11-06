/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Counting semaphores. down_timeout() waits at most @p timeout_ms milliseconds and answers
 * -ETIMEDOUT when the count did not arrive; a negative timeout waits forever.
 */

#ifndef CARDSIGHT_SEMAPHORE_H
#define CARDSIGHT_SEMAPHORE_H

struct semaphore {
	void *handle;
};

void sema_init_limit(struct semaphore *sem, uint32_t initial, uint32_t limit);
int down_timeout(struct semaphore *sem, int timeout_ms);
void up(struct semaphore *sem);

#endif /* CARDSIGHT_SEMAPHORE_H */
