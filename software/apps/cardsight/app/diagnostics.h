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

/**
 * @brief Validate the page that is currently visible.
 *
 * @details The shell command uses this after switching pages, because a full
 * validation would also walk pages that are not on screen.
 *
 * @param[in]  name     Page name reported in the failure log.
 * @param[out] failures Receives the number of new failures.
 */
void validate_active_page(const char *name, int *failures);

#endif /* CARDSIGHT_DIAGNOSTICS_H */
