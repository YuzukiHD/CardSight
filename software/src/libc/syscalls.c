/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file syscalls.c
 * @brief The C library's side of the platform: system call stubs, console streams and sbrk.
 *
 * stdout and stderr go to the console, stdin comes from it, and a bump allocator for malloc
 * hands out memory between the end of the image and the end of PSRAM. The stubs follow the
 * newlib convention: integer returns carry the result and set errno on failure, pointers
 * return (void *)-1 on failure.
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

/** @brief Start of the heap region reserved for the C library. */
extern char __heap_start[];
/** @brief End of the heap region, at the end of PSRAM. */
extern char __heap_end[];

/** @brief Next address sbrk will hand out; NULL until the first call. */
static char *sbrk_position;

/**
 * @brief Grow the C heap by a number of bytes and return the previous break.
 *
 * The allocator is a bump pointer: the first call starts it at __heap_start, and each call
 * moves it forward by @p increment, never back, because newlib's allocator is the only user
 * and never returns memory to the platform. Requests that would pass __heap_end fail with
 * ENOMEM, as does a negative increment, which cannot be honoured.
 *
 * @param[in] increment Bytes to add to the heap.
 * @return Address of the first new byte, or (void *)-1 with errno set to ENOMEM.
 */
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

/**
 * @brief Report how much of the C heap the bump allocator has handed out.
 *
 * @return Bytes between __heap_start and the current break, or 0 before the first sbrk call.
 */
size_t platform_heap_used(void)
{
	if (sbrk_position == NULL) {
		return 0U;
	}
	return (size_t)(sbrk_position - __heap_start);
}

/* aligned_alloc() of this C library asks for this one */
/**
 * @brief Allocate a block with a given alignment, in the posix_memalign() style.
 *
 * @p alignment must be a power of two and at least the size of a pointer, otherwise EINVAL is
 * returned without allocating. The block itself comes from memalign(), so a successful call
 * is released with free().
 *
 * @param[out] out       Receives the aligned block on success.
 * @param[in]  alignment Required alignment in bytes.
 * @param[in]  size      Requested size in bytes.
 * @return 0 on success, EINVAL for an invalid alignment, or ENOMEM when out of memory.
 */
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

/**
 * @brief Write bytes to the console, whatever the descriptor says.
 *
 * newlib routes both stdout and stderr here, so the descriptor is ignored and every stream
 * ends up on UART0.
 *
 * @param[in] descriptor Ignored.
 * @param[in] buffer     Bytes to write.
 * @param[in] length     Number of bytes.
 * @return Number of bytes accepted, always @p length.
 */
int _write(int descriptor, const void *buffer, size_t length)
{
	(void)descriptor;
	console_write(buffer, length);
	return (int)length;
}

/**
 * @brief Read one byte from the console.
 *
 * Delivers at most one byte per call, even when @p length allows more, because the console is
 * a character device and the reader consumes it a byte at a time. The read waits without a
 * timeout for a byte to arrive; an error or end of input is reported as zero bytes.
 *
 * @param[in]  descriptor Ignored.
 * @param[out] buffer     One-byte destination.
 * @param[in]  length     Size of @p buffer; 0 returns immediately.
 * @return 1 after a byte, 0 for a zero-length request or when nothing could be read.
 */
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

/**
 * @brief Refuse to close a stream, since the console cannot be closed.
 *
 * @param[in] descriptor Ignored.
 * @return Always -1.
 */
int _close(int descriptor)
{
	(void)descriptor;
	return -1;
}

/**
 * @brief Report every descriptor as a character device.
 *
 * newlib's stdio looks at the file type, and reporting a character device matches the
 * terminal-like stream the console provides.
 *
 * @param[in]  descriptor Ignored.
 * @param[out] st         Structure receiving S_IFCHR in st_mode.
 * @return Always 0.
 */
int _fstat(int descriptor, struct stat *st)
{
	(void)descriptor;
	st->st_mode = S_IFCHR;
	return 0;
}

/**
 * @brief Report every descriptor as a terminal.
 *
 * @param[in] descriptor Ignored.
 * @return Always 1.
 */
int _isatty(int descriptor)
{
	(void)descriptor;
	return 1;
}

/**
 * @brief Pretend the console is seekable at offset zero.
 *
 * @param[in] descriptor Ignored.
 * @param[in] offset     Ignored.
 * @param[in] whence     Ignored.
 * @return Always 0.
 */
off_t _lseek(int descriptor, off_t offset, int whence)
{
	(void)descriptor;
	(void)offset;
	(void)whence;
	return 0;
}

/**
 * @brief Report a fixed process id.
 *
 * There is only one address space, so every caller is process 1.
 *
 * @return Always 1.
 */
int _getpid(void)
{
	return 1;
}

/**
 * @brief Reject signals, which the system does not implement.
 *
 * @param[in] pid    Ignored.
 * @param[in] signal Ignored.
 * @return -1 with errno set to EINVAL.
 */
int _kill(int pid, int signal)
{
	(void)pid;
	(void)signal;
	errno = EINVAL;
	return -1;
}

/**
 * @brief Terminate the program by panicking.
 *
 * A task that exits is a bug, so the exit status is dropped and the hart stops in the panic
 * path rather than unwinding to an unknown caller.
 *
 * @param[in] status Exit status; unused.
 */
void _exit(int status)
{
	(void)status;
	cs_panic("exit");
}
