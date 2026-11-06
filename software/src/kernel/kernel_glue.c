/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The small wrappers the application uses: mutexes, message queues and semaphores. */

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include <hal/delay.h>
#include <hal/kernel.h>

#include "arch/arch.h"

static TickType_t ticks_for(int timeout_ms)
{
	if (timeout_ms < 0) {
		return portMAX_DELAY;
	}
	return pdMS_TO_TICKS((uint32_t)timeout_ms);
}

/* ---- mutex ---- */

void mutex_init(struct mutex *lock)
{
	if (lock != NULL) {
		lock->handle = xSemaphoreCreateMutex();
	}
}

int mutex_lock_timeout(struct mutex *lock, int timeout_ms)
{
	if (lock == NULL || lock->handle == NULL) {
		return -EINVAL;
	}
	return xSemaphoreTake(lock->handle, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

void mutex_unlock(struct mutex *lock)
{
	if (lock != NULL && lock->handle != NULL) {
		(void)xSemaphoreGive(lock->handle);
	}
}

/* ---- message queue ---- */

int msg_queue_init(struct message_queue *queue, unsigned int item_size, unsigned int capacity)
{
	if (queue == NULL || item_size == 0U || capacity == 0U) {
		return -EINVAL;
	}
	queue->handle = xQueueCreate(capacity, item_size);
	return queue->handle != NULL ? 0 : -ENOMEM;
}

int msg_queue_put(struct message_queue *queue, const void *item, int timeout_ms)
{
	if (queue == NULL || queue->handle == NULL) {
		return -EINVAL;
	}
	return xQueueSend(queue->handle, item, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

int msg_queue_get(struct message_queue *queue, void *item, int timeout_ms)
{
	if (queue == NULL || queue->handle == NULL) {
		return -EINVAL;
	}
	return xQueueReceive(queue->handle, item, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

/* ---- semaphore ---- */

void sema_init_limit(struct semaphore *semaphore, unsigned int initial, unsigned int limit)
{
	if (semaphore != NULL) {
		semaphore->handle = xSemaphoreCreateCounting(limit, initial);
	}
}

int down_timeout(struct semaphore *semaphore, int timeout_ms)
{
	if (semaphore == NULL || semaphore->handle == NULL) {
		return -EINVAL;
	}
	return xSemaphoreTake(semaphore->handle, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

void up(struct semaphore *semaphore)
{
	if (semaphore != NULL && semaphore->handle != NULL) {
		(void)xSemaphoreGive(semaphore->handle);
	}
}

/* ---- delays ---- */

void msleep(unsigned int milliseconds)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		vTaskDelay(pdMS_TO_TICKS(milliseconds));
	} else {
		cs_udelay(milliseconds * 1000U);
	}
}

/* ---- the hooks the kernel asks for ---- */

void vApplicationMallocFailedHook(void);

void vApplicationMallocFailedHook(void)
{
	cs_panic("out of kernel heap");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name);

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	cs_panic(name != NULL ? name : "stack overflow");
}
