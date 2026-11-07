// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file diagnostics.h
 * @brief Board-side diagnostics that drive the UI from the shell.
 */
#ifndef CARDSIGHT_DIAGNOSTICS_H
#define CARDSIGHT_DIAGNOSTICS_H

/**
 * @brief Register the cardsight shell command and its semaphore; call once at start-up.
 */
void cardsight_diagnostics_init(void);

/**
 * @brief Walk the pages and the corner masks and return the number of problems found.
 *
 * @return Number of layout problems detected, zero when the UI passes.
 */
int cardsight_validate_ui(void);

#endif /* CARDSIGHT_DIAGNOSTICS_H */
