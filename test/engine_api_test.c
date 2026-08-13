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
static unsigned sSecondDebugMessages;
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
static int sCheckLimitsContention;
static OoTResult sContendedLimitsResult = OOT_ENGINE_RESULT_OK;
static int sContendedLimitsWriteFree;
#endif

static void debug_callback(void *userData, const char *message)
{
    unsigned *counter = (unsigned *)userData;
    if (counter != NULL && message != NULL) {
        ++*counter;
    }
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    if (sCheckLimitsContention) {
        OoTEngineLimits limits = OOT_ENGINE_LIMITS_INIT;
        OoTEngineLimits snapshot;
        memset(&limits, 0xA5, sizeof(limits));
        limits.structSize = sizeof(limits);
        limits.version = OOT_ENGINE_LIMITS_VERSION;
        snapshot = limits;
        sCheckLimitsContention = 0;
        sContendedLimitsResult = oot_engine_get_limits(&limits);
        sContendedLimitsWriteFree =
            memcmp(&limits, &snapshot, sizeof(limits)) == 0;
    }
#endif
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
        frame->geometryBatches == NULL || frame->geometryBatchCount == 0u ||
        frame->geometryBatchCount > frame->geometryBatchCapacity ||
        frame->link.animId <= 0 ||
        !isfinite(frame->link.position[0]) || !isfinite(frame->link.position[1]) ||
        !isfinite(frame->link.position[2])) {
        return 0;
    }
    {
        uint32_t nextTriangle = 0u;
        for (i = 0u; i < frame->geometryBatchCount; ++i) {
            const struct OoTGeometryBatch *batch = &frame->geometryBatches[i];
            if (batch->firstTriangle != nextTriangle || batch->numTriangles == 0u ||
                batch->numTriangles > frame->geometry.numTriangles - nextTriangle ||
                batch->sourceKind > OOT_GEOMETRY_SOURCE_ACTOR ||
                batch->renderPass > OOT_GEOMETRY_PASS_TRANSLUCENT ||
                batch->depthMode > OOT_GEOMETRY_DEPTH_DECAL) {
                return 0;
            }
            nextTriangle += batch->numTriangles;
        }
        if (nextTriangle != frame->geometry.numTriangles) return 0;
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

static int scene_geometry_is_sane(const OoTEngineSceneGeometry *geometry)
{
    uint32_t nextTriangle = 0u;
    if (geometry == NULL || geometry->structSize != sizeof(*geometry) ||
        geometry->numTriangles == 0u ||
        geometry->numTriangles > geometry->triangleCapacity ||
        geometry->xluStartTriangle > geometry->numTriangles ||
        geometry->position == NULL || geometry->triTexture == NULL ||
        geometry->triFlags == NULL || geometry->batches == NULL ||
        geometry->batchCount == 0u ||
        geometry->batchCount > geometry->batchCapacity) {
        return 0;
    }
    for (uint32_t i = 0u; i < geometry->batchCount; ++i) {
        const struct OoTGeometryBatch *batch = &geometry->batches[i];
        uint8_t expectedPass = nextTriangle < geometry->xluStartTriangle
            ? OOT_GEOMETRY_PASS_OPAQUE : OOT_GEOMETRY_PASS_TRANSLUCENT;
        if (batch->firstTriangle != nextTriangle || batch->numTriangles == 0u ||
            batch->numTriangles > geometry->numTriangles - nextTriangle ||
            batch->sourceKind != OOT_GEOMETRY_SOURCE_SCENE_ROOM ||
            batch->renderPass != expectedPass ||
            (expectedPass == OOT_GEOMETRY_PASS_OPAQUE &&
             batch->firstTriangle + batch->numTriangles >
                 geometry->xluStartTriangle)) {
            return 0;
        }
        for (uint32_t tri = batch->firstTriangle;
             tri < batch->firstTriangle + batch->numTriangles; ++tri) {
            if (geometry->triTexture[tri] != batch->textureIndex ||
                geometry->triFlags[tri] != batch->triangleFlags) {
                return 0;
            }
        }
        nextTriangle += batch->numTriangles;
    }
    return nextTriangle == geometry->numTriangles;
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
    static const struct OoTSurface secondFloor[] = {
        { 0, {{ -800, 250, -1000 }, { -800, 250, 1000 }, { 1800, 250, 1000 }} },
        { 0, {{ -800, 250, -1000 }, { 1800, 250, 1000 }, { 1800, 250, -1000 }} },
    };
    static const struct OoTSurface degenerate[] = {
        { 0, {{ 0, 0, 0 }, { 10, 10, 10 }, { 20, 20, 20 }} },
    };
    static const struct OoTSurface distantPlane[] = {
        { 0, {{ 25000, 25000, 0 }, { 30000, 20000, 0 },
                { 25000, 25000, 100 }} },
    };
    static const struct OoTSurface dynamicPlatform[] = {
        { 0, {{ -100, 0, -100 }, { -100, 0, 100 }, { 100, 0, 100 }} },
        { 0, {{ -100, 0, -100 }, { 100, 0, 100 }, { 100, 0, -100 }} },
    };
    uint8_t *rom = NULL;
    size_t romSize = 0u;
    OoTEngineConfig config;
    OoTEngineInput input;
    OoTEngine *engine = NULL;
    OoTEngine *second = NULL;
    OoTEngine *failedEngine = NULL;
    OoTEngineTarget target = OOT_ENGINE_INVALID_TARGET;
    OoTEngineTarget staleTarget = OOT_ENGINE_INVALID_TARGET;
    OoTEngineTarget secondTarget = OOT_ENGINE_INVALID_TARGET;
    OoTEngineHostActor hostActor = OOT_ENGINE_INVALID_HOST_ACTOR;
    OoTEngineHostActor replacementActor = OOT_ENGINE_INVALID_HOST_ACTOR;
    OoTEngineHostActor secondHostActor = OOT_ENGINE_INVALID_HOST_ACTOR;
    OoTDynamicCollision dynamicCollision = OOT_DYNAMIC_COLLISION_INVALID;
    OoTDynamicCollision replacementDynamicCollision =
        OOT_DYNAMIC_COLLISION_INVALID;
    OoTDynamicCollision secondDynamicCollision = OOT_DYNAMIC_COLLISION_INVALID;
    struct OoTDynamicCollisionTransform dynamicTransform =
        OOT_DYNAMIC_COLLISION_TRANSFORM_INIT;
    struct OoTDynamicCollisionState dynamicState;
    struct OoTHostActorState hostState;
    struct OoTHostActorState hostSnapshot;
    OoTEngineActorContact actorContact;
    int sawBombContact = 0;
    struct OoTSurface *hostileWorld = NULL;
    const OoTEngineFrame *frame = NULL;
    const OoTEngineFrame *secondFrame = NULL;
    uint64_t secondTick = 0u;
    uint32_t steps = 0u;
    uint32_t textureCount = 0u;
    uint32_t firstTextureRevision = 0u;
    uint32_t audioFrames = 0u;
    int16_t audioPcm[64u * 2u];
    struct OoTAudioState audioState;
    OoTEngineTexture texture;
    const OoTEngineSceneGeometry *sceneGeometry = NULL;
    struct OoTSceneRuntime runtime;
    struct OoTSceneLoadOptions sceneOptions;
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
    config.linkTriangleCapacity = 4096u;
    config.sceneTriangleCapacity = 32768u;

    config.romSize = (size_t)OOT_ENGINE_MAX_ROM_SIZE + 1u;
    ok &= expect_result("oversized ROM", oot_engine_create(&config, &engine),
                        OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= engine == NULL;
    config.romSize = romSize;

    config.linkTriangleCapacity = OOT_ENGINE_MAX_GEOMETRY_TRIANGLE_CAPACITY + 1u;
    ok &= expect_result("oversized Link geometry capacity",
                        oot_engine_create(&config, &engine),
                        OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    ok &= engine == NULL;
    config.linkTriangleCapacity = 4096u;

#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    sCheckLimitsContention = 1;
#endif
    ok &= expect_result("create", oot_engine_create(&config, &engine),
                        OOT_ENGINE_RESULT_OK);
    if (!ok) {
        free(rom);
        return 1;
    }
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    ok &= expect_result("limits query under engine lock",
                        sContendedLimitsResult, OOT_ENGINE_RESULT_BUSY);
    ok &= sContendedLimitsWriteFree != 0;
#endif
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    config.debugUserData = &sSecondDebugMessages;
    ok &= expect_result("second engine", oot_engine_create(&config, &second),
                        OOT_ENGINE_RESULT_OK);
    ok &= second != NULL && second != engine;
    config.debugUserData = &sDebugMessages;
    {
        uint8_t invalidRom[OOT_ENGINE_MIN_ROM_SIZE] = { 0 };
        const uint8_t *savedRom = config.romData;
        size_t savedRomSize = config.romSize;
        config.romData = invalidRom;
        config.romSize = sizeof(invalidRom);
        ok &= expect_result("failed third engine leaves contexts intact",
                            oot_engine_create(&config, &failedEngine),
                            OOT_ENGINE_RESULT_ROM_UNSUPPORTED);
        ok &= failedEngine == NULL;
        config.romData = savedRom;
        config.romSize = savedRomSize;
    }
#else
    ok &= expect_result("singleton", oot_engine_create(&config, &second),
                        OOT_ENGINE_RESULT_SINGLETON_IN_USE);
    ok &= second == NULL;
#endif
    ok &= expect_result("foreign engine handle",
                        oot_engine_get_frame((OoTEngine *)(uintptr_t)1u, &frame),
                        OOT_ENGINE_RESULT_NOT_INITIALIZED);
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    ok &= expect_result("first engine audio play",
                        oot_engine_audio_sequence_play(
                            engine, OOT_AUDIO_PLAYER_MAIN, 0u, 0u),
                        OOT_ENGINE_RESULT_OK);
    memset(&audioState, 0, sizeof(audioState));
    audioState.structSize = sizeof(audioState);
    audioState.version = OOT_AUDIO_STATE_VERSION;
    ok &= expect_result("first engine audio state",
                        oot_engine_audio_sequence_get_state(
                            engine, OOT_AUDIO_PLAYER_MAIN, &audioState),
                        OOT_ENGINE_RESULT_OK);
    ok &= audioState.playing != 0u && audioState.sequenceId == 0u;
    memset(&audioState, 0, sizeof(audioState));
    audioState.structSize = sizeof(audioState);
    audioState.version = OOT_AUDIO_STATE_VERSION;
    ok &= expect_result("second engine audio state isolated",
                        oot_engine_audio_sequence_get_state(
                            second, OOT_AUDIO_PLAYER_MAIN, &audioState),
                        OOT_ENGINE_RESULT_OK);
    ok &= audioState.playing == 0u;
    ok &= expect_result("first engine fixed audio render",
                        oot_engine_audio_render_s16(
                            engine, audioPcm, 64u, 32000u, &audioFrames),
                        OOT_ENGINE_RESULT_OK);
    ok &= audioFrames == 64u;
    memset(&audioState, 0, sizeof(audioState));
    audioState.structSize = sizeof(audioState);
    audioState.version = OOT_AUDIO_STATE_VERSION;
    ok &= expect_result("first engine audio advanced",
                        oot_engine_audio_sequence_get_state(
                            engine, OOT_AUDIO_PLAYER_MAIN, &audioState),
                        OOT_ENGINE_RESULT_OK);
    ok &= audioState.framesRendered == 64u;
    memset(&audioState, 0, sizeof(audioState));
    audioState.structSize = sizeof(audioState);
    audioState.version = OOT_AUDIO_STATE_VERSION;
    ok &= expect_result("second engine audio still isolated",
                        oot_engine_audio_sequence_get_state(
                            second, OOT_AUDIO_PLAYER_MAIN, &audioState),
                        OOT_ENGINE_RESULT_OK);
    ok &= audioState.framesRendered == 0u && audioState.playing == 0u;
#endif
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
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    ok &= expect_result("second world", oot_engine_static_world_load(
                            second, secondFloor, 2u, NULL, 0u),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("second Link", oot_engine_link_create(
                            second, 600.0f, 250.0f, -500.0f),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("second Link pose", oot_engine_link_set_pose(
                            second, 700.0f, 250.0f, -450.0f, 0x2000),
                        OOT_ENGINE_RESULT_OK);
#endif

#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    dynamicTransform.position[0] = 4000.0f;
    ok &= expect_result("first dynamic collision",
                        oot_engine_dynamic_collision_create(
                            engine, dynamicPlatform, 2u, &dynamicTransform, 0u,
                            &dynamicCollision), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("second dynamic collision",
                        oot_engine_dynamic_collision_create(
                            second, dynamicPlatform, 2u, &dynamicTransform, 0u,
                            &secondDynamicCollision), OOT_ENGINE_RESULT_OK);
    ok &= dynamicCollision != OOT_DYNAMIC_COLLISION_INVALID &&
          secondDynamicCollision != OOT_DYNAMIC_COLLISION_INVALID &&
          dynamicCollision != secondDynamicCollision;
    memset(&dynamicState, 0, sizeof(dynamicState));
    dynamicState.structSize = sizeof(dynamicState);
    ok &= expect_result("cross-engine dynamic collision handle",
                        oot_engine_dynamic_collision_get_state(
                            second, dynamicCollision, &dynamicState),
                        OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND);
    ok &= expect_result("delete first dynamic collision",
                        oot_engine_dynamic_collision_delete(
                            engine, dynamicCollision), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("replace first dynamic collision",
                        oot_engine_dynamic_collision_create(
                            engine, dynamicPlatform, 2u, &dynamicTransform, 0u,
                            &replacementDynamicCollision), OOT_ENGINE_RESULT_OK);
    ok &= replacementDynamicCollision != dynamicCollision;
    memset(&dynamicState, 0, sizeof(dynamicState));
    dynamicState.structSize = sizeof(dynamicState);
    ok &= expect_result("stale dynamic collision after replacement",
                        oot_engine_dynamic_collision_get_state(
                            engine, dynamicCollision, &dynamicState),
                        OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND);
    ok &= expect_result("delete replacement dynamic collision",
                        oot_engine_dynamic_collision_delete(
                            engine, replacementDynamicCollision),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("delete second dynamic collision",
                        oot_engine_dynamic_collision_delete(
                            second, secondDynamicCollision),
                        OOT_ENGINE_RESULT_OK);
#endif
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
    staleTarget = target;
    ok &= expect_result("target remove before replacement",
                        oot_engine_target_remove(engine, staleTarget),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("replacement target", oot_engine_target_create(
                            engine, 10.0f, 0.0f, 190.0f, 30.0f, &target),
                        OOT_ENGINE_RESULT_OK);
    ok &= target != staleTarget;
    ok &= expect_result("stale target after replacement",
                        oot_engine_target_move(
                            engine, staleTarget, 0.0f, 0.0f, 200.0f),
                        OOT_ENGINE_RESULT_TARGET_NOT_FOUND);
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    ok &= expect_result("second target", oot_engine_target_create(
                            second, 700.0f, 250.0f, -250.0f, 30.0f,
                            &secondTarget), OOT_ENGINE_RESULT_OK);
    ok &= secondTarget != target;
    ok &= expect_result("cross-engine target handle",
                        oot_engine_target_move(
                            second, target, 700.0f, 250.0f, -240.0f),
                        OOT_ENGINE_RESULT_TARGET_NOT_FOUND);
    ok &= expect_result("remove second target",
                        oot_engine_target_remove(second, secondTarget),
                        OOT_ENGINE_RESULT_OK);
#endif

    ok &= expect_result("input init", oot_engine_input_init(&input),
                        OOT_ENGINE_RESULT_OK);
    input.stickY = 1.0f;
    input.buttons = OOT_ENGINE_BUTTON_Z;
    ok &= expect_result("partial advance", oot_engine_advance(
                            engine, 0.01f, &input, &steps, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= steps == 0u && frame == NULL;
    ok &= expect_result("complete advance", oot_engine_advance(
                            engine, OOT_ENGINE_DEFAULT_FIXED_STEP - 0.01f,
                            &input, &steps, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= steps == 1u && frame_is_sane(frame);
    ok &= frame != NULL && frame->geometry.triangleCapacity == 4096u &&
          frame->geometryBatchCapacity == 4096u;

    input.stickY = 0.0f;
    input.buttons = 0u;
    ok &= expect_result("exact step", oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame) && frame->simulationTick == 2u;

#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    /* Switching handles must restore the whole native image while keeping the
       wrapper-owned frame buffers distinct. Deliberately interleave calls and
       verify neither world, pose nor simulation clock bleeds across. */
    ok &= expect_result("second engine first step",
                        oot_engine_step(second, &input, &secondFrame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(secondFrame) && secondFrame->simulationTick == 1u &&
          fabsf(secondFrame->link.position[0] - 700.0f) < 0.01f &&
          fabsf(secondFrame->link.position[1] - 250.0f) < 0.01f &&
          fabsf(secondFrame->link.position[2] + 450.0f) < 0.01f;
    {
        uint32_t firstCount = 0u;
        uint32_t secondCount = 0u;
        OoTEngineTexture firstTexture;
        OoTEngineTexture secondTexture;
        ok &= expect_result("first interleaved texture count",
                            oot_engine_texture_count(engine, &firstCount),
                            OOT_ENGINE_RESULT_OK);
        ok &= expect_result("second interleaved texture count",
                            oot_engine_texture_count(second, &secondCount),
                            OOT_ENGINE_RESULT_OK);
        ok &= firstCount > 0u && secondCount > 0u;
        if (firstCount > 0u && secondCount > 0u) {
            memset(&firstTexture, 0, sizeof(firstTexture));
            memset(&secondTexture, 0, sizeof(secondTexture));
            ok &= expect_result("first interleaved texture",
                                oot_engine_texture_get(
                                    engine, 0u, &firstTexture),
                                OOT_ENGINE_RESULT_OK);
            ok &= expect_result("second interleaved texture",
                                oot_engine_texture_get(
                                    second, 0u, &secondTexture),
                                OOT_ENGINE_RESULT_OK);
            ok &= firstTexture.revision != 0u &&
                  secondTexture.revision != 0u &&
                  firstTexture.revision != secondTexture.revision;
        }
    }
    secondTick = secondFrame != NULL ? secondFrame->simulationTick : 0u;
    ok &= expect_result("first engine interleaved step",
                        oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(frame) && frame->simulationTick == 3u &&
          fabsf(frame->link.position[0] - 700.0f) > 100.0f;
    ok &= expect_result("second engine resumed step",
                        oot_engine_step(second, &input, &secondFrame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(secondFrame) &&
          secondFrame->simulationTick == secondTick + 1u &&
          fabsf(secondFrame->link.position[0] - 700.0f) < 0.01f &&
          fabsf(secondFrame->link.position[1] - 250.0f) < 0.01f;
    secondTick = secondFrame != NULL ? secondFrame->simulationTick : secondTick;
#endif

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

    if (frame_is_sane(frame)) {
        float poseX = frame->link.position[0];
        float poseY = frame->link.position[1];
        float poseZ = frame->link.position[2];

        ok &= expect_result("freeze before clean pose",
                            oot_engine_link_freeze(engine, 1u),
                            OOT_ENGINE_RESULT_OK);
        ok &= expect_result("clean Link pose", oot_engine_link_set_pose(
                                engine, poseX, poseY, poseZ, 0x1234),
                            OOT_ENGINE_RESULT_OK);
        ok &= expect_result("clean pose frame",
                            oot_engine_step(engine, &input, &frame),
                            OOT_ENGINE_RESULT_OK);
        ok &= frame_is_sane(frame) &&
              fabsf(frame->link.velocity[0]) < 0.001f &&
              fabsf(frame->link.velocity[1]) < 0.001f &&
              fabsf(frame->link.velocity[2]) < 0.001f &&
              fabsf(frame->link.linearVelocity) < 0.001f &&
              frame->link.faceAngle == 0x1234;
        ok &= expect_result("unfreeze after clean pose",
                            oot_engine_link_freeze(engine, 0u),
                            OOT_ENGINE_RESULT_OK);
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


    memset(&hostState, 0, sizeof(hostState));
    hostState.structSize = sizeof(hostState);
    hostState.version = OOT_HOST_ACTOR_STATE_VERSION;
    hostState.userTag = UINT64_C(0x123456789ABCDEF0);
    hostState.typeId = 77u;
    hostState.flags = OOT_HOST_ACTOR_ENABLED | OOT_HOST_ACTOR_HURT;
    hostState.position[0] = frame->link.position[0];
    hostState.position[1] = frame->link.position[1] - 500.0f;
    hostState.position[2] = frame->link.position[2];
    hostState.focusOffset[1] = 30.0f;
    hostState.hurtRadius = 1000.0f;
    hostState.hurtHeight = 1000.0f;
    hostState.room = -1;
    hostState.attentionRange = 3u;
    hostState.flags |= UINT32_C(0x80000000);
    ok &= expect_result("host actor rejects unknown flags",
                        oot_engine_host_actor_create(engine, &hostState, &hostActor),
                        OOT_ENGINE_RESULT_INVALID_ARGUMENT);
    hostState.flags &= ~UINT32_C(0x80000000);
    ok &= expect_result("host actor equip sword", oot_engine_link_set_equipment(
                            engine, OOT_SWORD_MASTER, OOT_SHIELD_HYLIAN,
                            OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("host actor create", oot_engine_host_actor_create(
                            engine, &hostState, &hostActor), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("host actor nonzero handle",
                        hostActor != OOT_ENGINE_INVALID_HOST_ACTOR
                            ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE,
                        OOT_ENGINE_RESULT_OK);
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    ok &= expect_result("second host actor", oot_engine_host_actor_create(
                            second, &hostState, &secondHostActor),
                        OOT_ENGINE_RESULT_OK);
    ok &= secondHostActor != hostActor;
    memset(&hostSnapshot, 0, sizeof(hostSnapshot));
    hostSnapshot.structSize = sizeof(hostSnapshot);
    hostSnapshot.version = OOT_HOST_ACTOR_STATE_VERSION;
    ok &= expect_result("cross-engine host actor handle",
                        oot_engine_host_actor_get(
                            second, hostActor, &hostSnapshot),
                        OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND);
    ok &= expect_result("remove second host actor",
                        oot_engine_host_actor_remove(second, secondHostActor),
                        OOT_ENGINE_RESULT_OK);
#endif
    memset(&hostSnapshot, 0, sizeof(hostSnapshot));
    hostSnapshot.structSize = sizeof(hostSnapshot);
    hostSnapshot.version = OOT_HOST_ACTOR_STATE_VERSION;
    ok &= expect_result("host actor get", oot_engine_host_actor_get(
                            engine, hostActor, &hostSnapshot), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("host actor state round trip",
                        hostSnapshot.userTag == hostState.userTag &&
                                hostSnapshot.typeId == hostState.typeId &&
                                hostSnapshot.position[2] == hostState.position[2]
                            ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE,
                        OOT_ENGINE_RESULT_OK);
    hostState.position[0] += 12.0f;
    ok &= expect_result("host actor update", oot_engine_host_actor_update(
                            engine, hostActor, &hostState), OOT_ENGINE_RESULT_OK);

    ok &= expect_result("host actor lifecycle step",
                        oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    /* The bomb equipped above has a native 70-tick fuse. The large host hurt
       cylinder encloses Link and deterministically receives its explosion. */
    for (uint32_t i = 0u; i < 90u && !sawBombContact; ++i) {
        OoTResult contactResult;
        ok &= expect_result("host actor contact step",
                            oot_engine_step(engine, &input, &frame),
                            OOT_ENGINE_RESULT_OK);
        do {
            memset(&actorContact, 0, sizeof(actorContact));
            actorContact.structSize = sizeof(actorContact);
            actorContact.version = OOT_HOST_ACTOR_CONTACT_VERSION;
            contactResult = oot_engine_host_actor_poll_contact(engine, &actorContact);
            if (contactResult == OOT_ENGINE_RESULT_OK) {
                if (actorContact.actor == hostActor &&
                    actorContact.source == OOT_HOST_CONTACT_BOMB &&
                    actorContact.userTag == hostState.userTag) {
                    sawBombContact = 1;
                }
            } else if (contactResult != OOT_ENGINE_RESULT_NOT_AVAILABLE) {
                ok &= expect_result("host actor contact poll", contactResult,
                                    OOT_ENGINE_RESULT_OK);
            }
        } while (contactResult == OOT_ENGINE_RESULT_OK && !sawBombContact);
    }
    ok &= expect_result("host actor bomb contact",
                        sawBombContact ? OOT_ENGINE_RESULT_OK
                                       : OOT_ENGINE_RESULT_NOT_AVAILABLE,
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("host actor remove", oot_engine_host_actor_remove(
                            engine, hostActor), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("stale host actor", oot_engine_host_actor_update(
                            engine, hostActor, &hostState),
                        OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND);
    ok &= expect_result("replacement host actor", oot_engine_host_actor_create(
                            engine, &hostState, &replacementActor),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("host actor generation changes",
                        replacementActor != hostActor
                            ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE,
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("clear host actors", oot_engine_host_actors_clear(engine),
                        OOT_ENGINE_RESULT_OK);


    /* PAL Rev 1 room commands must reach the live PlayState. These values are
       taken directly from cmd 0x08/0x16 in the supplied retail ROM. */
    sceneOptions = (struct OoTSceneLoadOptions)OOT_SCENE_LOAD_OPTIONS_INIT(
        OOT_SCENE_BAZAAR, 0, OOT_SCENE_LAYER_ADULT_NIGHT);
    ok &= expect_result("Bazaar extended scene load",
                        oot_engine_scene_load_ex(engine, &sceneOptions,
                                                 &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= nativeSceneResult == 0;
    {
        uint8_t layer = 0u, usedFallback = 0u;
        uint32_t exitCount = 0u, backgroundCount = 0u;
        struct OoTSceneBackground background = {
            .structSize = sizeof(background),
            .version = OOT_SCENE_BACKGROUND_VERSION,
        };
        struct OoTSceneMaterialState material = {
            .structSize = sizeof(material),
            .version = OOT_SCENE_MATERIAL_STATE_VERSION,
        };
        struct OoTWorldEvent event = {
            .structSize = sizeof(event),
            .version = OOT_WORLD_EVENT_VERSION,
        };
        const uint8_t *borrowedImage = NULL;
        size_t borrowedSize = 0u;
        uint8_t borrowedFirst = 0u, borrowedLast = 0u;
        ok &= expect_result("Bazaar active layer",
                            oot_engine_scene_get_active_layer(
                                engine, &layer, &usedFallback),
                            OOT_ENGINE_RESULT_OK);
        ok &= layer == OOT_SCENE_LAYER_ADULT_NIGHT && usedFallback <= 1u;
        ok &= expect_result("Bazaar exits",
                            oot_engine_scene_get_exit_count(engine, &exitCount),
                            OOT_ENGINE_RESULT_OK);
        ok &= expect_result("Bazaar backgrounds",
                            oot_engine_scene_get_background_count(
                                engine, &backgroundCount),
                            OOT_ENGINE_RESULT_OK);
        ok &= backgroundCount > 0u;
        ok &= expect_result("Bazaar background payload",
                            oot_engine_scene_get_background(engine, 0u,
                                                            &background),
                            OOT_ENGINE_RESULT_OK);
        ok &= background.encoding == OOT_SCENE_BACKGROUND_JPEG &&
              background.imageBytes != NULL && background.imageByteCount >= 4u &&
              background.imageBytes[0] == 0xFFu &&
              background.imageBytes[1] == 0xD8u &&
              background.imageBytes[background.imageByteCount - 2u] == 0xFFu &&
              background.imageBytes[background.imageByteCount - 1u] == 0xD9u;
        borrowedImage = background.imageBytes;
        borrowedSize = background.imageByteCount;
        if (borrowedImage != NULL && borrowedSize != 0u) {
            borrowedFirst = borrowedImage[0];
            borrowedLast = borrowedImage[borrowedSize - 1u];
        }
        ok &= expect_result("Bazaar material state",
                            oot_engine_scene_get_material_state(engine, &material),
                            OOT_ENGINE_RESULT_OK);
        ok &= expect_result("Bazaar scene geometry",
                            oot_engine_scene_get_geometry(engine, &sceneGeometry),
                            OOT_ENGINE_RESULT_OK);
        for (uint32_t i = 0u; i < material.referenceCount; ++i) {
            struct OoTSceneMaterialReference reference = {
                .structSize = sizeof(reference),
                .version = OOT_SCENE_MATERIAL_REFERENCE_VERSION,
            };
            ok &= expect_result("Bazaar material reference",
                                oot_engine_scene_get_material_reference(
                                    engine, i, &reference),
                                OOT_ENGINE_RESULT_OK);
            ok &= sceneGeometry != NULL &&
                  reference.firstTriangle <= sceneGeometry->numTriangles &&
                  reference.numTriangles <=
                      sceneGeometry->numTriangles - reference.firstTriangle &&
                  (reference.kind == OOT_SCENE_MATERIAL_TEXTURE_IMAGE ||
                   reference.numTriangles == 0u);
        }
        ok &= expect_result("empty world event queue",
                            oot_engine_world_event_poll(engine, &event),
                            OOT_ENGINE_RESULT_NOT_AVAILABLE);
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
        ok &= expect_result("second engine has no active ROM scene",
                            oot_engine_scene_get_active_layer(
                                second, &layer, &usedFallback),
                            OOT_ENGINE_RESULT_NOT_AVAILABLE);
        ok &= borrowedImage != NULL && borrowedSize != 0u &&
              borrowedImage[0] == borrowedFirst &&
              borrowedImage[borrowedSize - 1u] == borrowedLast;
#endif
    }
    ok &= expect_result("Fire Temple room 0 load",
                        oot_engine_scene_load(engine, OOT_SCENE_FIRE_TEMPLE, 0,
                                              &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= nativeSceneResult == 0;
    {
        uint32_t actorCount = 0u;
        struct OoTSceneActorEntry actorEntry;
        ok &= expect_result("Fire Temple actor catalog count",
                            oot_engine_scene_get_actor_count(engine, &actorCount),
                            OOT_ENGINE_RESULT_OK);
        ok &= expect_result("Fire Temple actor catalog nonempty",
                            actorCount > 0u ? OOT_ENGINE_RESULT_OK
                                            : OOT_ENGINE_RESULT_NOT_AVAILABLE,
                            OOT_ENGINE_RESULT_OK);
        memset(&actorEntry, 0, sizeof(actorEntry));
        actorEntry.structSize = sizeof(actorEntry);
        actorEntry.version = OOT_SCENE_ACTOR_ENTRY_VERSION;
        ok &= expect_result("Fire Temple actor catalog entry",
                            oot_engine_scene_get_actor(engine, 0u, &actorEntry),
                            OOT_ENGINE_RESULT_OK);
        ok &= expect_result("Fire Temple actor catalog metadata",
                            actorEntry.structSize == sizeof(actorEntry) &&
                                    actorEntry.version == OOT_SCENE_ACTOR_ENTRY_VERSION &&
                                    actorEntry.layer == OOT_SCENE_LAYER_CHILD_DAY &&
                                    (actorEntry.source == OOT_SCENE_ACTOR_SOURCE_SCENE ||
                                     (actorEntry.source == OOT_SCENE_ACTOR_SOURCE_ROOM &&
                                      actorEntry.room == 0))
                                ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE,
                            OOT_ENGINE_RESULT_OK);
    }
    ok &= expect_result("Fire Temple scene geometry",
                        oot_engine_scene_get_geometry(engine, &sceneGeometry),
                        OOT_ENGINE_RESULT_OK);
    ok &= sceneGeometry != NULL && sceneGeometry->triangleCapacity == 32768u &&
          sceneGeometry->batchCapacity == 32768u &&
          sceneGeometry->numTriangles > 0u && sceneGeometry->batchCount > 0u &&
          sceneGeometry->batches != NULL && scene_geometry_is_sane(sceneGeometry);
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
    ok &= expect_result("scene reload after custom world",
                        oot_engine_scene_load(engine, OOT_SCENE_FIRE_TEMPLE, 0,
                                              &nativeSceneResult),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("geometry after scene reload",
                        oot_engine_scene_get_geometry(engine, &sceneGeometry),
                        OOT_ENGINE_RESULT_OK);
    ok &= sceneGeometry != NULL && sceneGeometry->triangleCapacity == 32768u &&
          scene_geometry_is_sane(sceneGeometry);

    ok &= expect_result("texture count", oot_engine_texture_count(
                            engine, &textureCount), OOT_ENGINE_RESULT_OK);
    ok &= textureCount > 0u;
    if (textureCount > 0u) {
        memset(&texture, 0, sizeof(texture));
        ok &= expect_result("texture", oot_engine_texture_get(
                                engine, 0u, &texture), OOT_ENGINE_RESULT_OK);
        ok &= texture.rgbaPixels != NULL && texture.rgbaSize > 0u &&
              texture.width > 0u && texture.height > 0u;
        firstTextureRevision = texture.revision;
        ok &= firstTextureRevision != 0u;
    }
    ok &= sDebugMessages > 0u;
#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    ok &= sSecondDebugMessages > 0u;
#endif

    ok &= expect_result("link delete", oot_engine_link_delete(engine),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("step without Link", oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_LINK_NOT_FOUND);
    ok &= expect_result("destroy", oot_engine_destroy(engine),
                        OOT_ENGINE_RESULT_OK);
    engine = NULL;

#if defined(LIBOOT_EXPECT_MULTI_INSTANCE)
    ok &= expect_result("second survives first destroy",
                        oot_engine_step(second, &input, &secondFrame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame_is_sane(secondFrame) &&
          secondFrame->simulationTick == secondTick + 1u &&
          fabsf(secondFrame->link.position[0] - 700.0f) < 0.01f &&
          fabsf(secondFrame->link.position[1] - 250.0f) < 0.01f;
    ok &= expect_result("destroy second engine", oot_engine_destroy(second),
                        OOT_ENGINE_RESULT_OK);
    second = NULL;
#endif

    config.linkTriangleCapacity = 1u;
    config.sceneTriangleCapacity = 1u;
    ok &= expect_result("create one-triangle engine",
                        oot_engine_create(&config, &engine),
                        OOT_ENGINE_RESULT_OK);
    ok &= expect_result("one-triangle world", oot_engine_static_world_load(
                            engine, floor, 2u, NULL, 0u), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("one-triangle Link", oot_engine_link_create(
                            engine, 0.0f, 0.0f, 0.0f), OOT_ENGINE_RESULT_OK);
    ok &= expect_result("one-triangle step", oot_engine_step(engine, &input, &frame),
                        OOT_ENGINE_RESULT_OK);
    ok &= frame != NULL && frame->geometry.triangleCapacity == 1u &&
          frame->geometry.numTriangles == 1u && frame->geometryBatchCount == 1u &&
          frame->geometryBatches[0].firstTriangle == 0u &&
          frame->geometryBatches[0].numTriangles == 1u &&
          frame->linkGeometryTruncated != 0u;
    if (firstTextureRevision != 0u) {
        uint32_t recreatedTextureCount = 0u;
        OoTEngineTexture recreatedTexture;
        ok &= expect_result("recreated texture count",
                            oot_engine_texture_count(
                                engine, &recreatedTextureCount),
                            OOT_ENGINE_RESULT_OK);
        ok &= recreatedTextureCount > 0u;
        if (recreatedTextureCount > 0u) {
            memset(&recreatedTexture, 0, sizeof(recreatedTexture));
            ok &= expect_result("recreated texture",
                                oot_engine_texture_get(
                                    engine, 0u, &recreatedTexture),
                                OOT_ENGINE_RESULT_OK);
            ok &= recreatedTexture.revision > firstTextureRevision;
        }
    }
    ok &= expect_result("destroy one-triangle engine", oot_engine_destroy(engine),
                        OOT_ENGINE_RESULT_OK);
    engine = NULL;
    free(rom);
    rom = NULL;

    printf("engine API: %s (%u debug messages, %u textures)\n",
           ok ? "PASS" : "FAIL", sDebugMessages, textureCount);
    return ok ? 0 : 1;
}
