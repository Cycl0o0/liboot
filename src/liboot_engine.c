/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include "liboot_engine.h"
#include "core_context.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_MAGIC 0x4F4F5445u /* "OOTE" */
#define ENGINE_TARGET_SLOTS OOT_ENGINE_MAX_TARGETS
#define ENGINE_TARGET_INDEX_MASK 0xFFu
#define ENGINE_HOST_ACTOR_SLOTS OOT_ENGINE_MAX_HOST_ACTORS
#define ENGINE_HOST_ACTOR_INDEX_MASK 0xFFu
#define ENGINE_KNOWN_HOST_ACTOR_FLAGS \
    ((uint32_t)(OOT_HOST_ACTOR_ENABLED | OOT_HOST_ACTOR_TARGETABLE | \
                OOT_HOST_ACTOR_HOSTILE | OOT_HOST_ACTOR_HURT))
#define ENGINE_KNOWN_RENDER_FLAGS \
    ((uint32_t)(OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS))
#define ENGINE_KNOWN_BUTTONS \
    ((uint32_t)(OOT_ENGINE_BUTTON_A | OOT_ENGINE_BUTTON_B | OOT_ENGINE_BUTTON_Z | \
                OOT_ENGINE_BUTTON_R | OOT_ENGINE_BUTTON_ITEM | OOT_ENGINE_BUTTON_CUP))

#define MEMBER_END(type, member) \
    (offsetof(type, member) + sizeof(((type *)0)->member))

/* Hidden checked companion to the legacy void raw-world API. */
/* Returns 1 success, 0 invalid/unrepresentable geometry, -1 allocation failure. */
extern int liboot_static_world_load_checked(
    const struct OoTSurface *surfaces, uint32_t numSurfaces,
    const struct OoTWaterBox *waterBoxes, uint32_t numWaterBoxes);
/* liboot vNEXT: hidden scene shade-alpha accessor (scene.c). Kept internal so
   no new public raw symbol is added; the wrapper surfaces it via
   OoTEngineSceneGeometry.alpha. */
extern bool liboot_scene_get_alpha(const float **alpha);

typedef struct EngineGeometryStorage
{
    float *position;
    float *normal;
    float *color;
    float *uv;
    uint16_t *triTexture;
    float *alpha;   /* liboot vNEXT: 1 float/vertex shade alpha */
    uint8_t *triFlags;  /* liboot vNEXT: 1 byte/triangle render flags */
    struct OoTGeometryBatch *batches;
} EngineGeometryStorage;

typedef struct EngineTargetSlot
{
    int32_t nativeId;
    uint32_t generation;
    uint8_t active;
} EngineTargetSlot;

typedef struct EngineHostActorSlot
{
    int32_t nativeId;
    uint32_t generation;
    uint8_t active;
} EngineHostActorSlot;

typedef struct EngineDynamicCollisionSlot
{
    OoTDynamicCollision nativeHandle;
    uint32_t generation;
    uint8_t active;
} EngineDynamicCollisionSlot;

struct OoTEngine
{
    uint32_t magic;
    LibootCoreContext *coreContext;
    uint32_t actorCapacity;
    uint32_t linkTriangleCapacity;
    uint32_t sceneTriangleCapacity;
    uint32_t maxSubsteps;
    uint32_t renderFlags;
    float fixedStepSeconds;
    double accumulator;
    uint64_t simulationTick;
    uint32_t pendingButtons;
    uint8_t linkActive;
    uint8_t frameValid;
    uint8_t sceneGeometryValid;
    uint8_t currentAge;
    uint8_t coreInitSucceeded;
    uint8_t coreInitFailed;
    uint8_t worldLoaded;
    uint32_t sceneDroppedTriangles;

    OoTEngineDebugCallback debugCallback;
    void *debugUserData;
    OoTEngineSfxCallback sfxCallback;
    void *sfxUserData;

    EngineGeometryStorage frameStorage;
    EngineGeometryStorage sceneStorage;
    struct OoTActorInfo *actorStorage; /* actorCapacity + one truncation probe */
    OoTEngineFrame frame;
    OoTEngineSceneGeometry sceneGeometry;
    EngineTargetSlot targets[ENGINE_TARGET_SLOTS];
    EngineHostActorSlot hostActors[ENGINE_HOST_ACTOR_SLOTS];
    EngineDynamicCollisionSlot dynamicCollisions[OOT_DYNAMIC_COLLISION_MAX_OBJECTS];
};

/* This value intentionally belongs to the switched module image: each core
 * context records the wrapper handle that owns it. Callback dispatch is TLS
 * instead, because callbacks run only while the corresponding engine holds
 * the process-wide core lock. */
static OoTEngine *s_activeEngine;

static int engine_try_guard(void)
{
    return liboot_core_context_lock();
}

static void engine_unguard(void)
{
    (void)liboot_core_context_set_thread_owner(NULL);
    liboot_core_context_unlock();
}

static OoTResult engine_lock(OoTEngine *engine)
{
    if (engine == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (!engine_try_guard()) {
        return OOT_ENGINE_RESULT_BUSY;
    }
#if defined(LIBOOT_MULTI_INSTANCE)
    if (!liboot_core_context_activate_owner(engine, NULL)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_INITIALIZED;
    }
#endif
    if (s_activeEngine != engine || engine->magic != ENGINE_MAGIC) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_INITIALIZED;
    }
    if (!liboot_core_context_set_thread_owner(engine)) {
        liboot_core_context_unlock();
        return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    }
    return OOT_ENGINE_RESULT_OK;
}

static OoTResult engine_lock_link(OoTEngine *engine)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!engine->linkActive) {
        engine_unguard();
        return OOT_ENGINE_RESULT_LINK_NOT_FOUND;
    }
    return OOT_ENGINE_RESULT_OK;
}

static int config_has(const OoTEngineConfig *config, size_t memberEnd)
{
    return (size_t)config->structSize >= memberEnd;
}

static void engine_debug_trampoline(const char *message)
{
    OoTEngine *engine = (OoTEngine *)liboot_core_context_get_thread_owner();
    if (engine != NULL) {
        /* oot_global_init predates return codes.  Its terminal diagnostics
           are the only supported status channel, so consume them internally
           while still forwarding them to the host. */
        if (message != NULL && strcmp(message, "liboot: global init ok") == 0) {
            engine->coreInitSucceeded = 1u;
        } else if (message != NULL &&
                   (strcmp(message, "liboot: not a recognizable Zelda64 ROM") == 0 ||
                    strcmp(message, "liboot: link_animetion not found (unsupported ROM version?)") == 0 ||
                    strcmp(message, "liboot: object_link_boy not found in ROM") == 0)) {
            engine->coreInitFailed = 1u;
        }
        if (engine->debugCallback != NULL) {
            engine->debugCallback(engine->debugUserData, message);
        }
    }
}

static void engine_sfx_trampoline(const struct OoTSfxEvent *event)
{
    OoTEngine *engine = (OoTEngine *)liboot_core_context_get_thread_owner();
    if (engine != NULL && engine->sfxCallback != NULL) {
        engine->sfxCallback(engine->sfxUserData, event);
    }
}

static int geometry_storage_allocate(EngineGeometryStorage *storage, uint32_t triangleCapacity)
{
    size_t triangles = triangleCapacity;

    memset(storage, 0, sizeof(*storage));
    storage->position = (float *)malloc(triangles * 9u * sizeof(float));
    storage->normal = (float *)malloc(triangles * 9u * sizeof(float));
    storage->color = (float *)malloc(triangles * 9u * sizeof(float));
    storage->uv = (float *)malloc(triangles * 6u * sizeof(float));
    storage->triTexture = (uint16_t *)malloc(triangles * sizeof(uint16_t));
    storage->alpha = (float *)malloc(triangles * 3u * sizeof(float));
    storage->triFlags = (uint8_t *)malloc(triangles * sizeof(uint8_t));
    storage->batches = (struct OoTGeometryBatch *)malloc(
        triangles * sizeof(*storage->batches));

    return storage->position != NULL && storage->normal != NULL &&
           storage->color != NULL && storage->uv != NULL &&
           storage->triTexture != NULL && storage->alpha != NULL &&
           storage->triFlags != NULL && storage->batches != NULL;
}

static void geometry_storage_free(EngineGeometryStorage *storage)
{
    free(storage->position);
    free(storage->normal);
    free(storage->color);
    free(storage->uv);
    free(storage->triTexture);
    free(storage->alpha);
    free(storage->triFlags);
    free(storage->batches);
    memset(storage, 0, sizeof(*storage));
}

static void engine_free(OoTEngine *engine)
{
    if (engine == NULL) {
        return;
    }
    geometry_storage_free(&engine->frameStorage);
    geometry_storage_free(&engine->sceneStorage);
    free(engine->actorStorage);
    engine->magic = 0u;
    free(engine);
}

static void frame_views_init(OoTEngine *engine)
{
    OoTEngineFrame *frame = &engine->frame;

    memset(frame, 0, sizeof(*frame));
    frame->structSize = sizeof(*frame);
    frame->fixedStepSeconds = engine->fixedStepSeconds;
    frame->link.structSize = sizeof(frame->link);
    frame->geometry.structSize = sizeof(frame->geometry);
    frame->geometry.position = engine->frameStorage.position;
    frame->geometry.normal = engine->frameStorage.normal;
    frame->geometry.color = engine->frameStorage.color;
    frame->geometry.uv = engine->frameStorage.uv;
    frame->geometry.triTexture = engine->frameStorage.triTexture;
    frame->geometry.alpha = engine->frameStorage.alpha;
    frame->geometry.triFlags = engine->frameStorage.triFlags;
    frame->geometry.triangleCapacity = engine->linkTriangleCapacity;
    frame->actors = engine->actorStorage;
    frame->actorCapacity = engine->actorCapacity;
    frame->geometryBatches = engine->frameStorage.batches;
    frame->geometryBatchCapacity = engine->linkTriangleCapacity;
    frame->navi.structSize = sizeof(frame->navi);

    memset(&engine->sceneGeometry, 0, sizeof(engine->sceneGeometry));
    engine->sceneGeometry.structSize = sizeof(engine->sceneGeometry);
    engine->sceneGeometry.position = engine->sceneStorage.position;
    engine->sceneGeometry.normal = engine->sceneStorage.normal;
    engine->sceneGeometry.color = engine->sceneStorage.color;
    engine->sceneGeometry.uv = engine->sceneStorage.uv;
    engine->sceneGeometry.triTexture = engine->sceneStorage.triTexture;
    engine->sceneGeometry.alpha = engine->sceneStorage.alpha;
    engine->sceneGeometry.triFlags = engine->sceneStorage.triFlags;
    engine->sceneGeometry.triangleCapacity = engine->sceneTriangleCapacity;
    engine->sceneGeometry.batches = engine->sceneStorage.batches;
    engine->sceneGeometry.batchCapacity = engine->sceneTriangleCapacity;
}

