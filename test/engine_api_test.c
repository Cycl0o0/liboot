/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "liboot_engine.h"

static unsigned sDebugMessages;

static void debug_callback(void *userData, const char *message)
{
    unsigned *counter = (unsigned *)userData;
    if (counter != NULL && message != NULL) {
        ++*counter;
    }
}

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

static int expect_result(const char *label, OoTResult actual, OoTResult expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(stderr, "%s: got %d (%s), expected %d (%s)\n", label,
            (int)actual, oot_engine_result_string(actual), (int)expected,
            oot_engine_result_string(expected));
    return 0;
}

static int frame_is_sane(const OoTEngineFrame *frame)
{
    uint32_t i;
    uint32_t valueCount;

    if (frame == NULL || frame->structSize != sizeof(*frame) ||
        frame->geometry.numTriangles == 0u ||
        frame->geometry.numTriangles > frame->geometry.triangleCapacity ||
        frame->geometry.position == NULL || frame->geometry.normal == NULL ||
        frame->geometry.color == NULL || frame->geometry.uv == NULL ||
        frame->geometry.triTexture == NULL || !frame->skeletonAvailable ||
        frame->link.animId <= 0 ||
        !isfinite(frame->link.position[0]) || !isfinite(frame->link.position[1]) ||
        !isfinite(frame->link.position[2])) {
        return 0;
    }
    valueCount = frame->geometry.numTriangles * 9u;
    for (i = 0u; i < valueCount; ++i) {
        if (!isfinite(frame->geometry.position[i]) ||
            !isfinite(frame->geometry.normal[i]) ||
            !isfinite(frame->geometry.color[i])) {
            return 0;
        }
    }
    return 1;
}

