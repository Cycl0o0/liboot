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

void guMtxF2L(void)
{
}

void guMtxIdent(void)
{
}

void guMtxIdentF(void)
{
}

/* SM64 ports expose these names with floating-point return values. OoT's
 * libultra functions return signed fixed-point values, so accidentally
 * resolving either call to the host is an ABI mismatch, not just a duplicate
 * implementation. */
float sins(int16_t angle)
{
    (void)angle;
    return -0.25f;
}

float coss(int16_t angle)
{
    (void)angle;
    return -0.5f;
}

int16_t liboot_internal_sins(uint16_t angle);
int16_t liboot_internal_coss(uint16_t angle);

int main(void)
{
    if (oot_engine_api_version() != OOT_ENGINE_API_VERSION) {
        return 1;
    }
    if (liboot_internal_sins(0x4000u) < 32760 ||
        liboot_internal_coss(0u) < 32760) {
        return 2;
    }
    return 0;
}
