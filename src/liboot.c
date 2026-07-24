/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include "liboot.h"
#include "rom_util.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "ultra64.h"
#include "animation.h"
#include "bgcheck.h"
#include "player.h"
#include "play_state.h"
#include "controller.h"
#include "regs.h"
#include "save.h"
#include "object.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* provided by the shim layer */
extern PlayState *liboot_play( void );
extern void liboot_play_init( void );
extern void liboot_register_segment_span( int seg, void *base, size_t size );
extern bool liboot_bind_assets( void );          /* copies + validates runtime assets */
extern bool liboot_link_skeleton_valid( uint8_t age );
extern void liboot_reset_tha( void );
extern void liboot_set_child_blob( const void *blob, size_t size );
extern bool liboot_bgcheck_preflight( CollisionHeader *hdr );

/* Audio extraction module (audio_extract.c), declared like the gfx/scene
   hooks above: plain extern, NOT a weak stub. A weak local definition here
   would let a static link satisfy these references from liboot.o and never
   pull audio_extract.o out of the archive, silently dropping all real audio
   (oot_get_voice_sample / oot_get_ocarina_note would fall back to the stub
   and return "unavailable"). The unresolved externs force audio_extract.o to
   be linked, which also brings the strong public audio getters it defines. */
extern void liboot_audio_init( const uint8_t *rom, size_t romSize );
extern void liboot_audio_terminate( void );
extern void liboot_audio_sequence_reset( void );
extern s16 liboot_cam_yaw;

extern void liboot_gfx_terminate( void );        /* drops the decoded texture cache */
extern void liboot_gfx_evict_scene( void );      /* drops segment-2/3 texture entries */
extern void liboot_gfx_set_lights( const float ambient[3], const float dir0[3],
                                   const float col0[3], const float dir1[3],
                                   const float col1[3], int count );
extern void liboot_scene_terminate( void );      /* liboot v0.7: scene.c teardown */

extern void liboot_link_init( PlayState *play, Player *player, float x, float y, float z );
extern void liboot_link_update( PlayState *play, Player *player );
extern void liboot_render_link( PlayState *play, Player *player, struct OoTLinkGeometryBuffers *out );
extern s16 liboot_player_anim_id( const void *animation );
static void free_player_allocs( Player *player );

/* link_animetion is ~0x234000 bytes decompressed in every retail version and
   no other dmadata file falls in this window, so locate it by size rather
   than by index (PAL drops the kanji file, shifting all indices). */
#define LINK_ANIMETION_MIN 0x200000
#define LINK_ANIMETION_MAX 0x290000
/* Retail OoT images are far smaller than this.  Keeping a hard upper bound
   prevents accidental multi-gigabyte copies and keeps all DMADATA walks
   comfortably inside their 32-bit on-ROM address/index model. */
#define LIBOOT_MAX_ROM_SIZE (256u * 1024u * 1024u)

static uint8_t *find_link_animetion( const uint8_t *rom, size_t romSize, size_t *outSize )
{
    const uint8_t *dmadata = rom_find_dmadata( rom, romSize );
    LibootDmaEntry e;
    for( uint32_t i = 3; dmadata && dma_get( rom, romSize, dmadata, i, &e ); ++i ) {
        size_t size = e.vromEnd - e.vromStart;
        if( size >= LINK_ANIMETION_MIN && size <= LINK_ANIMETION_MAX )
            return rom_read_file( rom, romSize, i, outSize );
    }
    return NULL;
}

static struct {
    bool inited;
    uint8_t *romBlob;    size_t romSize;     /* liboot v0.7: normalized ROM copy
                                                kept for on-demand scene loads */
    uint8_t *animBlob;   size_t animSize;    /* segment 7: link_animetion   */
    uint8_t *objectBlob; size_t objectSize;  /* segment 6: object_link_boy  */
    uint8_t *childBlob;  size_t childSize;   /*            object_link_child */
    uint8_t *keepBlob;   size_t keepSize;    /* segment 4: gameplay_keep    */
    Player *player;
    float lastX, lastY, lastZ;
    /* runtime-built static collision */
    Vec3s *colVtx;
    CollisionPoly *colPoly;
    SurfaceType *colSurfaceTypes;
    WaterBox *colWaterBoxes;        /* liboot v0.5: native water boxes */
    uint16_t numColWaterBoxes;
    CollisionHeader colHeader;
} s_state;

static OoTDebugPrintFunctionPtr s_debugPrint;
static bool s_linkFrozen;   /* liboot vNEXT: oot_link_freeze holds the Player pose */

static void dbg( const char *msg )
{
    if( s_debugPrint ) s_debugPrint( msg );
}

/* liboot v0.7: scene.c reads scene/room files out of the retained ROM copy */
const uint8_t *liboot_rom( size_t *outSize )
{
    if( outSize ) *outSize = s_state.romSize;
    return s_state.romBlob;
}

/* A full collision-world replacement invalidates CollisionPoly pointers cached
   by every live actor. Room-only swaps deliberately do not call this because
   their scene collision remains allocated in place. */
void liboot_invalidate_actor_bg_cache( PlayState *play )
{
    if( play == NULL ) return;
    for( int category = 0; category < ACTORCAT_MAX; ++category ) {
        for( Actor *actor = play->actorCtx.actorLists[category].head;
             actor != NULL; actor = actor->next ) {
            actor->wallPoly = NULL;
            actor->floorPoly = NULL;
            actor->wallBgId = BGCHECK_SCENE;
            actor->floorBgId = BGCHECK_SCENE;
            actor->bgCheckFlags = 0;
        }
    }
}

/* Each link object carries its flex skeleton at a fixed offset: a segmented
   pointer to the 21-limb table followed by limbCount 0x15. Same layout in
   every retail version. */
static bool looks_like_link_object( const uint8_t *data, size_t size, uint32_t skelOff, uint32_t limbTableOff )
{
    if( size < skelOff + 9 ) return false;
    const uint8_t *hdr = data + skelOff;
    uint32_t tok = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16 | (uint32_t)hdr[2] << 8 | hdr[3];
    return tok == ( 0x06000000u | limbTableOff ) && hdr[4] == 0x15;
}

static uint8_t *find_link_object( const uint8_t *rom, size_t romSize, size_t *outSize,
                                  size_t minSize, size_t maxSize, uint32_t skelOff, uint32_t limbTableOff )
{
    const uint8_t *dmadata = rom_find_dmadata( rom, romSize );
    LibootDmaEntry e;
    for( uint32_t i = 3; dmadata && dma_get( rom, romSize, dmadata, i, &e ); ++i ) {
        size_t size = e.vromEnd - e.vromStart;
        if( size < minSize || size > maxSize ) continue;
        uint8_t *buf = rom_read_file( rom, romSize, i, &size );
        if( !buf ) continue;
        if( looks_like_link_object( buf, size, skelOff, limbTableOff )) { *outSize = size; return buf; }
        free( buf );
    }
    return NULL;
}

