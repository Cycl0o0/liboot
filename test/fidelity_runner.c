/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * Deterministic, ROM-backed fidelity trace recorder/comparator.  Trace files
 * contain numeric state and fixed-endian hashes only: never ROM bytes, decoded
 * assets, framebuffer captures, or PCM samples.
 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "liboot_engine.h"

#define TRACE_VERSION 3u
#define TRACE_LINE_MAX 4096u
#define TRACE_MAX_FIELDS 64u
#define DEFAULT_TICKS 300u
#define DEFAULT_TOLERANCE 0.0001f
#define DEFAULT_SCENE OOT_SCENE_HYRULE_FIELD
#define DEFAULT_ROOM (-1)
#define DEFAULT_SPAWN 0
#define FNV64_OFFSET UINT64_C(14695981039346656037)
#define FNV64_PRIME UINT64_C(1099511628211)

typedef struct TraceConfig {
    char scenario[32];
    uint32_t ticks;
    int32_t scene;
    int32_t room;
    int32_t spawn;
    uint8_t age;
    float tolerance;
} TraceConfig;

typedef struct FrameRecord {
    uint64_t tick;
    float position[3];
    float velocity[3];
    int32_t faceAngle;
    float linearVelocity;
    float animFrame;
    int32_t animId;
    uint32_t action;
    uint32_t stateFlags1;
    uint32_t stateFlags2;
    uint32_t stateFlags3;
    int32_t health;
    int32_t healthCapacity;
    int32_t magic;
    uint32_t magicLevel;
    uint32_t age;
    uint32_t isDead;
    int32_t heldItemAction;
    uint32_t meleeWeaponState;
    uint32_t lockOnActive;
    uint32_t inWater;
    float lockOnPosition[3];
    float waterSurfaceY;
    int32_t lookPitch;
    int32_t lookYaw;
    uint32_t floorSfxOffset;
    uint32_t attackAnim;
    uint32_t underwaterTimer;
    int32_t sceneIndex;
    int32_t activeRoomIndex;
    int32_t geometryRoomIndex;
    int32_t roomCount;
    int32_t worldMapArea;
    uint32_t roomType;
    uint32_t environmentType;
    int32_t echo;
    uint32_t lensMode;
    uint32_t warpSongsDisabled;
    uint32_t sceneCamType;
    uint32_t allRoomsLoaded;
    uint32_t roomMetadataValid;
    uint32_t triangleCount;
    uint64_t geometryHash;
    uint32_t skeletonAvailable;
    uint64_t skeletonHash;
    uint32_t actorCount;
    uint32_t actorListTruncated;
    uint64_t actorHash;
    uint32_t naviAvailable;
    uint64_t naviHash;
    uint32_t sfxCount;
    uint64_t sfxHash;
} FrameRecord;

typedef struct SfxDigest {
    uint32_t count;
    uint64_t hash;
} SfxDigest;

typedef struct DiffState {
    uint64_t fieldsCompared;
    uint64_t mismatches;
    uint32_t diagnostics;
} DiffState;

static void trace_config_defaults(TraceConfig *config)
{
    memset(config, 0, sizeof(*config));
    memcpy(config->scenario, "core", 5u);
    config->ticks = DEFAULT_TICKS;
    config->scene = DEFAULT_SCENE;
    config->room = DEFAULT_ROOM;
    config->spawn = DEFAULT_SPAWN;
    config->age = OOT_AGE_ADULT;
    config->tolerance = DEFAULT_TOLERANCE;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;

    for (i = 0u; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV64_PRIME;
    }
    return hash;
}

static uint64_t hash_u8(uint64_t hash, uint8_t value)
{
    return hash_bytes(hash, &value, sizeof(value));
}

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
    uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
    };
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_float(uint64_t hash, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return hash_u32(hash, bits);
}

static uint64_t hash_rom(const uint8_t *rom, size_t size)
{
    return hash_bytes(FNV64_OFFSET, rom, size);
}

static uint64_t hash_geometry(const OoTEngineGeometry *geometry)
{
    uint64_t hash = FNV64_OFFSET;
    uint32_t triangle;
    uint32_t value;

    hash = hash_u32(hash, geometry->numTriangles);
    for (value = 0u; value < geometry->numTriangles * 9u; ++value) {
        hash = hash_float(hash, geometry->position[value]);
        hash = hash_float(hash, geometry->normal[value]);
        hash = hash_float(hash, geometry->color[value]);
    }
    for (value = 0u; value < geometry->numTriangles * 6u; ++value) {
        hash = hash_float(hash, geometry->uv[value]);
    }
    for (triangle = 0u; triangle < geometry->numTriangles; ++triangle) {
        hash = hash_u16(hash, geometry->triTexture[triangle]);
    }
    hash = hash_u8(hash, geometry->alpha != NULL ? 1u : 0u);
    if (geometry->alpha != NULL) {
        for (value = 0u; value < geometry->numTriangles * 3u; ++value) {
            hash = hash_float(hash, geometry->alpha[value]);
        }
    }
    hash = hash_u8(hash, geometry->triFlags != NULL ? 1u : 0u);
    if (geometry->triFlags != NULL) {
        for (triangle = 0u; triangle < geometry->numTriangles; ++triangle) {
            hash = hash_u8(hash, geometry->triFlags[triangle]);
        }
    }
    return hash;
}

