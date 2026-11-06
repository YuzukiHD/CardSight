/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The command line on the console: commands are registered in a linker section, the shell
 * splits a line and calls the matching one.
 */

#ifndef CARDSIGHT_SHELL_H
#define CARDSIGHT_SHELL_H

struct shell_cmd {
	const char *name;
	const char *help;
	int (*fn)(int argc, char **argv);
};

/* SHELL_CMD(mycmd, "what it does", mycmd_fn);  int mycmd_fn(int argc, char **argv) */
#define SHELL_CMD(name, help, fn)                        \
	static const struct shell_cmd __shell_cmd_##name \
		__attribute__((used, section(".shell_cmd"))) = { #name, help, fn }

/** Runs a command line (modified in place); returns what the command returned. */
int shell_exec(char *line);

#endif /* CARDSIGHT_SHELL_H */