#define ADULT_SKEL_OFF 0x377F4
#define ADULT_LIMB_OFF 0x377A0
#define CHILD_SKEL_OFF 0x2CF6C
#define CHILD_LIMB_OFF 0x2CF18

/* liboot v0.4: gameplay_keep ("segment 4" common assets: Navi's fairy model,
   lock-on reticle, shadows, pause-link poses...). Version-invariant
   detection: decompressed size window plus 10 consecutive 8-byte
   PlayerAnimationHeader records { u16 frameCount 1..1000, u16 0, u8 0x07 seg
   token } at file offset 0x2310 — that offset precedes every per-version
   layout fork in the decomp XML. */
#define GKEEP_MIN 0x40000
#define GKEEP_MAX 0x80000

static bool looks_like_gameplay_keep( const uint8_t *data, size_t size )
{
    const uint32_t recOff = 0x2310;
    if( size < recOff + 10 * 8 ) return false;
    for( int i = 0; i < 10; ++i ) {
        const uint8_t *rec = data + recOff + i * 8;
        uint16_t frameCount = (uint16_t)( rec[0] << 8 | rec[1] );
        if( frameCount < 1 || frameCount > 1000 ) return false;
        if( rec[2] != 0 || rec[3] != 0 ) return false;
        if( rec[4] != 0x07 ) return false;
    }
    return true;
}

static uint8_t *find_gameplay_keep( const uint8_t *rom, size_t romSize, size_t *outSize )
{
    const uint8_t *dmadata = rom_find_dmadata( rom, romSize );
    LibootDmaEntry e;
    for( uint32_t i = 3; dmadata && dma_get( rom, romSize, dmadata, i, &e ); ++i ) {
        size_t size = e.vromEnd - e.vromStart;
        if( size < GKEEP_MIN || size > GKEEP_MAX ) continue;
        uint8_t *buf = rom_read_file( rom, romSize, i, &size );
        if( !buf ) continue;
        if( looks_like_gameplay_keep( buf, size )) { *outSize = size; return buf; }
        free( buf );
    }
    return NULL;
}

void oot_set_debug_print_function( OoTDebugPrintFunctionPtr fn )
{
    s_debugPrint = fn;
}

void oot_global_init( const uint8_t *rom, size_t romSize, uint8_t *outTexture )
{
    if( s_state.inited ) oot_global_terminate();

    if( rom == NULL || romSize < 0x1060u || romSize > LIBOOT_MAX_ROM_SIZE ) {
        dbg( "liboot: ROM buffer has an unsupported size" );
        return;
    }

    uint8_t *romCopy = malloc( romSize );
    if( romCopy == NULL ) {
        dbg( "liboot: unable to allocate ROM copy" );
        return;
    }
    memcpy( romCopy, rom, romSize );
    if( !rom_normalize( romCopy, romSize )) {
        dbg( "liboot: not a recognizable Zelda64 ROM" );
        free( romCopy );
        return;
    }

    s_state.animBlob = find_link_animetion( romCopy, romSize, &s_state.animSize );
    if( !s_state.animBlob ) {
        dbg( "liboot: link_animetion not found (unsupported ROM version?)" );
        free( romCopy );
        return;
    }
    liboot_audio_sequence_reset();
    liboot_audio_init( romCopy, romSize );
    s_state.objectBlob = find_link_object( romCopy, romSize, &s_state.objectSize,
                                           0x37800, 0x40000, ADULT_SKEL_OFF, ADULT_LIMB_OFF );
    s_state.childBlob = find_link_object( romCopy, romSize, &s_state.childSize,
                                          0x2CF00, 0x38000, CHILD_SKEL_OFF, CHILD_LIMB_OFF );
    s_state.keepBlob = find_gameplay_keep( romCopy, romSize, &s_state.keepSize );
    /* liboot v0.7: keep the normalized ROM — oot_scene_load extracts scene
       and room files from it on demand */
    s_state.romBlob = romCopy;
    s_state.romSize = romSize;
    if( !s_state.objectBlob ) {
        dbg( "liboot: object_link_boy not found in ROM" );
        oot_global_terminate();
        return;
    }
    if( !s_state.childBlob )
        dbg( "liboot: object_link_child not found; child Link unavailable" );
    if( !s_state.keepBlob )
        dbg( "liboot: gameplay_keep not found; Navi/reticle assets unavailable" );

    liboot_register_segment_span( 7, s_state.animBlob, s_state.animSize );
    liboot_register_segment_span( 6, s_state.objectBlob, s_state.objectSize );
    /* liboot v0.4: segment 4 must be live before liboot_bind_assets so the
       seg-4 binds (pause joint tables, lens mask, fairy skeleton) resolve */
    liboot_register_segment_span( 4, s_state.keepBlob, s_state.keepSize );
    liboot_set_child_blob( s_state.childBlob, s_state.childSize );
    if( !liboot_bind_assets()) {
        dbg( "liboot: invalid adult Link skeleton" );
        oot_global_terminate();
        return;
    }
    liboot_play_init();

    (void)outTexture; /* texture atlas extraction lands with the gfx adapter */
    s_state.inited = true;
    dbg( "liboot: global init ok" );
}

void oot_global_terminate( void )
{
    /* Drop all sequence voices before audio_extract frees their PCM backing. */
    liboot_audio_sequence_reset();
    liboot_audio_terminate();
    liboot_scene_terminate();   /* liboot v0.7: scene blobs, table, collision */
    liboot_gfx_terminate();
    free( s_state.romBlob );
    free( s_state.animBlob );
    free( s_state.objectBlob );
    free( s_state.childBlob );
    free( s_state.keepBlob );
    free( s_state.colVtx );
    free( s_state.colPoly );
    free( s_state.colSurfaceTypes );
    free( s_state.colWaterBoxes );
    free_player_allocs( s_state.player );  /* giObjectSegment from Player_Init */
    free( s_state.player );
    memset( &s_state, 0, sizeof( s_state ));
    /* s_linkFrozen lives outside s_state, so clear it here too: oot_global_init
       auto-terminates a live session without going through oot_link_delete, and
       a leaked freeze would silently immobilize the next session's Link. */
    s_linkFrozen = false;
}

/* liboot vNEXT: enum OoTSurfaceType -> real SurfaceType data words, built with
   the game's own SURFACETYPE0/SURFACETYPE1 macros. Index == OoTSurfaceType.
   OOT_SURFACE_DEFAULT is byte-identical to the pre-vNEXT single surface type
   (data[0] = 0, data[1] = canHookshot bit), so existing callers are unchanged.
   No preset sets exitIndex or a void floorProperty (5/12) — those freeze Link
   and deref the NULL exitList in the fake play state (see the fill masks). */
