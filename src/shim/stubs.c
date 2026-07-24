#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "fake_play.h"
#include "audio_extract.h"

#include "actor.h"
#include "audio.h"
#include "bgcheck.h"
#include "camera.h"
#include "collision_check.h"
#include "draw.h"
#include "effect.h"
#include "environment.h"
#include "fault.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "horse.h"
#include "interface.h"
#include "inventory.h"
#include "letterbox.h"
#include "lifemeter.h"
#include "light.h"
#include "map.h"
#include "message.h"
#include "object.h"
#include "ocarina.h"
#include "one_point_cutscene.h"
#include "player.h"
#include "quake.h"
#include "room.h"
#include "rumble.h"
#include "scene.h"
#include "save.h"
#include "sfx.h"
#include "sfx_source.h"
#include "sys_matrix.h"
#include "z_actor_dlftbls.h"
#include "z_en_item00.h"
#include "zelda_arena.h"
#include "libu64/debug.h"
#include "libu64/overlay.h"
#include "quest_hint.h"

#ifdef __APPLE__
/*
 * The decomp runtime calls glibc's three-argument __assert entry point.
 * Darwin exposes __assert_rtn instead, so provide the expected ABI locally.
 */
#ifdef __assert
#undef __assert
#endif
__attribute__((noreturn)) void __assert(const char* assertion, const char* file, int line) {
    fprintf(stderr, "Assertion failed: %s (%s:%d)\n", assertion, file, line);
    abort();
}
#endif

/* Actor overlays are never loaded in the standalone, single-Player world. */
void ActorOverlayTable_Init(void) {}
void ActorOverlayTable_Cleanup(void) {}

/* Audio backend: silent, but every sfx request the game makes is forwarded
   to the host application so it can play extracted/synthesized sounds.
   liboot v0.2: */
static void (*sLibootSfxCb)(u16 sfxId);
static void (*sLibootSfxCbEx)(const struct OoTSfxEvent* event);
void oot_set_sfx_callback(void (*cb)(u16 sfxId)) { sLibootSfxCb = cb; }
void oot_set_sfx_callback_ex(void (*cb)(const struct OoTSfxEvent* event)) { sLibootSfxCbEx = cb; }
static void liboot_sfx_event(u16 sfxId, Vec3f* pos, u8 token, f32 freqScale, f32 volume, s8 reverb) {
    struct OoTSfxEvent event = {
        .sfxId = sfxId, .token = token, .reverb = reverb,
        .action = OOT_SFX_PLAY, .isRefresh = sfxId != 0 && (sfxId & SFX_FLAG) == 0,
        .position = { pos ? pos->x : 0.0f, pos ? pos->y : 0.0f, pos ? pos->z : 0.0f },
        .freqScale = freqScale, .volume = volume,
    };
    if (sLibootSfxCbEx != NULL) sLibootSfxCbEx(&event);
    if (sLibootSfxCb != NULL) sLibootSfxCb(sfxId);
}
static void liboot_sfx_stop(u8 action, u16 sfxId, Vec3f* pos) {
    struct OoTSfxEvent event = {
        .sfxId = sfxId, .action = action,
        .position = { pos ? pos->x : 0.0f, pos ? pos->y : 0.0f, pos ? pos->z : 0.0f },
    };
    if (sLibootSfxCbEx != NULL) sLibootSfxCbEx(&event);
}
static void liboot_sfx(u16 sfxId) { liboot_sfx_event(sfxId, NULL, 4, 1.0f, 1.0f, 0); }
void AudioOcarina_SetInstrument(u8 ocarinaInstrumentId) { (void)ocarinaInstrumentId; }
void Audio_PlayFanfare(u16 seqId) { (void)seqId; }
void Audio_PlaySfxGeneral(u16 sfxId, Vec3f* pos, u8 token, f32* freqScale, f32* vol, s8* reverbAdd) {
    liboot_sfx_event(sfxId, pos, token, freqScale ? *freqScale : 1.0f,
                     vol ? *vol : 1.0f, reverbAdd ? *reverbAdd : 0);
}
void Audio_SetBaseFilter(u8 filter) { (void)filter; }
/* liboot vNEXT: the real Player code calls this each tick a hostile enemy is
   within OoT's 500-unit battle range. Forward the distance to the opt-in
   proximity-driven battle-BGM driver (liboot_enemy_bgm_tick decides playback). */
void Audio_SetBgmEnemyVolume(f32 dist) { liboot_enemy_bgm_signal(dist); }
void Audio_SetBgmVolumeOffDuringFanfare(void) {}
void Audio_SetBgmVolumeOnDuringFanfare(void) {}
void Audio_SetCodeReverb(s8 reverb) { (void)reverb; }
void Audio_SetSequenceMode(u8 seqMode) { (void)seqMode; }
void Audio_StopBgmAndFanfare(u16 fadeOutDuration) { (void)fadeOutDuration; }
void Audio_StopSfxById(u32 sfxId) { liboot_sfx_stop(OOT_SFX_STOP_ID, (u16)sfxId, NULL); }
void Audio_StopSfxByPos(Vec3f* pos) { liboot_sfx_stop(OOT_SFX_STOP_POSITION, 0, pos); }
void SfxSource_PlaySfxAtFixedWorldPos(PlayState* play, Vec3f* worldPos, s32 duration, u16 sfxId) {
    (void)play; (void)duration;
    liboot_sfx_event(sfxId, worldPos, 4, 1.0f, 1.0f, 0);
}
static f32 liboot_step_scales(f32 speed, f32* freq, f32* volume) {
    f32 ratio = 1.0f;
    if (speed > 6.0f) {
        *volume = 1.0f;
        *freq = 1.1f;
    } else {
        ratio = speed / 6.0f;
        *volume = (ratio * 0.22500002f) + 0.775f;
        *freq = (ratio * 0.2f) + 0.9f;
    }
    return ratio;
}
void func_800F4010(Vec3f* pos, u16 sfxId, f32 speed) {
    static u32 randomState = 0x71C39E2Du;
    f32 freq, volume;
    f32 ratio = liboot_step_scales(speed, &freq, &volume);
    liboot_sfx_event(sfxId, pos, 4, freq, volume, 0);

    randomState = randomState * 1664525u + 1013904223u;
    if ((((sfxId & 0xF0) == 0xB0) && speed > 0.3f) ||
        (((sfxId & 0xF0) != 0xB0) && speed > 1.1f && ((randomState >> 31) != 0))) {
        u16 metalId = (sfxId & 0x80) ? NA_SE_PL_METALEFFECT_ADULT : NA_SE_PL_METALEFFECT_KID;
        f32 metalVolume = ((sfxId & 0xF0) == 0xB0) ? 1.0f : (ratio * 0.7f) + 0.3f;
        liboot_sfx_event(metalId, pos, 4, freq, metalVolume, 0);
    }
}
/* EnElf/Navi: play with the supplied reverb offset. */
void func_800F4524(Vec3f* pos, u16 sfxId, s8 reverb) {
    liboot_sfx_event(sfxId, pos, 4, 1.0f, 1.0f, reverb);
}
void func_800F4138(Vec3f* pos, u16 sfxId, f32 speed) {
    f32 freq, volume;
    liboot_step_scales(speed, &freq, &volume);
    liboot_sfx_event(sfxId, pos, 4, freq, volume, 0);
}
void func_800F4190(Vec3f* pos, u16 sfxId) {
    liboot_sfx_event(sfxId, pos, 4, 0.7950898f, 1.0f, 35);
}
void func_800F4C58(Vec3f* pos, u16 sfxId, u8 arg2) { (void)pos; (void)sfxId; (void)arg2; }
void func_800F6964(u16 arg0) { (void)arg0; }

