// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_DIAGNOSTICS_H
#define CARDSIGHT_DIAGNOSTICS_H

/* Registers the cardsight shell command; call once at start-up. */
void cardsight_diagnostics_init(void);

/* Walks the pages and the corner masks; returns the number of problems found. */
int cardsight_validate_ui(void);

#endif /* CARDSIGHT_DIAGNOSTICS_H */