static uint64_t hash_skeleton(const OoTEngineFrame *frame)
{
    uint64_t hash = FNV64_OFFSET;
    uint32_t joint;
    uint32_t axis;

    hash = hash_u8(hash, frame->skeletonAvailable);
    if (!frame->skeletonAvailable) {
        return hash;
    }
    hash = hash_u8(hash, frame->skeleton.numJoints);
    for (joint = 0u; joint < frame->skeleton.numJoints; ++joint) {
        hash = hash_u8(hash, frame->skeleton.parent[joint]);
        for (axis = 0u; axis < 3u; ++axis) {
            hash = hash_float(hash, frame->skeleton.jointPos[joint][axis]);
        }
    }
    return hash;
}

static uint64_t hash_actors(const OoTEngineFrame *frame)
{
    uint64_t hash = FNV64_OFFSET;
    uint32_t actor;
    uint32_t axis;

    hash = hash_u32(hash, frame->actorCount);
    hash = hash_u8(hash, frame->actorListTruncated);
    for (actor = 0u; actor < frame->actorCount; ++actor) {
        const struct OoTActorInfo *info = &frame->actors[actor];

        hash = hash_u16(hash, (uint16_t)info->id);
        hash = hash_u16(hash, (uint16_t)info->category);
        hash = hash_u16(hash, (uint16_t)info->params);
        hash = hash_u16(hash, (uint16_t)info->yaw);
        hash = hash_u8(hash, info->active);
        for (axis = 0u; axis < 3u; ++axis) {
            hash = hash_float(hash, info->pos[axis]);
        }
    }
    return hash;
}

static uint64_t hash_navi(const OoTEngineNaviState *navi)
{
    uint64_t hash = FNV64_OFFSET;
    uint32_t i;

    hash = hash_u8(hash, navi->available);
    if (!navi->available) {
        return hash;
    }
    for (i = 0u; i < 3u; ++i) {
        hash = hash_float(hash, navi->position[i]);
    }
    for (i = 0u; i < 4u; ++i) {
        hash = hash_float(hash, navi->innerColor[i]);
        hash = hash_float(hash, navi->outerColor[i]);
    }
    return hash_float(hash, navi->scale);
}