/* Camera service: retain only movement modes and a caller-controlled yaw. */
static s32 Liboot_CameraModeSupported(s16 mode) {
    switch (mode) {
        case CAM_MODE_NORMAL:
        // liboot v0.3: lock-on modes accepted so the host can read camera->mode
        // while Z-targeting; nothing internal reads it back.
        case CAM_MODE_Z_TARGET_FRIENDLY:
        case CAM_MODE_Z_TARGET_UNFRIENDLY:
        case CAM_MODE_Z_PARALLEL:
        case CAM_MODE_WALL_CLIMB:
        case CAM_MODE_Z_WALL_CLIMB:
        case CAM_MODE_JUMP:
        case CAM_MODE_LEDGE_HANG:
        case CAM_MODE_Z_LEDGE_HANG:
        case CAM_MODE_FREE_FALL:
        case CAM_MODE_CHARGE:
        case CAM_MODE_STILL:
        case CAM_MODE_PUSH_PULL:
            return true;
        default:
            return false;
    }
}

s32 Camera_RequestMode(Camera* camera, s16 mode) {
    if ((camera == NULL) || !Liboot_CameraModeSupported(mode)) {
        return false;
    }
    camera->mode = mode;
    return true;
}

s32 Camera_CheckValidMode(Camera* camera, s16 mode) {
    (void)camera;
    return Liboot_CameraModeSupported(mode);
}

s32 Camera_RequestSetting(Camera* camera, s16 setting) {
    if (camera == NULL) {
        return false;
    }
    camera->prevSetting = camera->setting;
    camera->setting = setting;
    return true;
}

s16 Camera_GetInputDirYaw(Camera* camera) { (void)camera; return liboot_cam_yaw; }
s16 Camera_GetCamDirYaw(Camera* camera) { (void)camera; return liboot_cam_yaw; }

s32 Camera_SetViewParam(Camera* camera, s32 viewFlag, void* param) {
    if (camera == NULL) {
        return false;
    }
    camera->viewFlags |= viewFlag;
    if (viewFlag & CAM_VIEW_TARGET) {
        camera->target = param;
    }
    return true;
}

void Camera_SetCameraData(Camera* camera, s16 setDataFlags, void* data0, void* data1, s16 data2, s16 data3,
                          s32 data4) {
    (void)data4;
    if (camera != NULL) {
        camera->data0 = data0;
        camera->data1 = data1;
        camera->data2 = data2;
        camera->data3 = data3;
        camera->stateFlags |= setDataFlags;
    }
}

s16 Camera_SetFinishedFlag(Camera* camera) {
    if (camera != NULL) {
        camera->animState = 0;
    }
    return true;
}

s32 Camera_ChangeDoorCam(Camera* camera, Actor* doorActor, s16 bgCamIndex, f32 arg3, s16 timer1, s16 timer2,
                         s16 timer3) {
    (void)doorActor; (void)arg3; (void)timer1; (void)timer2; (void)timer3;
    if (camera != NULL) {
        camera->bgCamIndex = bgCamIndex;
    }
    return true;
}

/* Collider state is local and coherent even though registration is disabled. */
static void Liboot_SetColliderBase(Collider* dest, Actor* actor, ColliderInit* src) {
    memset(dest, 0, sizeof(*dest));
    dest->actor = actor;
    dest->atFlags = src->atFlags;
    dest->acFlags = src->acFlags;
    dest->ocFlags1 = src->ocFlags1;
    dest->ocFlags2 = src->ocFlags2;
    dest->colMaterial = src->colMaterial;
    dest->shape = src->shape;
}

static void Liboot_SetColliderElement(ColliderElement* dest, ColliderElementInit* src) {
    memset(dest, 0, sizeof(*dest));
    dest->atDmgInfo = src->atDmgInfo;
    dest->acDmgInfo.dmgFlags = src->acDmgInfo.dmgFlags;
    dest->acDmgInfo.hitBacklash = src->acDmgInfo.hitBacklash;
    dest->acDmgInfo.defense = src->acDmgInfo.defense;
    dest->elemMaterial = src->elemMaterial;
    dest->atElemFlags = src->atElemFlags;
    dest->acElemFlags = src->acElemFlags;
    dest->ocElemFlags = src->ocElemFlags;
}

s32 Collider_InitCylinder(PlayState* play, ColliderCylinder* cyl) {
    (void)play;
    if (cyl == NULL) return false;
    memset(cyl, 0, sizeof(*cyl));
    cyl->base.shape = COLSHAPE_CYLINDER;
    return true;
}