static void targets_invalidate(OoTEngine *engine, int removeNative)
{
    uint32_t i;
    for (i = 0u; i < ENGINE_TARGET_SLOTS; ++i) {
        EngineTargetSlot *slot = &engine->targets[i];
        if (slot->active && removeNative) {
            oot_target_remove(slot->nativeId);
        }
        slot->active = 0u;
        slot->nativeId = -1;
    }
}

static OoTEngineTarget target_handle(uint32_t index, uint32_t generation)
{
    return (generation << 8) | (index + 1u);
}

static EngineTargetSlot *target_find(OoTEngine *engine, OoTEngineTarget target)
{
    uint32_t encodedIndex;
    uint32_t index;
    uint32_t generation;
    EngineTargetSlot *slot;

    if (target == OOT_ENGINE_INVALID_TARGET) {
        return NULL;
    }
    encodedIndex = target & ENGINE_TARGET_INDEX_MASK;
    if (encodedIndex == 0u) {
        return NULL;
    }
    index = encodedIndex - 1u;
    generation = target >> 8;
    if (index >= ENGINE_TARGET_SLOTS || generation == 0u) {
        return NULL;
    }
    slot = &engine->targets[index];
    if (!slot->active || slot->generation != generation) {
        return NULL;
    }
    return slot;
}

static OoTEngineHostActor host_actor_handle(uint32_t index, uint32_t generation)
{
    return (generation << 8) | (index + 1u);
}

static EngineHostActorSlot *host_actor_find(OoTEngine *engine,
                                             OoTEngineHostActor actor)
{
    uint32_t encodedIndex = actor & ENGINE_HOST_ACTOR_INDEX_MASK;
    uint32_t index;
    uint32_t generation;
    EngineHostActorSlot *slot;
    if (actor == OOT_ENGINE_INVALID_HOST_ACTOR || encodedIndex == 0u) {
        return NULL;
    }
    index = encodedIndex - 1u;
    generation = actor >> 8;
    if (index >= ENGINE_HOST_ACTOR_SLOTS || generation == 0u) {
        return NULL;
    }
    slot = &engine->hostActors[index];
    return slot->active && slot->generation == generation ? slot : NULL;
}

static void host_actors_invalidate(OoTEngine *engine, int removeNative)
{
    uint32_t i;
    for (i = 0u; i < ENGINE_HOST_ACTOR_SLOTS; ++i) {
        EngineHostActorSlot *slot = &engine->hostActors[i];
        if (slot->active && removeNative) {
            (void)oot_host_actor_remove(slot->nativeId);
        }
        slot->active = 0u;
        slot->nativeId = -1;
    }
}

static OoTDynamicCollision dynamic_collision_handle(uint32_t index,
                                                     uint32_t generation)
{
    return (generation << 8) | (index + 1u);
}

static EngineDynamicCollisionSlot *dynamic_collision_find(
    OoTEngine *engine, OoTDynamicCollision handle)
{
    uint32_t encodedIndex = handle & 0xFFu;
    uint32_t generation = handle >> 8;
    if (encodedIndex == 0u ||
        encodedIndex > OOT_DYNAMIC_COLLISION_MAX_OBJECTS ||
        generation == 0u)
        return NULL;
    EngineDynamicCollisionSlot *slot =
        &engine->dynamicCollisions[encodedIndex - 1u];
    return slot->active && slot->generation == generation ? slot : NULL;
}

static int finite3(float x, float y, float z)
{
    return isfinite(x) && isfinite(y) && isfinite(z);
}

static int host_actor_state_valid(const struct OoTHostActorState *state)
{
    return state != NULL && state->structSize >= sizeof(*state) &&
           state->version == OOT_HOST_ACTOR_STATE_VERSION &&
           (state->flags & ~ENGINE_KNOWN_HOST_ACTOR_FLAGS) == 0u &&
           finite3(state->position[0], state->position[1], state->position[2]) &&
           finite3(state->focusOffset[0], state->focusOffset[1],
                   state->focusOffset[2]) &&
           isfinite(state->hurtRadius) && state->hurtRadius >= 0.0f &&
           isfinite(state->hurtHeight) && state->hurtHeight >= 0.0f &&
           isfinite(state->hurtYOffset) && state->room >= -1 &&
           state->room <= INT8_MAX && state->attentionRange < 10u;
}

static OoTResult input_convert(const OoTEngineInput *input, uint32_t extraButtons,
                               struct OoTLinkInputs *nativeInput)
{
    float length;
    uint32_t buttons;

    memset(nativeInput, 0, sizeof(*nativeInput));
    nativeInput->camLookZ = 1.0f;
    if (input == NULL) {
        buttons = extraButtons;
    } else {
        if ((size_t)input->structSize < MEMBER_END(OoTEngineInput, buttons)) {
            return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
        }
        if (!isfinite(input->camLookX) || !isfinite(input->camLookZ) ||
            !isfinite(input->stickX) || !isfinite(input->stickY)) {
            return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
        }

        length = sqrtf(input->camLookX * input->camLookX +
                       input->camLookZ * input->camLookZ);
        if (length > 0.00001f) {
            nativeInput->camLookX = input->camLookX / length;
            nativeInput->camLookZ = input->camLookZ / length;
        }
        nativeInput->stickX = fmaxf(-1.0f, fminf(1.0f, input->stickX));
        nativeInput->stickY = fmaxf(-1.0f, fminf(1.0f, input->stickY));
        buttons = input->buttons | extraButtons;
    }

    buttons &= ENGINE_KNOWN_BUTTONS;
    nativeInput->buttonA = (buttons & OOT_ENGINE_BUTTON_A) != 0u;
    nativeInput->buttonB = (buttons & OOT_ENGINE_BUTTON_B) != 0u;
    nativeInput->buttonZ = (buttons & OOT_ENGINE_BUTTON_Z) != 0u;
    nativeInput->buttonR = (buttons & OOT_ENGINE_BUTTON_R) != 0u;
    nativeInput->buttonItem = (buttons & OOT_ENGINE_BUTTON_ITEM) != 0u;
    nativeInput->buttonCUp = (buttons & OOT_ENGINE_BUTTON_CUP) != 0u;
    return OOT_ENGINE_RESULT_OK;
}

static void link_state_copy(OoTEngineLinkState *out, const struct OoTLinkState *in)
{
    out->structSize = sizeof(*out);
    memcpy(out->position, in->position, sizeof(out->position));
    memcpy(out->velocity, in->velocity, sizeof(out->velocity));
    out->faceAngle = in->faceAngle;
    out->health = in->health;
    out->healthCapacity = in->healthCapacity;
    out->magic = in->magic;
    out->linearVelocity = in->linearVelocity;
    out->animFrame = in->animFrame;
    out->stateFlags1 = in->stateFlags1;
    out->stateFlags2 = in->stateFlags2;
    out->magicLevel = in->magicLevel;
    out->age = in->age;
    out->isDead = in->isDead;
    out->heldItemAction = in->heldItemAction;
    out->meleeWeaponState = in->meleeWeaponState;
    out->lockOnActive = in->lockOnActive;
    out->inWater = in->inWater;
    out->reserved0 = 0u;
    memcpy(out->lockOnPos, in->lockOnPos, sizeof(out->lockOnPos));
    out->waterSurfaceY = in->waterSurfaceY;
    out->action = in->action;
    out->lookPitch = in->lookPitch;
    out->lookYaw = in->lookYaw;
    out->floorSfxOffset = in->floorSfxOffset;
    out->attackAnim = in->attackAnim;
    out->stateFlags3 = in->stateFlags3;
    out->underwaterTimer = in->underwaterTimer;
    out->animId = in->animId;
}

