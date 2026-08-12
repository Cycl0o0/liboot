/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include "liboot.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ultra64.h"
#include "actor.h"
#include "bgcheck.h"
#include "play_state.h"
#include "skin_matrix.h"

#define DYNAMIC_HANDLE_INDEX_MASK 0xFFu
#define DYNAMIC_KNOWN_FLAGS \
    ((uint32_t)(OOT_DYNAMIC_COLLISION_CARRY_POSITION | \
                OOT_DYNAMIC_COLLISION_CARRY_ROTATION_Y))

extern PlayState *liboot_play( void );
extern void liboot_surface_type_from_preset( uint16_t preset, SurfaceType *out );

typedef struct LibootDynamicSlot
{
    DynaPolyActor dynaActor;
    CollisionHeader header;
    Vec3s *vertices;
    CollisionPoly *polygons;
    SurfaceType *surfaceTypes;
    struct OoTDynamicCollisionTransform transform;
    uint32_t generation;
    uint16_t numVertices;
    uint16_t numPolygons;
    uint8_t active;
    uint8_t registered;
    uint8_t enabled;
} LibootDynamicSlot;

static LibootDynamicSlot s_dynamic[OOT_DYNAMIC_COLLISION_MAX_OBJECTS];

static void dynamic_actor_noop( Actor *actor, PlayState *play )
{
    (void)actor;
    (void)play;
}

static OoTDynamicCollision dynamic_handle( uint32_t index, uint32_t generation )
{
    return ( generation << 8 ) | ( index + 1u );
}

static LibootDynamicSlot *dynamic_find( OoTDynamicCollision handle )
{
    uint32_t encodedIndex = handle & DYNAMIC_HANDLE_INDEX_MASK;
    uint32_t generation = handle >> 8;
    LibootDynamicSlot *slot;

    if( encodedIndex == 0u || generation == 0u ||
        encodedIndex > OOT_DYNAMIC_COLLISION_MAX_OBJECTS )
        return NULL;
    slot = &s_dynamic[encodedIndex - 1u];
    return slot->active && slot->generation == generation ? slot : NULL;
}

static bool dynamic_transform_valid( const struct OoTDynamicCollisionTransform *transform,
                                     const Vec3s *vertices, uint16_t numVertices )
{
    MtxF matrix;

    if( transform == NULL ||
        transform->structSize < sizeof(struct OoTDynamicCollisionTransform) ||
        !isfinite( transform->position[0] ) || !isfinite( transform->position[1] ) ||
        !isfinite( transform->position[2] ) || !isfinite( transform->scale[0] ) ||
        !isfinite( transform->scale[1] ) || !isfinite( transform->scale[2] ) ||
        transform->scale[0] == 0.0f || transform->scale[1] == 0.0f ||
        transform->scale[2] == 0.0f )
        return false;

    SkinMatrix_SetTranslateRotateYXZScale(
        &matrix, transform->scale[0], transform->scale[1], transform->scale[2],
        transform->rotation[0], transform->rotation[1], transform->rotation[2],
        transform->position[0], transform->position[1], transform->position[2] );
    for( uint16_t i = 0; i < numVertices; ++i ) {
        Vec3f local = { vertices[i].x, vertices[i].y, vertices[i].z };
        Vec3f world;
        SkinMatrix_Vec3fMtxFMultXYZ( &matrix, &local, &world );
        if( !isfinite( world.x ) || !isfinite( world.y ) || !isfinite( world.z ) ||
            world.x <= -BGCHECK_XYZ_ABSMAX || world.x >= BGCHECK_XYZ_ABSMAX ||
            world.y <= -BGCHECK_XYZ_ABSMAX || world.y >= BGCHECK_XYZ_ABSMAX ||
            world.z <= -BGCHECK_XYZ_ABSMAX || world.z >= BGCHECK_XYZ_ABSMAX )
            return false;
    }
    return true;
}

