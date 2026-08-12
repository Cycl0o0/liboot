/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include "liboot_engine.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_file(const char *path, uint8_t **outData, size_t *outSize)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
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

static int expect(const char *name, OoTResult actual, OoTResult expected)
{
    if (actual == expected) return 1;
    fprintf(stderr, "%s: got %s (%d), expected %s (%d)\n", name,
            oot_engine_result_string(actual), actual,
            oot_engine_result_string(expected), expected);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct OoTSurface world[] = {
        { OOT_SURFACE_STONE, {{ -1000, -100, -1000 }, { -1000, -100, 1000 }, { 1000, -100, 1000 }} },
        { OOT_SURFACE_STONE, {{ -1000, -100, -1000 }, { 1000, -100, 1000 }, { 1000, -100, -1000 }} },
    };
    static const struct OoTSurface replacementWorld[] = {
        { OOT_SURFACE_GRASS, {{ -1000, -200, -1000 }, { -1000, -200, 1000 }, { 1000, -200, 1000 }} },
        { OOT_SURFACE_GRASS, {{ -1000, -200, -1000 }, { 1000, -200, 1000 }, { 1000, -200, -1000 }} },
    };
    static const struct OoTSurface platform[] = {
        { OOT_SURFACE_DEFAULT, {{ -100, 0, -100 }, { -100, 0, 100 }, { 100, 0, 100 }} },
        { OOT_SURFACE_DEFAULT, {{ -100, 0, -100 }, { 100, 0, 100 }, { 100, 0, -100 }} },
    };
    uint8_t *rom = NULL;
    size_t romSize = 0u;
    OoTEngineConfig config = OOT_ENGINE_CONFIG_INIT;
    OoTEngineInput input = OOT_ENGINE_INPUT_INIT;
    OoTEngine *engine = NULL;
    OoTDynamicCollision handle = OOT_DYNAMIC_COLLISION_INVALID;
    OoTDynamicCollision replacementHandle = OOT_DYNAMIC_COLLISION_INVALID;
    struct OoTDynamicCollisionTransform transform = OOT_DYNAMIC_COLLISION_TRANSFORM_INIT;
    struct OoTDynamicCollisionState state;
    struct OoTSurfaceInfo surface;
    const OoTEngineFrame *frame = NULL;
    int ok = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <legally-obtained-oot-rom>\n", argv[0]);
        return 2;
    }
    if (!read_file(argv[1], &rom, &romSize)) return 1;
    config.romData = rom;
    config.romSize = romSize;
    ok &= expect("create engine", oot_engine_create(&config, &engine),
                 OOT_ENGINE_RESULT_OK);
    free(rom);
    if (!ok) return 1;

    ok &= expect("load world", oot_engine_static_world_load(
                     engine, world, 2u, NULL, 0u), OOT_ENGINE_RESULT_OK);
    ok &= expect("create platform", oot_engine_dynamic_collision_create(
                     engine, platform, 2u, &transform,
                     OOT_DYNAMIC_COLLISION_CARRY_POSITION |
                         OOT_DYNAMIC_COLLISION_CARRY_ROTATION_Y,
                     &handle), OOT_ENGINE_RESULT_OK);
    ok &= handle != OOT_DYNAMIC_COLLISION_INVALID;
    ok &= expect("query platform", oot_engine_scene_query_surface(
                     engine, 0.0f, 100.0f, 0.0f, &surface), OOT_ENGINE_RESULT_OK);
    ok &= fabsf(surface.groundY) < 0.01f;

    transform.position[1] = 50.0f;
    ok &= expect("move platform", oot_engine_dynamic_collision_set_transform(
                     engine, handle, &transform), OOT_ENGINE_RESULT_OK);
    ok &= expect("query moved platform", oot_engine_scene_query_surface(
                     engine, 0.0f, 100.0f, 0.0f, &surface), OOT_ENGINE_RESULT_OK);
    ok &= fabsf(surface.groundY - 50.0f) < 0.01f;

    ok &= expect("disable platform", oot_engine_dynamic_collision_set_enabled(
                     engine, handle, 0u), OOT_ENGINE_RESULT_OK);
    ok &= expect("query disabled platform", oot_engine_scene_query_surface(
                     engine, 0.0f, 100.0f, 0.0f, &surface), OOT_ENGINE_RESULT_OK);
    ok &= fabsf(surface.groundY + 100.0f) < 0.01f;
    ok &= expect("enable platform", oot_engine_dynamic_collision_set_enabled(
                     engine, handle, 1u), OOT_ENGINE_RESULT_OK);

    /* Dynamic objects are host entities, not scene-owned objects. Replacing
       the static world rebinds their collision and preserves their handles. */
    ok &= expect("replace world", oot_engine_static_world_load(
                     engine, replacementWorld, 2u, NULL, 0u), OOT_ENGINE_RESULT_OK);
    state.structSize = sizeof(state);
    ok &= expect("state after rebind", oot_engine_dynamic_collision_get_state(
                     engine, handle, &state), OOT_ENGINE_RESULT_OK);
    ok &= state.enabled && fabsf(state.transform.position[1] - 50.0f) < 0.01f;
    ok &= expect("query rebound platform", oot_engine_scene_query_surface(
                     engine, 0.0f, 100.0f, 0.0f, &surface), OOT_ENGINE_RESULT_OK);
    ok &= fabsf(surface.groundY - 50.0f) < 0.01f;

    /* Settle Link onto the platform, then move it between ticks. The retail
       DynaPoly carry transform should move Link by the platform delta. */
    ok &= expect("create Link", oot_engine_link_create(engine, 0.0f, 50.0f, 0.0f),
                 OOT_ENGINE_RESULT_OK);
    ok &= expect("settle Link", oot_engine_step(engine, &input, &frame),
                 OOT_ENGINE_RESULT_OK);
    transform.position[0] = 40.0f;
    ok &= expect("translate under Link", oot_engine_dynamic_collision_set_transform(
                     engine, handle, &transform), OOT_ENGINE_RESULT_OK);
    ok &= expect("carry Link", oot_engine_step(engine, &input, &frame),
                 OOT_ENGINE_RESULT_OK);
    ok &= frame != NULL && fabsf(frame->link.position[0] - 40.0f) < 1.0f;
    state.structSize = sizeof(state);
    ok &= expect("platform interaction state", oot_engine_dynamic_collision_get_state(
                     engine, handle, &state), OOT_ENGINE_RESULT_OK);
    ok &= state.playerOnTop || state.playerAbove;

    ok &= expect("delete platform", oot_engine_dynamic_collision_delete(engine, handle),
                 OOT_ENGINE_RESULT_OK);
    state.structSize = sizeof(state);
    ok &= expect("stale handle", oot_engine_dynamic_collision_get_state(
                     engine, handle, &state),
                 OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND);
    ok &= expect("replacement platform", oot_engine_dynamic_collision_create(
                     engine, platform, 2u, &transform, 0u,
                     &replacementHandle), OOT_ENGINE_RESULT_OK);
    ok &= replacementHandle != handle;
    state.structSize = sizeof(state);
    ok &= expect("stale handle after slot reuse",
                 oot_engine_dynamic_collision_get_state(engine, handle, &state),
                 OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND);
    ok &= expect("delete replacement",
                 oot_engine_dynamic_collision_delete(engine, replacementHandle),
                 OOT_ENGINE_RESULT_OK);
    ok &= expect("destroy", oot_engine_destroy(engine), OOT_ENGINE_RESULT_OK);

    if (!ok) {
        fprintf(stderr, "dynamic collision test failed\n");
        return 1;
    }
    puts("dynamic collision test passed");
    return 0;
}
