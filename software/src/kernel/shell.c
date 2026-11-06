/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The line shell on the console. Commands register themselves in the .shell_cmd section; the
 * shell splits a line at spaces and calls the first word's command.
 */

#include <bits.h>
#include <errno.h>
#include <malloc.h>
#define LOG_MODULE "shell"

#include <init.h>
#include <stdio.h>
#include <string.h>

#include <shell.h>
#include <console.h>
#include <hal/kernel.h>
#include <hal/delay.h>
#include <log.h>

#include "FreeRTOS.h"
#include "task.h"


extern const struct shell_cmd __shell_cmd_start[];
extern const struct shell_cmd __shell_cmd_end[];

int shell_exec(char *line)
{
	char *argv[8];
	int argc = 0;
	char *cursor = line;

	while (*cursor != '\0' && argc < (int)ARRAY_SIZE(argv)) {
		while (*cursor == ' ' || *cursor == '\t') {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}
		argv[argc++] = cursor;
		while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
			cursor++;
		}
		if (*cursor != '\0') {
			*cursor++ = '\0';
		}
	}
	if (argc == 0) {
		return 0;
	}
	for (const struct shell_cmd *command = __shell_cmd_start; command < __shell_cmd_end; command++) {
		if (strcmp(command->name, argv[0]) == 0) {
			return command->fn(argc, argv);
		}
	}
	printf("%s: command not found (try 'help')\n", argv[0]);
	return -1;
}

static int command_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	for (const struct shell_cmd *command = __shell_cmd_start; command < __shell_cmd_end; command++) {
		printf("  %-14s %s\n", command->name, command->help);
	}
	return 0;
}
SHELL_CMD(help, "list the commands", command_help);

static int command_heap(int argc, char **argv)
{
	HeapStats_t kernel;
	struct mallinfo libc;

	(void)argc;
	(void)argv;
	vPortGetHeapStats(&kernel);
	libc = mallinfo();
	printf("kernel heap: %u free, %u low, %u largest block, %u blocks\n",
		(uint32_t)kernel.xAvailableHeapSpaceInBytes,
		(uint32_t)kernel.xMinimumEverFreeBytesRemaining,
		(uint32_t)kernel.xSizeOfLargestFreeBlockInBytes, (uint32_t)kernel.xNumberOfFreeBlocks);
	printf("c heap: %u in use, %u free, %u handed out by sbrk\n", (uint32_t)libc.uordblks,
		(uint32_t)libc.fordblks, (uint32_t)platform_heap_used());
	return 0;
}
SHELL_CMD(heap, "kernel and C library heap usage", command_heap);

static const char *task_state_name(eTaskState state)
{
	switch (state) {
	case eRunning: return "run";
	case eReady: return "ready";
	case eBlocked: return "block";
	case eSuspended: return "susp";
	case eDeleted: return "dead";
	default: return "?";
	}
}

/* The high-water mark is the smallest the stack ever got, so a value close to zero is a warning. */
static int command_stack(int argc, char **argv)
{
	TaskStatus_t tasks[16];
	UBaseType_t count;

	(void)argc;
	(void)argv;
	count = uxTaskGetSystemState(tasks, ARRAY_SIZE(tasks), NULL);
	printf("%-10s %-5s %-5s %s\n", "task", "prio", "state", "stack free");
	for (UBaseType_t index = 0U; index < count; index++) {
		printf("%-10s %-5u %-5s %u bytes\n", tasks[index].pcTaskName,
			(unsigned)tasks[index].uxCurrentPriority, task_state_name(tasks[index].eCurrentState),
			(unsigned)tasks[index].usStackHighWaterMark * (unsigned)sizeof(StackType_t));
	}
	return 0;
}
SHELL_CMD(stack, "task stack high-water marks", command_stack);

static int command_uptime(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("up %llu ms\n", (unsigned long long)ktime_get_ms());
	return 0;
}
SHELL_CMD(uptime, "milliseconds since the boot", command_uptime);

static void shell_task(void *argument)
{
	(void)argument;
	char line[128];
	size_t length = 0U;

	printf("\r\ncardsight> ");
	for (;;) {
		int character = console_read(1000);

		if (character < 0) {
			continue;
		}
		if (character == '\r' || character == '\n') {
			printf("\n");
			line[length] = '\0';
			if (length != 0U) {
				(void)shell_exec(line);
			}
			length = 0U;
			printf("cardsight> ");
		} else if (character == 0x7f || character == 0x08) {
			if (length != 0U) {
				length--;
				console_write("\b \b", 3U);
			}
		} else if (character >= 0x20 && length + 1U < sizeof(line)) {
			char echo = (char)character;

			line[length++] = echo;
			console_write(&echo, 1U);
		}
	}
}

static int shell_init(void)
{
	if (xTaskCreate(shell_task, "shell", 4096U, NULL, 3U, NULL) != pdPASS) {
		return -ENOMEM;
	}
	return 0;
}
INITCALL(3, 20, shell_init);