static OoTResult engine_tick_locked(OoTEngine *engine, const OoTEngineInput *input,
                                    uint32_t extraButtons)
{
    struct OoTLinkInputs nativeInput;
    struct OoTLinkState nativeState;
    struct OoTLinkGeometryBuffers nativeGeometry;
    OoTResult result;
    int32_t actorCount;
    uint32_t actorQueryCapacity = engine->actorCapacity + 1u;

    result = input_convert(input, extraButtons, &nativeInput);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }

    memset(&nativeState, 0, sizeof(nativeState));
    memset(&nativeGeometry, 0, sizeof(nativeGeometry));
    nativeGeometry.position = engine->frameStorage.position;
    nativeGeometry.normal = engine->frameStorage.normal;
    nativeGeometry.color = engine->frameStorage.color;
    nativeGeometry.uv = engine->frameStorage.uv;
    nativeGeometry.triTexture = engine->frameStorage.triTexture;
    nativeGeometry.alpha = engine->frameStorage.alpha;
    nativeGeometry.triFlags = engine->frameStorage.triFlags;
    nativeGeometry.triangleCapacity = engine->linkTriangleCapacity;
    nativeGeometry.batches = engine->frameStorage.batches;
    nativeGeometry.batchCapacity = engine->linkTriangleCapacity;
    nativeGeometry.numTrianglesUsed = 0u;

    oot_link_tick_sized(0, &nativeInput, &nativeState, &nativeGeometry,
                        (uint32_t)sizeof(nativeGeometry));
    link_state_copy(&engine->frame.link, &nativeState);
    engine->currentAge = nativeState.age;
    engine->frame.geometry.numTriangles = nativeGeometry.numTrianglesUsed32;
    engine->frame.geometryBatchCount = nativeGeometry.numBatchesUsed;
    engine->frame.linkGeometryTruncated =
        oot_link_get_geometry_dropped_triangles() != 0u;

    actorCount = oot_actor_query(engine->actorStorage, (int32_t)actorQueryCapacity);
    if (actorCount < 0) {
        actorCount = 0;
    }
    engine->frame.actorListTruncated = (uint32_t)actorCount > engine->actorCapacity;
    engine->frame.actorCount = engine->frame.actorListTruncated
        ? engine->actorCapacity : (uint32_t)actorCount;

    memset(&engine->frame.skeleton, 0, sizeof(engine->frame.skeleton));
    engine->frame.skeletonAvailable =
        oot_link_get_skeleton(0, &engine->frame.skeleton) ? 1u : 0u;

    memset(&engine->frame.navi.position, 0,
           sizeof(engine->frame.navi) - offsetof(OoTEngineNaviState, position));
    engine->frame.navi.available = oot_navi_get(
        engine->frame.navi.position,
        engine->frame.navi.innerColor,
        engine->frame.navi.outerColor,
        &engine->frame.navi.scale) ? 1u : 0u;

    engine->simulationTick++;
    engine->frame.simulationTick = engine->simulationTick;
    engine->frame.fixedStepSeconds = engine->fixedStepSeconds;
    engine->frame.interpolationAlpha = (float)(engine->accumulator /
                                               (double)engine->fixedStepSeconds);
    engine->frameValid = 1u;
    return OOT_ENGINE_RESULT_OK;
}

static int item_allowed_for_age(uint8_t item, uint8_t age)
{
    if (age == OOT_AGE_CHILD) {
        return item != OOT_ITEM_HAMMER && item != OOT_ITEM_BOW &&
               item != OOT_ITEM_HOOKSHOT;
    }
    return item != OOT_ITEM_DEKU_STICK && item != OOT_ITEM_BOOMERANG;
}

static void scene_geometry_clear(OoTEngine *engine)
{
    engine->sceneGeometry.numTriangles = 0u;
    engine->sceneGeometry.xluStartTriangle = 0u;
    engine->sceneGeometry.batchCount = 0u;
    engine->sceneDroppedTriangles = 0u;
    engine->sceneGeometryValid = 0u;
}

static OoTResult scene_geometry_copy(OoTEngine *engine)
{
    const float *position = NULL;
    const float *normal = NULL;
    const float *color = NULL;
    const float *uv = NULL;
    const uint16_t *triTexture = NULL;
    const struct OoTGeometryBatch *batches = NULL;
    uint32_t numTriangles = 0u;
    uint32_t xluStart = 0u;
    uint32_t numBatches = 0u;

    scene_geometry_clear(engine);
    if (!oot_scene_get_geometry(&position, &normal, &color, &uv, &triTexture,
                                &numTriangles, &xluStart)) {
        return OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE;
    }
    if (numTriangles > engine->sceneTriangleCapacity || xluStart > numTriangles ||
        (numTriangles != 0u && (position == NULL || normal == NULL || color == NULL ||
                               uv == NULL || triTexture == NULL))) {
        return OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE;
    }
    if (!oot_scene_get_geometry_batches(&batches, &numBatches) ||
        numBatches > numTriangles ||
        (numBatches != 0u && batches == NULL)) {
        return OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE;
    }
    for (uint32_t batchIndex = 0u; batchIndex < numBatches; ++batchIndex) {
        const struct OoTGeometryBatch *batch = &batches[batchIndex];
        if (batch->numTriangles == 0u || batch->firstTriangle > numTriangles ||
            batch->numTriangles > numTriangles - batch->firstTriangle) {
            return OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE;
        }
    }

    if (numTriangles != 0u) {
        memcpy(engine->sceneStorage.position, position,
               (size_t)numTriangles * 9u * sizeof(float));
        memcpy(engine->sceneStorage.normal, normal,
               (size_t)numTriangles * 9u * sizeof(float));
        memcpy(engine->sceneStorage.color, color,
               (size_t)numTriangles * 9u * sizeof(float));
        memcpy(engine->sceneStorage.uv, uv,
               (size_t)numTriangles * 6u * sizeof(float));
        memcpy(engine->sceneStorage.triTexture, triTexture,
               (size_t)numTriangles * sizeof(uint16_t));
        {
            /* liboot vNEXT: scene shade alpha (parallel to color). Copy it if
               the core exposes it; otherwise fill opaque so consumers never
               read uninitialized alpha. */
            const float *sceneAlpha = NULL;
            if (liboot_scene_get_alpha(&sceneAlpha) && sceneAlpha != NULL) {
                memcpy(engine->sceneStorage.alpha, sceneAlpha,
                       (size_t)numTriangles * 3u * sizeof(float));
            } else {
                for (uint32_t k = 0u; k < numTriangles * 3u; ++k) {
                    engine->sceneStorage.alpha[k] = 1.0f;
                }
            }
        }
        {
            /* liboot vNEXT: per-triangle render flags (parallel to triTexture).
               Copy if present; otherwise clear so consumers read no flags. */
            const uint8_t *sceneFlags = NULL;
            if (oot_scene_get_triangle_flags(&sceneFlags) && sceneFlags != NULL) {
                memcpy(engine->sceneStorage.triFlags, sceneFlags,
                       (size_t)numTriangles * sizeof(uint8_t));
            } else {
                memset(engine->sceneStorage.triFlags, 0,
                       (size_t)numTriangles * sizeof(uint8_t));
            }
        }
        if (numBatches != 0u) {
            memcpy(engine->sceneStorage.batches, batches,
                   (size_t)numBatches * sizeof(*batches));
        }
    }
    engine->sceneGeometry.numTriangles = numTriangles;
    engine->sceneGeometry.xluStartTriangle = xluStart;
    engine->sceneGeometry.batchCount = numBatches;
    engine->sceneDroppedTriangles = oot_scene_get_geometry_dropped_triangles();
    engine->sceneGeometryValid = 1u;
    return OOT_ENGINE_RESULT_OK;
}

uint32_t oot_engine_api_version(void)
{
    return OOT_ENGINE_API_VERSION;
}