static void dynamic_apply_transform( LibootDynamicSlot *slot,
                                     const struct OoTDynamicCollisionTransform *transform )
{
    DynaPolyActor *dyna = &slot->dynaActor;
    slot->transform = *transform;
    slot->transform.structSize = sizeof(slot->transform);
    slot->transform.reserved = 0u;
    dyna->actor.world.pos.x = transform->position[0];
    dyna->actor.world.pos.y = transform->position[1];
    dyna->actor.world.pos.z = transform->position[2];
    dyna->actor.world.rot.x = transform->rotation[0];
    dyna->actor.world.rot.y = transform->rotation[1];
    dyna->actor.world.rot.z = transform->rotation[2];
    dyna->actor.shape.rot = dyna->actor.world.rot;
    dyna->actor.scale.x = transform->scale[0];
    dyna->actor.scale.y = transform->scale[1];
    dyna->actor.scale.z = transform->scale[2];
}

static uint16_t dynamic_vertex_add( Vec3s *vertices, uint16_t *count,
                                    const int32_t input[3] )
{
    uint16_t i;
    for( i = 0; i < *count; ++i ) {
        if( vertices[i].x == input[0] && vertices[i].y == input[1] &&
            vertices[i].z == input[2] )
            return i;
    }
    if( *count >= OOT_DYNAMIC_COLLISION_MAX_VERTICES )
        return UINT16_MAX;
    vertices[*count].x = (s16)input[0];
    vertices[*count].y = (s16)input[1];
    vertices[*count].z = (s16)input[2];
    return ( *count )++;
}

