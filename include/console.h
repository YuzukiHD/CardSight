/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The console stream the log, the shell and the C library's stdout share. */

#ifndef CARDSIGHT_CONSOLE_H
#define CARDSIGHT_CONSOLE_H

#include <stddef.h>

void console_write(const char *data, size_t length);
/** One byte, or -EAGAIN after @p timeout_ms milliseconds. */
int console_read(int timeout_ms);
/** The receive side needs the kernel; call once the scheduler runs. */
int console_start_receive(void);

#endif /* CARDSIGHT_CONSOLE_H */
