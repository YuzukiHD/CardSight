/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file message_queue.h
 * @brief Message queues: fixed-size items between tasks.
 *
 * A negative timeout waits forever, 0 does not wait; get and put answer 0 or -ETIMEDOUT /
 * -ENOMEM.
 */

#ifndef CARDSIGHT_MESSAGE_QUEUE_H
#define CARDSIGHT_MESSAGE_QUEUE_H

#include <errno.h>

/**
 * @brief A queue of equal-sized items between tasks.
 */
struct message_queue {
	void *handle; /**< Kernel queue handle, set by msg_queue_init(). */
};

/**
 * @brief Create a message queue.
 *
 * @param[out] queue     Queue structure to initialize.
 * @param[in]  item_size Size of one item in bytes; must be non-zero.
 * @param[in]  capacity  Number of items the queue holds; must be non-zero.
 * @return 0 on success, -EINVAL for a bad argument, or -ENOMEM without queue memory.
 */
int msg_queue_init(struct message_queue *queue, uint32_t item_size, uint32_t capacity);
/**
 * @brief Copy an item into a queue.
 *
 * @param[in] queue      Initialized queue.
 * @param[in] item       Item to copy; @p item_size bytes are read.
 * @param[in] timeout_ms Milliseconds to wait for room; negative waits forever, 0 does not wait.
 * @return 0 when queued, -ETIMEDOUT when no room arrived in time, or -EINVAL for a bad queue.
 */
int msg_queue_put(struct message_queue *queue, const void *item, int timeout_ms);
/**
 * @brief Copy the oldest item out of a queue.
 *
 * @param[in]  queue      Initialized queue.
 * @param[out] item       Destination for the item; @p item_size bytes are written.
 * @param[in]  timeout_ms Milliseconds to wait for an item; negative waits forever, 0 does not
 * wait.
 * @return 0 when an item was copied, -ETIMEDOUT when none arrived in time, or -EINVAL for a
 * bad queue.
 */
int msg_queue_get(struct message_queue *queue, void *item, int timeout_ms);

#endif /* CARDSIGHT_MESSAGE_QUEUE_H */