static const SurfaceType kOotSurfacePresets[OOT_SURFACE_PRESET_COUNT] = {
    [OOT_SURFACE_DEFAULT]     = { { 0, SURFACETYPE1( SURFACE_MATERIAL_DIRT,  0, 0, 0, 1, 0, 0, 0 ) } },
    [OOT_SURFACE_SAND]        = { { 0, SURFACETYPE1( SURFACE_MATERIAL_SAND,  0, 0, 0, 1, 0, 0, 0 ) } },
    [OOT_SURFACE_GRASS]       = { { 0, SURFACETYPE1( SURFACE_MATERIAL_GRASS, 0, 0, 0, 1, 0, 0, 0 ) } },
    [OOT_SURFACE_STONE]       = { { 0, SURFACETYPE1( SURFACE_MATERIAL_STONE, 0, 0, 0, 1, 0, 0, 0 ) } },
    [OOT_SURFACE_DAMAGE]      = { { SURFACETYPE0( 0, 0, FLOOR_TYPE_2, 0, 0, 0, 0, 0 ),
                                    SURFACETYPE1( SURFACE_MATERIAL_LAVA, 0, 0, 0, 1, 0, 0, 0 ) } },
    [OOT_SURFACE_SLIPPERY]    = { { SURFACETYPE0( 0, 0, FLOOR_TYPE_5, 0, 0, 0, 0, 0 ),
                                    SURFACETYPE1( SURFACE_MATERIAL_ICE, 0, 0, 0, 1, 0, 0, 0 ) } },
    [OOT_SURFACE_CLIMB_WALL]  = { { SURFACETYPE0( 0, 0, 0, 0, WALL_TYPE_2, 0, 0, 0 ),
                                    SURFACETYPE1( SURFACE_MATERIAL_DIRT, 0, 0, 0, 1, 0, 0, 0 ) } },
    [OOT_SURFACE_CONVEYOR]    = { { 0, SURFACETYPE1( SURFACE_MATERIAL_DIRT, 0, 0, 0, 1,
                                                     CONVEYOR_SPEED_MEDIUM, 0, 0 ) } },
    [OOT_SURFACE_NO_HOOKSHOT] = { { 0, 0 } },
};

/* Checked internal form used by the engine-neutral wrapper. Returns 1 on
   success, 0 for invalid/unrepresentable geometry, and -1 on allocation
   failure. The legacy public function below keeps its original void ABI. */
