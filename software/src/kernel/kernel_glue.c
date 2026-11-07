/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file kernel_glue.c
 * @brief The small wrappers the application uses: mutexes, message queues and semaphores.
 *
 * Every timeout is in milliseconds and maps to FreeRTOS ticks; a negative timeout means wait
 * forever. Failures are reported as negative errno values rather than FreeRTOS return codes.
 */

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include <hal/delay.h>
#include <hal/kernel.h>

#include "arch/arch.h"

/**
 * @brief Convert a millisecond timeout to FreeRTOS ticks.
 *
 * @param[in] timeout_ms Timeout in milliseconds, or a negative value for an infinite wait.
 * @return portMAX_DELAY for a negative timeout, else the equivalent number of ticks.
 */
static TickType_t ticks_for(int timeout_ms)
{
	if (timeout_ms < 0) {
		return portMAX_DELAY;
	}
	return pdMS_TO_TICKS((uint32_t)timeout_ms);
}

/* ---- mutex ---- */

/**
 * @brief Create the FreeRTOS mutex behind a lock object.
 *
 * A NULL @p lock is ignored, leaving nothing to initialise.
 *
 * @param[in,out] lock Lock object receiving the mutex handle.
 */
void mutex_init(struct mutex *lock)
{
	if (lock != NULL) {
		lock->handle = xSemaphoreCreateMutex();
	}
}

/**
 * @brief Take a mutex, waiting at most for the given timeout.
 *
 * @param[in] lock       Initialised lock object.
 * @param[in] timeout_ms Wait in milliseconds, or a negative value to wait forever.
 * @return 0 once the lock is held, -EINVAL for an uninitialised lock, or -ETIMEDOUT.
 */
int mutex_lock_timeout(struct mutex *lock, int timeout_ms)
{
	if (lock == NULL || lock->handle == NULL) {
		return -EINVAL;
	}
	return xSemaphoreTake(lock->handle, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

/**
 * @brief Release a mutex.
 *
 * Does nothing for a NULL or uninitialised lock.
 *
 * @param[in] lock Lock object to release.
 */
void mutex_unlock(struct mutex *lock)
{
	if (lock != NULL && lock->handle != NULL) {
		(void)xSemaphoreGive(lock->handle);
	}
}

/* ---- message queue ---- */

/**
 * @brief Create a fixed-size message queue.
 *
 * Rejects a NULL queue or a zero item size or capacity before allocating, and reports an
 * allocation failure as -ENOMEM.
 *
 * @param[in,out] queue     Queue object receiving the handle.
 * @param[in]     item_size Size of one message in bytes.
 * @param[in]     capacity  Number of messages the queue can hold.
 * @return 0 on success, -EINVAL for bad arguments, or -ENOMEM when the allocation failed.
 */
int msg_queue_init(struct message_queue *queue, uint32_t item_size, uint32_t capacity)
{
	if (queue == NULL || item_size == 0U || capacity == 0U) {
		return -EINVAL;
	}
	queue->handle = xQueueCreate(capacity, item_size);
	return queue->handle != NULL ? 0 : -ENOMEM;
}

/**
 * @brief Post one message, waiting at most for the given timeout.
 *
 * @param[in] queue      Initialised queue.
 * @param[in] item       Message to copy into the queue.
 * @param[in] timeout_ms Wait in milliseconds, or a negative value to wait forever.
 * @return 0 on success, -EINVAL for an uninitialised queue, or -ETIMEDOUT when it stayed full.
 */
int msg_queue_put(struct message_queue *queue, const void *item, int timeout_ms)
{
	if (queue == NULL || queue->handle == NULL) {
		return -EINVAL;
	}
	return xQueueSend(queue->handle, item, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

/**
 * @brief Receive one message, waiting at most for the given timeout.
 *
 * @param[in]  queue      Initialised queue.
 * @param[out] item       Buffer receiving the message.
 * @param[in]  timeout_ms Wait in milliseconds, or a negative value to wait forever.
 * @return 0 on success, -EINVAL for an uninitialised queue, or -ETIMEDOUT when it stayed empty.
 */
int msg_queue_get(struct message_queue *queue, void *item, int timeout_ms)
{
	if (queue == NULL || queue->handle == NULL) {
		return -EINVAL;
	}
	return xQueueReceive(queue->handle, item, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

/* ---- semaphore ---- */

/**
 * @brief Create a counting semaphore with an initial count and a ceiling.
 *
 * @param[in,out] semaphore Semaphore object receiving the handle.
 * @param[in]     initial   Count the semaphore starts with.
 * @param[in]     limit     Maximum count the semaphore allows.
 *
 * @note A NULL @p semaphore is ignored.
 */
void sema_init_limit(struct semaphore *semaphore, uint32_t initial, uint32_t limit)
{
	if (semaphore != NULL) {
		semaphore->handle = xSemaphoreCreateCounting(limit, initial);
	}
}

/**
 * @brief Take one count from a semaphore, waiting at most for the given timeout.
 *
 * @param[in] semaphore  Initialised semaphore.
 * @param[in] timeout_ms Wait in milliseconds, or a negative value to wait forever.
 * @return 0 on success, -EINVAL for an uninitialised semaphore, or -ETIMEDOUT.
 */
int down_timeout(struct semaphore *semaphore, int timeout_ms)
{
	if (semaphore == NULL || semaphore->handle == NULL) {
		return -EINVAL;
	}
	return xSemaphoreTake(semaphore->handle, ticks_for(timeout_ms)) == pdTRUE ? 0 : -ETIMEDOUT;
}

/**
 * @brief Give one count back to a semaphore.
 *
 * Does nothing for a NULL or uninitialised semaphore.
 *
 * @param[in] semaphore Semaphore to signal.
 */
void up(struct semaphore *semaphore)
{
	if (semaphore != NULL && semaphore->handle != NULL) {
		(void)xSemaphoreGive(semaphore->handle);
	}
}

/* ---- delays ---- */

/**
 * @brief Sleep for a number of milliseconds in any execution context.
 *
 * Yields to the scheduler when it is running, so other tasks get the CPU. Before the scheduler
 * exists it falls back to cs_udelay() and busy-waits instead, which keeps early drivers and
 * polling loops working.
 *
 * @param[in] milliseconds Sleep length in milliseconds.
 */
void msleep(uint32_t milliseconds)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		vTaskDelay(pdMS_TO_TICKS(milliseconds));
	} else {
		cs_udelay(milliseconds * 1000U);
	}
}

/* ---- the hooks the kernel asks for ---- */

void vApplicationMallocFailedHook(void);

/**
 * @brief Halt the system when the FreeRTOS heap runs out.
 *
 * There is no way to recover from a failed kernel allocation here, so the hook turns it into
 * a panic instead of letting the caller run with a NULL handle.
 */
void vApplicationMallocFailedHook(void)
{
	cs_panic("out of kernel heap");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name);

/**
 * @brief Halt the system when FreeRTOS detects a stack overflow.
 *
 * Prints the offending task's name when FreeRTOS supplies one, and the panic message
 * otherwise.
 *
 * @param[in] task Handle of the overflowing task; unused beyond identification by name.
 * @param[in] name Name of the overflowing task, or NULL.
 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	cs_panic(name != NULL ? name : "stack overflow");
}