/* 1 success, 0 invalid/unrepresentable geometry, -1 allocation failure. */
static int dynamic_build_geometry( LibootDynamicSlot *slot,
                                   const struct OoTSurface *surfaces,
                                   uint32_t numSurfaces )
{
    int16_t presetSlots[OOT_SURFACE_PRESET_COUNT];
    uint16_t numVertices = 0u;
    uint16_t numTypes = 0u;

    memset( presetSlots, 0xFF, sizeof(presetSlots) );
    slot->vertices = calloc( OOT_DYNAMIC_COLLISION_MAX_VERTICES, sizeof(Vec3s) );
    slot->polygons = calloc( numSurfaces, sizeof(CollisionPoly) );
    slot->surfaceTypes = calloc( OOT_SURFACE_PRESET_COUNT, sizeof(SurfaceType) );
    if( slot->vertices == NULL || slot->polygons == NULL || slot->surfaceTypes == NULL )
        return -1;

    slot->header.minBounds.x = slot->header.minBounds.y = slot->header.minBounds.z = INT16_MAX;
    slot->header.maxBounds.x = slot->header.maxBounds.y = slot->header.maxBounds.z = INT16_MIN;
    for( uint32_t i = 0u; i < numSurfaces; ++i ) {
        uint16_t indices[3];
        Vec3f points[3];
        uint16_t preset = surfaces[i].type < OOT_SURFACE_PRESET_COUNT
            ? surfaces[i].type : OOT_SURFACE_DEFAULT;

        if( presetSlots[preset] < 0 ) {
            presetSlots[preset] = (int16_t)numTypes;
            liboot_surface_type_from_preset( preset, &slot->surfaceTypes[numTypes++] );
        }
        for( uint32_t vertex = 0u; vertex < 3u; ++vertex ) {
            for( uint32_t axis = 0u; axis < 3u; ++axis ) {
                int32_t value = surfaces[i].vertices[vertex][axis];
                if( value < INT16_MIN || value > INT16_MAX )
                    return 0;
                ((float *)&points[vertex])[axis] = (float)value;
            }
            indices[vertex] = dynamic_vertex_add( slot->vertices, &numVertices,
                                                  surfaces[i].vertices[vertex] );
            if( indices[vertex] == UINT16_MAX )
                return 0;
            if( surfaces[i].vertices[vertex][0] < slot->header.minBounds.x )
                slot->header.minBounds.x = (s16)surfaces[i].vertices[vertex][0];
            if( surfaces[i].vertices[vertex][1] < slot->header.minBounds.y )
                slot->header.minBounds.y = (s16)surfaces[i].vertices[vertex][1];
            if( surfaces[i].vertices[vertex][2] < slot->header.minBounds.z )
                slot->header.minBounds.z = (s16)surfaces[i].vertices[vertex][2];
            if( surfaces[i].vertices[vertex][0] > slot->header.maxBounds.x )
                slot->header.maxBounds.x = (s16)surfaces[i].vertices[vertex][0];
            if( surfaces[i].vertices[vertex][1] > slot->header.maxBounds.y )
                slot->header.maxBounds.y = (s16)surfaces[i].vertices[vertex][1];
            if( surfaces[i].vertices[vertex][2] > slot->header.maxBounds.z )
                slot->header.maxBounds.z = (s16)surfaces[i].vertices[vertex][2];
        }

        {
            float e1x = points[1].x - points[0].x;
            float e1y = points[1].y - points[0].y;
            float e1z = points[1].z - points[0].z;
            float e2x = points[2].x - points[0].x;
            float e2y = points[2].y - points[0].y;
            float e2z = points[2].z - points[0].z;
            float nx = e1y * e2z - e1z * e2y;
            float ny = e1z * e2x - e1x * e2z;
            float nz = e1x * e2y - e1y * e2x;
            float length = sqrtf( nx * nx + ny * ny + nz * nz );
            float distance;
            CollisionPoly *poly = &slot->polygons[i];
            if( !isfinite( length ) || length <= 0.000001f )
                return 0;
            nx /= length;
            ny /= length;
            nz /= length;
            distance = -( nx * points[0].x + ny * points[0].y + nz * points[0].z );
            if( !isfinite( distance ) || distance < INT16_MIN || distance > INT16_MAX )
                return 0;
            poly->type = (u16)presetSlots[preset];
            poly->flags_vIA = indices[0];
            poly->flags_vIB = indices[1];
            if( preset == OOT_SURFACE_CONVEYOR )
                poly->flags_vIB |= 0x2000u;
            poly->vIC = indices[2];
            poly->normal.x = (s16)( nx * 32767.0f );
            poly->normal.y = (s16)( ny * 32767.0f );
            poly->normal.z = (s16)( nz * 32767.0f );
            poly->dist = (s16)distance;
        }
    }

    slot->numVertices = numVertices;
    slot->numPolygons = (uint16_t)numSurfaces;
    slot->header.numVertices = numVertices;
    slot->header.vtxList = slot->vertices;
    slot->header.numPolygons = (uint16_t)numSurfaces;
    slot->header.polyList = slot->polygons;
    slot->header.surfaceTypeList = slot->surfaceTypes;
    return 1;
}

static void dynamic_slot_free( LibootDynamicSlot *slot )
{
    uint32_t generation = slot->generation + 1u;
    if( generation == 0u || generation > 0xFFFFFFu )
        generation = 1u;
    free( slot->vertices );
    free( slot->polygons );
    free( slot->surfaceTypes );
    memset( slot, 0, sizeof(*slot) );
    slot->generation = generation;
    slot->dynaActor.bgId = BGACTOR_NEG_ONE;
}

static void dynamic_totals( uint32_t *outPolygons, uint32_t *outVertices )
{
    uint32_t polygons = 0u;
    uint32_t vertices = 0u;
    for( uint32_t i = 0u; i < OOT_DYNAMIC_COLLISION_MAX_OBJECTS; ++i ) {
        if( s_dynamic[i].active ) {
            polygons += s_dynamic[i].numPolygons;
            vertices += s_dynamic[i].numVertices;
        }
    }
    *outPolygons = polygons;
    *outVertices = vertices;
}

