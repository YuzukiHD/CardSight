// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/app.h"
#include "ui/ui.h"
#include "app/diagnostics.h"

void app_main(void)
{
	cardsight_diagnostics_init();
	cardsight_app_init();
}
