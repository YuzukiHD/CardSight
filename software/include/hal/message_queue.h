/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Message queues: fixed-size items between tasks. A negative timeout waits forever, 0 does not
 * wait; get and put answer 0 or -ETIMEDOUT / -ENOMEM.
 */

#ifndef CARDSIGHT_MESSAGE_QUEUE_H
#define CARDSIGHT_MESSAGE_QUEUE_H

#include <errno.h>

struct message_queue {
	void *handle;
};

int msg_queue_init(struct message_queue *queue, uint32_t item_size, uint32_t capacity);
int msg_queue_put(struct message_queue *queue, const void *item, int timeout_ms);
int msg_queue_get(struct message_queue *queue, void *item, int timeout_ms);

#endif /* CARDSIGHT_MESSAGE_QUEUE_H */
