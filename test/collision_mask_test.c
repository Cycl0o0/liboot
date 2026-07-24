/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * Copyright-free regression for the host-side packed collision masks. This
 * deliberately uses a private decomp symbol and is therefore linked only
 * against a static liboot test build; it is not part of the public ABI.
 */

#include <stdio.h>

#include "bgcheck.h"
#include "player.h"
#include "sys_math3d.h"

extern s32 BgCheck_PolyIntersectsSubdivision(Vec3f *min, Vec3f *max,
                                             CollisionPoly *polyList,
                                             Vec3s *vtxList, s16 polyId);

int main(void)
{
    Vec3f min = { -1.0f, -1.0f, -1.0f };
    Vec3f max = {  1.0f,  1.0f,  1.0f };
    Vec3f a = { -2.0f, -2.0f, -2.0f };
    Vec3f b = {  2.0f,  2.0f,  2.0f };
    Vec3s vertices[3] = {
        { -2, -2, -2 },
        {  2,  2,  2 },
        { -2,  2,  2 },
    };
    CollisionPoly polygon = {
        .type = 0,
        .flags_vIA = 0,
        .flags_vIB = 1,
        .vIC = 2,
        .normal = { 0, -23170, 23170 },
        .dist = 0,
    };
    volatile u32 one = 1;
    u32 playerHighBit = PLAYER_STATE1_31;
    u32 surfaceHighBit = SURFACETYPE0(0, 0, 0, 0, 0, 0, 0, one);

    if (playerHighBit != 0x80000000u || surfaceHighBit != 0x80000000u) {
        fputs("collision mask regression: high-bit packing failed\n", stderr);
        return 1;
    }

    /* Endpoint a sets vertex bit 0x80. Packing that bit into the high byte
     * used to evaluate signed 0x80 << 24 before the intersection succeeded. */
    if (!Math3D_LineVsCube(&min, &max, &a, &b)) {
        fputs("collision mask regression: expected an intersection\n", stderr);
        return 1;
    }

    /* The same packed mask code is duplicated by the static-polygon
     * subdivision test. This triangle crosses the cube along a-b while its
     * first vertex takes the 0x80 high-bit path. */
    if (!BgCheck_PolyIntersectsSubdivision(&min, &max, &polygon,
                                            vertices, 0)) {
        fputs("collision mask regression: polygon should intersect\n", stderr);
        return 1;
    }

    puts("collision mask regression: PASS");
    return 0;
}
