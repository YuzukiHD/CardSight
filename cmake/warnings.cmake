# SPDX-License-Identifier: GPL-2.0-or-later

# Warnings: the project's own code is held to these; third-party sources are compiled with -w.
set(CARDSIGHT_HOST_WARNINGS
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-sign-compare
    -Werror=implicit-function-declaration
    -Werror=return-type
)

if(CARDSIGHT_WERROR)
    list(APPEND CARDSIGHT_HOST_WARNINGS -Werror)
endif()
