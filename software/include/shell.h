/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file shell.h
 * @brief The command line on the console: commands are registered in a linker section, the
 * shell splits a line and calls the matching one.
 */

#ifndef CARDSIGHT_SHELL_H
#define CARDSIGHT_SHELL_H

/**
 * @brief One command registered in the .shell_cmd linker section.
 */
struct shell_cmd {
	const char *name; /**< Word that selects the command. */
	const char *help; /**< One-line description shown by help. */
	int (*fn)(int argc, char **argv); /**< Handler; returns the command result. */
};

/**
 * @brief Register a shell command.
 *
 * @details Places a static struct shell_cmd in the .shell_cmd section, keeps it despite not
 * being referenced, and uses @p name both as the command word and as the symbol suffix.
 *
 * @param name Command word, e.g. @c mycmd.
 * @param help One-line description shown by the help command.
 * @param fn   Handler with the signature @c int @c fn(int @c argc, @c char @c **argv).
 */
#define SHELL_CMD(name, help, fn)                        \
	static const struct shell_cmd __shell_cmd_##name \
		__attribute__((used, section(".shell_cmd"))) = { #name, help, fn }

/**
 * @brief Run a command line.
 *
 * @details Splits @p line at spaces and tabs in place, at most eight words, then calls the
 * command whose name matches the first word. An unknown command prints a hint and returns -1.
 *
 * @param[in,out] line NUL-terminated line; modified by the splitting.
 * @return What the command returned, -1 for an unknown command, or 0 for an empty line.
 */
int shell_exec(char *line);

#endif /* CARDSIGHT_SHELL_H */