OoTResult oot_engine_get_limits(OoTEngineLimits *outLimits)
{
    uint64_t capabilities =
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
        (uint64_t)OOT_ENGINE_CAPABILITY_SCENE_MATERIAL_METADATA;
    OoTEngineLimits limits;
    uint32_t callerSize;
    size_t copySize;

    if (outLimits == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    callerSize = outLimits->structSize;
    if ((size_t)callerSize < MEMBER_END(OoTEngineLimits, capabilityFlags)) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (outLimits->version != OOT_ENGINE_LIMITS_VERSION) {
        return OOT_ENGINE_RESULT_API_VERSION;
    }

#if defined(LIBOOT_MULTI_INSTANCE)
    /* Capability discovery is protected by the same process-wide guard as
     * engine calls. Reporting a transient third capability state would make
     * identical queries disagree under contention, so fail without writing
     * and let the caller retry. */
    if (!liboot_core_context_lock()) {
        return OOT_ENGINE_RESULT_BUSY;
    }
    if (liboot_core_context_available())
        capabilities |= (uint64_t)OOT_ENGINE_CAPABILITY_MULTI_INSTANCE;
    liboot_core_context_unlock();
#else
    capabilities |= (uint64_t)OOT_ENGINE_CAPABILITY_PROCESS_SINGLETON;
#endif

    memset(&limits, 0, sizeof(limits));
    limits.structSize = callerSize;
    limits.version = OOT_ENGINE_LIMITS_VERSION;
    limits.capabilityFlags = capabilities;
    limits.linkTriangleCapacity = OOT_ENGINE_DEFAULT_LINK_TRIANGLE_CAPACITY;
    limits.sceneTriangleCapacity = OOT_ENGINE_DEFAULT_SCENE_TRIANGLE_CAPACITY;
    limits.staticSurfaceCapacity = OOT_ENGINE_MAX_STATIC_SURFACES;
    limits.waterBoxCapacity = OOT_ENGINE_MAX_WATER_BOXES;
    limits.maxActorCapacity = OOT_ENGINE_MAX_ACTOR_CAPACITY;
    limits.targetCapacity = OOT_ENGINE_MAX_TARGETS;
    limits.textureCapacity = OOT_ENGINE_MAX_TEXTURES;
    limits.maxSubsteps = OOT_ENGINE_MAX_SUBSTEPS;
    limits.minFixedStepSeconds = OOT_ENGINE_MIN_FIXED_STEP_SECONDS;
    limits.maxFixedStepSeconds = OOT_ENGINE_MAX_FIXED_STEP_SECONDS;
    limits.defaultFixedStepSeconds = OOT_ENGINE_DEFAULT_FIXED_STEP;
    limits.defaultMaxSubsteps = OOT_ENGINE_DEFAULT_MAX_SUBSTEPS;
    limits.maxLinkTriangleCapacity = OOT_ENGINE_MAX_GEOMETRY_TRIANGLE_CAPACITY;
    limits.maxSceneTriangleCapacity = OOT_ENGINE_MAX_GEOMETRY_TRIANGLE_CAPACITY;

    copySize = (size_t)callerSize < sizeof(limits)
                   ? (size_t)callerSize : sizeof(limits);
    memcpy(outLimits, &limits, copySize);
    return OOT_ENGINE_RESULT_OK;
}

const char *oot_engine_result_string(OoTResult result)
{
    switch (result) {
    case OOT_ENGINE_RESULT_OK: return "ok";
    case OOT_ENGINE_RESULT_INVALID_ARGUMENT: return "invalid argument";
    case OOT_ENGINE_RESULT_API_VERSION: return "API version mismatch";
    case OOT_ENGINE_RESULT_OUT_OF_MEMORY: return "out of memory";
    case OOT_ENGINE_RESULT_SINGLETON_IN_USE: return "an engine already exists";
    case OOT_ENGINE_RESULT_ROM_UNSUPPORTED: return "unsupported or invalid ROM";
    case OOT_ENGINE_RESULT_NOT_INITIALIZED: return "engine is not initialized";
    case OOT_ENGINE_RESULT_BUSY: return "engine is busy or callback-reentrant";
    case OOT_ENGINE_RESULT_LINK_ALREADY_EXISTS: return "Link already exists";
    case OOT_ENGINE_RESULT_LINK_NOT_FOUND: return "Link does not exist";
    case OOT_ENGINE_RESULT_AGE_RESTRICTED: return "item is unavailable for this age";
    case OOT_ENGINE_RESULT_TARGET_CAPACITY: return "target capacity reached";
    case OOT_ENGINE_RESULT_TARGET_NOT_FOUND: return "target handle is stale or invalid";
    case OOT_ENGINE_RESULT_SCENE_LOAD_FAILED: return "scene load failed";
    case OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE: return "scene geometry unavailable";
    case OOT_ENGINE_RESULT_NO_FRAME: return "no completed simulation frame";
    case OOT_ENGINE_RESULT_NOT_AVAILABLE: return "resource is not available";
    case OOT_ENGINE_RESULT_DYNAMIC_COLLISION_CAPACITY:
        return "dynamic collision capacity reached";
    case OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND:
        return "dynamic collision object was not found";
    case OOT_ENGINE_RESULT_HOST_ACTOR_CAPACITY:
        return "host actor capacity reached";
    case OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND:
        return "host actor handle is stale or invalid";
    default: return "unknown result";
    }
}

OoTResult oot_engine_config_init_sized(OoTEngineConfig *config,
                                       uint32_t structSize,
                                       uint32_t apiVersion)
{
    OoTEngineConfig defaults;
    size_t copySize;

    if (config == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (apiVersion != OOT_ENGINE_API_VERSION) {
        return OOT_ENGINE_RESULT_API_VERSION;
    }
    if ((size_t)structSize < MEMBER_END(OoTEngineConfig, romSize)) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(&defaults, 0, sizeof(defaults));
    defaults.structSize = (uint32_t)sizeof(defaults);
    defaults.apiVersion = OOT_ENGINE_API_VERSION;
    defaults.actorCapacity = OOT_ENGINE_DEFAULT_ACTOR_CAPACITY;
    defaults.maxSubsteps = OOT_ENGINE_DEFAULT_MAX_SUBSTEPS;
    defaults.fixedStepSeconds = OOT_ENGINE_DEFAULT_FIXED_STEP;
    defaults.linkTriangleCapacity = OOT_ENGINE_DEFAULT_LINK_TRIANGLE_CAPACITY;
    defaults.sceneTriangleCapacity = OOT_ENGINE_DEFAULT_SCENE_TRIANGLE_CAPACITY;
    copySize = (size_t)structSize < sizeof(defaults)
                   ? (size_t)structSize : sizeof(defaults);
    memcpy(config, &defaults, copySize);
    config->structSize = structSize;
    config->apiVersion = apiVersion;
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_input_init_sized(OoTEngineInput *input,
                                      uint32_t structSize,
                                      uint32_t apiVersion)
{
    OoTEngineInput defaults;
    size_t copySize;

    if (input == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (apiVersion != OOT_ENGINE_API_VERSION) {
        return OOT_ENGINE_RESULT_API_VERSION;
    }
    if ((size_t)structSize < MEMBER_END(OoTEngineInput, buttons)) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(&defaults, 0, sizeof(defaults));
    defaults.structSize = (uint32_t)sizeof(defaults);
    defaults.camLookZ = 1.0f;
    copySize = (size_t)structSize < sizeof(defaults)
                   ? (size_t)structSize : sizeof(defaults);
    memcpy(input, &defaults, copySize);
    input->structSize = structSize;
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_create(const OoTEngineConfig *config, OoTEngine **outEngine)
{
    const size_t requiredSize = MEMBER_END(OoTEngineConfig, romSize);
    OoTEngine *engine;
    uint32_t actorCapacity = OOT_ENGINE_DEFAULT_ACTOR_CAPACITY;
    uint32_t maxSubsteps = OOT_ENGINE_DEFAULT_MAX_SUBSTEPS;
    uint32_t renderFlags = 0u;
    uint32_t linkTriangleCapacity = OOT_ENGINE_DEFAULT_LINK_TRIANGLE_CAPACITY;
    uint32_t sceneTriangleCapacity = OOT_ENGINE_DEFAULT_SCENE_TRIANGLE_CAPACITY;
    float fixedStep = OOT_ENGINE_DEFAULT_FIXED_STEP;

    if (outEngine == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outEngine = NULL;
    if (config == NULL || (size_t)config->structSize < requiredSize ||
        config->romData == NULL || config->romSize < OOT_ENGINE_MIN_ROM_SIZE ||
        config->romSize > OOT_ENGINE_MAX_ROM_SIZE) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (config->apiVersion != OOT_ENGINE_API_VERSION) {
        return OOT_ENGINE_RESULT_API_VERSION;
    }

    if (config_has(config, MEMBER_END(OoTEngineConfig, actorCapacity)) &&
        config->actorCapacity != 0u) {
        actorCapacity = config->actorCapacity;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, maxSubsteps)) &&
        config->maxSubsteps != 0u) {
        maxSubsteps = config->maxSubsteps;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, fixedStepSeconds)) &&
        config->fixedStepSeconds != 0.0f) {
        fixedStep = config->fixedStepSeconds;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, renderFlags))) {
        renderFlags = config->renderFlags;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, linkTriangleCapacity)) &&
        config->linkTriangleCapacity != 0u) {
        linkTriangleCapacity = config->linkTriangleCapacity;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, sceneTriangleCapacity)) &&
        config->sceneTriangleCapacity != 0u) {
        sceneTriangleCapacity = config->sceneTriangleCapacity;
    }
    if (actorCapacity > OOT_ENGINE_MAX_ACTOR_CAPACITY ||
        maxSubsteps > OOT_ENGINE_MAX_SUBSTEPS || !isfinite(fixedStep) ||
        fixedStep < OOT_ENGINE_MIN_FIXED_STEP_SECONDS ||
        fixedStep > OOT_ENGINE_MAX_FIXED_STEP_SECONDS ||
        linkTriangleCapacity > OOT_ENGINE_MAX_GEOMETRY_TRIANGLE_CAPACITY ||
        sceneTriangleCapacity > OOT_ENGINE_MAX_GEOMETRY_TRIANGLE_CAPACITY ||
        (renderFlags & ~ENGINE_KNOWN_RENDER_FLAGS) != 0u) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }

    if (!engine_try_guard()) {
        return OOT_ENGINE_RESULT_BUSY;
    }
#if defined(LIBOOT_MULTI_INSTANCE)
    if (!liboot_core_context_available() ||
        !liboot_core_context_activate(NULL)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
#else
    if (s_activeEngine != NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_SINGLETON_IN_USE;
    }
#endif

    engine = (OoTEngine *)calloc(1u, sizeof(*engine));
    if (engine == NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    }
    engine->magic = ENGINE_MAGIC;
    engine->actorCapacity = actorCapacity;
    engine->linkTriangleCapacity = linkTriangleCapacity;
    engine->sceneTriangleCapacity = sceneTriangleCapacity;
    engine->maxSubsteps = maxSubsteps;
    engine->renderFlags = renderFlags;
    engine->fixedStepSeconds = fixedStep;
    engine->currentAge = OOT_AGE_ADULT;
    s_activeEngine = engine;
    if (!liboot_core_context_set_thread_owner(engine)) {
        s_activeEngine = NULL;
#if defined(LIBOOT_MULTI_INSTANCE)
        (void)liboot_core_context_activate(NULL);
#endif
        engine_free(engine);
        engine_unguard();
        return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    }

    if (config_has(config, MEMBER_END(OoTEngineConfig, debugCallback))) {
        engine->debugCallback = config->debugCallback;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, debugUserData))) {
        engine->debugUserData = config->debugUserData;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, sfxCallback))) {
        engine->sfxCallback = config->sfxCallback;
    }
    if (config_has(config, MEMBER_END(OoTEngineConfig, sfxUserData))) {
        engine->sfxUserData = config->sfxUserData;
    }

    if (!geometry_storage_allocate(&engine->frameStorage, linkTriangleCapacity) ||
        !geometry_storage_allocate(&engine->sceneStorage, sceneTriangleCapacity)) {
        s_activeEngine = NULL;
#if defined(LIBOOT_MULTI_INSTANCE)
        (void)liboot_core_context_activate(NULL);
#endif
        engine_free(engine);
        engine_unguard();
        return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    }
    engine->actorStorage = (struct OoTActorInfo *)calloc(
        (size_t)actorCapacity + 1u, sizeof(*engine->actorStorage));
    if (engine->actorStorage == NULL) {
        s_activeEngine = NULL;
#if defined(LIBOOT_MULTI_INSTANCE)
        (void)liboot_core_context_activate(NULL);
#endif
        engine_free(engine);
        engine_unguard();
        return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    }
    frame_views_init(engine);
    targets_invalidate(engine, 0);
    host_actors_invalidate(engine, 0);

    oot_set_debug_print_function(engine_debug_trampoline);
    oot_set_sfx_callback(NULL);
    oot_set_sfx_callback_ex(engine_sfx_trampoline);
    oot_global_init(config->romData, config->romSize, NULL);

    if (!engine->coreInitSucceeded || engine->coreInitFailed) {
        oot_set_sfx_callback_ex(NULL);
        oot_set_debug_print_function(NULL);
        oot_global_terminate();
        s_activeEngine = NULL;
#if defined(LIBOOT_MULTI_INSTANCE)
        (void)liboot_core_context_activate(NULL);
#endif
        engine_free(engine);
        engine_unguard();
        return OOT_ENGINE_RESULT_ROM_UNSUPPORTED;
    }
    /* Raw host actors are process-global just like the rest of the native
       core. Defensive clear also covers a previous failed/abrupt lifecycle. */
    oot_host_actor_clear();
    if (!oot_scene_set_geometry_capacity(sceneTriangleCapacity)) {
        oot_set_sfx_callback_ex(NULL);
        oot_set_debug_print_function(NULL);
        oot_global_terminate();
        s_activeEngine = NULL;
#if defined(LIBOOT_MULTI_INSTANCE)
        (void)liboot_core_context_activate(NULL);
#endif
        engine_free(engine);
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }

    oot_navi_set_render((renderFlags & OOT_ENGINE_RENDER_NAVI) != 0u);
    oot_actor_set_render((renderFlags & OOT_ENGINE_RENDER_ACTORS) != 0u);
