/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * Preferred minimal host using liboot's versioned engine-neutral API.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "liboot_engine.h"

static int read_file(const char *path, uint8_t **outData, size_t *outSize)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;

    if (file == NULL) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *outData = data;
    *outSize = (size_t)length;
    return 1;
}

static int require_ok(OoTResult result, const char *operation)
{
    if (result == OOT_ENGINE_RESULT_OK) {
        return 1;
    }
    fprintf(stderr, "%s: %s\n", operation, oot_engine_result_string(result));
    return 0;
}

int main(int argc, char **argv)
{
    static const struct OoTSurface floor[] = {
        { 0, {{ -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 }} },
        { 0, {{ -1000, 0, -1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 }} },
    };
    uint8_t *rom = NULL;
    size_t romSize = 0u;
    OoTEngineConfig config;
    OoTEngineInput input;
    OoTEngine *engine = NULL;
    const OoTEngineFrame *frame = NULL;
    int tick;
    int exitCode = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <legally-obtained-oot-rom.z64>\n", argv[0]);
        return 2;
    }
    if (!read_file(argv[1], &rom, &romSize)) {
        return 1;
    }

    if (oot_engine_api_version() != OOT_ENGINE_API_VERSION) {
        fprintf(stderr, "liboot engine API version mismatch\n");
        free(rom);
        return 1;
    }
    if (!require_ok(oot_engine_config_init(&config), "config init")) {
        free(rom);
        return 1;
    }
    config.romData = rom;
    config.romSize = romSize;
    config.renderFlags = OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS;
    if (!require_ok(oot_engine_create(&config, &engine), "engine create")) {
        free(rom);
        return 1;
    }
    free(rom); /* creation copied the ROM synchronously */

    if (!require_ok(oot_engine_static_world_load(
                        engine, floor, 2u, NULL, 0u), "world load") ||
        !require_ok(oot_engine_link_create(engine, 0.0f, 0.0f, 0.0f),
                    "Link create") ||
        !require_ok(oot_engine_link_set_equipment(
                        engine, OOT_SWORD_MASTER, OOT_SHIELD_HYLIAN,
                        OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI), "equipment")) {
        goto done;
    }

    if (!require_ok(oot_engine_input_init(&input), "input init")) {
        goto done;
    }
    for (tick = 0; tick < 20; ++tick) {
        input.stickY = tick < 12 ? 1.0f : 0.0f;
        input.buttons = tick == 14 ? OOT_ENGINE_BUTTON_A : 0u;
        if (!require_ok(oot_engine_step(engine, &input, &frame), "simulation step")) {
            goto done;
        }
    }

    printf("Link position: %.2f %.2f %.2f\n", frame->link.position[0],
           frame->link.position[1], frame->link.position[2]);
    printf("Frame: %u triangles, %u actors, tick %llu\n",
           frame->geometry.numTriangles, frame->actorCount,
           (unsigned long long)frame->simulationTick);
    exitCode = 0;

done:
    if (engine != NULL) {
        OoTResult destroyResult = oot_engine_destroy(engine);
        if (destroyResult != OOT_ENGINE_RESULT_OK) {
            fprintf(stderr, "engine destroy: %s\n",
                    oot_engine_result_string(destroyResult));
            exitCode = 1;
        }
    }
    return exitCode;
}
