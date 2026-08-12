/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "liboot_engine.h"

_Static_assert(sizeof(OoTEngineLimits) == 72, "limits ABI size");
_Static_assert(offsetof(OoTEngineLimits, capabilityFlags) == 8,
               "limits capability ABI offset");

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
_Static_assert(sizeof(struct OoTLinkGeometryBuffers) == 88,
               "raw geometry ABI size changed");
_Static_assert(sizeof(struct OoTGeometryBatch) == 96,
               "geometry batch ABI size changed");
_Static_assert(sizeof(OoTEngineSceneGeometry) == 96,
               "scene geometry ABI size changed");
_Static_assert(offsetof(OoTEngineSceneGeometry, alpha) == 64,
               "scene alpha ABI offset changed");
_Static_assert(sizeof(OoTEngineFrame) == 560, "frame ABI size changed");
_Static_assert(offsetof(OoTEngineFrame, linkGeometryTruncated) == 210,
               "frame truncation flag must use reserved storage");
_Static_assert(offsetof(OoTEngineFrame, skeleton) == 212,
               "frame skeleton ABI offset changed");
_Static_assert(offsetof(OoTEngineFrame, geometryBatches) == 544,
               "frame geometry batch ABI offset changed");
#endif

static int expect(const char *label, int condition)
{
    if (!condition) {
        fprintf(stderr, "engine limits: FAIL: %s\n", label);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const uint64_t expectedCapabilities =
        (uint64_t)OOT_ENGINE_CAPABILITY_STATIC_WORLD |
        (uint64_t)OOT_ENGINE_CAPABILITY_ROM_SCENE_LOADING |
        (uint64_t)OOT_ENGINE_CAPABILITY_LINK_GEOMETRY |
        (uint64_t)OOT_ENGINE_CAPABILITY_SCENE_GEOMETRY |
        (uint64_t)OOT_ENGINE_CAPABILITY_GEOMETRY_TRUNCATION |
        (uint64_t)OOT_ENGINE_CAPABILITY_ACTOR_QUERY |
        (uint64_t)OOT_ENGINE_CAPABILITY_TARGETS |
        (uint64_t)OOT_ENGINE_CAPABILITY_TEXTURES |
        (uint64_t)OOT_ENGINE_CAPABILITY_FIXED_STEP |
        (uint64_t)OOT_ENGINE_CAPABILITY_AUDIO |
        (uint64_t)OOT_ENGINE_CAPABILITY_DYNAMIC_COLLISION |
        (uint64_t)OOT_ENGINE_CAPABILITY_GEOMETRY_BATCHES |
        (uint64_t)OOT_ENGINE_CAPABILITY_HOST_ACTORS |
        (uint64_t)OOT_ENGINE_CAPABILITY_SCENE_ACTOR_CATALOG |
        (uint64_t)OOT_ENGINE_CAPABILITY_SCENE_LAYERS |
        (uint64_t)OOT_ENGINE_CAPABILITY_SCENE_TRANSITIONS |
        (uint64_t)OOT_ENGINE_CAPABILITY_SCENE_BACKGROUNDS |
        (uint64_t)OOT_ENGINE_CAPABILITY_SCENE_MATERIAL_METADATA |
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
        (uint64_t)OOT_ENGINE_CAPABILITY_MULTI_INSTANCE;
#else
        (uint64_t)OOT_ENGINE_CAPABILITY_PROCESS_SINGLETON;
#endif
    struct {
        OoTEngineLimits limits;
        unsigned char extension[16];
    } extended;
    OoTEngineLimits limits;
    unsigned char snapshot[sizeof(limits)];
    unsigned char *bytes = (unsigned char *)&limits;
    const uint32_t prefixSize =
        (uint32_t)(offsetof(OoTEngineLimits, capabilityFlags) +
                   sizeof(limits.capabilityFlags));
    uint32_t dropped = 0xA5A5A5A5u;
    size_t i;
    int ok = 1;

    ok &= expect("null output", oot_engine_get_limits(NULL) ==
                                   OOT_ENGINE_RESULT_INVALID_ARGUMENT);

    memset(&limits, 0, sizeof(limits));
    memcpy(snapshot, &limits, sizeof(limits));
    ok &= expect("uninitialized tags", oot_engine_get_limits(&limits) ==
                                         OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= expect("uninitialized input is write-free",
                 memcmp(&limits, snapshot, sizeof(limits)) == 0);

    memset(&limits, 0xA5, sizeof(limits));
    limits.structSize =
        (uint32_t)(offsetof(OoTEngineLimits, capabilityFlags) +
                   sizeof(limits.capabilityFlags) - 1u);
    limits.version = OOT_ENGINE_LIMITS_VERSION;
    memcpy(snapshot, &limits, sizeof(limits));
    ok &= expect("short prefix", oot_engine_get_limits(&limits) ==
                                     OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= expect("short prefix is write-free",
                 memcmp(&limits, snapshot, sizeof(limits)) == 0);

    memset(&limits, 0xA5, sizeof(limits));
    limits.structSize = (uint32_t)sizeof(limits);
    limits.version = OOT_ENGINE_LIMITS_VERSION + 1u;
    memcpy(snapshot, &limits, sizeof(limits));
    ok &= expect("version mismatch", oot_engine_get_limits(&limits) ==
                                        OOT_ENGINE_RESULT_API_VERSION);
    ok &= expect("version mismatch is write-free",
                 memcmp(&limits, snapshot, sizeof(limits)) == 0);

    memset(&limits, 0xA5, sizeof(limits));
    limits.structSize = prefixSize;
    limits.version = OOT_ENGINE_LIMITS_VERSION;
    ok &= expect("prefix query", oot_engine_get_limits(&limits) ==
                                    OOT_ENGINE_RESULT_OK);
    ok &= expect("prefix values",
                 limits.structSize == prefixSize &&
                 limits.version == OOT_ENGINE_LIMITS_VERSION &&
                 limits.capabilityFlags == expectedCapabilities);
    for (i = prefixSize; i < sizeof(limits); ++i) {
        ok &= expect("prefix canary", bytes[i] == 0xA5u);
    }

    limits = (OoTEngineLimits)OOT_ENGINE_LIMITS_INIT;
    ok &= expect("full query", oot_engine_get_limits(&limits) ==
                                  OOT_ENGINE_RESULT_OK);
    ok &= expect("full tags and capabilities",
                 limits.structSize == sizeof(limits) &&
                 limits.version == OOT_ENGINE_LIMITS_VERSION &&
                 limits.capabilityFlags == expectedCapabilities);
    ok &= expect("geometry limits",
                 limits.linkTriangleCapacity == OOT_GEO_MAX_TRIANGLES &&
                 limits.sceneTriangleCapacity == OOT_SCENE_MAX_TRIANGLES &&
                 limits.maxLinkTriangleCapacity == OOT_GEO_MAX_CONFIGURABLE_TRIANGLES &&
                 limits.maxSceneTriangleCapacity == OOT_GEO_MAX_CONFIGURABLE_TRIANGLES);
    ok &= expect("world limits",
                 limits.staticSurfaceCapacity == OOT_ENGINE_MAX_STATIC_SURFACES &&
                 limits.waterBoxCapacity == OOT_ENGINE_MAX_WATER_BOXES);
    ok &= expect("host resource limits",
                 limits.maxActorCapacity == OOT_ENGINE_MAX_ACTOR_CAPACITY &&
                 limits.targetCapacity == OOT_ENGINE_MAX_TARGETS &&
                 limits.textureCapacity == OOT_ENGINE_MAX_TEXTURES);
    ok &= expect("time-step limits",
                 limits.maxSubsteps == OOT_ENGINE_MAX_SUBSTEPS &&
                 limits.minFixedStepSeconds == OOT_ENGINE_MIN_FIXED_STEP_SECONDS &&
                 limits.maxFixedStepSeconds == OOT_ENGINE_MAX_FIXED_STEP_SECONDS &&
                 limits.defaultFixedStepSeconds == OOT_ENGINE_DEFAULT_FIXED_STEP &&
                 limits.defaultMaxSubsteps == OOT_ENGINE_DEFAULT_MAX_SUBSTEPS);

    memset(&extended, 0xA5, sizeof(extended));
    extended.limits.structSize = (uint32_t)sizeof(extended);
    extended.limits.version = OOT_ENGINE_LIMITS_VERSION;
    ok &= expect("extended query", oot_engine_get_limits(&extended.limits) ==
                                      OOT_ENGINE_RESULT_OK);
    ok &= expect("extended caller size preserved",
                 extended.limits.structSize == sizeof(extended));
    for (i = 0; i < sizeof(extended.extension); ++i) {
        ok &= expect("extended canary", extended.extension[i] == 0xA5u);
    }

    ok &= expect("uninitialized Link truncation",
                 oot_link_get_geometry_dropped_triangles() == 0u);
    ok &= expect("uninitialized scene truncation",
                 oot_scene_get_geometry_dropped_triangles() == 0u);
    ok &= expect("guarded scene truncation null output",
                 oot_engine_scene_get_dropped_triangles(NULL, NULL) ==
                     OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= expect("guarded scene truncation invalid engine",
                 oot_engine_scene_get_dropped_triangles(NULL, &dropped) ==
                     OOT_ENGINE_RESULT_INVALID_ARGUMENT &&
                 dropped == 0u);

    if (!ok) {
        return 1;
    }
    puts("engine limits: PASS");
    return 0;
}