static bool dynamic_register( LibootDynamicSlot *slot )
{
    PlayState *play = liboot_play();
    s32 bgId = DynaPoly_SetBgActor( play, &play->colCtx.dyna,
                                    &slot->dynaActor.actor, &slot->header );
    if( bgId < 0 || bgId >= BG_ACTOR_MAX )
        return false;
    slot->dynaActor.bgId = bgId;
    slot->registered = 1u;
    if( !slot->enabled )
        DynaPoly_DisableCollision( play, &play->colCtx.dyna, bgId );
    return true;
}

int32_t oot_dynamic_collision_create(
    const struct OoTSurface *surfaces, uint32_t numSurfaces,
    const struct OoTDynamicCollisionTransform *transform, uint32_t flags,
    OoTDynamicCollision *outHandle )
{
    PlayState *play = liboot_play();
    LibootDynamicSlot *slot = NULL;
    uint32_t slotIndex = 0u;
    uint32_t totalPolygons;
    uint32_t totalVertices;

    if( outHandle != NULL )
        *outHandle = OOT_DYNAMIC_COLLISION_INVALID;
    if( surfaces == NULL || numSurfaces == 0u ||
        numSurfaces > OOT_DYNAMIC_COLLISION_MAX_SURFACES || outHandle == NULL ||
        ( flags & ~DYNAMIC_KNOWN_FLAGS ) != 0u || play->colCtx.lookupTbl == NULL )
        return play->colCtx.lookupTbl == NULL ? -2 : -1;
    for( slotIndex = 0u; slotIndex < OOT_DYNAMIC_COLLISION_MAX_OBJECTS; ++slotIndex ) {
        if( !s_dynamic[slotIndex].active ) {
            slot = &s_dynamic[slotIndex];
            break;
        }
    }
    if( slot == NULL )
        return -3;
    if( slot->generation == 0u )
        slot->generation = 1u;
    {
        int buildResult = dynamic_build_geometry( slot, surfaces, numSurfaces );
        if( buildResult <= 0 ) {
            dynamic_slot_free( slot );
            return buildResult < 0 ? -4 : -1;
        }
    }
    if( !dynamic_transform_valid( transform, slot->vertices, slot->numVertices ) ) {
        dynamic_slot_free( slot );
        return -1;
    }
    dynamic_totals( &totalPolygons, &totalVertices );
    if( totalPolygons + slot->numPolygons > OOT_DYNAMIC_COLLISION_MAX_SURFACES ||
        totalVertices + slot->numVertices > OOT_DYNAMIC_COLLISION_MAX_VERTICES ) {
        dynamic_slot_free( slot );
        return -3;
    }

    memset( &slot->dynaActor, 0, sizeof(slot->dynaActor) );
    DynaPolyActor_Init( &slot->dynaActor,
        (( flags & OOT_DYNAMIC_COLLISION_CARRY_POSITION ) ? DYNA_TRANSFORM_POS : 0) |
        (( flags & OOT_DYNAMIC_COLLISION_CARRY_ROTATION_Y ) ? DYNA_TRANSFORM_ROT_Y : 0) );
    slot->dynaActor.actor.update = dynamic_actor_noop;
    slot->dynaActor.actor.id = -1;
    dynamic_apply_transform( slot, transform );
    slot->active = 1u;
    slot->enabled = 1u;
    if( !dynamic_register( slot ) ) {
        dynamic_slot_free( slot );
        return -3;
    }
    DynaPoly_UpdateContext( play, &play->colCtx.dyna );
    play->colCtx.dyna.bgActors[slot->dynaActor.bgId].prevTransform =
        play->colCtx.dyna.bgActors[slot->dynaActor.bgId].curTransform;
    *outHandle = dynamic_handle( slotIndex, slot->generation );
    return 0;
}

bool oot_dynamic_collision_set_transform(
    OoTDynamicCollision handle, const struct OoTDynamicCollisionTransform *transform )
{
    LibootDynamicSlot *slot = dynamic_find( handle );
    PlayState *play = liboot_play();
    if( slot == NULL || !slot->registered ||
        !dynamic_transform_valid( transform, slot->vertices, slot->numVertices ) )
        return false;
    dynamic_apply_transform( slot, transform );
    DynaPoly_UpdateContext( play, &play->colCtx.dyna );
    return true;
}