#if defined(LIBOOT_MULTI_INSTANCE)
    engine->coreContext = liboot_core_context_capture(engine);
    if (engine->coreContext == NULL ||
        !liboot_core_context_adopt(engine->coreContext)) {
        if (engine->coreContext != NULL) {
            liboot_core_context_discard(engine->coreContext);
            engine->coreContext = NULL;
        }
        oot_set_sfx_callback_ex(NULL);
        oot_set_sfx_callback(NULL);
        oot_set_debug_print_function(NULL);
        oot_global_terminate();
        s_activeEngine = NULL;
        (void)liboot_core_context_activate(NULL);
        engine_free(engine);
        engine_unguard();
        return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    }
#endif
    *outEngine = engine;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_destroy(OoTEngine *engine)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }

    if (engine->linkActive) {
        targets_invalidate(engine, 1);
        host_actors_invalidate(engine, 1);
        oot_link_delete(0);
        engine->linkActive = 0u;
    }
    oot_host_actor_clear();
    oot_set_sfx_callback_ex(NULL);
    oot_set_sfx_callback(NULL);
    oot_set_debug_print_function(NULL);
    oot_global_terminate();
#if defined(LIBOOT_MULTI_INSTANCE)
    {
        LibootCoreContext *context = engine->coreContext;
        engine->coreContext = NULL;
        s_activeEngine = NULL;
        liboot_core_context_discard(context);
    }
#else
    s_activeEngine = NULL;