s32 Collider_DestroyCylinder(PlayState* play, ColliderCylinder* cyl) {
    (void)play;
    if (cyl != NULL) memset(cyl, 0, sizeof(*cyl));
    return true;
}

s32 Collider_SetCylinder(PlayState* play, ColliderCylinder* dest, Actor* actor, ColliderCylinderInit* src) {
    (void)play;
    if ((dest == NULL) || (src == NULL)) return false;
    Liboot_SetColliderBase(&dest->base, actor, &src->base);
    Liboot_SetColliderElement(&dest->elem, &src->elem);
    dest->dim = src->dim;
    Collider_UpdateCylinder(actor, dest);
    return true;
}

void Collider_UpdateCylinder(Actor* actor, ColliderCylinder* cyl) {
    if ((actor == NULL) || (cyl == NULL)) return;
    cyl->base.actor = actor;
    cyl->dim.pos.x = actor->world.pos.x;
    cyl->dim.pos.y = actor->world.pos.y;
    cyl->dim.pos.z = actor->world.pos.z;
}

s32 Collider_ResetCylinderAC(PlayState* play, Collider* col) {
    ColliderCylinder* cyl = (ColliderCylinder*)col;
    (void)play;
    if (cyl == NULL) return false;
    cyl->base.ac = NULL;
    cyl->base.acFlags &= ~AC_HIT;
    cyl->elem.acHit = NULL;
    cyl->elem.acHitElem = NULL;
    cyl->elem.acElemFlags &= ~(ACELEM_HIT | ACELEM_DRAW_HITMARK);
    return true;
}

/* liboot v0.6 (EnBom): the JntSph quartet is "minimal-real" — EnBom writes
   elements[0].base.atDmgInfo.damage right after SetJntSph and reads/writes
   dim.modelSphere/worldSphere every explosion frame, so dest->elements MUST
   point at the caller's element array with dims copied (a pure no-op
   segfaults). Registration stays disabled like every other collider. */
s32 Collider_InitJntSph(PlayState* play, ColliderJntSph* jntSph) {
    (void)play;
    if (jntSph == NULL) return false;
    memset(jntSph, 0, sizeof(*jntSph));
    jntSph->base.shape = COLSHAPE_JNTSPH;
    return true;
}

s32 Collider_DestroyJntSph(PlayState* play, ColliderJntSph* jntSph) {
    (void)play;
    if (jntSph != NULL) memset(jntSph, 0, sizeof(*jntSph));
    return true;
}

s32 Collider_SetJntSph(PlayState* play, ColliderJntSph* dest, Actor* actor, ColliderJntSphInit* src,
                       ColliderJntSphElement* elements) {
    s32 i;
    (void)play;
    if ((dest == NULL) || (src == NULL) || (elements == NULL)) return false;
    Liboot_SetColliderBase(&dest->base, actor, &src->base);
    dest->count = src->count;
    dest->elements = elements;
    for (i = 0; i < src->count; i++) {
        ColliderJntSphElement* destElem = &dest->elements[i];
        ColliderJntSphElementInit* srcElem = &src->elements[i];
        memset(destElem, 0, sizeof(*destElem));
        Liboot_SetColliderElement(&destElem->base, &srcElem->base);
        destElem->dim.limb = srcElem->dim.limb;
        destElem->dim.modelSphere = srcElem->dim.modelSphere;
        destElem->dim.scale = srcElem->dim.scale * 0.01f;
        destElem->dim.worldSphere.center.x = actor != NULL ? (s16)actor->world.pos.x : 0;
        destElem->dim.worldSphere.center.y = actor != NULL ? (s16)actor->world.pos.y : 0;
        destElem->dim.worldSphere.center.z = actor != NULL ? (s16)actor->world.pos.z : 0;
        destElem->dim.worldSphere.radius = (s16)(destElem->dim.modelSphere.radius * destElem->dim.scale);
    }
    return true;
}

/* limb-matrix world-sphere refresh: only runs from real draw passes, which
   liboot replays without matrix state for colliders — keep spheres as set. */
void Collider_UpdateSpheres(s32 limb, ColliderJntSph* jntSph) { (void)limb; (void)jntSph; }

s32 Collider_InitQuad(PlayState* play, ColliderQuad* quad) {
    (void)play;
    if (quad == NULL) return false;
    memset(quad, 0, sizeof(*quad));
    quad->base.shape = COLSHAPE_QUAD;
    quad->dim.acDistSq = FLT_MAX;
    return true;
}

s32 Collider_DestroyQuad(PlayState* play, ColliderQuad* quad) {
    (void)play;
    if (quad != NULL) memset(quad, 0, sizeof(*quad));
    return true;
}

s32 Collider_SetQuad(PlayState* play, ColliderQuad* dest, Actor* actor, ColliderQuadInit* src) {
    (void)play;
    if ((dest == NULL) || (src == NULL)) return false;
    Liboot_SetColliderBase(&dest->base, actor, &src->base);
    Liboot_SetColliderElement(&dest->elem, &src->elem);
    Collider_SetQuadVertices(dest, &src->dim.quad[0], &src->dim.quad[1], &src->dim.quad[2], &src->dim.quad[3]);
    return true;
}

void Collider_SetQuadVertices(ColliderQuad* quad, Vec3f* a, Vec3f* b, Vec3f* c, Vec3f* d) {
    if ((quad == NULL) || (a == NULL) || (b == NULL) || (c == NULL) || (d == NULL)) return;
    quad->dim.quad[0] = *a;
    quad->dim.quad[1] = *b;
    quad->dim.quad[2] = *c;
    quad->dim.quad[3] = *d;
    quad->dim.dcMid.x = (s16)((d->x + c->x) * 0.5f);
    quad->dim.dcMid.y = (s16)((d->y + c->y) * 0.5f);
    quad->dim.dcMid.z = (s16)((d->z + c->z) * 0.5f);
    quad->dim.baMid.x = (s16)((b->x + a->x) * 0.5f);
    quad->dim.baMid.y = (s16)((b->y + a->y) * 0.5f);
    quad->dim.baMid.z = (s16)((b->z + a->z) * 0.5f);
    quad->dim.acDistSq = FLT_MAX;
}

