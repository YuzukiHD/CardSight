/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file shell.c
 * @brief The line shell on the console, plus the built-in help, heap, stack and uptime commands.
 *
 * Commands register themselves in the .shell_cmd section; the shell splits a line at spaces and
 * calls the first word's command. A shell task owns the input line and runs commands directly,
 * so a command blocks the prompt until it returns.
 */

#include <bits.h>
#include <errno.h>
#include <malloc.h>
/** @brief Module name the shell messages are tagged with in the kernel log. */
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


/** @brief First entry of the linker-assembled .shell_cmd section. */
extern const struct shell_cmd __shell_cmd_start[];
/** @brief One past the last entry of the .shell_cmd section. */
extern const struct shell_cmd __shell_cmd_end[];

/**
 * @brief Split a command line and run the matching command.
 *
 * Splits @p line in place at spaces and tabs, collapsing runs of separators and capping the
 * argument vector at eight entries (argv[0] is the command name); the rest of an over-long line
 * is ignored. An empty line is a no-op. The command is looked up by exact name in the
 * .shell_cmd section and called with the argument vector; an unknown name prints a hint and
 * fails. The commands may modify the line, since they receive pointers into it.
 *
 * @param[in,out] line NUL-terminated command line, modified in place.
 * @return The command's return value, 0 for an empty line, or -1 for an unknown command.
 */
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

/**
 * @brief List every registered command with its one-line help text.
 *
 * @param[in] argc Unused.
 * @param[in] argv Unused.
 * @return Always 0.
 */
static int command_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	for (const struct shell_cmd *command = __shell_cmd_start; command < __shell_cmd_end; command++) {
		printf("  %-14s %s\n", command->name, command->help);
	}
	return 0;
}
/** @brief Register the "help" command. */
SHELL_CMD(help, "list the commands", command_help);

/**
 * @brief Print the kernel heap and the C library heap usage side by side.
 *
 * Both allocators sit in PSRAM: FreeRTOS manages its own fixed heap, while newlib's malloc
 * grows through _sbrk(); the third line reports how much of that region sbrk has handed out.
 *
 * @param[in] argc Unused.
 * @param[in] argv Unused.
 * @return Always 0.
 */
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
/** @brief Register the "heap" command. */
SHELL_CMD(heap, "kernel and C library heap usage", command_heap);

/**
 * @brief Short printable name for a FreeRTOS task state.
 *
 * @param[in] state Task state to name.
 * @return A fixed string such as "run", or "?" for an unknown state.
 */
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
/**
 * @brief Print each task's name, priority, state and remaining stack.
 *
 * Collects a snapshot of at most 16 tasks with uxTaskGetSystemState(), then prints one row per
 * task. The stack figure is the high-water mark in StackType_t units, converted to bytes.
 *
 * @param[in] argc Unused.
 * @param[in] argv Unused.
 * @return Always 0.
 */
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
/** @brief Register the "stack" command. */
SHELL_CMD(stack, "task stack high-water marks", command_stack);

/**
 * @brief Print the milliseconds elapsed since boot.
 *
 * @param[in] argc Unused.
 * @param[in] argv Unused.
 * @return Always 0.
 */
static int command_uptime(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("up %llu ms\n", (unsigned long long)ktime_get_ms());
	return 0;
}
/** @brief Register the "uptime" command. */
SHELL_CMD(uptime, "milliseconds since the boot", command_uptime);

/**
 * @brief Read console characters into a line and run it on Enter.
 *
 * Prints the prompt, then loops on console_read() with a one second timeout; a timeout just
 * restarts the loop. CR and LF terminate the line, echo a newline and run shell_exec() when
 * the line is not empty. DEL and BS erase the last character by echoing backspace, space and
 * backspace. Printable characters are echoed and stored while the 128-byte line has room;
 * anything else is dropped. The task never exits.
 *
 * @param[in] argument Unused task argument.
 */
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

/**
 * @brief Create the shell task.
 *
 * Runs as an init call at level 3, so the scheduler and the console receive path are already
 * up. The shell task uses a 4 KiB stack and priority 3, below the boot task.
 *
 * @return 0 on success, or -ENOMEM when the task cannot be created.
 */
static int shell_init(void)
{
	if (xTaskCreate(shell_task, "shell", 4096U, NULL, 3U, NULL) != pdPASS) {
		return -ENOMEM;
	}
	return 0;
}
/** @brief Start the shell at init level 3, priority 20. */
INITCALL(3, 20, shell_init);