static int expect_scene_runtime(const char *label, OoTEngine *engine,
                                int32_t scene, int32_t activeRoom,
                                int32_t geometryRoom, int32_t roomCount,
                                uint8_t roomType, uint8_t environmentType,
                                int8_t echo, uint8_t lensMode,
                                uint8_t allRoomsLoaded)
{
    struct OoTSceneRuntime runtime;
    OoTResult result = oot_engine_scene_get_runtime(engine, &runtime);
    if (!expect_result(label, result, OOT_ENGINE_RESULT_OK)) {
        return 0;
    }
    if (runtime.structSize != sizeof(runtime) ||
        runtime.version != OOT_SCENE_RUNTIME_VERSION ||
        runtime.sceneIndex != scene || runtime.activeRoomIndex != activeRoom ||
        runtime.geometryRoomIndex != geometryRoom ||
        runtime.roomCount != roomCount || runtime.roomType != roomType ||
        runtime.environmentType != environmentType || runtime.echo != echo ||
        runtime.lensMode != lensMode || runtime.warpSongsDisabled != 0u ||
        runtime.allRoomsLoaded != allRoomsLoaded ||
        runtime.roomMetadataValid != 1u) {
        fprintf(stderr,
                "%s: scene=%d active=%d geometry=%d rooms=%d type=%u env=%u "
                "echo=%d lens=%u warp=%u all=%u metadata=%u\n",
                label, runtime.sceneIndex, runtime.activeRoomIndex,
                runtime.geometryRoomIndex, runtime.roomCount, runtime.roomType,
                runtime.environmentType, runtime.echo, runtime.lensMode,
                runtime.warpSongsDisabled, runtime.allRoomsLoaded,
                runtime.roomMetadataValid);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    static const struct OoTSurface floor[] = {
        { 0, {{ -1200, 0, -1200 }, { -1200, 0, 1200 }, { 1200, 0, 1200 }} },
        { 0, {{ -1200, 0, -1200 }, { 1200, 0, 1200 }, { 1200, 0, -1200 }} },
    };
    static const struct OoTSurface degenerate[] = {
        { 0, {{ 0, 0, 0 }, { 10, 10, 10 }, { 20, 20, 20 }} },
    };
    static const struct OoTSurface distantPlane[] = {
        { 0, {{ 25000, 25000, 0 }, { 30000, 20000, 0 },
                { 25000, 25000, 100 }} },
    };
    uint8_t *rom = NULL;
    size_t romSize = 0u;
    OoTEngineConfig config;
    OoTEngineInput input;
    OoTEngine *engine = NULL;
    OoTEngine *second = NULL;
    OoTEngineTarget target = OOT_ENGINE_INVALID_TARGET;
    struct OoTSurface *hostileWorld = NULL;
    const OoTEngineFrame *frame = NULL;
    uint32_t steps = 0u;
    uint32_t textureCount = 0u;
    OoTEngineTexture texture;
    struct OoTSceneRuntime runtime;
    int32_t nativeSceneResult = 0;
    int ok = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <legally-obtained-oot-rom>\n", argv[0]);
        return 2;
    }
    if (!read_file(argv[1], &rom, &romSize)) {
        return 1;
    }

    ok &= oot_engine_api_version() == OOT_ENGINE_API_VERSION;
    memset(&config, 0xA5, sizeof(config));
    {
        unsigned char unchanged[sizeof(config)];

        memcpy(unchanged, &config, sizeof(config));
        ok &= expect_result("initializer API mismatch",
                            oot_engine_config_init_sized(
                                &config, (uint32_t)sizeof(config),
                                OOT_ENGINE_API_VERSION + 1u),
                            OOT_ENGINE_RESULT_API_VERSION);
        ok &= memcmp(&config, unchanged, sizeof(config)) == 0;
    }
    {
        const uint32_t requiredConfigSize =
            (uint32_t)(offsetof(OoTEngineConfig, romSize) +
                       sizeof(config.romSize));
        unsigned char *bytes = (unsigned char *)&config;
        size_t i;

        memset(&config, 0xA5, sizeof(config));
        ok &= expect_result("partial config init",
                            oot_engine_config_init_sized(
                                &config, requiredConfigSize,
                                OOT_ENGINE_API_VERSION),
                            OOT_ENGINE_RESULT_OK);
        ok &= config.structSize == requiredConfigSize &&
              config.apiVersion == OOT_ENGINE_API_VERSION &&
              config.romData == NULL && config.romSize == 0u;
        for (i = requiredConfigSize; i < sizeof(config); ++i) {
            ok &= bytes[i] == 0xA5u;
        }
    }
    ok &= expect_result("config init", oot_engine_config_init(&config),
                        OOT_ENGINE_RESULT_OK);
    config.romData = rom;
    config.romSize = romSize;
    config.renderFlags = OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS;
    config.debugCallback = debug_callback;
    config.debugUserData = &sDebugMessages;

    config.romSize = (size_t)OOT_ENGINE_MAX_ROM_SIZE + 1u;
    ok &= expect_result("oversized ROM", oot_engine_create(&config, &engine),
                        OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= engine == NULL;
    config.romSize = romSize;

    ok &= expect_result("create", oot_engine_create(&config, &engine),
                        OOT_ENGINE_RESULT_OK);
    if (!ok) {
        free(rom);
        return 1;
    }
    ok &= expect_result("singleton", oot_engine_create(&config, &second),
                        OOT_ENGINE_RESULT_SINGLETON_IN_USE);
    ok &= second == NULL;
    free(rom);
    rom = NULL;

    ok &= expect_result("Link before world", oot_engine_link_create(
                            engine, 0.0f, 0.0f, 0.0f),
                        OOT_ENGINE_RESULT_NOT_AVAILABLE);
    ok &= expect_result("world", oot_engine_static_world_load(
                            engine, floor, 2u, NULL, 0u), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("custom world has no ROM scene",
                        oot_engine_scene_get_runtime(engine, &runtime),
                        OOT_ENGINE_RESULT_NOT_AVAILABLE);
    ok &= runtime.structSize == sizeof(runtime) &&
          runtime.version == OOT_SCENE_RUNTIME_VERSION &&
          runtime.sceneIndex == -1 && runtime.activeRoomIndex == -1;
    ok &= expect_result("degenerate world", oot_engine_static_world_load(
                            engine, degenerate, 1u, NULL, 0u),
                        OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= expect_result("unrepresentable plane", oot_engine_static_world_load(
                            engine, distantPlane, 1u, NULL, 0u),
                        OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    /* Repeated world-spanning triangles would overflow z_bgcheck's fixed
       static-node table. The loader must reject them without aborting or
       discarding the valid floor that is already live. */
    hostileWorld = (struct OoTSurface *)calloc(512u, sizeof(*hostileWorld));
    if (hostileWorld == NULL) {
        ok = 0;
    } else {
        for (uint32_t i = 0u; i < 512u; ++i) {
            hostileWorld[i].vertices[0][0] = -32768;
            hostileWorld[i].vertices[0][1] = 0;
            hostileWorld[i].vertices[0][2] = -32768;
            hostileWorld[i].vertices[1][0] = 32767;
            hostileWorld[i].vertices[1][1] = 0;
            hostileWorld[i].vertices[1][2] = -32768;
            hostileWorld[i].vertices[2][0] = 32767;
            hostileWorld[i].vertices[2][1] = 0;
            hostileWorld[i].vertices[2][2] = 32767;
        }
        ok &= expect_result("oversized collision lookup",
                            oot_engine_static_world_load(engine, hostileWorld,
                                                         512u, NULL, 0u),
                            OOT_ENGINE_RESULT_INVALID_ARGUMENT);
        free(hostileWorld);
        hostileWorld = NULL;
    }
    ok &= expect_result("link create", oot_engine_link_create(
                            engine, 0.0f, 0.0f, 0.0f), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("duplicate Link", oot_engine_link_create(
                            engine, 0.0f, 0.0f, 0.0f),
                        OOT_ENGINE_RESULT_LINK_ALREADY_EXISTS);
    ok &= expect_result("invalid age", oot_engine_link_set_age(engine, 2u),
                        OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= expect_result("equipment", oot_engine_link_set_equipment(
                            engine, OOT_SWORD_MASTER, OOT_SHIELD_HYLIAN,
                            OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("health", oot_engine_link_set_health(engine, 0x30, 0x30),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("magic", oot_engine_link_set_magic(engine, 1u, 0x30),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("target", oot_engine_target_create(
                            engine, 0.0f, 0.0f, 180.0f, 30.0f, &target),
                        OOT_ENGINE_RESULT_OK);
    ok &= target != OOT_ENGINE_INVALID_TARGET;
    ok &= expect_result("target move", oot_engine_target_move(
                            engine, target, 10.0f, 0.0f, 190.0f),
                        OOT_ENGINE_RESULT_OK);

    ok &= expect_result("input init", oot_engine_input_init(&input),
                        OOT_ENGINE_RESULT_OK);
    input.stickY = 1.0f;
    input.buttons = OOT_ENGINE_BUTTON_Z;
    ok &= expect_result("partial advance", oot_engine_advance(
                            engine, 0.01f, &input, &steps, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= steps == 0u && frame == NULL;
    ok &= expect_result("complete advance", oot_engine_advance(
                            engine, 0.04f, &input, &steps, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= steps == 1u && frame_is_sane(frame);

    input.stickY = 0.0f;
    input.buttons = 0u;
    ok &= expect_result("exact step", oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame) && frame->simulationTick == 2u;

    /* The exported animation identity must be canonical and must follow the
       real Player when locomotion changes the active LinkAnimationHeader. */
    if (frame_is_sane(frame)) {
        int16_t initialAnimId = frame->link.animId;
        int sawAnimChange = 0;

        input.stickY = 1.0f;
        for (uint32_t i = 0u; i < 80u; ++i) {
            OoTResult stepResult = oot_engine_step(engine, &input, &frame);
            ok &= expect_result("animation step", stepResult, OOT_ENGINE_RESULT_OK);
            if (stepResult != OOT_ENGINE_RESULT_OK || !frame_is_sane(frame)) {
                ok = 0;
                break;
            }
            if (frame->link.animId != initialAnimId) {
                sawAnimChange = 1;
            }
        }
        if (!sawAnimChange) {
            fprintf(stderr, "Player animId did not change during locomotion\n");
            ok = 0;
        }
        input.stickY = 0.0f;
    }

    ok &= expect_result("child age", oot_engine_link_set_age(engine, OOT_AGE_CHILD),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("stale target", oot_engine_target_move(
                            engine, target, 0.0f, 0.0f, 200.0f),
                        OOT_ENGINE_RESULT_TARGET_NOT_FOUND);
    ok &= expect_result("child bow", oot_engine_link_use_item(engine, OOT_ITEM_BOW),
                        OOT_ENGINE_RESULT_AGE_RESTRICTED);
    ok &= expect_result("adult age", oot_engine_link_set_age(engine, OOT_AGE_ADULT),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("bomb", oot_engine_link_use_item(engine, OOT_ITEM_BOMB),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("post-age step", oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame);

    /* PAL Rev 1 room commands must reach the live PlayState. These values are
       taken directly from cmd 0x08/0x16 in the supplied retail ROM. */
    ok &= expect_result("Fire Temple room 0 load",
                        oot_engine_scene_load(engine, OOT_SCENE_FIRE_TEMPLE, 0,
                                              &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= nativeSceneResult == 0;
    ok &= expect_scene_runtime("Fire Temple room 0", engine,
                               OOT_SCENE_FIRE_TEMPLE, 0, 0, 27,
                               1u, 2u, 4, 0u, 0u);
    ok &= expect_result("scene-preserving child age",
                        oot_engine_link_set_age(engine, OOT_AGE_CHILD),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_scene_runtime("Fire after child age", engine,
                               OOT_SCENE_FIRE_TEMPLE, 0, 0, 27,
                               1u, 2u, 4, 0u, 0u);
    ok &= expect_result("scene-preserving adult age",
                        oot_engine_link_set_age(engine, OOT_AGE_ADULT),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_scene_runtime("Fire after adult age", engine,
                               OOT_SCENE_FIRE_TEMPLE, 0, 0, 27,
                               1u, 2u, 4, 0u, 0u);
    ok &= expect_result("Fire Temple room 1",
                        oot_engine_scene_set_room(engine, 1, &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_scene_runtime("Fire Temple hot room", engine,
                               OOT_SCENE_FIRE_TEMPLE, 1, 1, 27,
                               1u, 3u, 4, 0u, 0u);
    ok &= expect_result("tick after hot-room swap",
                        oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame);
    ok &= expect_result("Fire Temple room 3",
                        oot_engine_scene_set_room(engine, 3, &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_scene_runtime("Fire Temple echo room", engine,
                               OOT_SCENE_FIRE_TEMPLE, 3, 3, 27,
                               1u, 2u, 7, 0u, 0u);
    ok &= expect_result("tick after echo-room swap",
                        oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame);
    ok &= expect_result("Fire Temple invalid room",
                        oot_engine_scene_set_room(engine, 27, &nativeSceneResult),
                        OOT_ENGINE_RESULT_SCENE_LOAD_FAILED);
    ok &= nativeSceneResult == -5;
    ok &= expect_scene_runtime("Fire survives invalid room", engine,
                               OOT_SCENE_FIRE_TEMPLE, 3, 3, 27,
                               1u, 2u, 7, 0u, 0u);

    ok &= expect_result("Shadow Temple room 0 load",
                        oot_engine_scene_load(engine, OOT_SCENE_SHADOW_TEMPLE, 0,
                                              &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_scene_runtime("Shadow Temple lens room", engine,
                               OOT_SCENE_SHADOW_TEMPLE, 0, 0, 23,
                               1u, 0u, 10, 1u, 0u);
    ok &= expect_result("Shadow Temple room 1",
                        oot_engine_scene_set_room(engine, 1, &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_scene_runtime("Shadow Temple normal lens", engine,
                               OOT_SCENE_SHADOW_TEMPLE, 1, 1, 23,
                               1u, 0u, 10, 0u, 0u);
    ok &= expect_result("tick after lens-room swap",
                        oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame);

    ok &= expect_result("Hyrule Field all rooms",
                        oot_engine_scene_load(engine, OOT_SCENE_HYRULE_FIELD, -1,
                                              &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_scene_runtime("Hyrule Field aggregate", engine,
                               OOT_SCENE_HYRULE_FIELD, 0, -1, 1,
                               0u, 0u, 7, 0u, 1u);
    ok &= expect_result("tick in aggregate-room mode",
                        oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame);
    ok &= expect_result("leave ROM scene for custom world",
                        oot_engine_static_world_load(engine, floor, 2u, NULL, 0u),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("ROM scene cleared by custom world",
                        oot_engine_scene_get_runtime(engine, &runtime),
                        OOT_ENGINE_RESULT_NOT_AVAILABLE);
    ok &= expect_result("tick after collision-world replacement",
                        oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame);

    ok &= expect_result("texture count", oot_engine_texture_count(
                            engine, &textureCount), OOT_ENGINE_RESULT_OK);
    ok &= textureCount > 0u;
    if (textureCount > 0u) {
        memset(&texture, 0, sizeof(texture));
        ok &= expect_result("texture", oot_engine_texture_get(
                                engine, 0u, &texture), OOT_ENGINE_RESULT_OK);
        ok &= texture.rgbaPixels != NULL && texture.rgbaSize > 0u &&
              texture.width > 0u && texture.height > 0u;
    }
    ok &= sDebugMessages > 0u;

    ok &= expect_result("link delete", oot_engine_link_delete(engine),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("step without Link", oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_LINK_NOT_FOUND);
    ok &= expect_result("destroy", oot_engine_destroy(engine),
                        OOT_ENGINE_RESULT_OK);
    engine = NULL;

    printf("engine API: %s (%u debug messages, %u textures)\n",
           ok ? "PASS" : "FAIL", sDebugMessages, textureCount);
    return ok ? 0 : 1;
}