s32 Collider_ResetQuadAC(PlayState* play, Collider* col) {
    ColliderQuad* quad = (ColliderQuad*)col;
    (void)play;
    if (quad == NULL) return false;
    quad->base.ac = NULL;
    quad->base.acFlags &= ~AC_HIT;
    quad->elem.acHit = NULL;
    quad->elem.acHitElem = NULL;
    quad->elem.acElemFlags &= ~(ACELEM_HIT | ACELEM_DRAW_HITMARK);
    quad->dim.acDistSq = FLT_MAX;
    return true;
}

s32 Collider_ResetQuadAT(PlayState* play, Collider* col) {
    ColliderQuad* quad = (ColliderQuad*)col;
    (void)play;
    if (quad == NULL) return false;
    quad->base.at = NULL;
    quad->base.atFlags &= ~(AT_HIT | AT_BOUNCED);
    quad->elem.atHit = NULL;
    quad->elem.atHitElem = NULL;
    quad->elem.atElemFlags &= ~(ATELEM_HIT | ATELEM_DREW_HITMARK);
    return true;
}

void CollisionCheck_ClearContext(PlayState* play, CollisionCheckContext* colChkCtx) {
    (void)play;
    if (colChkCtx != NULL) memset(colChkCtx, 0, sizeof(*colChkCtx));
}

void CollisionCheck_InitInfo(CollisionCheckInfo* info) {
    if (info == NULL) return;
    memset(info, 0, sizeof(*info));
    info->health = 1;
    info->mass = MASS_IMMOVABLE;
}

void CollisionCheck_ResetDamage(CollisionCheckInfo* info) {
    if (info == NULL) return;
    info->damage = 0;
    info->damageReaction = HIT_SPECIAL_EFFECT_NONE;
    info->atHitBacklash = HIT_BACKLASH_NONE;
    info->acHitSpecialEffect = HIT_SPECIAL_EFFECT_NONE;
}

s32 CollisionCheck_SetAC(PlayState* play, CollisionCheckContext* colChkCtx, Collider* collider) {
    (void)play; (void)colChkCtx; (void)collider; return false;
}
s32 CollisionCheck_SetAT(PlayState* play, CollisionCheckContext* colChkCtx, Collider* collider) {
    (void)play; (void)colChkCtx; (void)collider; return false;
}
s32 CollisionCheck_SetOC(PlayState* play, CollisionCheckContext* colChkCtx, Collider* collider) {
    (void)play; (void)colChkCtx; (void)collider; return false;
}

void CollisionCheck_BlueBlood(PlayState* play, Collider* collider, Vec3f* v) {
    (void)play; (void)collider; (void)v;
}
void CollisionCheck_SpawnShieldParticles(PlayState* play, Vec3f* v) { (void)play; (void)v; }
void CollisionCheck_SpawnShieldParticlesMetal(PlayState* play, Vec3f* v) { (void)play; (void)v; }
void CollisionCheck_SpawnShieldParticlesWood(PlayState* play, Vec3f* v, Vec3f* actorPos) {
    (void)play; (void)v; (void)actorPos;
}

s32 CollisionCheck_CylSideVsLineSeg(f32 radius, f32 height, f32 offset, Vec3f* actorPos, Vec3f* itemPos,
                                    Vec3f* itemProjPos, Vec3f* out1, Vec3f* out2) {
    (void)radius; (void)height; (void)offset; (void)actorPos; (void)itemPos; (void)itemProjPos;
    if (out1 != NULL) memset(out1, 0, sizeof(*out1));
    if (out2 != NULL) memset(out2, 0, sizeof(*out2));
    return false;
}

/* No dynamic collision actors exist in the imported static scene. */
s32 DynaPolyActor_TransformCarriedActor(CollisionContext* colCtx, s32 bgId, Actor* carriedActor) {
    (void)colCtx; (void)bgId; (void)carriedActor; return false;
}
void DynaPolyActor_UnsetAllInteractFlags(DynaPolyActor* dynaActor) { (void)dynaActor; }
void DynaPoly_SetPlayerAbove(CollisionContext* colCtx, s32 floorBgId) { (void)colCtx; (void)floorBgId; }
void DynaPoly_SetPlayerOnTop(CollisionContext* colCtx, s32 floorBgId) { (void)colCtx; (void)floorBgId; }
void func_80043334(CollisionContext* colCtx, Actor* actor, s32 bgId) { (void)colCtx; (void)actor; (void)bgId; }

