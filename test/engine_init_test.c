/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "liboot_engine.h"

static int expect(const char *label, int condition)
{
    if (!condition) {
        fprintf(stderr, "engine init: FAIL: %s\n", label);
        return 0;
    }
    return 1;
}

int main(void)
{
    OoTEngineConfig config;
    OoTEngineInput input;
    unsigned char snapshot[sizeof(config)];
    unsigned char *bytes = (unsigned char *)&config;
    const uint32_t requiredConfigSize =
        (uint32_t)(offsetof(OoTEngineConfig, romSize) + sizeof(config.romSize));
    size_t i;
    int ok = 1;

    ok &= expect("runtime API", oot_engine_api_version() == OOT_ENGINE_API_VERSION);
    ok &= expect("null config", oot_engine_config_init_sized(
                     NULL, (uint32_t)sizeof(config), OOT_ENGINE_API_VERSION) ==
                     OOT_ENGINE_RESULT_INVALID_ARGUMENT);

    memset(&config, 0xA5, sizeof(config));
    memcpy(snapshot, &config, sizeof(config));
    ok &= expect("config mismatch result", oot_engine_config_init_sized(
                     &config, (uint32_t)sizeof(config),
                     OOT_ENGINE_API_VERSION + 1u) ==
                     OOT_ENGINE_RESULT_API_VERSION);
    ok &= expect("config mismatch is write-free",
                 memcmp(&config, snapshot, sizeof(config)) == 0);

    ok &= expect("short config result", oot_engine_config_init_sized(
                     &config, requiredConfigSize - 1u,
                     OOT_ENGINE_API_VERSION) ==
                     OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= expect("short config is write-free",
                 memcmp(&config, snapshot, sizeof(config)) == 0);

    ok &= expect("partial config result", oot_engine_config_init_sized(
                     &config, requiredConfigSize, OOT_ENGINE_API_VERSION) ==
                     OOT_ENGINE_RESULT_OK);
    ok &= expect("partial config fields",
                 config.structSize == requiredConfigSize &&
                 config.apiVersion == OOT_ENGINE_API_VERSION &&
                 config.romData == NULL && config.romSize == 0u);
    for (i = requiredConfigSize; i < sizeof(config); ++i) {
        ok &= expect("partial config canary", bytes[i] == 0xA5u);
    }

    ok &= expect("full config result",
                 oot_engine_config_init(&config) == OOT_ENGINE_RESULT_OK);
    ok &= expect("full config defaults",
                 config.structSize == sizeof(config) &&
                 config.apiVersion == OOT_ENGINE_API_VERSION &&
                 config.actorCapacity == OOT_ENGINE_DEFAULT_ACTOR_CAPACITY &&
                 config.maxSubsteps == OOT_ENGINE_DEFAULT_MAX_SUBSTEPS &&
                 config.fixedStepSeconds == OOT_ENGINE_DEFAULT_FIXED_STEP);

    memset(&input, 0xA5, sizeof(input));
    ok &= expect("input mismatch result", oot_engine_input_init_sized(
                     &input, (uint32_t)sizeof(input),
                     OOT_ENGINE_API_VERSION + 1u) ==
                     OOT_ENGINE_RESULT_API_VERSION);
    ok &= expect("input init result",
                 oot_engine_input_init(&input) == OOT_ENGINE_RESULT_OK);
    ok &= expect("input defaults",
                 input.structSize == sizeof(input) && input.camLookX == 0.0f &&
                 input.camLookZ == 1.0f && input.stickX == 0.0f &&
                 input.stickY == 0.0f && input.buttons == 0u);

    if (!ok) {
        return 1;
    }
    puts("engine init: PASS");
    return 0;
}
