/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The line shell on the console. Commands register themselves in the .shell_cmd section; the
 * shell splits a line at spaces and calls the first word's command.
 */

#include <bits.h>
#include <errno.h>
#define LOG_MODULE "shell"

#include <init.h>
#include <stdio.h>
#include <string.h>

#include <shell.h>
#include <console.h>
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
	(void)argc;
	(void)argv;
	printf("kernel heap: %u bytes free, %u bytes ever\n", (unsigned)xPortGetFreeHeapSize(),
		(unsigned)xPortGetMinimumEverFreeHeapSize());
	return 0;
}
SHELL_CMD(heap, "show the kernel heap", command_heap);

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