/* Effects are deliberately absent. */
void EffectBlure_AddVertex(EffectBlure* this, Vec3f* p1, Vec3f* p2) { (void)this; (void)p1; (void)p2; }
void Effect_Add(PlayState* play, s32* pIndex, s32 type, u8 arg3, u8 arg4, void* initParams) {
    (void)play; (void)type; (void)arg3; (void)arg4; (void)initParams;
    if (pIndex != NULL) *pIndex = -1;
}
void Effect_Delete(PlayState* play, s32 index) { (void)play; (void)index; }
void Effect_DrawAll(GraphicsContext* gfxCtx) { (void)gfxCtx; }
void* Effect_GetByIndex(s32 index) { (void)index; return NULL; }
PlayState* Effect_GetPlayState(void) { return liboot_get_fake_play_state(); }
void EffectSs_DrawAll(PlayState* play) { (void)play; }
void EffectSsBlast_SpawnWhiteShockwave(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel) {
    (void)play; (void)pos; (void)velocity; (void)accel;
}
void EffectSsBubble_Spawn(PlayState* play, Vec3f* pos, f32 yPosOffset, f32 yPosRandScale, f32 xzPosRandScale,
                          f32 scale) {
    (void)play; (void)pos; (void)yPosOffset; (void)yPosRandScale; (void)xzPosRandScale; (void)scale;
}
void EffectSsFhgFlash_SpawnShock(PlayState* play, Actor* actor, Vec3f* pos, s16 scale, u8 param) {
    (void)play; (void)actor; (void)pos; (void)scale; (void)param;
}
void EffectSsFireTail_SpawnFlameOnPlayer(PlayState* play, f32 scale, s16 bodyPart, f32 colorIntensity) {
    (void)play; (void)scale; (void)bodyPart; (void)colorIntensity;
}
void EffectSsGFire_Spawn(PlayState* play, Vec3f* pos) { (void)play; (void)pos; }
void EffectSsGRipple_Spawn(PlayState* play, Vec3f* pos, s16 radius, s16 radiusMax, s16 life) {
    (void)play; (void)pos; (void)radius; (void)radiusMax; (void)life;
}
void EffectSsGSplash_Spawn(PlayState* play, Vec3f* pos, Color_RGBA8* primColor, Color_RGBA8* envColor, s16 type,
                           s16 scale) {
    (void)play; (void)pos; (void)primColor; (void)envColor; (void)type; (void)scale;
}
void EffectSsIcePiece_SpawnBurst(PlayState* play, Vec3f* refPos, f32 scale) { (void)play; (void)refPos; (void)scale; }
void EffectSsKiraKira_SpawnDispersed(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel,
                                     Color_RGBA8* primColor, Color_RGBA8* envColor, s16 scale, s32 life) {
    (void)play; (void)pos; (void)velocity; (void)accel; (void)primColor; (void)envColor; (void)scale; (void)life;
}
void EffectSsKiraKira_SpawnSmall(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel,
                                 Color_RGBA8* primColor, Color_RGBA8* envColor) {
    (void)play; (void)pos; (void)velocity; (void)accel; (void)primColor; (void)envColor;
}
void EffectSsStick_Spawn(PlayState* play, Vec3f* pos, s16 yaw) { (void)play; (void)pos; (void)yaw; }
/* liboot v0.6 (projectiles): visual-only soft-particle spawners stay absent. */
void EffectSsBomb2_SpawnLayered(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel, s16 scale,
                                s16 scaleStep) {
    (void)play; (void)pos; (void)velocity; (void)accel; (void)scale; (void)scaleStep;
}
void EffectSsGSpk_SpawnFuse(PlayState* play, Actor* actor, Vec3f* pos, Vec3f* velocity, Vec3f* accel) {
    (void)play; (void)actor; (void)pos; (void)velocity; (void)accel;
}
void EffectSsHitMark_SpawnCustomScale(PlayState* play, s32 type, s16 scale, Vec3f* pos) {
    (void)play; (void)type; (void)scale; (void)pos;
}
void EffectSsStone1_Spawn(PlayState* play, Vec3f* pos, s32 arg2) { (void)play; (void)pos; (void)arg2; }
/* "dead sound" = a positional sfx carried by an invisible particle; route the
   id straight to the host sfx callback (EnBom underwater defuse etc.). */
void EffectSsDeadSound_SpawnStationary(PlayState* play, Vec3f* pos, u16 sfxId, s16 lowerPriority, s16 repeatMode,
                                       s32 life) {
    (void)lowerPriority; (void)repeatMode; (void)life;
    SfxSource_PlaySfxAtFixedWorldPos(play, pos, 0, sfxId);
}
void func_80026400(PlayState* play, Color_RGBA8* color, s16 arg2, s16 arg3) {
    (void)play; (void)color; (void)arg2; (void)arg3;
}
void func_80026608(PlayState* play) { (void)play; }
void func_80026860(PlayState* play, Color_RGBA8* color, s16 arg2, s16 arg3) {
    (void)play; (void)color; (void)arg2; (void)arg3;
}
void func_80026A6C(PlayState* play) { (void)play; }
void func_8002836C(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel, Color_RGBA8* primColor,
                   Color_RGBA8* envColor, s16 scale, s16 scaleStep, s16 life) {
    (void)play; (void)pos; (void)velocity; (void)accel; (void)primColor; (void)envColor;
    (void)scale; (void)scaleStep; (void)life;
}
/* liboot v0.6 (EnBom fuse dust) */
void func_8002829C(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel, Color_RGBA8* primColor,
                   Color_RGBA8* envColor, s16 scale, s16 scaleStep) {
    (void)play; (void)pos; (void)velocity; (void)accel; (void)primColor; (void)envColor;
    (void)scale; (void)scaleStep;
}
void func_8002857C(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel) {
    (void)play; (void)pos; (void)velocity; (void)accel;
}
void func_8002865C(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel, s16 scale, s16 scaleStep) {
    (void)play; (void)pos; (void)velocity; (void)accel; (void)scale; (void)scaleStep;
}
void func_800286CC(PlayState* play, Vec3f* pos, Vec3f* velocity, Vec3f* accel, s16 scale, s16 scaleStep) {
    (void)play; (void)pos; (void)velocity; (void)accel; (void)scale; (void)scaleStep;
}

