/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Mutexes. WAIT_FOREVER (a negative timeout) waits without a bound; NO_WAIT takes the lock only
 * when it is free. Like every mutex, the task that takes it must be the one that releases it.
 */

#ifndef CARDSIGHT_MUTEX_H
#define CARDSIGHT_MUTEX_H

#define WAIT_FOREVER (-1)
#define NO_WAIT		 0

struct mutex {
	void *handle;
};

void mutex_init(struct mutex *lock);
int mutex_lock_timeout(struct mutex *lock, int timeout_ms);
void mutex_unlock(struct mutex *lock);

#endif /* CARDSIGHT_MUTEX_H */