int liboot_static_world_load_checked( const struct OoTSurface *surfaces, uint32_t numSurfaces,
                                      const struct OoTWaterBox *waterBoxes, uint32_t numWaterBoxes )
{
    if( !s_state.inited ) return 0;

    /* CollisionPoly vertex indices are 13-bit and the public water count is
       stored as u16. Reject malformed/oversized host worlds before touching
       the currently active collision state. */
    if( surfaces == NULL || numSurfaces == 0 || numSurfaces > 2730 ||
        ( numWaterBoxes > 0 && waterBoxes == NULL ) || numWaterBoxes > UINT16_MAX ) {
        dbg( "liboot: invalid or oversized static world" );
        return 0;
    }
    for( uint32_t i = 0; i < numWaterBoxes; ++i ) {
        if( waterBoxes[i].xLength <= 0 || waterBoxes[i].zLength <= 0 ) {
            dbg( "liboot: static world contains an invalid water box" );
            return 0;
        }
    }

    /* liboot vNEXT: honor OoTSurface.type. Dedup the presets actually used into
       a compact SurfaceType list; every poly[i].type indexes it. Unknown types
       clamp to DEFAULT here AND in the poly loop below with the SAME clamp, so
       every poly[i].type is guaranteed < nTypes (SurfaceType_GetData does no
       bounds check). nTypes >= 1 because numSurfaces >= 1. */
    int16_t presetSlot[OOT_SURFACE_PRESET_COUNT];
    for( uint32_t p = 0; p < OOT_SURFACE_PRESET_COUNT; ++p ) presetSlot[p] = -1;
    uint32_t nTypes = 0;
    for( uint32_t i = 0; i < numSurfaces; ++i ) {
        uint16_t t = surfaces[i].type;
        if( t >= OOT_SURFACE_PRESET_COUNT ) t = OOT_SURFACE_DEFAULT;
        if( presetSlot[t] < 0 ) presetSlot[t] = (int16_t)nTypes++;
    }

    Vec3s *vtx = calloc( numSurfaces * 3, sizeof( Vec3s ));
    CollisionPoly *poly = calloc( numSurfaces, sizeof( CollisionPoly ));
    SurfaceType *types = calloc( nTypes, sizeof( SurfaceType ));
    WaterBox *newWaterBoxes = numWaterBoxes > 0
        ? calloc( numWaterBoxes, sizeof( WaterBox )) : NULL;
    if( vtx == NULL || poly == NULL || types == NULL ||
        ( numWaterBoxes > 0 && newWaterBoxes == NULL )) {
        free( vtx );
        free( poly );
        free( types );
        free( newWaterBoxes );
        dbg( "liboot: unable to allocate static world" );
        return -1;
    }

    /* Fill each used SurfaceType slot from the preset table, applying scene.c's
       relocate_collision safety masks defensively: clear exitIndex (bits 8-12)
       and any void floorProperty (5/12) so a preset can never freeze Link or
       index the NULL exitList. OOT_SURFACE_DEFAULT reproduces the historical
       data[1] = 1u << 17 (hookshot-attachable) exactly. */
    for( uint32_t p = 0; p < OOT_SURFACE_PRESET_COUNT; ++p ) {
        if( presetSlot[p] < 0 ) continue;
        uint32_t d0 = kOotSurfacePresets[p].data[0];
        d0 &= ~( 0x1Fu << 8 );                 /* exitIndex */
        uint32_t floorProp = ( d0 >> 26 ) & 0xF;
        if( floorProp == 5 || floorProp == 12 ) d0 &= ~( 0xFu << 26 );  /* void */
        types[presetSlot[p]].data[0] = d0;
        types[presetSlot[p]].data[1] = kOotSurfacePresets[p].data[1];
    }

    Vec3s mins = { 32767, 32767, 32767 }, maxs = { -32768, -32768, -32768 };

    for( uint32_t i = 0; i < numSurfaces; ++i ) {
        float vf[3][3];
        for( int v = 0; v < 3; ++v ) {
            for( int axis = 0; axis < 3; ++axis ) {
                int32_t value = surfaces[i].vertices[v][axis];
                if( value < INT16_MIN || value > INT16_MAX ) {
                    free( vtx );
                    free( poly );
                    free( types );
                    free( newWaterBoxes );
                    dbg( "liboot: static-world vertex exceeds signed 16-bit range" );
                    return 0;
                }
            }
            Vec3s *out = &vtx[i * 3 + v];
            out->x = (s16)surfaces[i].vertices[v][0];
            out->y = (s16)surfaces[i].vertices[v][1];
            out->z = (s16)surfaces[i].vertices[v][2];
            vf[v][0] = out->x; vf[v][1] = out->y; vf[v][2] = out->z;
            if( out->x < mins.x ) mins.x = out->x;
            if( out->y < mins.y ) mins.y = out->y;
            if( out->z < mins.z ) mins.z = out->z;
            if( out->x > maxs.x ) maxs.x = out->x;
            if( out->y > maxs.y ) maxs.y = out->y;
            if( out->z > maxs.z ) maxs.z = out->z;
        }
        float e1[3] = { vf[1][0]-vf[0][0], vf[1][1]-vf[0][1], vf[1][2]-vf[0][2] };
        float e2[3] = { vf[2][0]-vf[0][0], vf[2][1]-vf[0][1], vf[2][2]-vf[0][2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        float len = sqrtf( n[0]*n[0] + n[1]*n[1] + n[2]*n[2] );
        if( !isfinite( len ) || len <= 0.000001f ) {
            free( vtx );
            free( poly );
            free( types );
            free( newWaterBoxes );
            dbg( "liboot: static world contains a degenerate triangle" );
            return 0;
        }
        n[0] /= len; n[1] /= len; n[2] /= len;
        float planeDist = -( n[0]*vf[0][0] + n[1]*vf[0][1] + n[2]*vf[0][2] );
        if( !isfinite( planeDist ) || planeDist < INT16_MIN || planeDist > INT16_MAX ) {
            free( vtx );
            free( poly );
            free( types );
            free( newWaterBoxes );
            dbg( "liboot: static-world collision plane exceeds signed 16-bit range" );
            return 0;
        }

        uint16_t surfType = surfaces[i].type;
        if( surfType >= OOT_SURFACE_PRESET_COUNT ) surfType = OOT_SURFACE_DEFAULT;
        poly[i].type = (u16)presetSlot[surfType];   /* < nTypes by construction */
        poly[i].flags_vIA = ( i * 3 + 0 ) & 0x1FFF;
        poly[i].flags_vIB = ( i * 3 + 1 ) & 0x1FFF;
        /* SurfaceType_IsFloorConveyor reads this poly bit (0x2000), not the
           SurfaceType word, so the conveyor preset must set both. */
        if( surfType == OOT_SURFACE_CONVEYOR ) poly[i].flags_vIB |= 0x2000u;
        poly[i].vIC       = ( i * 3 + 2 ) & 0x1FFF;
        poly[i].normal.x = (s16)( n[0] * 32767.0f );
        poly[i].normal.y = (s16)( n[1] * 32767.0f );
        poly[i].normal.z = (s16)( n[2] * 32767.0f );
        poly[i].dist = (s16)planeDist;
    }

    /* liboot v0.5: native water boxes. Only WaterBox_GetSurface1 ever reads
       them (Actor_UpdateBgCheckInfo's FLAG_2 block + Player's own probes);
       BgCheck_Allocate never touches the list, so attaching it to the header
       is the whole wiring. Neutral properties: bgCam/light are dead fields
       here, room 0x3F ("all rooms") works with the neutral room 0 used for a
       custom world, and FLAG_19 (inactive box) stays clear. */
    if( numWaterBoxes > 0 ) {
        for( uint32_t i = 0; i < numWaterBoxes; ++i ) {
            newWaterBoxes[i].xMin = waterBoxes[i].xMin;
            newWaterBoxes[i].ySurface = waterBoxes[i].ySurface;
            newWaterBoxes[i].zMin = waterBoxes[i].zMin;
            newWaterBoxes[i].xLength = waterBoxes[i].xLength;
            newWaterBoxes[i].zLength = waterBoxes[i].zLength;
            newWaterBoxes[i].properties = WATERBOX_PROPERTIES( 0, 0, WATERBOX_ROOM_ALL, 0 );
        }
    }

    CollisionHeader candidate;
    memset( &candidate, 0, sizeof( candidate ));
    candidate.minBounds = mins;
    candidate.maxBounds = maxs;
    candidate.numVertices = numSurfaces * 3;
    candidate.vtxList = vtx;
    candidate.numPolygons = numSurfaces;
    candidate.polyList = poly;
    candidate.surfaceTypeList = types;
    candidate.numWaterBoxes = (uint16_t)numWaterBoxes;
    candidate.waterBoxes = newWaterBoxes;

    /* BgCheck's retail SSNode allocator aborts on exhaustion. Preflight the
       candidate before replacing any live-world pointer so failure is fully
       transactional and the caller can keep simulating the previous world. */
    if( !liboot_bgcheck_preflight( &candidate )) {
        free( vtx );
        free( poly );
        free( types );
        free( newWaterBoxes );
        dbg( "liboot: static world exceeds collision lookup capacity" );
        return 0;
    }

    /* A successful custom-world load ends any active ROM scene. Keep this
       after every fallible validation/allocation step so a rejected candidate
       leaves the old scene and collision fully intact. */
    liboot_scene_terminate();
    liboot_gfx_evict_scene();

    free( s_state.colVtx );
    free( s_state.colPoly );
    free( s_state.colSurfaceTypes );
    free( s_state.colWaterBoxes );
    s_state.colVtx = vtx;
    s_state.colPoly = poly;
    s_state.colSurfaceTypes = types;
    s_state.colWaterBoxes = newWaterBoxes;
    s_state.numColWaterBoxes = (uint16_t)numWaterBoxes;
    s_state.colHeader = candidate;

    PlayState *play = liboot_play();
    /* The standalone runtime intentionally uses the neutral scene-0/default
       camera lookup budget for every host world. Clear stale ROM-scene values
       so allocation always matches liboot_bgcheck_preflight. */
    play->sceneId = SCENE_DEKU_TREE;
    R_SCENE_CAM_TYPE = SCENE_CAM_TYPE_DEFAULT;
    memset( &play->roomCtx.curRoom, 0, sizeof( play->roomCtx.curRoom ));
    memset( &play->roomCtx.prevRoom, 0, sizeof( play->roomCtx.prevRoom ));
    play->roomCtx.curRoom.num = 0;
    play->roomCtx.prevRoom.num = -1;
    play->msgCtx.disableWarpSongs = 0;
    gSaveContext.worldMapArea = 0;
    liboot_reset_tha(); /* z_bgcheck is the only THA user; drop its old tables */
    BgCheck_Allocate( &play->colCtx, play, &s_state.colHeader );
    liboot_invalidate_actor_bg_cache( play );

    /* same rationale as oot_scene_load: a new world is liboot's "scene
       transition" — drop the y < -4000 void-out transitionTrigger latch that
       would otherwise zero stick speed for every Player forever. */
    play->transitionTrigger = TRANS_TRIGGER_OFF;
    /* A host/custom world carries no scene light settings; fall back to the
       renderer's neutral daylight so leaving a dungeon does not keep its dark
       shade (scene loads install their own via apply_scene_environment). */
    liboot_gfx_set_lights( NULL, NULL, NULL, NULL, NULL, 0 );
    return 1;
}

void oot_static_world_load( const struct OoTSurface *surfaces, uint32_t numSurfaces,
                            const struct OoTWaterBox *waterBoxes, uint32_t numWaterBoxes )
{
    (void)liboot_static_world_load_checked( surfaces, numSurfaces,
                                            waterBoxes, numWaterBoxes );
}

void oot_static_surfaces_load( const struct OoTSurface *surfaces, uint32_t numSurfaces )
{
    oot_static_world_load( surfaces, numSurfaces, NULL, 0 );
}

/* liboot v0.5: emulated Z-lock camera.

   Player converts the stick to a world direction with
   "stick angle + Camera_GetInputDirYaw" (z_player.c:4145), so the strafe
   orbit around a lock-on target only emerges when the camera swings around
   the target with Link (the real game's Camera_Battle1/KeepOn1). With a
   static host camera, holding the stick sideways makes Link run off on a
   straight tangent until the ~429-unit leash drops the lock. A pure
   follow camera on the target axis is not enough either: the walk action
   (Player_Action_8084193C) chases its yaw target proportionally
   (temp3 * 0.1 per tick), so against a rotating tangent command Link keeps
   a constant angular lag that tilts his path outward and he spirals away.
   The real game closes that loop with battle-camera swing dynamics plus
   the player's own micro corrections.

   liboot has no camera system, so while a lock-on focus is held and Link
   is moving on the ground the camera yaw fed to Player is emulated: it
   swings onto the Link->target axis, tilted by the strafe-weighted
   distance error so the lock-time distance is held (the emergent behavior
   of the real lock-on camera). The swing rides on top of the host camera
   yaw and decays back to it whenever the stick is neutral, Link is
   airborne, or the lock drops, so the host camera stays authoritative
   everywhere else. */
#define LOCKCAM_GAIN       500.0f /* binang of inward tilt per unit of distance error */
#define LOCKCAM_TILT_MAX   6144.0f /* +-33.7 deg */
#define LOCKCAM_SWING_STEP 2000   /* binang/tick swing rate while orbiting */
#define LOCKCAM_DECAY_STEP 3000   /* binang/tick swing decay toward the host camera */

static struct {
    Actor *focus;
    float lockDist;
    s16 swing;
} s_lockCam;

static s16 lockcam_step_yaw( s16 cur, s16 target, s16 step )
{
    s16 diff = (s16)( target - cur );
    if( diff > step )  return (s16)( cur + step );
    if( diff < -step ) return (s16)( cur - step );
    return target;
}

static s16 lockcam_yaw( Player *player, const struct OoTLinkInputs *inputs, s16 hostYaw )
{
    Actor *focus = player->focusActor;
    float mag = sqrtf( inputs->stickX * inputs->stickX + inputs->stickY * inputs->stickY );

    if( focus != NULL ) {
        float dx = focus->focus.pos.x - player->actor.world.pos.x;
        float dz = focus->focus.pos.z - player->actor.world.pos.z;
        float dist = sqrtf( dx * dx + dz * dz );
        if( focus != s_lockCam.focus ) { /* fresh lock: remember the orbit radius */
            s_lockCam.focus = focus;
            s_lockCam.lockDist = dist;
        }
        if( mag > 0.1f && ( player->actor.bgCheckFlags & 1 /* BGCHECKFLAG_GROUND */ )) {
            s16 axis = (s16)( atan2f( dx, dz ) * 32768.0f / (float)M_PI );
            /* inward tilt ~ distance error, scaled by how sideways the stick
               is pointing so straight approach/retreat stays possible */
            float tilt = ( dist - s_lockCam.lockDist ) * LOCKCAM_GAIN * ( fabsf( inputs->stickX ) / mag );
            if( tilt > LOCKCAM_TILT_MAX )  tilt = LOCKCAM_TILT_MAX;
            if( tilt < -LOCKCAM_TILT_MAX ) tilt = -LOCKCAM_TILT_MAX;
            /* orbit sense from the current movement yaw, so the tilt bends
               the path toward the target for either strafe direction */
            s16 sense = ( (s16)( player->yaw - axis ) >= 0 ) ? 1 : -1;
            s16 desired = (s16)( axis - sense * (s16)tilt );
            s_lockCam.swing = (s16)( lockcam_step_yaw( (s16)( hostYaw + s_lockCam.swing ),
                                                       desired, LOCKCAM_SWING_STEP ) - hostYaw );
            return (s16)( hostYaw + s_lockCam.swing );
        }
    } else {
        s_lockCam.focus = NULL;
    }
    s_lockCam.swing = lockcam_step_yaw( s_lockCam.swing, 0, LOCKCAM_DECAY_STEP );
    return (s16)( hostYaw + s_lockCam.swing );
}

int32_t oot_link_create( float x, float y, float z )
{
    if( !s_state.inited || s_state.player ||
        !liboot_link_skeleton_valid((uint8_t)gSaveContext.save.linkAge )) return -1;

    s_state.player = calloc( 1, sizeof( Player ));
    if( s_state.player == NULL ) {
        dbg( "liboot: unable to allocate Link" );
        return -1;
    }
    memset( &s_lockCam, 0, sizeof( s_lockCam ));
    s_linkFrozen = false;   /* a freshly created Link is never born frozen */
    liboot_link_init( liboot_play(), s_state.player, x, y, z );
    return 0;
}

/* PadUtils_UpdateRelXY: 7-unit deadzone, clamped to +/-60 */
static s8 pad_rescale( s32 cur )
{
    if( cur > 7 )  return ( cur < 0x43 ) ? cur - 7 : 0x43 - 7;
    if( cur < -7 ) return ( cur > -0x43 ) ? cur + 7 : -0x43 + 7;
    return 0;
}

/* liboot vNEXT: map the live Player action function to a stable OoTAction id.
   Only the decomp's named action functions are classified (the rest report
   OOT_ACTION_OTHER). All of these are non-static symbols in z_player.c, the
   same linkage load_assets.c already relies on for ArmsHook_Wait. */
extern void Player_Action_Idle( Player*, PlayState* );
extern void Player_Action_TurnInPlace( Player*, PlayState* );
extern void Player_Action_Roll( Player*, PlayState* );
extern void Player_Action_Talk( Player*, PlayState* );
extern void Player_Action_SwingBottle( Player*, PlayState* );
extern void Player_Action_ExchangeItem( Player*, PlayState* );
extern void Player_Action_HookshotFly( Player*, PlayState* );
extern void Player_Action_SlideOnSlope( Player*, PlayState* );
extern void Player_Action_TryOpeningDoor( Player*, PlayState* );
extern void Player_Action_ExitGrotto( Player*, PlayState* );
extern void Player_Action_CsAction( Player*, PlayState* );
extern void Player_Action_WaitForCutscene( Player*, PlayState* );
extern void Player_Action_WaitForPutAway( Player*, PlayState* );
extern void Player_Action_BlueWarpArrive( Player*, PlayState* );
extern void Player_Action_FaroresWindArrive( Player*, PlayState* );
extern void Player_Action_StartWarpSongArrive( Player*, PlayState* );
extern void Player_Action_TimeTravelEnd( Player*, PlayState* );

static uint32_t liboot_action_id( PlayerActionFunc fn )
{
    static const struct { PlayerActionFunc fn; uint32_t id; } map[] = {
        { Player_Action_Idle,               OOT_ACTION_IDLE },
        { Player_Action_TurnInPlace,        OOT_ACTION_TURN_IN_PLACE },
        { Player_Action_Roll,               OOT_ACTION_ROLL },
        { Player_Action_Talk,               OOT_ACTION_TALK },
        { Player_Action_SwingBottle,        OOT_ACTION_SWING_BOTTLE },
        { Player_Action_ExchangeItem,       OOT_ACTION_EXCHANGE_ITEM },
        { Player_Action_HookshotFly,        OOT_ACTION_HOOKSHOT_FLY },
        { Player_Action_SlideOnSlope,       OOT_ACTION_SLIDE_ON_SLOPE },
        { Player_Action_TryOpeningDoor,     OOT_ACTION_TRY_OPENING_DOOR },
        { Player_Action_ExitGrotto,         OOT_ACTION_EXIT_GROTTO },
        { Player_Action_CsAction,           OOT_ACTION_CS_ACTION },
        { Player_Action_WaitForCutscene,    OOT_ACTION_WAIT_FOR_CUTSCENE },
        { Player_Action_WaitForPutAway,     OOT_ACTION_WAIT_FOR_PUT_AWAY },
        { Player_Action_BlueWarpArrive,     OOT_ACTION_BLUE_WARP_ARRIVE },
        { Player_Action_FaroresWindArrive,  OOT_ACTION_FARORES_WIND_ARRIVE },
        { Player_Action_StartWarpSongArrive, OOT_ACTION_START_WARP_SONG_ARRIVE },
        { Player_Action_TimeTravelEnd,      OOT_ACTION_TIME_TRAVEL_END },
    };
    if( fn != NULL )
        for( size_t i = 0; i < sizeof( map ) / sizeof( map[0] ); ++i )
            if( fn == map[i].fn ) return map[i].id;
    return OOT_ACTION_OTHER;
}

void oot_link_tick( int32_t linkId,
                    const struct OoTLinkInputs *inputs,
                    struct OoTLinkState *outState,
                    struct OoTLinkGeometryBuffers *outBuffers )
{
    if( linkId != 0 || !s_state.player || inputs == NULL ) return;
    Player *player = s_state.player;
    PlayState *play = liboot_play();

    /* camera yaw drives stick->world rotation inside Player (ANALYSIS §4.3);
       while a lock-on target is held it swings with the emulated Z-lock
       camera so the real strafe-orbit behavior emerges (liboot v0.5) */
    {
        s16 hostYaw = (s16)( atan2f( inputs->camLookX, inputs->camLookZ ) * 32768.0f / (float)M_PI );
        liboot_cam_yaw = lockcam_yaw( player, inputs, hostYaw );
    }

    Input *input = &play->state.input[0];
    OSContPad prev = input->cur;
    memset( input, 0, sizeof( *input ));
    /* scale to the hardware range PadUtils_UpdateRelXY expects (deadzone 7,
       clamp 0x43), so rel reaches the full +/-60 the movement code uses */
    input->cur.stick_x = (s8)( inputs->stickX * 67.0f );
    input->cur.stick_y = (s8)( inputs->stickY * 67.0f );
    input->rel.stick_x = pad_rescale( input->cur.stick_x );
    input->rel.stick_y = pad_rescale( input->cur.stick_y );
    input->prev = prev;
    if( inputs->buttonA ) input->cur.button |= BTN_A;
    if( inputs->buttonB ) input->cur.button |= BTN_B;
    if( inputs->buttonZ ) input->cur.button |= BTN_Z;
    if( inputs->buttonR ) input->cur.button |= BTN_R;
    /* liboot v0.6: the item button rides C-left — oot_link_use_item mirrors
       the active item onto buttonItems[1] (= C_BTN_ITEM(0)), which is what
       Player_ProcessItemButtons scans for press/hold/release semantics */
    if( inputs->buttonItem ) input->cur.button |= BTN_CLEFT;
    /* liboot vNEXT: C-up drives the real first-person / look-around path. */
    if( inputs->buttonCUp ) input->cur.button |= BTN_CUP;
    input->press.button = input->cur.button & ~prev.button;
    input->rel.button = prev.button & ~input->cur.button;

    /* liboot vNEXT: while frozen, hold the Player pose (skip the update) but
       still report state and rebuild geometry below so a paused Link renders. */
    if( !s_linkFrozen )
        liboot_link_update( play, player );

    if( outState ) {
        outState->position[0] = player->actor.world.pos.x;
        outState->position[1] = player->actor.world.pos.y;
        outState->position[2] = player->actor.world.pos.z;
        outState->velocity[0] = player->actor.velocity.x;
        outState->velocity[1] = player->actor.velocity.y;
        outState->velocity[2] = player->actor.velocity.z;
        outState->faceAngle = player->actor.shape.rot.y;
        outState->linearVelocity = player->speedXZ;
        outState->health = gSaveContext.save.info.playerData.health;
        outState->healthCapacity = gSaveContext.save.info.playerData.healthCapacity;
        outState->magic = gSaveContext.save.info.playerData.magic;
        outState->magicLevel = gSaveContext.save.info.playerData.magicLevel;
        outState->age = (uint8_t)gSaveContext.save.linkAge;
        outState->isDead = ( gSaveContext.save.info.playerData.health <= 0 ) ||
                           (( player->stateFlags1 & PLAYER_STATE1_DEAD ) != 0 );
        outState->heldItemAction = player->heldItemAction;
        outState->meleeWeaponState = (uint8_t)player->meleeWeaponState;
        outState->stateFlags1 = player->stateFlags1;
        outState->stateFlags2 = player->stateFlags2;
        outState->animId = liboot_player_anim_id( player->skelAnime.animation );
        outState->animFrame = player->skelAnime.curFrame;
        /* liboot v0.3: Z-targeting lock state (real Attention system) */
        if( player->focusActor != NULL ) {
            outState->lockOnActive = 1;
            outState->lockOnPos[0] = player->focusActor->focus.pos.x;
            outState->lockOnPos[1] = player->focusActor->focus.pos.y;
            outState->lockOnPos[2] = player->focusActor->focus.pos.z;
        } else {
            outState->lockOnActive = 0;
            outState->lockOnPos[0] = outState->lockOnPos[1] = outState->lockOnPos[2] = 0.0f;
        }
        /* liboot v0.5: water state. depthInWater = surface - pos.y whenever a
           box covers the position, so the surface is recoverable from it. */
        outState->inWater = ( player->actor.bgCheckFlags & BGCHECKFLAG_WATER ) ? 1 : 0;
        outState->waterSurfaceY = outState->inWater
            ? player->actor.world.pos.y + player->actor.depthInWater : 0.0f;
        /* liboot vNEXT: appended Link state, all computed by the real Player. */
        outState->action = liboot_action_id( player->actionFunc );
        outState->attackAnim = (uint8_t)player->meleeWeaponAnimation;
        outState->stateFlags3 = player->stateFlags3;
        outState->lookPitch = player->actor.focus.rot.x;
        outState->lookYaw = player->actor.focus.rot.y;
        outState->floorSfxOffset = player->floorSfxOffset;
        outState->underwaterTimer = player->underwaterTimer;
    }
    if( outBuffers )
        liboot_render_link( play, player, outBuffers );
}

extern void liboot_despawn_actors( PlayState *play );

/* Player_Init allocates giObjectSegment from the malloc-backed arena and
   liboot has no Player_Destroy path — release it on delete / age re-init.
   The stored pointer was aligned up ((base + 8) & ~0xF), so free the base. */
static void free_player_allocs( Player *player )
{
    /* glibc malloc is 16-aligned, so Player_Init's (base+8)&~0xF alignment
       lands back on the malloc base and the stored pointer frees directly. */
    if( player && player->giObjectSegment )
        free( player->giObjectSegment );
}

void oot_link_delete( int32_t linkId )
{
    if( linkId != 0 ) return;
    if( s_state.player ) {
        PlayState *play = liboot_play();
        /* liboot v0.4/v0.6: no dangling EnElf or projectile actors */
        liboot_despawn_actors( play );
        /* liboot_link_init points the PLAYER list head at the Player being
           freed below and only the next oot_link_create re-points it; drop it
           so GET_PLAYER users (equip.c liboot_get_link) see "no player"
           instead of reading freed memory. */
        play->actorCtx.actorLists[ACTORCAT_PLAYER].head = NULL;
        free_player_allocs( s_state.player );
    }
    free( s_state.player );
    s_state.player = NULL;
    s_linkFrozen = false;
}

/* liboot vNEXT: reposition Link in place. Snap prevPos/home to the new spot so
   the next update does not treat the move as a one-frame velocity spike. */
bool oot_link_set_pose( int32_t linkId, float x, float y, float z, int16_t yaw )
{
    if( linkId != 0 || !s_state.player ) return false;
    Player *player = s_state.player;
    player->actor.world.pos.x = x;
    player->actor.world.pos.y = y;
    player->actor.world.pos.z = z;
    player->actor.prevPos = player->actor.world.pos;
    player->actor.home.pos = player->actor.world.pos;
    player->actor.shape.rot.y = yaw;
    player->actor.world.rot.y = yaw;
    return true;
}

void oot_link_freeze( int32_t linkId, bool frozen )
{
    if( linkId != 0 ) return;
    s_linkFrozen = frozen;
}

void oot_link_set_invincible( int32_t linkId, int8_t frames )
{
    if( linkId != 0 || !s_state.player ) return;
    /* The game's own field: positive = intangibility, negative = invulnerability.
       It is decremented toward zero, so a host must re-apply to hold it. */
    s_state.player->invincibilityTimer = frames;
}

/* liboot vNEXT: downward raycast against the live collision world. */
bool oot_scene_query_surface( float x, float y, float z, struct OoTSurfaceInfo *outInfo )
{
    if( outInfo ) memset( outInfo, 0, sizeof( *outInfo ));
    if( !s_state.inited ) return false;
    PlayState *play = liboot_play();
    /* No static lookup is allocated until a static world or scene is loaded.
       Raycasting an all-zero colCtx would loop forever (subdivLength.y == 0)
       or deref a wild lookup pointer, so refuse the query until a world exists. */
    if( play->colCtx.lookupTbl == NULL ) return false;
    CollisionPoly *poly = NULL;
    s32 bgId = 0;
    Vec3f pos = { x, y, z };
    f32 groundY = BgCheck_EntityRaycastDown3( &play->colCtx, &poly, &bgId, &pos );
    if( poly == NULL || groundY <= BGCHECK_Y_MIN ) return false;
    if( outInfo ) {
        outInfo->groundY = groundY;
        outInfo->floorType = SurfaceType_GetFloorType( &play->colCtx, poly, bgId );
        outInfo->material = SurfaceType_GetMaterial( &play->colCtx, poly, bgId );
        outInfo->hookshot = SurfaceType_CanHookshot( &play->colCtx, poly, bgId ) ? 1u : 0u;
    }
    return true;
}

bool oot_link_set_age( int32_t linkId, uint8_t age )
{
    if( linkId != 0 || !s_state.player || age > OOT_AGE_CHILD ) return false;
    if( age == OOT_AGE_CHILD &&
        ( !s_state.childBlob || !liboot_link_skeleton_valid( age ))) return false;
    if( gSaveContext.save.linkAge == age ) return true;

    PlayState *play = liboot_play();
    Player *player = s_state.player;
    float x = player->actor.world.pos.x;
    float y = player->actor.world.pos.y;
    float z = player->actor.world.pos.z;

    gSaveContext.save.linkAge = age;
    if( age == OOT_AGE_CHILD ) {
        liboot_register_segment_span( 6, s_state.childBlob, s_state.childSize );
        play->objectCtx.slots[0].id = OBJECT_LINK_CHILD;
    } else {
        liboot_register_segment_span( 6, s_state.objectBlob, s_state.objectSize );
        play->objectCtx.slots[0].id = OBJECT_LINK_BOY;
    }

    /* age changes the whole model group + properties: re-init in place */
    free_player_allocs( player );
    memset( player, 0, sizeof( *player ));
    liboot_link_init( play, player, x, y, z );
    return true;
}

void oot_link_set_health( int32_t linkId, int16_t health, int16_t capacity )
{
    if( linkId != 0 ) return;
    if( capacity > 0 ) gSaveContext.save.info.playerData.healthCapacity = capacity;
    if( health >= 0 ) {
        int16_t cap = gSaveContext.save.info.playerData.healthCapacity;
        gSaveContext.save.info.playerData.health = health > cap ? cap : health;
    }
}

void oot_link_set_magic( int32_t linkId, uint8_t level, int16_t amount )
{
    if( linkId != 0 || level > 2 ) return;
    gSaveContext.save.info.playerData.isMagicAcquired = level >= 1;
    gSaveContext.save.info.playerData.isDoubleMagicAcquired = level >= 2;
    gSaveContext.magicCapacity = level * 0x30;
    gSaveContext.save.info.playerData.magicLevel = level;
    int16_t cap = level * 0x30;
    gSaveContext.save.info.playerData.magic = amount > cap ? cap : ( amount < 0 ? 0 : amount );
}

/* liboot v0.4: Navi state export (vendored EnElf layout) */
#include "decomp/src/overlays/actors/ovl_En_Elf/z_en_elf.h"

bool oot_navi_get( float outPos[3], float outInnerColor[4], float outOuterColor[4], float *outScale )
{
    if( !s_state.player ) return false;
    Actor *actor = s_state.player->naviActor;
    if( actor == NULL || actor->id != ACTOR_EN_ELF ) return false;
    EnElf *navi = (EnElf *)actor;

    if( outPos ) {
        outPos[0] = actor->world.pos.x;
        outPos[1] = actor->world.pos.y;
        outPos[2] = actor->world.pos.z;
    }
    if( outInnerColor ) {
        outInnerColor[0] = navi->innerColor.r / 255.0f;
        outInnerColor[1] = navi->innerColor.g / 255.0f;
        outInnerColor[2] = navi->innerColor.b / 255.0f;
        outInnerColor[3] = navi->innerColor.a / 255.0f;
    }
    if( outOuterColor ) {
        outOuterColor[0] = navi->outerColor.r / 255.0f;
        outOuterColor[1] = navi->outerColor.g / 255.0f;
        outOuterColor[2] = navi->outerColor.b / 255.0f;
        outOuterColor[3] = navi->outerColor.a / 255.0f;
    }
    if( outScale ) {
        /* hidden states (tucked under the hat / flagged invisible) report 0 */
        bool hidden = ( navi->unk_2A8 == 8 ) || ( navi->fairyFlags & 8 );
        *outScale = hidden ? 0.0f : actor->scale.x;
    }
    return true;
}

/* liboot v0.6: snapshot of every live non-Player actor (projectiles, Navi,
   fake targets) for host placement/telemetry. */
int32_t oot_actor_query( struct OoTActorInfo *out, int32_t maxCount )
{
    if( !s_state.inited || out == NULL || maxCount <= 0 ) return 0;

    PlayState *play = liboot_play();
    int32_t n = 0;

    for( int cat = 0; cat < ACTORCAT_MAX && n < maxCount; cat++ ) {
        if( cat == ACTORCAT_PLAYER ) continue;
        for( Actor *a = play->actorCtx.actorLists[cat].head; a != NULL && n < maxCount; a = a->next ) {
            out[n].id = a->id;
            out[n].category = a->category;
            out[n].params = a->params;
            out[n].yaw = a->shape.rot.y;
            out[n].active = ( a->update != NULL );
            out[n].pos[0] = a->world.pos.x;
            out[n].pos[1] = a->world.pos.y;
            out[n].pos[2] = a->world.pos.z;
            n++;
        }
    }
    return n;
}

/* liboot vNEXT: strictly-guarded standalone actor placement. Actor_Spawn indexes
   gActorOverlayTable[actorId] with NO bounds check, so the id is validated
   against a tiny allowlist BEFORE the call. Only actors that are safe without a
   Player parent / shooter are permitted (currently just the bomb). */
int32_t oot_actor_spawn( int16_t actorId, float x, float y, float z,
                         int16_t rotX, int16_t rotY, int16_t rotZ, int16_t params )
{
    if( !s_state.inited || s_state.player == NULL ) return -1;
    static const int16_t kSpawnAllow[] = { ACTOR_EN_BOM };
    bool allowed = false;
    for( size_t i = 0; i < sizeof( kSpawnAllow ) / sizeof( kSpawnAllow[0] ); ++i )
        if( kSpawnAllow[i] == actorId ) { allowed = true; break; }
    if( !allowed ) return -2;
    PlayState *play = liboot_play();
    Actor *a = Actor_Spawn( &play->actorCtx, play, actorId,
                            x, y, z, rotX, rotY, rotZ, params );
    return a != NULL ? 0 : -3;
}

/* liboot vNEXT: the twelve ocarina songs as note-index sequences (0..4 =
   A/C-down/C-right/C-left/C-up, matching oot_get_ocarina_note and the game's
   OCARINA_BTN_* values), in OcarinaSongId order. The first six are the warp
   songs. Canonical retail sequences. */
static const struct { uint8_t len; uint8_t notes[8]; } kOcarinaSongs[OOT_SONG_COUNT] = {
    [OOT_SONG_MINUET]   = { 6, { 0, 4, 3, 2, 3, 2 } },
    [OOT_SONG_BOLERO]   = { 8, { 1, 0, 1, 0, 2, 1, 2, 1 } },
    [OOT_SONG_SERENADE] = { 5, { 0, 1, 2, 2, 3 } },
    [OOT_SONG_REQUIEM]  = { 6, { 0, 1, 0, 2, 1, 0 } },
    [OOT_SONG_NOCTURNE] = { 7, { 3, 2, 2, 0, 3, 2, 1 } },
    [OOT_SONG_PRELUDE]  = { 6, { 4, 2, 4, 2, 3, 4 } },
    [OOT_SONG_SARIAS]   = { 6, { 1, 2, 3, 1, 2, 3 } },
    [OOT_SONG_EPONAS]   = { 6, { 4, 3, 2, 4, 3, 2 } },
    [OOT_SONG_LULLABY]  = { 6, { 3, 4, 2, 3, 4, 2 } },
    [OOT_SONG_SUNS]     = { 6, { 2, 1, 4, 2, 1, 4 } },
    [OOT_SONG_TIME]     = { 6, { 2, 0, 1, 2, 0, 1 } },
    [OOT_SONG_STORMS]   = { 6, { 0, 1, 4, 0, 1, 4 } },
};

bool oot_ocarina_song_notes( int32_t song, uint8_t outNotes[8], int32_t *outCount )
{
    if( song < 0 || song >= OOT_SONG_COUNT || outNotes == NULL ) return false;
    uint8_t n = kOcarinaSongs[song].len;
    memcpy( outNotes, kOcarinaSongs[song].notes, n );
    if( outCount ) *outCount = n;
    return true;
}

int32_t oot_ocarina_match( const uint8_t *notes, int32_t count )
{
    int32_t best = -1, bestLen = 0;
    if( notes == NULL || count <= 0 ) return -1;
    for( int32_t s = 0; s < OOT_SONG_COUNT; ++s ) {
        int32_t len = kOcarinaSongs[s].len;
        if( count < len ) continue;
        const uint8_t *tail = notes + ( count - len );
        int ok = 1;
        for( int32_t i = 0; i < len; ++i )
            if( tail[i] != kOcarinaSongs[s].notes[i] ) { ok = 0; break; }
        if( ok && len > bestLen ) { best = s; bestLen = len; }  /* prefer the longest match */
    }
    return best;
}

extern s32 Player_InflictDamage( PlayState *play, s32 damage );
extern bool oot_link_get_skeleton_impl( Player *player, struct OoTSkeletonPose *out );

bool oot_link_get_skeleton( int32_t linkId, struct OoTSkeletonPose *out )
{
    if( linkId != 0 || !s_state.player || !out ) return false;
    return oot_link_get_skeleton_impl( s_state.player, out );
}

void oot_link_damage( int32_t linkId, int16_t amount )
{
    if( linkId != 0 || !s_state.player || amount <= 0 ) return;
    Player_InflictDamage( liboot_play(), -amount );
}