/* Message, cutscene, transition, room, and respawn guards. */
u8 Message_GetState(MessageContext* msgCtx) {
    // liboot v0.2: preserve the one closing state Player waits on.
    return ((msgCtx != NULL) && (msgCtx->msgMode == MSGMODE_TEXT_CLOSING)) ? TEXT_STATE_CLOSING : TEXT_STATE_NONE;
}
u8 Message_ShouldAdvance(PlayState* play) { (void)play; return false; }
void Message_CloseTextbox(PlayState* play) { (void)play; } /* liboot v0.4 (EnElf) */
void Message_StartTextbox(PlayState* play, u16 textId, Actor* actor) { (void)play; (void)textId; (void)actor; }
void Message_ContinueTextbox(PlayState* play, u16 textId) { (void)play; (void)textId; }
void Message_StartOcarina(PlayState* play, u16 ocarinaActionId) {
    // liboot v0.2: model the persistent free-play state used by Player's ocarina action.
    play->msgCtx.ocarinaAction = ocarinaActionId;
    play->msgCtx.msgMode = MSGMODE_OCARINA_PLAYING;
    if (play->msgCtx.ocarinaMode != OCARINA_MODE_04) {
        play->msgCtx.ocarinaMode = OCARINA_MODE_01;
    }
}
void liboot_message_prepare_ocarina(PlayState* play) {
    // liboot v0.2: clear a previous completed session before taking the instrument out again.
    play->msgCtx.msgMode = MSGMODE_NONE;
    play->msgCtx.ocarinaMode = OCARINA_MODE_00;
}
void liboot_message_stop_ocarina(PlayState* play) {
    // liboot v0.2: Player_Action_8084E3C4 consumes mode 4 and restores normal control.
    play->msgCtx.msgMode = MSGMODE_NONE;
    play->msgCtx.ocarinaMode = OCARINA_MODE_04;
}
s16 OnePointCutscene_Init(PlayState* play, s16 csId, s16 timer, Actor* actor, s16 parentCamId) {
    (void)play; (void)csId; (void)timer; (void)actor; (void)parentCamId; return CAM_ID_NONE;
}
s16 OnePointCutscene_EndCutscene(PlayState* play, s16 subCamId) { (void)play; (void)subCamId; return CAM_ID_NONE; }
s32 Room_RequestNewRoom(PlayState* play, RoomContext* roomCtx, s32 roomNum) {
    (void)play; (void)roomCtx; (void)roomNum; return false;
}
void Room_FinishRoomChange(PlayState* play, RoomContext* roomCtx) { (void)play; (void)roomCtx; }
void Scene_SetTransitionForNextEntrance(PlayState* play) { (void)play; }
void Environment_WarpSongLeave(PlayState* play) { (void)play; }
void Map_SavePlayerInitialInfo(PlayState* play) { (void)play; }
void Play_SaveSceneFlags(PlayState* play) { (void)play; }
void Play_SetupRespawnPoint(PlayState* play, s32 respawnMode, s32 playerParams) {
    (void)play; (void)respawnMode; (void)playerParams;
}
void Play_TriggerRespawn(PlayState* play) { (void)play; liboot_reset_requested = true; }
void Play_TriggerVoidOut(PlayState* play) { (void)play; liboot_reset_requested = true; }
void Letterbox_SetSizeTarget(s32 target) { (void)target; }

/* HUD, inventory, save, magic, health, and object services. */
void Interface_ChangeHudVisibilityMode(u16 hudVisibilityMode) { (void)hudVisibilityMode; }
void Interface_SetDoAction(PlayState* play, u16 action) { (void)play; (void)action; }
void Interface_SetNaviCall(PlayState* play, u16 naviCallState) { (void)play; (void)naviCallState; }
void Interface_SetSubTimerToFinalSecond(PlayState* play) { (void)play; }
void func_800849EC(PlayState* play) { (void)play; }
void Inventory_ChangeAmmo(s16 item, s16 ammoChange) { (void)item; (void)ammoChange; }
void Inventory_ChangeEquipment(s16 equipment, u16 value) { (void)equipment; (void)value; }
s32 Inventory_ConsumeFairy(PlayState* play) { (void)play; return false; }
u8 Inventory_DeleteEquipment(PlayState* play, s16 equipment) { (void)play; (void)equipment; return ITEM_NONE; }
void Inventory_UpdateBottleItem(PlayState* play, u8 item, u8 button) { (void)play; (void)item; (void)button; }
u8 Item_CheckObtainability(u8 item) { (void)item; return ITEM_NONE; }
EnItem00* Item_DropCollectible(PlayState* play, Vec3f* spawnPos, s16 params) {
    (void)play; (void)spawnPos; (void)params; return NULL;
}
u8 Item_Give(PlayState* play, u8 item) { (void)play; (void)item; return ITEM_NONE; }
void GetItem_Draw(PlayState* play, s16 giDrawId) { (void)play; (void)giDrawId; }
void Magic_Fill(PlayState* play) { (void)play; gSaveContext.save.info.playerData.magic = 0; }
s32 Magic_RequestChange(PlayState* play, s16 amount, s16 type) {
    (void)play; (void)amount; (void)type; gSaveContext.magicState = MAGIC_STATE_IDLE; return false;
}
void Magic_Reset(PlayState* play) {
    (void)play; gSaveContext.save.info.playerData.magic = 0; gSaveContext.magicState = MAGIC_STATE_IDLE;
}
s32 Health_ChangeBy(PlayState* play, s16 amount) {
    s32 health = gSaveContext.save.info.playerData.health + amount;
    s32 capacity = gSaveContext.save.info.playerData.healthCapacity;
    (void)play;
    if (health < 0) health = 0;
    if (health > capacity) health = capacity;
    gSaveContext.save.info.playerData.health = health;
    if (health == 0) liboot_reset_requested = true;
    return health != 0;
}
u32 Health_IsCritical(void) { return false; }
void Rupees_ChangeBy(s16 rupeeChange) { (void)rupeeChange; }
s32 Object_GetSlot(ObjectContext* objectCtx, s16 objectId) {
    /* liboot v0.2: slot 0 holds whichever link object is active (adult or
       child). liboot v0.4: scan every populated slot — slot 1 carries
       gameplay_keep for EnElf/Navi. */
    s32 i;
    if (objectCtx == NULL) return -1;
    for (i = 0; i < objectCtx->numEntries; i++) {
        if (objectCtx->slots[i].id == objectId) return i;
    }
    return -1;
}
s32 Object_IsLoaded(ObjectContext* objectCtx, s32 slot) {
    return (objectCtx != NULL) && (slot >= 0) && (slot < objectCtx->numEntries) &&
           (objectCtx->slots[slot].segment != NULL);
}
void Horse_InitPlayerHorse(PlayState* play, Player* player) { (void)play; (void)player; }

