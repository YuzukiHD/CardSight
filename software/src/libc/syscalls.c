/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The C library's side of the platform: stdout and stderr on the console, stdin from it, and a
 * bump allocator for malloc between the end of the image and the end of PSRAM.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <console.h>

#include "arch/arch.h"

extern char __heap_start[];
extern char __heap_end[];

static char *sbrk_position;

void *_sbrk(ptrdiff_t increment)
{
	char *previous;

	if (sbrk_position == NULL) {
		sbrk_position = __heap_start;
	}
	if (increment < 0) {
		errno = ENOMEM;
		return (void *)-1;
	}
	if ((size_t)(__heap_end - sbrk_position) < (size_t)increment) {
		errno = ENOMEM;
		return (void *)-1;
	}
	previous = sbrk_position;
	sbrk_position += increment;
	return previous;
}

/* aligned_alloc() of this C library asks for this one */
int posix_memalign(void **out, size_t alignment, size_t size)
{
	void *block;

	if (alignment < sizeof(void *) || (alignment & (alignment - 1U)) != 0U) {
		return EINVAL;
	}
	block = memalign(alignment, size);
	if (block == NULL) {
		return ENOMEM;
	}
	*out = block;
	return 0;
}

int _write(int descriptor, const void *buffer, size_t length)
{
	(void)descriptor;
	console_write(buffer, length);
	return (int)length;
}

int _read(int descriptor, void *buffer, size_t length)
{
	int character;

	(void)descriptor;
	if (length == 0U) {
		return 0;
	}
	character = console_read(-1);
	if (character < 0) {
		return 0;
	}
	*(char *)buffer = (char)character;
	return 1;
}

int _close(int descriptor)
{
	(void)descriptor;
	return -1;
}

int _fstat(int descriptor, struct stat *st)
{
	(void)descriptor;
	st->st_mode = S_IFCHR;
	return 0;
}

int _isatty(int descriptor)
{
	(void)descriptor;
	return 1;
}

off_t _lseek(int descriptor, off_t offset, int whence)
{
	(void)descriptor;
	(void)offset;
	(void)whence;
	return 0;
}

int _getpid(void)
{
	return 1;
}

int _kill(int pid, int signal)
{
	(void)pid;
	(void)signal;
	errno = EINVAL;
	return -1;
}

void _exit(int status)
{
	(void)status;
	cs_panic("exit");
}