static void sfx_callback(void *userData, const struct OoTSfxEvent *event)
{
    SfxDigest *digest = (SfxDigest *)userData;
    uint32_t axis;

    if (digest == NULL || event == NULL) {
        return;
    }
    digest->count++;
    digest->hash = hash_u16(digest->hash, event->sfxId);
    digest->hash = hash_u8(digest->hash, event->token);
    digest->hash = hash_u8(digest->hash, (uint8_t)event->reverb);
    digest->hash = hash_u8(digest->hash, event->action);
    digest->hash = hash_u8(digest->hash, event->isRefresh);
    for (axis = 0u; axis < 3u; ++axis) {
        digest->hash = hash_float(digest->hash, event->position[axis]);
    }
    digest->hash = hash_float(digest->hash, event->freqScale);
    digest->hash = hash_float(digest->hash, event->volume);
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
        (size_t)length > OOT_ENGINE_MAX_ROM_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: invalid or oversized file\n", path);
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL || fread(data, 1u, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "%s: could not read file\n", path);
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

static void scenario_input(const char *scenario, uint32_t tick, OoTEngineInput *input)
{
    uint32_t phase;

    (void)oot_engine_input_init(input);
    if (strcmp(scenario, "idle") == 0) {
        return;
    }

    /* A stable mix of movement, roll, sword, targeting, shielding and C-up.
       Buttons are levels; single-tick pulses are followed by a released tick. */
    phase = tick % 300u;
    if (phase >= 20u && phase < 100u) {
        input->stickY = 1.0f;
    } else if (phase >= 100u && phase < 140u) {
        input->stickX = 0.65f;
        input->stickY = 0.35f;
    } else if (phase >= 180u && phase < 220u) {
        input->stickY = 0.6f;
        input->buttons |= OOT_ENGINE_BUTTON_Z;
    }
    if (phase == 80u) {
        input->buttons |= OOT_ENGINE_BUTTON_A;
    }
    if (phase == 145u || phase == 158u) {
        input->buttons |= OOT_ENGINE_BUTTON_B;
    }
    if (phase >= 220u && phase < 240u) {
        input->buttons |= OOT_ENGINE_BUTTON_R;
    }
    if (phase == 250u) {
        input->buttons |= OOT_ENGINE_BUTTON_CUP;
    }
}

static void frame_record_build(FrameRecord *record, const OoTEngineFrame *frame,
                               const SfxDigest *sfx,
                               const struct OoTSceneRuntime *scene)
{
    const OoTEngineLinkState *link = &frame->link;

    memset(record, 0, sizeof(*record));
    record->tick = frame->simulationTick;
    memcpy(record->position, link->position, sizeof(record->position));
    memcpy(record->velocity, link->velocity, sizeof(record->velocity));
    record->faceAngle = link->faceAngle;
    record->linearVelocity = link->linearVelocity;
    record->animFrame = link->animFrame;
    record->animId = link->animId;
    record->action = link->action;
    record->stateFlags1 = link->stateFlags1;
    record->stateFlags2 = link->stateFlags2;
    record->stateFlags3 = link->stateFlags3;
    record->health = link->health;
    record->healthCapacity = link->healthCapacity;
    record->magic = link->magic;
    record->magicLevel = link->magicLevel;
    record->age = link->age;
    record->isDead = link->isDead;
    record->heldItemAction = link->heldItemAction;
    record->meleeWeaponState = link->meleeWeaponState;
    record->lockOnActive = link->lockOnActive;
    record->inWater = link->inWater;
    memcpy(record->lockOnPosition, link->lockOnPos, sizeof(record->lockOnPosition));
    record->waterSurfaceY = link->waterSurfaceY;
    record->lookPitch = link->lookPitch;
    record->lookYaw = link->lookYaw;
    record->floorSfxOffset = link->floorSfxOffset;
    record->attackAnim = link->attackAnim;
    record->underwaterTimer = link->underwaterTimer;
    record->sceneIndex = scene->sceneIndex;
    record->activeRoomIndex = scene->activeRoomIndex;
    record->geometryRoomIndex = scene->geometryRoomIndex;
    record->roomCount = scene->roomCount;
    record->worldMapArea = scene->worldMapArea;
    record->roomType = scene->roomType;
    record->environmentType = scene->environmentType;
    record->echo = scene->echo;
    record->lensMode = scene->lensMode;
    record->warpSongsDisabled = scene->warpSongsDisabled;
    record->sceneCamType = scene->sceneCamType;
    record->allRoomsLoaded = scene->allRoomsLoaded;
    record->roomMetadataValid = scene->roomMetadataValid;
    record->triangleCount = frame->geometry.numTriangles;
    record->geometryHash = hash_geometry(&frame->geometry);
    record->skeletonAvailable = frame->skeletonAvailable;
    record->skeletonHash = hash_skeleton(frame);
    record->actorCount = frame->actorCount;
    record->actorListTruncated = frame->actorListTruncated;
    record->actorHash = hash_actors(frame);
    record->naviAvailable = frame->navi.available;
    record->naviHash = hash_navi(&frame->navi);
    record->sfxCount = sfx->count;
    record->sfxHash = sfx->hash;
}

static int write_header(FILE *file, const TraceConfig *config, uint64_t romHash)
{
    return fprintf(file,
                   "LIBOOT_FIDELITY_TRACE\t%u\n"
                   "library\t%s\n"
                   "engine_api\t%u\n"
                   "scenario\t%s\n"
                   "ticks\t%u\n"
                   "scene\t%d\n"
                   "room\t%d\n"
                   "spawn\t%d\n"
                   "age\t%u\n"
                   "rom_fnv64\t%016" PRIx64 "\n"
                   "float_tolerance\t%.9g\n"
                   "columns\ttick pos[3] vel[3] face speed anim animId action flags[3] "
                   "health capacity magic magicLevel age dead held melee lock water "
                   "lockPos[3] waterY lookPitch lookYaw floorSfx attack underwater "
                   "scene activeRoom geometryRoom roomCount worldMap roomType roomEnv "
                   "echo lens warpDisabled sceneCam allRooms roomMetadata "
                   "triangles geometryHash skeleton skeletonHash actors actorTruncated "
                   "actorHash navi naviHash sfxCount sfxHash\n",
                   TRACE_VERSION, LIBOOT_VERSION_STRING, OOT_ENGINE_API_VERSION,
                   config->scenario, config->ticks, config->scene, config->room,
                   config->spawn, config->age, romHash, config->tolerance) > 0;
}

static int write_frame(FILE *file, const FrameRecord *record)
{
    return fprintf(file,
                   "F\t%" PRIu64
                   "\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g"
                   "\t%d\t%.9g\t%.9g\t%d\t%u\t%08x\t%08x\t%02x"
                   "\t%d\t%d\t%d\t%u\t%u\t%u\t%d\t%u\t%u\t%u"
                   "\t%.9g\t%.9g\t%.9g\t%.9g\t%d\t%d\t%u\t%u\t%u"
                   "\t%d\t%d\t%d\t%d\t%d\t%u\t%u\t%d\t%u\t%u\t%u\t%u\t%u"
                   "\t%u\t%016" PRIx64 "\t%u\t%016" PRIx64
                   "\t%u\t%u\t%016" PRIx64 "\t%u\t%016" PRIx64
                   "\t%u\t%016" PRIx64 "\n",
                   record->tick,
                   record->position[0], record->position[1], record->position[2],
                   record->velocity[0], record->velocity[1], record->velocity[2],
                   record->faceAngle, record->linearVelocity, record->animFrame,
                   record->animId, record->action, record->stateFlags1, record->stateFlags2,
                   record->stateFlags3, record->health, record->healthCapacity,
                   record->magic, record->magicLevel, record->age, record->isDead,
                   record->heldItemAction, record->meleeWeaponState,
                   record->lockOnActive, record->inWater,
                   record->lockOnPosition[0], record->lockOnPosition[1],
                   record->lockOnPosition[2], record->waterSurfaceY,
                   record->lookPitch, record->lookYaw, record->floorSfxOffset,
                   record->attackAnim, record->underwaterTimer,
                   record->sceneIndex, record->activeRoomIndex,
                   record->geometryRoomIndex, record->roomCount,
                   record->worldMapArea, record->roomType,
                   record->environmentType, record->echo, record->lensMode,
                   record->warpSongsDisabled, record->sceneCamType,
                   record->allRoomsLoaded, record->roomMetadataValid,
                   record->triangleCount, record->geometryHash,
                   record->skeletonAvailable, record->skeletonHash,
                   record->actorCount, record->actorListTruncated, record->actorHash,
                   record->naviAvailable, record->naviHash,
                   record->sfxCount, record->sfxHash) > 0;
}

static int parse_u64(const char *text, int base, uint64_t *out)
{
    char *end;
    uintmax_t value;

    errno = 0;
    value = strtoumax(text, &end, base);
    while (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t') {
        ++end;
    }
    if (errno != 0 || end == text || *end != '\0' || value > UINT64_MAX) {
        return 0;
    }
    *out = (uint64_t)value;
    return 1;
}

static int parse_u32(const char *text, int base, uint32_t *out)
{
    uint64_t value;

    if (!parse_u64(text, base, &value) || value > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static int parse_i32(const char *text, int base, int32_t *out)
{
    char *end;
    intmax_t value;

    errno = 0;
    value = strtoimax(text, &end, base);
    while (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t') {
        ++end;
    }
    if (errno != 0 || end == text || *end != '\0' ||
        value < INT32_MIN || value > INT32_MAX) {
        return 0;
    }
    *out = (int32_t)value;
    return 1;
}

static int parse_float_value(const char *text, float *out)
{
    char *end;
    float value;

    errno = 0;
    value = strtof(text, &end);
    while (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t') {
        ++end;
    }
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value)) {
        return 0;
    }
    *out = value;
    return 1;
}

static size_t split_fields(char *line, char **fields, size_t capacity)
{
    size_t count = 0u;
    char *token = strtok(line, "\t");

    while (token != NULL && count < capacity) {
        fields[count++] = token;
        token = strtok(NULL, "\t");
    }
    return token == NULL ? count : capacity + 1u;
}

static int parse_frame(char *line, FrameRecord *record)
{
    char *field[TRACE_MAX_FIELDS];
    size_t count = split_fields(line, field, TRACE_MAX_FIELDS);
    size_t i = 1u;

#define U32(member, base) (parse_u32(field[i++], (base), &record->member))
#define I32(member, base) (parse_i32(field[i++], (base), &record->member))
#define U64(member, base) (parse_u64(field[i++], (base), &record->member))
#define FLT(member) (parse_float_value(field[i++], &record->member))

    if (count != 59u || strcmp(field[0], "F") != 0) {
        return 0;
    }
    memset(record, 0, sizeof(*record));
    if (!U64(tick, 10) ||
        !FLT(position[0]) || !FLT(position[1]) || !FLT(position[2]) ||
        !FLT(velocity[0]) || !FLT(velocity[1]) || !FLT(velocity[2]) ||
        !I32(faceAngle, 10) || !FLT(linearVelocity) || !FLT(animFrame) ||
        !I32(animId, 10) || !U32(action, 10) ||
        !U32(stateFlags1, 16) || !U32(stateFlags2, 16) ||
        !U32(stateFlags3, 16) || !I32(health, 10) ||
        !I32(healthCapacity, 10) || !I32(magic, 10) ||
        !U32(magicLevel, 10) || !U32(age, 10) || !U32(isDead, 10) ||
        !I32(heldItemAction, 10) || !U32(meleeWeaponState, 10) ||
        !U32(lockOnActive, 10) || !U32(inWater, 10) ||
        !FLT(lockOnPosition[0]) || !FLT(lockOnPosition[1]) ||
        !FLT(lockOnPosition[2]) || !FLT(waterSurfaceY) ||
        !I32(lookPitch, 10) || !I32(lookYaw, 10) ||
        !U32(floorSfxOffset, 10) || !U32(attackAnim, 10) ||
        !U32(underwaterTimer, 10) ||
        !I32(sceneIndex, 10) || !I32(activeRoomIndex, 10) ||
        !I32(geometryRoomIndex, 10) || !I32(roomCount, 10) ||
        !I32(worldMapArea, 10) || !U32(roomType, 10) ||
        !U32(environmentType, 10) || !I32(echo, 10) ||
        !U32(lensMode, 10) || !U32(warpSongsDisabled, 10) ||
        !U32(sceneCamType, 10) || !U32(allRoomsLoaded, 10) ||
        !U32(roomMetadataValid, 10) || !U32(triangleCount, 10) ||
        !U64(geometryHash, 16) || !U32(skeletonAvailable, 10) ||
        !U64(skeletonHash, 16) || !U32(actorCount, 10) ||
        !U32(actorListTruncated, 10) || !U64(actorHash, 16) ||
        !U32(naviAvailable, 10) || !U64(naviHash, 16) ||
        !U32(sfxCount, 10) || !U64(sfxHash, 16)) {
        return 0;
    }
    return i == 59u;

#undef U32
#undef I32
#undef U64
#undef FLT
}

static int read_header(FILE *file, TraceConfig *config, uint64_t *romHash,
                       char firstFrame[TRACE_LINE_MAX])
{
    char line[TRACE_LINE_MAX];
    uint32_t version = 0u;
    int haveMagic = 0;
    int haveEngineApi = 0;
    int haveTicks = 0;
    int haveScenario = 0;
    int haveRom = 0;

    trace_config_defaults(config);
    while (fgets(line, sizeof(line), file) != NULL) {
        char *field[3];
        size_t count;

        if (line[0] == 'F' && line[1] == '\t') {
            if (!haveMagic || !haveEngineApi || !haveTicks || !haveScenario || !haveRom) {
                return 0;
            }
            memcpy(firstFrame, line, strlen(line) + 1u);
            return 1;
        }
        count = split_fields(line, field, 3u);
        if (count < 2u) {
            continue;
        }
        if (strcmp(field[0], "LIBOOT_FIDELITY_TRACE") == 0) {
            if (!parse_u32(field[1], 10, &version) || version != TRACE_VERSION) {
                return 0;
            }
            haveMagic = 1;
        } else if (strcmp(field[0], "engine_api") == 0) {
            uint32_t apiVersion;
            if (!parse_u32(field[1], 10, &apiVersion) ||
                apiVersion != OOT_ENGINE_API_VERSION) {
                return 0;
            }
            haveEngineApi = 1;
        } else if (strcmp(field[0], "scenario") == 0) {
            field[1][strcspn(field[1], "\r\n")] = '\0';
            if (strlen(field[1]) >= sizeof(config->scenario)) {
                return 0;
            }
            strcpy(config->scenario, field[1]);
            haveScenario = 1;
        } else if (strcmp(field[0], "ticks") == 0) {
            if (!parse_u32(field[1], 10, &config->ticks) || config->ticks == 0u) {
                return 0;
            }
            haveTicks = 1;
        } else if (strcmp(field[0], "scene") == 0) {
            if (!parse_i32(field[1], 10, &config->scene)) return 0;
        } else if (strcmp(field[0], "room") == 0) {
            if (!parse_i32(field[1], 10, &config->room)) return 0;
        } else if (strcmp(field[0], "spawn") == 0) {
            if (!parse_i32(field[1], 10, &config->spawn)) return 0;
        } else if (strcmp(field[0], "age") == 0) {
            uint32_t age;
            if (!parse_u32(field[1], 10, &age) || age > OOT_AGE_CHILD) return 0;
            config->age = (uint8_t)age;
        } else if (strcmp(field[0], "rom_fnv64") == 0) {
            if (!parse_u64(field[1], 16, romHash)) return 0;
            haveRom = 1;
        } else if (strcmp(field[0], "float_tolerance") == 0) {
            if (!parse_float_value(field[1], &config->tolerance) ||
                config->tolerance < 0.0f) return 0;
        }
    }
    return 0;
}

static int float_matches(float expected, float actual, float tolerance)
{
    float scale = fmaxf(1.0f, fabsf(expected));
    return isfinite(expected) && isfinite(actual) &&
           fabsf(expected - actual) <= tolerance * scale;
}

static void diff_u64(DiffState *diff, uint64_t tick, const char *name,
                     uint64_t expected, uint64_t actual, int hash)
{
    diff->fieldsCompared++;
    if (expected == actual) {
        return;
    }
    diff->mismatches++;
    if (diff->diagnostics++ < 24u) {
        if (hash) {
            fprintf(stderr, "tick %" PRIu64 ": %s expected %016" PRIx64
                    ", got %016" PRIx64 "\n", tick, name, expected, actual);
        } else {
            fprintf(stderr, "tick %" PRIu64 ": %s expected %" PRIu64
                    ", got %" PRIu64 "\n", tick, name, expected, actual);
        }
    }
}

static void diff_i32(DiffState *diff, uint64_t tick, const char *name,
                     int32_t expected, int32_t actual)
{
    diff->fieldsCompared++;
    if (expected == actual) {
        return;
    }
    diff->mismatches++;
    if (diff->diagnostics++ < 24u) {
        fprintf(stderr, "tick %" PRIu64 ": %s expected %d, got %d\n",
                tick, name, expected, actual);
    }
}

static void diff_float(DiffState *diff, uint64_t tick, const char *name,
                       float expected, float actual, float tolerance)
{
    diff->fieldsCompared++;
    if (float_matches(expected, actual, tolerance)) {
        return;
    }
    diff->mismatches++;
    if (diff->diagnostics++ < 24u) {
        fprintf(stderr, "tick %" PRIu64 ": %s expected %.9g, got %.9g\n",
                tick, name, expected, actual);
    }
}

static void compare_frames(DiffState *diff, const FrameRecord *expected,
                           const FrameRecord *actual, float tolerance)
{
    uint32_t axis;
    static const char *positionNames[3] = { "position.x", "position.y", "position.z" };
    static const char *velocityNames[3] = { "velocity.x", "velocity.y", "velocity.z" };
    static const char *lockNames[3] = { "lock.x", "lock.y", "lock.z" };
    uint64_t tick = expected->tick;

#define DU(member) diff_u64(diff, tick, #member, expected->member, actual->member, 0)
#define DH(member) do { if (expected->member != 0u) \
    diff_u64(diff, tick, #member, expected->member, actual->member, 1); } while (0)
#define DI(member) diff_i32(diff, tick, #member, expected->member, actual->member)
#define DF(member) diff_float(diff, tick, #member, expected->member, actual->member, tolerance)

    DU(tick);
    for (axis = 0u; axis < 3u; ++axis) {
        diff_float(diff, tick, positionNames[axis], expected->position[axis],
                   actual->position[axis], tolerance);
        diff_float(diff, tick, velocityNames[axis], expected->velocity[axis],
                   actual->velocity[axis], tolerance);
        diff_float(diff, tick, lockNames[axis], expected->lockOnPosition[axis],
                   actual->lockOnPosition[axis], tolerance);
    }
    DI(faceAngle); DF(linearVelocity); DF(animFrame); DI(animId); DU(action);
    DU(stateFlags1); DU(stateFlags2); DU(stateFlags3);
    DI(health); DI(healthCapacity); DI(magic); DU(magicLevel); DU(age);
    DU(isDead); DI(heldItemAction); DU(meleeWeaponState); DU(lockOnActive);
    DU(inWater); DF(waterSurfaceY); DI(lookPitch); DI(lookYaw);
    DU(floorSfxOffset); DU(attackAnim); DU(underwaterTimer);
    DI(sceneIndex); DI(activeRoomIndex); DI(geometryRoomIndex); DI(roomCount);
    DI(worldMapArea); DU(roomType); DU(environmentType); DI(echo);
    DU(lensMode); DU(warpSongsDisabled); DU(sceneCamType); DU(allRoomsLoaded);
    DU(roomMetadataValid);
    DU(triangleCount); DH(geometryHash); DU(skeletonAvailable); DH(skeletonHash);
    DU(actorCount); DU(actorListTruncated); DH(actorHash);
    DU(naviAvailable); DH(naviHash); DU(sfxCount); DH(sfxHash);

#undef DU
#undef DH
#undef DI
#undef DF
}

static int parse_positive_u32(const char *text, uint32_t *out)
{
    return parse_u32(text, 0, out) && *out > 0u;
}

static int scenario_valid(const char *scenario)
{
    return strcmp(scenario, "core") == 0 || strcmp(scenario, "idle") == 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage:\n"
            "  %s --self-test\n"
            "  %s <rom> --record <trace> [--scenario core|idle] [--ticks N]\n"
            "       [--scene INDEX] [--room INDEX] [--spawn INDEX] [--age adult|child]\n"
            "  %s <rom> --compare <trace> [--tolerance FLOAT]\n",
            program, program, program);
}

static int self_test(void)
{
    TraceConfig written;
    TraceConfig parsed;
    FrameRecord expected;
    FrameRecord actual;
    DiffState diff = { 0u, 0u, 0u };
    FILE *file = tmpfile();
    char firstFrame[TRACE_LINE_MAX];
    uint64_t parsedRom = 0u;

    if (file == NULL) {
        fprintf(stderr, "fidelity self-test: tmpfile failed\n");
        return 0;
    }
    trace_config_defaults(&written);
    written.ticks = 1u;
    memset(&expected, 0, sizeof(expected));
    expected.tick = 1u;
    expected.position[0] = 1.25f;
    expected.velocity[2] = -0.0f;
    expected.faceAngle = -123;
    expected.animId = 321;
    expected.health = 0x30;
    expected.heldItemAction = -1;
    expected.triangleCount = 42u;
    expected.geometryHash = UINT64_C(0x0123456789ABCDEF);
    expected.skeletonHash = UINT64_C(0x1111222233334444);
    expected.actorHash = UINT64_C(0x5555666677778888);
    expected.naviHash = UINT64_C(0x9999AAAABBBBCCCC);
    expected.sfxHash = UINT64_C(0xDDDDEEEEFFFF0001);

    if (!write_header(file, &written, UINT64_C(0xAABBCCDDEEFF0011)) ||
        !write_frame(file, &expected) || fflush(file) != 0 || fseek(file, 0, SEEK_SET) != 0 ||
        !read_header(file, &parsed, &parsedRom, firstFrame) ||
        !parse_frame(firstFrame, &actual)) {
        fclose(file);
        fprintf(stderr, "fidelity self-test: trace round-trip failed\n");
        return 0;
    }
    fclose(file);
    compare_frames(&diff, &expected, &actual, written.tolerance);
    if (diff.mismatches != 0u || parsedRom != UINT64_C(0xAABBCCDDEEFF0011) ||
        parsed.ticks != 1u || strcmp(parsed.scenario, "core") != 0) {
        fprintf(stderr, "fidelity self-test: parsed values differ\n");
        return 0;
    }
    actual.animId++;
    memset(&diff, 0, sizeof(diff));
    diff.diagnostics = 24u; /* expected negative check: keep self-test output clean */
    compare_frames(&diff, &expected, &actual, written.tolerance);
    if (diff.mismatches != 1u) {
        fprintf(stderr, "fidelity self-test: mismatch detection failed\n");
        return 0;
    }
    puts("fidelity runner: PASS");
    return 1;
}

int main(int argc, char **argv)
{
    const char *romPath;
    const char *tracePath = NULL;
    int recordMode = 0;
    int compareMode = 0;
    int toleranceOverride = 0;
    float requestedTolerance = DEFAULT_TOLERANCE;
    TraceConfig config;
    uint8_t *rom = NULL;
    size_t romSize = 0u;
    uint64_t romHash;
    FILE *trace = NULL;
    char expectedLine[TRACE_LINE_MAX] = { 0 };
    uint64_t expectedRomHash = 0u;
    OoTEngineConfig engineConfig;
    OoTEngine *engine = NULL;
    OoTEngineInput input;
    const OoTEngineFrame *frame = NULL;
    struct OoTSceneRuntime sceneRuntime;
    SfxDigest sfx = { 0u, FNV64_OFFSET };
    DiffState diff = { 0u, 0u, 0u };
    int32_t nativeResult = 0;
    float spawnPosition[3];
    int16_t spawnYaw = 0;
    int32_t firstAnimId = 0;
    int sawAnimChange = 0;
    uint32_t tick;
    int exitCode = 1;
    int i;

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return self_test() ? 0 : 1;
    }
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    romPath = argv[1];
    trace_config_defaults(&config);
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--record") == 0 && i + 1 < argc && !compareMode) {
            recordMode = 1;
            tracePath = argv[++i];
        } else if (strcmp(argv[i], "--compare") == 0 && i + 1 < argc && !recordMode) {
            compareMode = 1;
            tracePath = argv[++i];
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc && recordMode) {
            const char *scenario = argv[++i];
            if (!scenario_valid(scenario) || strlen(scenario) >= sizeof(config.scenario)) {
                usage(argv[0]); return 2;
            }
            strcpy(config.scenario, scenario);
        } else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc && recordMode) {
            if (!parse_positive_u32(argv[++i], &config.ticks)) { usage(argv[0]); return 2; }
        } else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc && recordMode) {
            if (!parse_i32(argv[++i], 0, &config.scene)) { usage(argv[0]); return 2; }
        } else if (strcmp(argv[i], "--room") == 0 && i + 1 < argc && recordMode) {
            if (!parse_i32(argv[++i], 0, &config.room)) { usage(argv[0]); return 2; }
        } else if (strcmp(argv[i], "--spawn") == 0 && i + 1 < argc && recordMode) {
            if (!parse_i32(argv[++i], 0, &config.spawn)) { usage(argv[0]); return 2; }
        } else if (strcmp(argv[i], "--age") == 0 && i + 1 < argc && recordMode) {
            const char *age = argv[++i];
            if (strcmp(age, "adult") == 0) config.age = OOT_AGE_ADULT;
            else if (strcmp(age, "child") == 0) config.age = OOT_AGE_CHILD;
            else { usage(argv[0]); return 2; }
        } else if (strcmp(argv[i], "--tolerance") == 0 && i + 1 < argc && compareMode) {
            if (!parse_float_value(argv[++i], &requestedTolerance) || requestedTolerance < 0.0f) {
                usage(argv[0]); return 2;
            }
            toleranceOverride = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if ((!recordMode && !compareMode) || tracePath == NULL || !scenario_valid(config.scenario)) {
        usage(argv[0]);
        return 2;
    }
    if (compareMode) {
        trace = fopen(tracePath, "rb");
        if (trace == NULL || !read_header(trace, &config, &expectedRomHash, expectedLine) ||
            !scenario_valid(config.scenario)) {
            fprintf(stderr, "%s: invalid fidelity trace\n", tracePath);
            if (trace != NULL) fclose(trace);
            return 1;
        }
        if (toleranceOverride) {
            config.tolerance = requestedTolerance;
        }
    }
    if (!read_file(romPath, &rom, &romSize)) {
        if (trace != NULL) fclose(trace);
        return 1;
    }
    romHash = hash_rom(rom, romSize);
    if (compareMode && romHash != expectedRomHash) {
        fprintf(stderr, "ROM mismatch: trace=%016" PRIx64 ", input=%016" PRIx64 "\n",
                expectedRomHash, romHash);
        goto done;
    }
    if (!require_ok(oot_engine_config_init(&engineConfig), "config init")) goto done;
    engineConfig.romData = rom;
    engineConfig.romSize = romSize;
    engineConfig.renderFlags = OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS;
    engineConfig.sfxCallback = sfx_callback;
    engineConfig.sfxUserData = &sfx;
    if (!require_ok(oot_engine_create(&engineConfig, &engine), "engine create")) goto done;
    free(rom);
    rom = NULL;
    if (!require_ok(oot_engine_scene_load(engine, config.scene, config.room, &nativeResult),
                    "scene load") ||
        !require_ok(oot_engine_scene_get_spawn(engine, config.spawn, spawnPosition, &spawnYaw),
                    "scene spawn") ||
        !require_ok(oot_engine_link_create(engine, spawnPosition[0], spawnPosition[1],
                                           spawnPosition[2]), "Link create") ||
        !require_ok(oot_engine_link_set_age(engine, config.age), "Link age") ||
        !require_ok(oot_engine_link_set_pose(engine, spawnPosition[0], spawnPosition[1],
                                             spawnPosition[2], spawnYaw), "Link pose")) {
        goto done;
    }
    if (config.age == OOT_AGE_CHILD) {
        if (!require_ok(oot_engine_link_set_equipment(engine, OOT_SWORD_KOKIRI,
                                                       OOT_SHIELD_DEKU, OOT_TUNIC_KOKIRI,
                                                       OOT_BOOTS_KOKIRI), "child equipment")) goto done;
    } else if (!require_ok(oot_engine_link_set_equipment(engine, OOT_SWORD_MASTER,
                                                          OOT_SHIELD_HYLIAN, OOT_TUNIC_KOKIRI,
                                                          OOT_BOOTS_KOKIRI), "adult equipment")) {
        goto done;
    }
    if (!require_ok(oot_engine_scene_get_runtime(engine, &sceneRuntime),
                    "scene runtime") ||
        sceneRuntime.structSize != sizeof(sceneRuntime) ||
        sceneRuntime.version != OOT_SCENE_RUNTIME_VERSION ||
        sceneRuntime.sceneIndex != config.scene ||
        sceneRuntime.activeRoomIndex != (config.room == -1 ? 0 : config.room) ||
        sceneRuntime.geometryRoomIndex != config.room ||
        sceneRuntime.roomMetadataValid == 0u) {
        fprintf(stderr, "scene runtime does not match the loaded scene/room\n");
        goto done;
    }
    if (recordMode) {
        trace = fopen(tracePath, "wb");
        if (trace == NULL || !write_header(trace, &config, romHash)) {
            fprintf(stderr, "%s: could not create trace\n", tracePath);
            goto done;
        }
    }

    for (tick = 0u; tick < config.ticks; ++tick) {
        FrameRecord actual;

        scenario_input(config.scenario, tick, &input);
        sfx.count = 0u;
        sfx.hash = FNV64_OFFSET;
        if (!require_ok(oot_engine_step(engine, &input, &frame), "simulation step") ||
            frame == NULL ||
            !require_ok(oot_engine_scene_get_runtime(engine, &sceneRuntime),
                        "scene runtime")) {
            goto done;
        }
        frame_record_build(&actual, frame, &sfx, &sceneRuntime);
        if (actual.animId <= 0) {
            fprintf(stderr, "tick %u: Player animation has no canonical animId\n", tick + 1u);
            goto done;
        }
        if (firstAnimId == 0) {
            firstAnimId = actual.animId;
        } else if (actual.animId != firstAnimId) {
            sawAnimChange = 1;
        }
        if (recordMode) {
            if (!write_frame(trace, &actual)) {
                fprintf(stderr, "%s: trace write failed\n", tracePath);
                goto done;
            }
        } else {
            FrameRecord expected;
            char line[TRACE_LINE_MAX];
            char *source = tick == 0u ? expectedLine : line;

            if (tick != 0u && fgets(line, sizeof(line), trace) == NULL) {
                fprintf(stderr, "%s: trace ended before tick %u\n", tracePath, tick + 1u);
                goto done;
            }
            if (!parse_frame(source, &expected)) {
                fprintf(stderr, "%s: malformed frame %u\n", tracePath, tick + 1u);
                goto done;
            }
            compare_frames(&diff, &expected, &actual, config.tolerance);
        }
    }
    if (strcmp(config.scenario, "core") == 0 && config.ticks >= 100u && !sawAnimChange) {
        fprintf(stderr, "core scenario did not change Player animation in %u ticks\n",
                config.ticks);
        goto done;
    }
    if (recordMode) {
        if (fflush(trace) != 0) {
            fprintf(stderr, "%s: trace flush failed\n", tracePath);
            goto done;
        }
        printf("recorded %u ticks to %s (ROM %016" PRIx64 ")\n",
               config.ticks, tracePath, romHash);
    } else if (diff.mismatches != 0u) {
        fprintf(stderr, "fidelity: FAILED, %" PRIu64 " mismatches across %" PRIu64
                " compared fields\n", diff.mismatches, diff.fieldsCompared);
        goto done;
    } else {
        char trailing[TRACE_LINE_MAX];
        while (fgets(trailing, sizeof(trailing), trace) != NULL) {
            if (trailing[strspn(trailing, " \t\r\n")] != '\0') {
                fprintf(stderr, "%s: unexpected data after %u frames\n",
                        tracePath, config.ticks);
                goto done;
            }
        }
        printf("fidelity: PASS, %u ticks and %" PRIu64 " fields matched (tolerance %.9g)\n",
               config.ticks, diff.fieldsCompared, config.tolerance);
    }
    exitCode = 0;

done:
    if (trace != NULL) fclose(trace);
    if (engine != NULL) {
        OoTResult destroyResult = oot_engine_destroy(engine);
        if (destroyResult != OOT_ENGINE_RESULT_OK) {
            fprintf(stderr, "engine destroy: %s\n", oot_engine_result_string(destroyResult));
            exitCode = 1;
        }
    }
    free(rom);
    return exitCode;
}
