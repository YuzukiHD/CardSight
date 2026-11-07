/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file FreeRTOSConfig.h
 * @brief FreeRTOS configuration for the CardSight C907.
 *
 * The machine timer is the CLINT at 24 MHz, so configCPU_CLOCK_HZ is that frequency, not the CPU
 * clock: the port derives the timer increment for one tick from it. The port's timer interrupt
 * path needs the compare register, which clint.c sets up in vPortSetupTimerInterrupt().
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configUSE_PREEMPTION	   1
#define configUSE_TIME_SLICING	   1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0

#define configCPU_CLOCK_HZ	((unsigned long)24000000)
#define configTICK_RATE_HZ	((TickType_t)1000)
#define configMAX_PRIORITIES	8
#define configMINIMAL_STACK_SIZE ((unsigned short)256)
#define configMAX_TASK_NAME_LEN	16
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS

/* The C907 has the FPU and the vector unit, so the port keeps their registers per task. */
#define configENABLE_FPU 1
#define configENABLE_VPU 1

#define configUSE_MUTEXES		  1
#define configUSE_RECURSIVE_MUTEXES	  1
#define configUSE_COUNTING_SEMAPHORES	  1
#define configUSE_TASK_NOTIFICATIONS	  1
#define configQUEUE_REGISTRY_SIZE	  8
#define configUSE_QUEUE_SETS		  0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0

#define configUSE_TIMERS	       1
#define configTIMER_TASK_PRIORITY      4
#define configTIMER_QUEUE_LENGTH       10
#define configTIMER_TASK_STACK_DEPTH   512

#define configSUPPORT_STATIC_ALLOCATION	 0
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE		 ((size_t)(3U * 1024U * 1024U))
#define configAPPLICATION_ALLOCATED_HEAP 0

#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK   1
#define configUSE_IDLE_HOOK	       0
#define configUSE_TICK_HOOK	       0
#define configUSE_TRACE_FACILITY       1
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configGENERATE_RUN_TIME_STATS  0
#define configUSE_NEWLIB_REENTRANT     0
#define configENABLE_BACKWARD_COMPATIBILITY 0
#define configUSE_APPLICATION_TASK_TAG 0
#define configRECORD_STACK_HIGH_ADDRESS 1

/* the machine timer: no memory-mapped mtime in the port, clint.c drives it */
#define configMTIME_BASE_ADDRESS    0
#define configMTIMECMP_BASE_ADDRESS 0
#define configISR_STACK_SIZE_WORDS  1024

#define INCLUDE_vTaskPrioritySet	    1
#define INCLUDE_uxTaskPriorityGet	    1
#define INCLUDE_vTaskDelete		    1
#define INCLUDE_vTaskSuspend		    1
#define INCLUDE_vTaskDelayUntil		    1
#define INCLUDE_vTaskDelay		    1
#define INCLUDE_xTaskGetSchedulerState	    1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle	    1
#define INCLUDE_eTaskGetState		    1
#define INCLUDE_xTaskAbortDelay		    1
#define INCLUDE_xTaskGetHandle		    1
#define INCLUDE_xTimerPendFunctionCall	    1

#ifndef __ASSEMBLY__
void cs_panic(const char *message) __attribute__((noreturn));
#define configASSERT(condition)                                                                    \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			cs_panic("assertion failed: " #condition);                                 \
		}                                                                                  \
	} while (0)
#endif

#endif /* FREERTOS_CONFIG_H */