/* Environment, quake, rumble, and lighting. */
void Environment_ChangeLightSetting(PlayState* play, u32 lightSetting) { (void)play; (void)lightSetting; }
/* liboot v0.4 (EnElf/Navi): no environment lighting pipeline to adjust. */
void Environment_AdjustLights(PlayState* play, f32 arg1, f32 arg2, f32 arg3, f32 arg4) {
    (void)play; (void)arg1; (void)arg2; (void)arg3; (void)arg4;
}
/* cutscene-cue weighting only; unreachable while csCtx.state stays 0 */
f32 Environment_LerpWeight(u16 max, u16 min, u16 val) { (void)max; (void)min; (void)val; return 0.0f; }
s32 Play_CheckViewpoint(PlayState* play, s16 viewpoint) { (void)play; (void)viewpoint; return 0; }
int Play_InCsMode(PlayState* play) { (void)play; return 0; }
u16 QuestHint_GetNaviTextId(PlayState* play) { (void)play; return 0; }
u16 QuestHint_GetSariaTextId(PlayState* play) { (void)play; return 0; }
s16 Quake_Request(Camera* camera, u32 type) { (void)camera; (void)type; return -1; }
/* liboot v0.6 (EnBom explosion): no camera shake system to drive. */
s32 Camera_RequestQuake(Camera* camera, s32 unused, s16 y, s32 duration) {
    (void)camera; (void)unused; (void)y; (void)duration; return 0;
}
u32 Quake_SetDuration(s16 index, s16 duration) { (void)index; (void)duration; return false; }
u32 Quake_SetPerturbations(s16 index, s16 y, s16 x, s16 fov, s16 roll) {
    (void)index; (void)y; (void)x; (void)fov; (void)roll; return false;
}
u32 Quake_SetSpeed(s16 index, s16 speed) { (void)index; (void)speed; return false; }
void Rumble_Request(f32 distSq, u8 sourceStrength, u8 duration, u8 decreaseRate) {
    (void)distSq; (void)sourceStrength; (void)duration; (void)decreaseRate;
}

static Lights sLightsPool[8];
static LightNode sLightNodePool[32];
static u32 sLightsPoolIndex;
static u32 sLightNodePoolIndex;

void Lights_PointNoGlowSetInfo(LightInfo* info, s16 x, s16 y, s16 z, u8 r, u8 g, u8 b, s16 radius) {
    if (info == NULL) return;
    memset(info, 0, sizeof(*info));
    info->type = LIGHT_POINT_NOGLOW;
    info->params.point.x = x; info->params.point.y = y; info->params.point.z = z;
    info->params.point.color[0] = r; info->params.point.color[1] = g; info->params.point.color[2] = b;
    info->params.point.radius = radius;
}

/* liboot v0.4 (EnElf/Navi): identical bookkeeping, glow variant. */
void Lights_PointGlowSetInfo(LightInfo* info, s16 x, s16 y, s16 z, u8 r, u8 g, u8 b, s16 radius) {
    if (info == NULL) return;
    memset(info, 0, sizeof(*info));
    info->type = LIGHT_POINT_GLOW;
    info->params.point.x = x; info->params.point.y = y; info->params.point.z = z;
    info->params.point.color[0] = r; info->params.point.color[1] = g; info->params.point.color[2] = b;
    info->params.point.radius = radius;
}

Lights* LightContext_NewLights(LightContext* lightCtx, GraphicsContext* gfxCtx) {
    Lights* lights = &sLightsPool[sLightsPoolIndex++ % 8];
    (void)gfxCtx;
    memset(lights, 0, sizeof(*lights));
    if (lightCtx != NULL) {
        lights->l.a.l.col[0] = lights->l.a.l.colc[0] = lightCtx->ambientColor[0];
        lights->l.a.l.col[1] = lights->l.a.l.colc[1] = lightCtx->ambientColor[1];
        lights->l.a.l.col[2] = lights->l.a.l.colc[2] = lightCtx->ambientColor[2];
    }
    return lights;
}

LightNode* LightContext_InsertLight(PlayState* play, LightContext* lightCtx, LightInfo* info) {
    LightNode* node;
    (void)play;
    if ((lightCtx == NULL) || (info == NULL)) return NULL;
    node = &sLightNodePool[sLightNodePoolIndex++ % 32];
    memset(node, 0, sizeof(*node));
    node->info = info;
    node->next = lightCtx->listHead;
    if (node->next != NULL) node->next->prev = node;
    lightCtx->listHead = node;
    return node;
}

void LightContext_RemoveLight(PlayState* play, LightContext* lightCtx, LightNode* node) {
    (void)play;
    if ((lightCtx == NULL) || (node == NULL)) return;
    if (node->prev != NULL) node->prev->next = node->next;
    else if (lightCtx->listHead == node) lightCtx->listHead = node->next;
    if (node->next != NULL) node->next->prev = node->prev;
    node->prev = node->next = NULL;
}

void Lights_BindAll(Lights* lights, LightNode* listHead, Vec3f* vec) { (void)lights; (void)listHead; (void)vec; }
void Lights_Draw(Lights* lights, GraphicsContext* gfxCtx) { (void)lights; (void)gfxCtx; }
void Lights_DrawGlow(PlayState* play) { (void)play; }

/* Rendering setup is a state no-op; returned lists/pointers remain valid. */
static Gfx sNoopDisplayList[] = { { .words = { (u32)G_ENDDL << 24, 0 } } };

Gfx* Gfx_SetFog2(Gfx* gfx, s32 r, s32 g, s32 b, s32 a, s32 near, s32 far) {
    (void)r; (void)g; (void)b; (void)a; (void)near; (void)far; return gfx;
}
Gfx* Gfx_SetupDL(Gfx* gfx, u32 i) { (void)i; return gfx; }
Gfx* Gfx_SetupDL_52NoCD(Gfx* gfx) { return gfx; }
void Gfx_SetupDL_25Opa(GraphicsContext* gfxCtx) { (void)gfxCtx; }
void Gfx_SetupDL_25Xlu(GraphicsContext* gfxCtx) { (void)gfxCtx; }
void Gfx_SetupDL_27Xlu(GraphicsContext* gfxCtx) { (void)gfxCtx; } /* liboot v0.4 (EnElf) */
void Gfx_SetupDL_25Xlu2(GraphicsContext* gfxCtx) { (void)gfxCtx; } /* liboot v0.6 (EnArrow) */
void func_80093C80(PlayState* play) { (void)play; }
Gfx* Gfx_TwoTexScroll(GraphicsContext* gfxCtx, s32 tile1, u32 x1, u32 y1, s32 width1, s32 height1, s32 tile2,
                      u32 x2, u32 y2, s32 width2, s32 height2) {
    (void)gfxCtx; (void)tile1; (void)x1; (void)y1; (void)width1; (void)height1;
    (void)tile2; (void)x2; (void)y2; (void)width2; (void)height2;
    return sNoopDisplayList;
}
Gfx* Play_SetFog(PlayState* play, Gfx* gfx) { (void)play; return gfx; }

