/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file semaphore.h
 * @brief Counting semaphores.
 *
 * down_timeout() waits at most @p timeout_ms milliseconds and answers -ETIMEDOUT when the count
 * did not arrive; a negative timeout waits forever.
 */

#ifndef CARDSIGHT_SEMAPHORE_H
#define CARDSIGHT_SEMAPHORE_H

/**
 * @brief A counting semaphore.
 */
struct semaphore {
	void *handle; /**< Kernel semaphore handle, set by sema_init_limit(). */
};

/**
 * @brief Initialize a counting semaphore.
 *
 * @param[out] sem     Semaphore structure to initialize.
 * @param[in]  initial Initial count.
 * @param[in]  limit   Maximum count.
 */
void sema_init_limit(struct semaphore *sem, uint32_t initial, uint32_t limit);
/**
 * @brief Take one count from a semaphore.
 *
 * @param[in] sem        Semaphore to take from.
 * @param[in] timeout_ms Milliseconds to wait; negative waits forever, 0 does not wait.
 * @return 0 when a count was taken, -ETIMEDOUT when none arrived, or -EINVAL for an
 * uninitialized semaphore.
 */
int down_timeout(struct semaphore *sem, int timeout_ms);
/**
 * @brief Add one count to a semaphore.
 *
 * @param[in] sem Semaphore to release.
 */
void up(struct semaphore *sem);

#endif /* CARDSIGHT_SEMAPHORE_H */
