/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file console.h
 * @brief The console stream the log, the shell and the C library's stdout share.
 */

#ifndef CARDSIGHT_CONSOLE_H
#define CARDSIGHT_CONSOLE_H

#include <stddef.h>

/**
 * @brief Write bytes to the console.
 *
 * @details Each newline is preceded by a carriage return so a terminal returns to column zero,
 * then the byte is sent synchronously to the UART.
 *
 * @param data   Bytes to write; may contain any value.
 * @param length Number of bytes in @p data.
 */
void console_write(const char *data, size_t length);
/**
 * @brief Read one byte from the console.
 *
 * @param timeout_ms Milliseconds to wait for input.
 * @return The byte value (0 to 255), -EAGAIN after @p timeout_ms milliseconds, or -ENODEV when
 * the receive side has not been started.
 */
int console_read(int timeout_ms);
/**
 * @brief Start interrupt-driven console input.
 *
 * @details Creates the receive queue and connects the UART interrupt, so the receive side needs
 * the kernel; call once the scheduler runs.
 *
 * @return 0 on success, -ENODEV without a configured UART, -ENOMEM without queue memory, or
 * -EINVAL when the interrupt cannot be connected.
 */
int console_start_receive(void);

#endif /* CARDSIGHT_CONSOLE_H */