f32 func_800BFCB8(PlayState* play, MtxF* mf, Vec3f* pos) {
    (void)play; (void)mf; (void)pos; return 0.0f;
}

/* Host matrix constructors used by retained get-item/hilite paths. */
static f32 Liboot_Dot3(f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz) {
    return ax * bx + ay * by + az * bz;
}

static void Liboot_Normalize3(f32* x, f32* y, f32* z) {
    f32 length = sqrtf(*x * *x + *y * *y + *z * *z);
    if (length > 0.000001f) {
        *x /= length; *y /= length; *z /= length;
    }
}

void guLookAt(Mtx* m, f32 xEye, f32 yEye, f32 zEye, f32 xAt, f32 yAt, f32 zAt, f32 xUp, f32 yUp, f32 zUp) {
    f32 lookX = xAt - xEye, lookY = yAt - yEye, lookZ = zAt - zEye;
    f32 rightX, rightY, rightZ, realUpX, realUpY, realUpZ;
    f32 mf[4][4] = { { 0 } };

    Liboot_Normalize3(&lookX, &lookY, &lookZ);
    rightX = lookY * zUp - lookZ * yUp;
    rightY = lookZ * xUp - lookX * zUp;
    rightZ = lookX * yUp - lookY * xUp;
    Liboot_Normalize3(&rightX, &rightY, &rightZ);
    realUpX = rightY * lookZ - rightZ * lookY;
    realUpY = rightZ * lookX - rightX * lookZ;
    realUpZ = rightX * lookY - rightY * lookX;

    mf[0][0] = rightX; mf[1][0] = rightY; mf[2][0] = rightZ;
    mf[0][1] = realUpX; mf[1][1] = realUpY; mf[2][1] = realUpZ;
    mf[0][2] = -lookX; mf[1][2] = -lookY; mf[2][2] = -lookZ;
    mf[3][0] = -Liboot_Dot3(xEye, yEye, zEye, rightX, rightY, rightZ);
    mf[3][1] = -Liboot_Dot3(xEye, yEye, zEye, realUpX, realUpY, realUpZ);
    mf[3][2] = Liboot_Dot3(xEye, yEye, zEye, lookX, lookY, lookZ);
    mf[3][3] = 1.0f;
    guMtxF2L(mf, m);
}

void guLookAtHilite(Mtx* m, LookAt* l, Hilite* h, f32 xEye, f32 yEye, f32 zEye, f32 xAt, f32 yAt, f32 zAt,
                    f32 xUp, f32 yUp, f32 zUp, f32 xl1, f32 yl1, f32 zl1, f32 xl2, f32 yl2, f32 zl2,
                    s32 hiliteWidth, s32 hiliteHeight) {
    (void)xl1; (void)yl1; (void)zl1; (void)xl2; (void)yl2; (void)zl2;
    guLookAt(m, xEye, yEye, zEye, xAt, yAt, zAt, xUp, yUp, zUp);
    if (l != NULL) memset(l, 0, sizeof(*l));
    if (h != NULL) {
        h->h.x1 = h->h.x2 = hiliteWidth / 2;
        h->h.y1 = h->h.y2 = hiliteHeight / 2;
    }
}

void guPerspective(Mtx* m, u16* perspNorm, f32 fovy, f32 aspect, f32 near, f32 far, f32 scale) {
    f32 mf[4][4] = { { 0 } };
    f32 cot = cosf(fovy * (f32)M_PI / 360.0f) / sinf(fovy * (f32)M_PI / 360.0f);
    f32 sum = near + far;

    mf[0][0] = cot * scale / aspect;
    mf[1][1] = cot * scale;
    mf[2][2] = sum * scale / (near - far);
    mf[2][3] = -scale;
    mf[3][2] = 2.0f * near * far * scale / (near - far);
    if (perspNorm != NULL) {
        *perspNorm = (sum <= 2.0f) ? 0xFFFF : (u16)fminf(0xFFFF, 131072.0f / sum);
    }
    guMtxF2L(mf, m);
}

/* Allocation, diagnostics, and unsupported overlay loading. */
static void* Liboot_CheckedAlloc(u32 size) {
    void* ptr = malloc(size != 0 ? size : 1);
    if (ptr == NULL) {
        fprintf(stderr, "liboot: ZeldaArena allocation of %u bytes failed\n", size);
        abort();
    }
    return ptr;
}

void* ZeldaArena_Malloc(u32 size) { return Liboot_CheckedAlloc(size); }
void* ZeldaArena_MallocR(u32 size) { return Liboot_CheckedAlloc(size); }
void ZeldaArena_Free(void* ptr) { free(ptr); }

void LogUtils_HungupThread(const char* name, int line) {
    fprintf(stderr, "liboot assertion failed at %s:%d\n", name != NULL ? name : "?", line);
    abort();
}
void Fault_AddClient(FaultClient* client, void* callback, void* arg0, void* arg1) {
    (void)client; (void)callback; (void)arg0; (void)arg1;
}
void Fault_RemoveClient(FaultClient* client) { (void)client; }
void Fault_SetCursor(s32 x, s32 y) { (void)x; (void)y; }
s32 Fault_Printf(const char* fmt, ...) {
    s32 result;
    va_list args;
    va_start(args, fmt);
    result = vfprintf(stderr, fmt, args);
    va_end(args);
    return result;
}

size_t Overlay_Load(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd,
                    void* allocatedRamAddr) {
    (void)vromStart; (void)vromEnd; (void)vramStart; (void)vramEnd; (void)allocatedRamAddr;
    return 0;
}

/*
 * The public loader and asset adapter are landing independently of this shim
 * pass.  Weak fallbacks keep this standalone link closed while allowing the
 * real implementations to replace them without a duplicate-symbol conflict.
 */
__attribute__((weak)) bool liboot_bind_assets(void) { return false; }
__attribute__((weak)) bool liboot_link_skeleton_valid(u8 age) { (void)age; return false; }
__attribute__((weak)) void liboot_link_init(PlayState* play, Player* player, float x, float y, float z) {
    (void)play; (void)player; (void)x; (void)y; (void)z;
}
__attribute__((weak)) void liboot_link_update(PlayState* play, Player* player) { (void)play; (void)player; }