#endif
    engine_free(engine);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_set_callbacks(OoTEngine *engine,
                                   OoTEngineDebugCallback debugCallback,
                                   void *debugUserData,
                                   OoTEngineSfxCallback sfxCallback,
                                   void *sfxUserData)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    engine->debugCallback = debugCallback;
    engine->debugUserData = debugUserData;
    engine->sfxCallback = sfxCallback;
    engine->sfxUserData = sfxUserData;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_create(OoTEngine *engine, float x, float y, float z)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (engine->linkActive) {
        engine_unguard();
        return OOT_ENGINE_RESULT_LINK_ALREADY_EXISTS;
    }
    if (!engine->worldLoaded) {
        /* Player_Init immediately probes BgCheck.  The raw core assumes the
           host has installed collision first; make that lifecycle dependency
           an explicit result instead of allowing a null collision context. */
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    if (!finite3(x, y, z)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (oot_link_create(x, y, z) < 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine->linkActive = 1u;
    engine->currentAge = OOT_AGE_ADULT;
    engine->frameValid = 0u;
    engine->accumulator = 0.0;
    engine->pendingButtons = 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_delete(OoTEngine *engine)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    targets_invalidate(engine, 1);
    host_actors_invalidate(engine, 1);
    oot_link_delete(0);
    engine->linkActive = 0u;
    engine->frameValid = 0u;
    engine->accumulator = 0.0;
    engine->pendingButtons = 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_set_age(OoTEngine *engine, uint8_t age)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (age > OOT_AGE_CHILD) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (age == engine->currentAge) {
        engine_unguard();
        return OOT_ENGINE_RESULT_OK;
    }
    if (!oot_link_set_age(0, age)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    /* A successful age switch reinitializes Link and despawns every actor,
       including native target slots. */
    targets_invalidate(engine, 0);
    host_actors_invalidate(engine, 0);
    engine->currentAge = age;
    engine->frameValid = 0u;
    engine->pendingButtons = 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_set_equipment(OoTEngine *engine, uint8_t sword,
                                        uint8_t shield, uint8_t tunic, uint8_t boots)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (sword > OOT_SWORD_BIGGORON || shield > OOT_SHIELD_MIRROR ||
        tunic > OOT_TUNIC_ZORA || boots > OOT_BOOTS_HOVER) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    oot_link_set_equipment(0, sword, shield, tunic, boots);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_use_item(OoTEngine *engine, uint8_t item)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (item > OOT_ITEM_BOMB) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (!item_allowed_for_age(item, engine->currentAge)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_AGE_RESTRICTED;
    }
    oot_link_use_item(0, item);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_set_health(OoTEngine *engine, int16_t health, int16_t capacity)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (capacity <= 0 || capacity > 0x140 || health < 0 || health > capacity) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    oot_link_set_health(0, health, capacity);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_damage(OoTEngine *engine, int16_t amount)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (amount <= 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    oot_link_damage(0, amount);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_set_magic(OoTEngine *engine, uint8_t level, int16_t amount)
{
    OoTResult result = engine_lock_link(engine);
    int16_t capacity;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    capacity = (int16_t)(level * 0x30);
    if (level > 2u || amount < 0 || amount > capacity) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    oot_link_set_magic(0, level, amount);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_set_pose(OoTEngine *engine, float x, float y, float z, int16_t yaw)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_link_set_pose(0, x, y, z, yaw)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_freeze(OoTEngine *engine, uint8_t frozen)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    oot_link_freeze(0, frozen != 0u);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_link_set_invincible(OoTEngine *engine, int8_t frames)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    oot_link_set_invincible(0, frames);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_query_surface(OoTEngine *engine, float x, float y, float z,
                                         struct OoTSurfaceInfo *outInfo)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_scene_query_surface(x, y, z, outInfo)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_step(OoTEngine *engine, const OoTEngineInput *input,
                          const OoTEngineFrame **outFrame)
{
    OoTResult result = engine_lock_link(engine);
    if (outFrame != NULL) {
        *outFrame = NULL;
    }
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    result = engine_tick_locked(engine, input, engine->pendingButtons);
    if (result == OOT_ENGINE_RESULT_OK) {
        engine->pendingButtons = 0u;
        engine->frame.interpolationAlpha = (float)(engine->accumulator /
                                                   (double)engine->fixedStepSeconds);
        if (outFrame != NULL) {
            *outFrame = &engine->frame;
        }
    }
    engine_unguard();
    return result;
}

OoTResult oot_engine_advance(OoTEngine *engine, float elapsedSeconds,
                             const OoTEngineInput *input, uint32_t *outSteps,
                             const OoTEngineFrame **outFrame)
{
    OoTResult result = engine_lock_link(engine);
    struct OoTLinkInputs validationInput;
    uint32_t buttons = 0u;
    uint32_t steps = 0u;
    double maximumAccumulator;
    double tickTolerance;

    if (outSteps != NULL) {
        *outSteps = 0u;
    }
    if (outFrame != NULL) {
        *outFrame = NULL;
    }
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!isfinite(elapsedSeconds) || elapsedSeconds < 0.0f) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    result = input_convert(input, 0u, &validationInput);
    if (result != OOT_ENGINE_RESULT_OK) {
        engine_unguard();
        return result;
    }
    if (input != NULL) {
        buttons = input->buttons & ENGINE_KNOWN_BUTTONS;
    }
    engine->pendingButtons |= buttons;
    engine->accumulator += elapsedSeconds;
    maximumAccumulator = (double)engine->fixedStepSeconds * engine->maxSubsteps;
    tickTolerance = (double)engine->fixedStepSeconds * 1e-6;
    if (engine->accumulator > maximumAccumulator) {
        engine->accumulator = maximumAccumulator;
    }

    while (engine->accumulator + tickTolerance >= (double)engine->fixedStepSeconds &&
           steps < engine->maxSubsteps) {
        uint32_t latchedButtons = steps == 0u ? engine->pendingButtons : 0u;
        result = engine_tick_locked(engine, input, latchedButtons);
        if (result != OOT_ENGINE_RESULT_OK) {
            break;
        }
        engine->pendingButtons = 0u;
        engine->accumulator -= engine->fixedStepSeconds;
        steps++;
    }
    if (engine->accumulator < 0.0) {
        engine->accumulator = 0.0;
    }
    if (engine->frameValid) {
        engine->frame.interpolationAlpha = (float)(engine->accumulator /
                                                   (double)engine->fixedStepSeconds);
    }
    if (outSteps != NULL) {
        *outSteps = steps;
    }
    if (outFrame != NULL && engine->frameValid) {
        *outFrame = &engine->frame;
    }
    engine_unguard();
    return result;
}

OoTResult oot_engine_get_frame(OoTEngine *engine, const OoTEngineFrame **outFrame)
{
    OoTResult result;
    if (outFrame == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outFrame = NULL;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!engine->frameValid) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NO_FRAME;
    }
    *outFrame = &engine->frame;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_reset_clock(OoTEngine *engine)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    engine->accumulator = 0.0;
    engine->pendingButtons = 0u;
    if (engine->frameValid) {
        engine->frame.interpolationAlpha = 0.0f;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_static_world_load(OoTEngine *engine,
                                       const struct OoTSurface *surfaces,
                                       uint32_t numSurfaces,
                                       const struct OoTWaterBox *waterBoxes,
                                       uint32_t numWaterBoxes)
{
    OoTResult result = engine_lock(engine);
    uint32_t i;
    uint32_t vertex;
    uint32_t axis;
    int worldResult;

    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (surfaces == NULL || numSurfaces == 0u ||
        numSurfaces > OOT_ENGINE_MAX_STATIC_SURFACES ||
        (numWaterBoxes != 0u && waterBoxes == NULL) ||
        numWaterBoxes > OOT_ENGINE_MAX_WATER_BOXES) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    for (i = 0u; i < numSurfaces; ++i) {
        for (vertex = 0u; vertex < 3u; ++vertex) {
            for (axis = 0u; axis < 3u; ++axis) {
                int32_t value = surfaces[i].vertices[vertex][axis];
                if (value < -32768 || value > 32767) {
                    engine_unguard();
                    return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
                }
            }
        }
    }
    for (i = 0u; i < numWaterBoxes; ++i) {
        if (waterBoxes[i].xLength <= 0 || waterBoxes[i].zLength <= 0) {
            engine_unguard();
            return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
        }
    }

    worldResult = liboot_static_world_load_checked(surfaces, numSurfaces,
                                                   waterBoxes, numWaterBoxes);
    if (worldResult < 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    }
    if (worldResult == 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    engine->worldLoaded = 1u;
    scene_geometry_clear(engine);
    engine->frameValid = 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_dynamic_collision_create(
    OoTEngine *engine, const struct OoTSurface *surfaces, uint32_t numSurfaces,
    const struct OoTDynamicCollisionTransform *transform, uint32_t flags,
    OoTDynamicCollision *outHandle)
{
    OoTResult result;
    int32_t nativeResult;
    OoTDynamicCollision nativeHandle = OOT_DYNAMIC_COLLISION_INVALID;
    uint32_t generation;
    uint32_t slotIndex;
    if (outHandle == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outHandle = OOT_DYNAMIC_COLLISION_INVALID;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!engine->worldLoaded) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    for (slotIndex = 0u; slotIndex < OOT_DYNAMIC_COLLISION_MAX_OBJECTS;
         ++slotIndex) {
        if (!engine->dynamicCollisions[slotIndex].active) break;
    }
    if (slotIndex == OOT_DYNAMIC_COLLISION_MAX_OBJECTS) {
        engine_unguard();
        return OOT_ENGINE_RESULT_DYNAMIC_COLLISION_CAPACITY;
    }
    nativeResult = oot_dynamic_collision_create(surfaces, numSurfaces, transform,
                                                 flags, &nativeHandle);
    if (nativeResult == 0) {
        generation = liboot_core_context_next_handle_namespace();
        if (generation == 0u) {
            (void)oot_dynamic_collision_delete(nativeHandle);
            engine_unguard();
            return OOT_ENGINE_RESULT_DYNAMIC_COLLISION_CAPACITY;
        }
        engine->dynamicCollisions[slotIndex].nativeHandle = nativeHandle;
        engine->dynamicCollisions[slotIndex].generation = generation;
        engine->dynamicCollisions[slotIndex].active = 1u;
        *outHandle = dynamic_collision_handle(slotIndex, generation);
    }
    engine_unguard();
    switch (nativeResult) {
    case 0: return OOT_ENGINE_RESULT_OK;
    case -2: return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    case -3: return OOT_ENGINE_RESULT_DYNAMIC_COLLISION_CAPACITY;
    case -4: return OOT_ENGINE_RESULT_OUT_OF_MEMORY;
    default: return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
}

OoTResult oot_engine_dynamic_collision_set_transform(
    OoTEngine *engine, OoTDynamicCollision handle,
    const struct OoTDynamicCollisionTransform *transform)
{
    OoTResult result = engine_lock(engine);
    EngineDynamicCollisionSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (handle == OOT_DYNAMIC_COLLISION_INVALID || transform == NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    slot = dynamic_collision_find(engine, handle);
    if (slot == NULL ||
        !oot_dynamic_collision_set_transform(slot->nativeHandle, transform)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND;
    }
    engine->frameValid = 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_dynamic_collision_set_enabled(
    OoTEngine *engine, OoTDynamicCollision handle, uint8_t enabled)
{
    OoTResult result = engine_lock(engine);
    EngineDynamicCollisionSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (handle == OOT_DYNAMIC_COLLISION_INVALID) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    slot = dynamic_collision_find(engine, handle);
    if (slot == NULL ||
        !oot_dynamic_collision_set_enabled(slot->nativeHandle, enabled != 0u)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND;
    }
    engine->frameValid = 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_dynamic_collision_get_state(
    OoTEngine *engine, OoTDynamicCollision handle,
    struct OoTDynamicCollisionState *outState)
{
    OoTResult result;
    EngineDynamicCollisionSlot *slot;
    if (outState == NULL ||
        outState->structSize < sizeof(struct OoTDynamicCollisionState)) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (handle == OOT_DYNAMIC_COLLISION_INVALID) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    slot = dynamic_collision_find(engine, handle);
    if (slot == NULL ||
        !oot_dynamic_collision_get_state(slot->nativeHandle, outState)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_dynamic_collision_delete(OoTEngine *engine,
                                               OoTDynamicCollision handle)
{
    OoTResult result = engine_lock(engine);
    EngineDynamicCollisionSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (handle == OOT_DYNAMIC_COLLISION_INVALID) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    slot = dynamic_collision_find(engine, handle);
    if (slot == NULL || !oot_dynamic_collision_delete(slot->nativeHandle)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND;
    }
    slot->active = 0u;
    slot->nativeHandle = OOT_DYNAMIC_COLLISION_INVALID;
    engine->frameValid = 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_load(OoTEngine *engine, int32_t sceneIndex,
                                int32_t roomIndex, int32_t *outNativeResult)
{
    const struct OoTSceneLoadOptions options =
        OOT_SCENE_LOAD_OPTIONS_INIT(sceneIndex, roomIndex,
                                    OOT_SCENE_LAYER_CHILD_DAY);
    return oot_engine_scene_load_ex(engine, &options, outNativeResult);
}

OoTResult oot_engine_scene_load_ex(
    OoTEngine *engine, const struct OoTSceneLoadOptions *options,
    int32_t *outNativeResult)
{
    struct OoTSceneLoadOptions optionsCopy;
    OoTResult result;
    int32_t nativeResult;
    if (options == NULL ||
        options->structSize < sizeof(struct OoTSceneLoadOptions)) {
        if (outNativeResult != NULL) {
            *outNativeResult = 0;
        }
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    optionsCopy = *options;
    if (outNativeResult != NULL) {
        *outNativeResult = 0;
    }
    if (optionsCopy.sceneIndex < 0 || optionsCopy.roomIndex < -1 ||
        optionsCopy.layer > OOT_SCENE_LAYER_ADULT_NIGHT) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }

    nativeResult = oot_scene_load_ex(&optionsCopy);
    if (outNativeResult != NULL) {
        *outNativeResult = nativeResult;
    }
    if (nativeResult == -9) {
        engine->worldLoaded = 1u; /* collision was committed by the core */
        scene_geometry_clear(engine);
        engine->frameValid = 0u;
        engine_unguard();
        return OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE;
    }
    if (nativeResult < 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_SCENE_LOAD_FAILED;
    }
    result = scene_geometry_copy(engine);
    engine->worldLoaded = 1u;
    engine->frameValid = 0u;
    engine_unguard();
    return result;
}

OoTResult oot_engine_scene_get_active_layer(
    OoTEngine *engine, uint8_t *outLayer, uint8_t *outUsedFallback)
{
    OoTResult result;
    bool usedFallback = false;
    if (outLayer == NULL || outUsedFallback == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outLayer = OOT_SCENE_LAYER_CHILD_DAY;
    *outUsedFallback = 0u;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_scene_get_active_layer(outLayer, &usedFallback)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    *outUsedFallback = usedFallback ? 1u : 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_set_room(OoTEngine *engine, int32_t roomIndex,
                                    int32_t *outNativeResult)
{
    OoTResult result = engine_lock(engine);
    int32_t nativeResult;
    if (outNativeResult != NULL) {
        *outNativeResult = 0;
    }
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (roomIndex < -1) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    nativeResult = oot_scene_set_room(roomIndex);
    if (outNativeResult != NULL) {
        *outNativeResult = nativeResult;
    }
    if (nativeResult == -9) {
        engine->worldLoaded = 1u;
        scene_geometry_clear(engine);
        engine->frameValid = 0u;
        engine_unguard();
        return OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE;
    }
    if (nativeResult < 0) {
        engine_unguard();
        return nativeResult == -1 ? OOT_ENGINE_RESULT_NOT_AVAILABLE
                                  : OOT_ENGINE_RESULT_SCENE_LOAD_FAILED;
    }
    result = scene_geometry_copy(engine);
    engine->worldLoaded = 1u;
    engine->frameValid = 0u;
    engine_unguard();
    return result;
}

OoTResult oot_engine_scene_get_door_count(OoTEngine *engine, uint32_t *outCount)
{
    OoTResult result;
    if (outCount == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outCount = 0u;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    *outCount = (uint32_t)oot_scene_get_door_count();
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_door(OoTEngine *engine, uint32_t index,
                                    struct OoTDoor *outDoor)
{
    OoTResult result;
    if (outDoor == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_scene_get_door((int32_t)index, outDoor)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_sequence_id(OoTEngine *engine, int32_t *outSeqId)
{
    OoTResult result;
    if (outSeqId == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outSeqId = -1;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    *outSeqId = oot_scene_get_sequence_id();
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_ambience_id(OoTEngine *engine, int32_t *outAmbienceId)
{
    OoTResult result;
    if (outAmbienceId == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outAmbienceId = -1;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    *outAmbienceId = oot_scene_get_ambience_id();
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_environment(OoTEngine *engine, struct OoTSceneEnvironment *outEnv)
{
    OoTResult result;
    if (outEnv == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(outEnv, 0, sizeof(*outEnv));
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    (void)oot_scene_get_environment(outEnv);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_runtime(OoTEngine *engine,
                                       struct OoTSceneRuntime *outRuntime)
{
    OoTResult result;
    if (outRuntime == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(outRuntime, 0, sizeof(*outRuntime));
    outRuntime->structSize = sizeof(*outRuntime);
    outRuntime->version = OOT_SCENE_RUNTIME_VERSION;
    outRuntime->sceneIndex = -1;
    outRuntime->activeRoomIndex = -1;
    outRuntime->geometryRoomIndex = -1;
    outRuntime->worldMapArea = -1;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_scene_get_runtime(outRuntime)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_background_count(OoTEngine *engine,
                                                uint32_t *outCount)
{
    OoTResult result;
    if (outCount == NULL) return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    *outCount = 0u;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    if (!oot_scene_get_active_layer(NULL, NULL)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    *outCount = (uint32_t)oot_scene_get_background_count();
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_background(OoTEngine *engine, uint32_t index,
                                          struct OoTSceneBackground *outBackground)
{
    OoTResult result;
    if (outBackground == NULL ||
        outBackground->structSize < sizeof(*outBackground) ||
        outBackground->version != OOT_SCENE_BACKGROUND_VERSION)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    if (!oot_scene_get_background((int32_t)index, outBackground)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_material_state(
    OoTEngine *engine, struct OoTSceneMaterialState *outState)
{
    OoTResult result;
    if (outState == NULL || outState->structSize < sizeof(*outState) ||
        outState->version != OOT_SCENE_MATERIAL_STATE_VERSION)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    if (!oot_scene_get_material_state(outState)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_material_reference(
    OoTEngine *engine, uint32_t index,
    struct OoTSceneMaterialReference *outReference)
{
    OoTResult result;
    if (outReference == NULL ||
        outReference->structSize < sizeof(*outReference) ||
        outReference->version != OOT_SCENE_MATERIAL_REFERENCE_VERSION)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    if (!oot_scene_get_material_reference((int32_t)index, outReference)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_geometry(OoTEngine *engine,
                                        const OoTEngineSceneGeometry **outGeometry)
{
    OoTResult result;
    if (outGeometry == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outGeometry = NULL;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!engine->sceneGeometryValid) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    *outGeometry = &engine->sceneGeometry;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_dropped_triangles(
    OoTEngine *engine, uint32_t *outDroppedTriangles)
{
    OoTResult result;
    if (outDroppedTriangles == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outDroppedTriangles = 0u;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!engine->sceneGeometryValid) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    *outDroppedTriangles = engine->sceneDroppedTriangles;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_spawn(OoTEngine *engine, int32_t spawnIndex,
                                     float outPosition[3], int16_t *outYaw)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (spawnIndex < 0 || outPosition == NULL ||
        !oot_scene_spawn(spawnIndex, outPosition, outYaw)) {
        engine_unguard();
        return spawnIndex < 0 || outPosition == NULL
            ? OOT_ENGINE_RESULT_INVALID_ARGUMENT
            : OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_actor_count(OoTEngine *engine, uint32_t *outCount)
{
    OoTResult result;
    int32_t count;
    if (outCount == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outCount = 0u;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_scene_get_active_layer(NULL, NULL)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    count = oot_scene_get_actor_count();
    *outCount = (uint32_t)count;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_actor(OoTEngine *engine, uint32_t index,
                                     struct OoTSceneActorEntry *outEntry)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (outEntry == NULL || outEntry->structSize < sizeof(*outEntry) ||
        outEntry->version != OOT_SCENE_ACTOR_ENTRY_VERSION) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    if (index > INT32_MAX || !oot_scene_get_actor((int32_t)index, outEntry)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_exit_count(OoTEngine *engine,
                                          uint32_t *outCount)
{
    OoTResult result;
    int32_t count;
    if (outCount == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outCount = 0u;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_scene_get_active_layer(NULL, NULL)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    count = oot_scene_get_exit_count();
    *outCount = count > 0 ? (uint32_t)count : 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_scene_get_exit(OoTEngine *engine, uint32_t index,
                                    int16_t *outEntranceIndex)
{
    OoTResult result;
    if (outEntranceIndex == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outEntranceIndex = -1;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (index > INT32_MAX ||
        !oot_scene_get_exit((int32_t)index, outEntranceIndex)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_world_event_poll(OoTEngine *engine,
                                      struct OoTWorldEvent *outEvent)
{
    OoTResult result;
    if (outEvent == NULL || outEvent->structSize < sizeof(*outEvent) ||
        outEvent->version != OOT_WORLD_EVENT_VERSION) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_world_event_poll(outEvent)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_target_create(OoTEngine *engine, float x, float y, float z,
                                   float focusHeight, OoTEngineTarget *outTarget)
{
    OoTResult result;
    uint32_t i;
    int32_t nativeId;

    if (outTarget == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outTarget = OOT_ENGINE_INVALID_TARGET;
    result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!finite3(x, y, z) || !isfinite(focusHeight) || focusHeight < 0.0f) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    for (i = 0u; i < ENGINE_TARGET_SLOTS && engine->targets[i].active; ++i) {
    }
    if (i == ENGINE_TARGET_SLOTS) {
        engine_unguard();
        return OOT_ENGINE_RESULT_TARGET_CAPACITY;
    }
    nativeId = oot_target_create(x, y, z, focusHeight);
    if (nativeId < 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_TARGET_CAPACITY;
    }
    engine->targets[i].generation =
        liboot_core_context_next_handle_namespace();
    if (engine->targets[i].generation == 0u) {
        oot_target_remove(nativeId);
        engine_unguard();
        return OOT_ENGINE_RESULT_TARGET_CAPACITY;
    }
    engine->targets[i].nativeId = nativeId;
    engine->targets[i].active = 1u;
    *outTarget = target_handle(i, engine->targets[i].generation);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_target_move(OoTEngine *engine, OoTEngineTarget target,
                                 float x, float y, float z)
{
    OoTResult result = engine_lock_link(engine);
    EngineTargetSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!finite3(x, y, z)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    slot = target_find(engine, target);
    if (slot == NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_TARGET_NOT_FOUND;
    }
    oot_target_move(slot->nativeId, x, y, z);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_target_remove(OoTEngine *engine, OoTEngineTarget target)
{
    OoTResult result = engine_lock_link(engine);
    EngineTargetSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    slot = target_find(engine, target);
    if (slot == NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_TARGET_NOT_FOUND;
    }
    oot_target_remove(slot->nativeId);
    slot->active = 0u;
    slot->nativeId = -1;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_targets_clear(OoTEngine *engine)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    targets_invalidate(engine, 1);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_host_actor_create(OoTEngine *engine,
                                       const struct OoTHostActorState *state,
                                       OoTEngineHostActor *outActor)
{
    OoTResult result;
    uint32_t i;
    int32_t nativeId;
    if (outActor == NULL || state == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outActor = OOT_ENGINE_INVALID_HOST_ACTOR;
    result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!host_actor_state_valid(state)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    for (i = 0u; i < ENGINE_HOST_ACTOR_SLOTS && engine->hostActors[i].active; ++i) {
    }
    if (i == ENGINE_HOST_ACTOR_SLOTS) {
        engine_unguard();
        return OOT_ENGINE_RESULT_HOST_ACTOR_CAPACITY;
    }
    nativeId = oot_host_actor_create(state);
    if (nativeId < 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_HOST_ACTOR_CAPACITY;
    }
    engine->hostActors[i].generation =
        liboot_core_context_next_handle_namespace();
    if (engine->hostActors[i].generation == 0u) {
        (void)oot_host_actor_remove(nativeId);
        engine_unguard();
        return OOT_ENGINE_RESULT_HOST_ACTOR_CAPACITY;
    }
    engine->hostActors[i].nativeId = nativeId;
    engine->hostActors[i].active = 1u;
    *outActor = host_actor_handle(i, engine->hostActors[i].generation);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_host_actor_update(OoTEngine *engine,
                                       OoTEngineHostActor actor,
                                       const struct OoTHostActorState *state)
{
    OoTResult result = engine_lock_link(engine);
    EngineHostActorSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!host_actor_state_valid(state)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    slot = host_actor_find(engine, actor);
    if (slot == NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND;
    }
    if (!oot_host_actor_update(slot->nativeId, state)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_host_actor_get(OoTEngine *engine,
                                    OoTEngineHostActor actor,
                                    struct OoTHostActorState *outState)
{
    OoTResult result = engine_lock_link(engine);
    EngineHostActorSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (outState == NULL || outState->structSize < sizeof(*outState) ||
        outState->version != OOT_HOST_ACTOR_STATE_VERSION) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    slot = host_actor_find(engine, actor);
    if (slot == NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND;
    }
    if (!oot_host_actor_get(slot->nativeId, outState)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND;
    }
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_host_actor_remove(OoTEngine *engine,
                                       OoTEngineHostActor actor)
{
    OoTResult result = engine_lock_link(engine);
    EngineHostActorSlot *slot;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    slot = host_actor_find(engine, actor);
    if (slot == NULL) {
        engine_unguard();
        return OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND;
    }
    (void)oot_host_actor_remove(slot->nativeId);
    slot->active = 0u;
    slot->nativeId = -1;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_host_actors_clear(OoTEngine *engine)
{
    OoTResult result = engine_lock_link(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    host_actors_invalidate(engine, 1);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_host_actor_poll_contact(OoTEngine *engine,
                                             OoTEngineActorContact *outContact)
{
    OoTResult result = engine_lock_link(engine);
    struct OoTHostActorContact native;
    uint32_t i;
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (outContact == NULL || outContact->structSize < sizeof(*outContact) ||
        outContact->version != OOT_HOST_ACTOR_CONTACT_VERSION) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(&native, 0, sizeof(native));
    native.structSize = sizeof(native);
    native.version = OOT_HOST_ACTOR_CONTACT_VERSION;
    for (;;) {
        if (!oot_host_actor_poll_contact(&native)) {
            engine_unguard();
            return OOT_ENGINE_RESULT_NOT_AVAILABLE;
        }
        for (i = 0u; i < ENGINE_HOST_ACTOR_SLOTS; ++i) {
            if (engine->hostActors[i].active &&
                engine->hostActors[i].nativeId == native.actorId) {
                break;
            }
        }
        if (i != ENGINE_HOST_ACTOR_SLOTS) {
            break;
        }
        /* Raw and engine lifecycle calls must not be mixed, but if a stale
           native event is present, skip it instead of hiding later contacts
           for live engine handles. */
    }
    memset(outContact, 0, sizeof(*outContact));
    outContact->structSize = sizeof(*outContact);
    outContact->version = OOT_HOST_ACTOR_CONTACT_VERSION;
    outContact->actor = host_actor_handle(i, engine->hostActors[i].generation);
    outContact->source = native.source;
    outContact->userTag = native.userTag;
    outContact->sourceActorId = native.sourceActorId;
    outContact->simulationTick = (uint32_t)engine->simulationTick;
    memcpy(outContact->position, native.position, sizeof(outContact->position));
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_set_render_flags(OoTEngine *engine, uint32_t renderFlags)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if ((renderFlags & ~ENGINE_KNOWN_RENDER_FLAGS) != 0u) {
        engine_unguard();
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    oot_navi_set_render((renderFlags & OOT_ENGINE_RENDER_NAVI) != 0u);
    oot_actor_set_render((renderFlags & OOT_ENGINE_RENDER_ACTORS) != 0u);
    engine->renderFlags = renderFlags;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_get_render_flags(OoTEngine *engine, uint32_t *outRenderFlags)
{
    OoTResult result;
    if (outRenderFlags == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    *outRenderFlags = engine->renderFlags;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_texture_count(OoTEngine *engine, uint32_t *outCount)
{
    OoTResult result;
    int32_t count;
    if (outCount == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    *outCount = 0u;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    count = oot_get_texture_count();
    if (count < 0) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    *outCount = (uint32_t)count;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_texture_get(OoTEngine *engine, uint32_t index,
                                 OoTEngineTexture *outTexture)
{
    OoTResult result;
    struct OoTTextureInfo info;
    const uint8_t *pixels = NULL;
    int32_t count;

    if (outTexture == NULL || index > INT32_MAX) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(outTexture, 0, sizeof(*outTexture));
    outTexture->structSize = sizeof(*outTexture);
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    count = oot_get_texture_count();
    if (count < 0 || index >= (uint32_t)count ||
        !oot_get_texture((int32_t)index, &info, &pixels)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    outTexture->width = info.width;
    outTexture->height = info.height;
    outTexture->wrapS = info.wrapS;
    outTexture->wrapT = info.wrapT;
    outTexture->revision = info.revision;
    outTexture->rgbaPixels = pixels;
    outTexture->rgbaSize = (size_t)info.width * info.height * 4u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_voice_get(OoTEngine *engine, uint16_t sfxId, OoTEnginePcm *outPcm)
{
    OoTResult result;
    const int16_t *samples = NULL;
    uint32_t sampleCount = 0u;
    uint32_t sampleRate = 0u;

    if (outPcm == NULL) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(outPcm, 0, sizeof(*outPcm));
    outPcm->structSize = sizeof(*outPcm);
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_get_voice_sample(sfxId, &samples, &sampleCount, &sampleRate)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    outPcm->samples = samples;
    outPcm->sampleCount = sampleCount;
    outPcm->sampleRate = sampleRate;
    outPcm->loopStart = sampleCount;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_ocarina_note_get(OoTEngine *engine, uint8_t noteIndex,
                                      OoTEnginePcm *outPcm)
{
    OoTResult result;
    const int16_t *samples = NULL;
    uint32_t sampleCount = 0u;
    uint32_t sampleRate = 0u;
    uint32_t loopStart = 0u;

    if (outPcm == NULL || noteIndex > 4u) {
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    }
    memset(outPcm, 0, sizeof(*outPcm));
    outPcm->structSize = sizeof(*outPcm);
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    if (!oot_get_ocarina_note(noteIndex, &samples, &sampleCount, &sampleRate,
                              &loopStart)) {
        engine_unguard();
        return OOT_ENGINE_RESULT_NOT_AVAILABLE;
    }
    outPcm->samples = samples;
    outPcm->sampleCount = sampleCount;
    outPcm->sampleRate = sampleRate;
    outPcm->loopStart = loopStart;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

static int audio_player_valid(uint8_t player)
{
    return player < OOT_AUDIO_PLAYER_COUNT;
}

OoTResult oot_engine_audio_sequence_prewarm(OoTEngine *engine,
                                             uint16_t sequenceId)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    result = oot_audio_sequence_prewarm(sequenceId)
        ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE;
    engine_unguard();
    return result;
}

OoTResult oot_engine_audio_sequence_play(OoTEngine *engine, uint8_t player,
                                          uint16_t sequenceId,
                                          uint16_t fadeInMs)
{
    OoTResult result;
    if (!audio_player_valid(player)) return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    result = oot_audio_sequence_play(player, sequenceId, fadeInMs)
        ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE;
    engine_unguard();
    return result;
}

OoTResult oot_engine_audio_nature_play(OoTEngine *engine, uint8_t player,
                                        uint8_t ambienceId, uint16_t fadeInMs)
{
    OoTResult result;
    if (!audio_player_valid(player)) return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    result = oot_audio_nature_play(player, ambienceId, fadeInMs)
        ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE;
    engine_unguard();
    return result;
}

OoTResult oot_engine_audio_sequence_stop(OoTEngine *engine, uint8_t player,
                                          uint16_t fadeOutMs)
{
    OoTResult result;
    if (!audio_player_valid(player)) return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_sequence_stop(player, fadeOutMs);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_sequence_pause(OoTEngine *engine, uint8_t player,
                                           uint8_t paused)
{
    OoTResult result;
    if (!audio_player_valid(player)) return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_sequence_pause(player, paused != 0u);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_sequence_set_volume(OoTEngine *engine,
                                                uint8_t player, float volume)
{
    OoTResult result;
    if (!audio_player_valid(player) || !isfinite(volume) ||
        volume < 0.0f || volume > 1.0f)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_sequence_set_volume(player, volume);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_sequence_set_io(OoTEngine *engine, uint8_t player,
                                            uint8_t port, int8_t value)
{
    OoTResult result;
    if (!audio_player_valid(player) || port >= 8u)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_sequence_set_io(player, port, value);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_channel_set_io(OoTEngine *engine, uint8_t player,
                                           uint8_t channel, uint8_t port,
                                           int8_t value)
{
    OoTResult result;
    if (!audio_player_valid(player) || channel >= 16u || port >= 8u)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_channel_set_io(player, channel, port, value);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_sequence_get_state(OoTEngine *engine,
                                               uint8_t player,
                                               struct OoTAudioState *outState)
{
    OoTResult result;
    if (!audio_player_valid(player) || outState == NULL ||
        outState->structSize < sizeof(*outState) ||
        outState->version != OOT_AUDIO_STATE_VERSION)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    if (!oot_audio_sequence_get_state(player, outState))
        result = OOT_ENGINE_RESULT_NOT_AVAILABLE;
    engine_unguard();
    return result;
}

OoTResult oot_engine_audio_set_master_volume(OoTEngine *engine, float volume)
{
    OoTResult result;
    if (!isfinite(volume) || volume < 0.0f || volume > 1.0f)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_set_master_volume(volume);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_stop_all(OoTEngine *engine, uint16_t fadeOutMs)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_stop_all(fadeOutMs);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_render_f32(OoTEngine *engine, float *stereo,
                                      uint32_t frames, uint32_t sampleRate,
                                      uint32_t *outFrames)
{
    OoTResult result;
    uint32_t rendered;
    if (outFrames != NULL) *outFrames = 0u;
    if (stereo == NULL || outFrames == NULL || frames == 0u ||
        sampleRate < 8000u || sampleRate > 192000u)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    rendered = oot_audio_render_f32(stereo, frames, sampleRate);
    *outFrames = rendered;
    engine_unguard();
    return rendered == frames ? OOT_ENGINE_RESULT_OK
                              : OOT_ENGINE_RESULT_NOT_AVAILABLE;
}

OoTResult oot_engine_audio_render_s16(OoTEngine *engine, int16_t *stereo,
                                      uint32_t frames, uint32_t sampleRate,
                                      uint32_t *outFrames)
{
    OoTResult result;
    uint32_t rendered;
    if (outFrames != NULL) *outFrames = 0u;
    if (stereo == NULL || outFrames == NULL || frames == 0u ||
        sampleRate < 8000u || sampleRate > 192000u)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    rendered = oot_audio_render_s16(stereo, frames, sampleRate);
    if (rendered > 0u) *outFrames = rendered;
    engine_unguard();
    return rendered == frames ? OOT_ENGINE_RESULT_OK
                              : OOT_ENGINE_RESULT_NOT_AVAILABLE;
}

OoTResult oot_engine_audio_sfx_play(OoTEngine *engine, uint16_t sfxId,
                                     float pan, float volume)
{
    OoTResult result;
    if (!isfinite(pan) || !isfinite(volume) || pan < -1.0f || pan > 1.0f ||
        volume < 0.0f || volume > 1.0f)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    result = oot_audio_sfx_play(sfxId, pan, volume)
        ? OOT_ENGINE_RESULT_OK : OOT_ENGINE_RESULT_NOT_AVAILABLE;
    engine_unguard();
    return result;
}

OoTResult oot_engine_audio_sfx_stop(OoTEngine *engine, uint16_t sfxId)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_sfx_stop(sfxId);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_audio_sfx_stop_all(OoTEngine *engine)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    oot_audio_sfx_stop_all();
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_get_enemy_bgm(OoTEngine *engine, uint8_t *outEnabled,
                                    uint8_t *outActive, float *outDistance)
{
    OoTResult result;
    bool active = false;
    bool enabled;
    if (outEnabled == NULL) return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) return result;
    enabled = oot_audio_get_enemy_bgm(&active, outDistance);
    *outEnabled = enabled ? 1u : 0u;
    if (outActive != NULL) *outActive = active ? 1u : 0u;
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}

OoTResult oot_engine_set_enemy_bgm(OoTEngine *engine, uint8_t enabled)
{
    OoTResult result = engine_lock(engine);
    if (result != OOT_ENGINE_RESULT_OK) {
        return result;
    }
    /* 0xFF player / 0 seqId keep the defaults (OOT_AUDIO_PLAYER_SUB, NA_BGM_ENEMY). */
    oot_audio_set_enemy_bgm(enabled != 0u, 0xFFu, 0u, 400u);
    engine_unguard();
    return OOT_ENGINE_RESULT_OK;
}
