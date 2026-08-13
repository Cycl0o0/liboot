/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include "liboot_engine.h"

#include <stdint.h>

/* These signatures are intentionally host-local. Only the global symbol names
 * matter: liboot's static archive must not define the unprefixed fallbacks. */
void osCreateMesgQueue(void)
{
}

int32_t osSendMesg(void)
{
    return 0;
}

int32_t osRecvMesg(void)
{
    return 0;
}

void guPerspective(void)
{
}

int main(void)
{
    return oot_engine_api_version() == OOT_ENGINE_API_VERSION ? 0 : 1;
}