bool oot_dynamic_collision_set_enabled( OoTDynamicCollision handle, bool enabled )
{
    LibootDynamicSlot *slot = dynamic_find( handle );
    PlayState *play = liboot_play();
    if( slot == NULL || !slot->registered )
        return false;
    if( enabled )
        DynaPoly_EnableCollision( play, &play->colCtx.dyna, slot->dynaActor.bgId );
    else
        DynaPoly_DisableCollision( play, &play->colCtx.dyna, slot->dynaActor.bgId );
    slot->enabled = enabled ? 1u : 0u;
    DynaPoly_UpdateContext( play, &play->colCtx.dyna );
    return true;
}

bool oot_dynamic_collision_get_state( OoTDynamicCollision handle,
                                      struct OoTDynamicCollisionState *outState )
{
    LibootDynamicSlot *slot = dynamic_find( handle );
    if( slot == NULL || outState == NULL ||
        outState->structSize < sizeof(struct OoTDynamicCollisionState) )
        return false;
    memset( outState, 0, sizeof(*outState) );
    outState->structSize = sizeof(*outState);
    outState->transform = slot->transform;
    outState->nativeBgId = slot->registered ? slot->dynaActor.bgId : BGACTOR_NEG_ONE;
    outState->enabled = slot->enabled;
    outState->playerOnTop = DynaPolyActor_IsPlayerOnTop( &slot->dynaActor ) ? 1u : 0u;
    outState->playerAbove = DynaPolyActor_IsPlayerAbove( &slot->dynaActor ) ? 1u : 0u;
    outState->actorOnTop = DynaPolyActor_IsActorOnTop( &slot->dynaActor ) ? 1u : 0u;
    return true;
}

bool oot_dynamic_collision_delete( OoTDynamicCollision handle )
{
    LibootDynamicSlot *slot = dynamic_find( handle );
    PlayState *play = liboot_play();
    if( slot == NULL )
        return false;
    if( slot->registered ) {
        DynaPoly_DeleteBgActor( play, &play->colCtx.dyna, slot->dynaActor.bgId );
        DynaPoly_UpdateContext( play, &play->colCtx.dyna );
    }
    dynamic_slot_free( slot );
    return true;
}

void liboot_dynamic_collision_begin_tick( void )
{
    for( uint32_t i = 0u; i < OOT_DYNAMIC_COLLISION_MAX_OBJECTS; ++i )
        if( s_dynamic[i].active )
            DynaPolyActor_UnsetAllInteractFlags( &s_dynamic[i].dynaActor );
}

void liboot_dynamic_collision_end_tick( void )
{
    PlayState *play = liboot_play();
    if( play->colCtx.lookupTbl != NULL )
        DynaPoly_UpdateBgActorTransforms( play, &play->colCtx.dyna );
}

void liboot_dynamic_collision_rebind_all( void )
{
    PlayState *play = liboot_play();
    bool any = false;
    for( uint32_t i = 0u; i < OOT_DYNAMIC_COLLISION_MAX_OBJECTS; ++i ) {
        LibootDynamicSlot *slot = &s_dynamic[i];
        if( !slot->active )
            continue;
        slot->registered = 0u;
        slot->dynaActor.bgId = BGACTOR_NEG_ONE;
        if( dynamic_register( slot ) )
            any = true;
    }
    if( any ) {
        DynaPoly_UpdateContext( play, &play->colCtx.dyna );
        DynaPoly_UpdateBgActorTransforms( play, &play->colCtx.dyna );
    }
}

void liboot_dynamic_collision_terminate( void )
{
    for( uint32_t i = 0u; i < OOT_DYNAMIC_COLLISION_MAX_OBJECTS; ++i ) {
        free( s_dynamic[i].vertices );
        free( s_dynamic[i].polygons );
        free( s_dynamic[i].surfaceTypes );
    }
    memset( s_dynamic, 0, sizeof(s_dynamic) );
}
