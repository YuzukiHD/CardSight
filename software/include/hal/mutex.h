/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file mutex.h
 * @brief Mutexes.
 *
 * WAIT_FOREVER (a negative timeout) waits without a bound; NO_WAIT takes the lock only when it
 * is free. Like every mutex, the task that takes it must be the one that releases it.
 */

#ifndef CARDSIGHT_MUTEX_H
#define CARDSIGHT_MUTEX_H

/** @brief Negative timeout: wait without a bound. */
#define WAIT_FOREVER (-1)
/** @brief Timeout of zero: take the lock only when it is free. */
#define NO_WAIT		 0

/**
 * @brief A sleeping mutual-exclusion lock.
 */
struct mutex {
	void *handle; /**< Kernel semaphore handle, set by mutex_init(). */
};

/**
 * @brief Initialize a mutex.
 *
 * @param[out] lock Mutex structure to initialize.
 */
void mutex_init(struct mutex *lock);
/**
 * @brief Take a mutex, waiting at most a bounded time.
 *
 * @param[in] lock       Mutex to take.
 * @param[in] timeout_ms Milliseconds to wait; negative waits forever, 0 does not wait.
 * @return 0 when the lock was taken, -ETIMEDOUT when it stayed busy, or -EINVAL for an
 * uninitialized mutex.
 */
int mutex_lock_timeout(struct mutex *lock, int timeout_ms);
/**
 * @brief Release a mutex.
 *
 * @param[in] lock Mutex to release; must be held by the calling task.
 */
void mutex_unlock(struct mutex *lock);

#endif /* CARDSIGHT_MUTEX_H */
