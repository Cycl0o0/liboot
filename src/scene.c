/* liboot v0.7: real scene loading from the caller's ROM.
 *
 * Locates gSceneTable inside the 'code' file by structural fingerprint (the
 * longest run of 0x14-byte entries whose scene vrom pair exactly matches a
 * dmadata file — 101 entries on PAL retail), extracts the requested scene +
 * room files (Yaz0), relocates the scene's serialized CollisionHeader into
 * native byteswapped tables (including water boxes) and hands the room mesh
 * display lists to the F3DZEX2 interpreter (segments 2/3).
 *
 * Everything is re-derived from the ROM at runtime — no version constants
 * beyond the serialized formats themselves.
 */
#include "liboot.h"
#include "rom_util.h"
#include "scene_header.h"
#include "scene_room_image.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ultra64.h"
#include "bgcheck.h"
#include "play_state.h"
#include "player.h"
#include "regs.h"
#include "save.h"
#include "scene.h"

/* shim / core hooks */
extern PlayState *liboot_play( void );
extern void liboot_reset_tha( void );
extern void liboot_invalidate_actor_bg_cache( PlayState *play );
extern void liboot_register_segment_span( int seg, void *base, size_t size );
extern const uint8_t *liboot_rom( size_t *outSize );

/* gfx_adapter scene entry points */
extern void liboot_scene_dl_begin( struct OoTLinkGeometryBuffers *out, int maxTris );
extern void liboot_scene_dl_run( uint32_t dlAddr );
extern void liboot_scene_dl_set_source( int roomIdx, int pass );
extern void liboot_gfx_evict_scene( void );
extern void liboot_gfx_set_room( int roomIdx );   /* per-room texture-cache key */
extern void liboot_gfx_set_lights( const float ambient[3], const float dir0[3],
                                   const float col0[3], const float dir1[3],
                                   const float col1[3], int count );
extern uint32_t liboot_scene_material_reference_count( void );
extern bool liboot_scene_material_reference_get(
    uint32_t index, struct OoTSceneMaterialReference *outReference );
extern uint16_t liboot_scene_material_segment_mask( void );
extern bool liboot_scene_material_references_truncated( void );
extern void liboot_dynamic_collision_rebind_all( void );
extern void liboot_host_actor_sync_room( void );

/* Internal z_bgcheck helpers used to perform the same subdivision walk as
   BgCheck_Allocate without allocating or mutating the live CollisionContext. */
extern void BgCheck_SetSubdivisionDimension( f32 min, s32 subdivAmount, f32 *max,
                                             f32 *subdivLength, f32 *subdivLengthInv );
extern void BgCheck_GetPolySubdivisionBounds( CollisionContext *colCtx, Vec3s *vtxList,
                                               CollisionPoly *polyList,
                                               s32 *subdivMinX, s32 *subdivMinY,
                                               s32 *subdivMinZ, s32 *subdivMaxX,
                                               s32 *subdivMaxY, s32 *subdivMaxZ,
                                               s16 polyId );
extern s32 BgCheck_PolyIntersectsSubdivision( Vec3f *min, Vec3f *max,
                                              CollisionPoly *polyList,
                                              Vec3s *vtxList, s16 polyId );

static u32 be32( const u8 *p ) { return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]; }
static u16 be16u( const u8 *p ) { return (u16)((u16)p[0]<<8 | p[1]); }
static s16 be16s( const u8 *p ) { return (s16)be16u( p ); }

/* ---- dmadata vrom-pair index (sorted, for the table fingerprint scan) --- */
typedef struct {
    u32 vromStart, vromEnd;
    u32 index;
} DmaPair;

#define SCENE_TABLE_ENTRY  0x14
#define SCENE_TABLE_MIN    90      /* PAL retail has 101 entries */
#define SCENE_TABLE_MAX    512
#define CODE_SCAN_DMA_MAX  64
#define CODE_MIN_SIZE      0xC0000
#define CODE_MAX_SIZE      0x300000
#define DMA_PAIR_MAX       65536u

static struct {
    /* scene table (copied out of 'code') */
    bool tableReady;
    u8 *table;
    int tableCount;
    DmaPair *pairs;                /* sorted by vromStart */
    int numPairs;
    /* current scene/room files (segments 2/3) */
    u8 *sceneBlob; size_t sceneSize;
    u8 *roomBlob;  size_t roomSize;
    int32_t curScene, curRoom;
    uint8_t layer;
    bool layerFallback;
    uint8_t drawConfigId;
    /* scene header facts */
    u32 playerEntryOff, spawnListOff;   /* 0 = absent */
    int numRooms;
    int numDoors;                       /* liboot vNEXT: transition actors */
    u32 doorListOff;                    /* seg2 offset into sceneBlob */
    int seqId, ambienceId;              /* liboot vNEXT: bgm + ambience ids, -1 = absent */
    bool roomMetadataValid;             /* active header carried cmds 0x08 + 0x16 */
    struct OoTSceneEnvironment env;     /* liboot vNEXT: active light/fog settings */
    struct OoTSceneActorEntry *actors;  /* effective scene + loaded-room cmd 0x01 */
    uint32_t numActors;
    struct OoTSceneBackground *backgrounds;
    uint32_t numBackgrounds;
    /* relocated native collision */
    Vec3s *colVtx;
    CollisionPoly *colPoly;
    SurfaceType *colSurf;
    SurfaceType *colRawSurf;
    uint32_t numColSurf;
    int16_t *exits;
    uint8_t numExits;
    WaterBox *colWb;
    CollisionHeader colHeader;
    /* lib-owned interpreted room geometry (opa first, then xlu) */
    float *pos, *nrm, *col, *uv;
    uint16_t *triTex;
    float *alpha;   /* liboot vNEXT: per-vertex shade alpha, parallel to col */
    uint8_t *triFlags;  /* liboot vNEXT: per-triangle render flags (OoTTriangleFlags) */
    struct OoTGeometryBatch *batches;
    uint32_t numTris, xluStart;
    uint32_t numBatches;
    uint32_t allocatedGeometryCapacity;
} s_scene;

#define BACKGROUND_CATALOG_MAX_BYTES ( 256u * 1024u * 1024u )

typedef struct BackgroundCatalog
{
    struct OoTSceneBackground *entries;
    uint32_t count;
    uint32_t capacity;
    size_t ownedBytes;
} BackgroundCatalog;

/* Kept outside s_scene so replacing a ROM scene with a custom static world
   does not silently discard the engine's configured capacity. */
static uint32_t sSceneGeometryCapacity = OOT_SCENE_MAX_TRIANGLES;

#define WORLD_EVENT_QUEUE_CAPACITY 8u
static struct {
    struct OoTWorldEvent queue[WORLD_EVENT_QUEUE_CAPACITY];
    uint64_t nextSequence;
    uint32_t head;
    uint32_t count;
    uint32_t contactKind;
    int16_t contactExitIndex;
    uint8_t contactFloorProperty;
} s_worldEvents;

static void world_events_clear( void )
{
    s_worldEvents.head = 0u;
    s_worldEvents.count = 0u;
    s_worldEvents.contactKind = 0u;
    s_worldEvents.contactExitIndex = 0;
    s_worldEvents.contactFloorProperty = 0u;
}

static bool world_event_push( uint32_t kind, int16_t exitIndex,
                              int16_t entranceIndex, uint8_t floorProperty,
                              const PlayState *play, const Player *player )
{
    struct OoTWorldEvent *event;
    uint32_t tail;
    if( s_worldEvents.count >= WORLD_EVENT_QUEUE_CAPACITY ||
        play == NULL || player == NULL )
        return false;
    tail = ( s_worldEvents.head + s_worldEvents.count ) % WORLD_EVENT_QUEUE_CAPACITY;
    event = &s_worldEvents.queue[tail];
    memset( event, 0, sizeof( *event ));
    event->structSize = sizeof( *event );
    event->version = OOT_WORLD_EVENT_VERSION;
    if( ++s_worldEvents.nextSequence == 0u )
        s_worldEvents.nextSequence = 1u;
    event->sequence = s_worldEvents.nextSequence;
    event->kind = kind;
    event->sceneIndex = s_scene.sceneBlob ? s_scene.curScene : -1;
    event->roomIndex = s_scene.sceneBlob ? play->roomCtx.curRoom.num : -1;
    event->exitIndex = exitIndex;
    event->entranceIndex = entranceIndex;
    event->floorProperty = floorProperty;
    event->layer = s_scene.sceneBlob ? s_scene.layer : OOT_SCENE_LAYER_CHILD_DAY;
    event->position[0] = player->actor.world.pos.x;
    event->position[1] = player->actor.world.pos.y;
    event->position[2] = player->actor.world.pos.z;
    s_worldEvents.count++;
    return true;
}

/* ------------------------------------------------------------------ */
static int pair_cmp( const void *a, const void *b )
{
    const DmaPair *pa = a, *pb = b;
    if( pa->vromStart != pb->vromStart )
        return pa->vromStart < pb->vromStart ? -1 : 1;
    return pa->vromEnd < pb->vromEnd ? -1 : ( pa->vromEnd > pb->vromEnd ? 1 : 0 );
}

/* exact (vromStart, vromEnd) -> dma index, or -1 */
static int dma_index_for_vrom( u32 s, u32 e )
{
    int lo = 0, hi = s_scene.numPairs - 1;
    while( lo <= hi ) {
        int mid = ( lo + hi ) / 2;
        const DmaPair *p = &s_scene.pairs[mid];
        if( p->vromStart == s && p->vromEnd == e ) return (int)p->index;
        if( p->vromStart < s || ( p->vromStart == s && p->vromEnd < e )) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static bool build_dma_pairs( const u8 *rom, size_t romSize )
{
    const u8 *dmadata = rom_find_dmadata( rom, romSize );
    LibootDmaEntry e;
    u32 n = 0;

    if( !dmadata ) return false;
    while( n < DMA_PAIR_MAX && dma_get( rom, romSize, dmadata, n, &e ))
        n++;
    /* The explicit cap keeps both the allocation and the later int count in
       range on every supported (32- or 64-bit) host. */
    if( n == 0 || n == DMA_PAIR_MAX )
        return false;

    free( s_scene.pairs );
    s_scene.pairs = malloc( n * sizeof( DmaPair ));
    if( !s_scene.pairs ) return false;
    for( u32 i = 0; i < n; ++i ) {
        if( !dma_get( rom, romSize, dmadata, i, &e )) {
            free( s_scene.pairs );
            s_scene.pairs = NULL;
            return false;
        }
        s_scene.pairs[i].vromStart = e.vromStart;
        s_scene.pairs[i].vromEnd = e.vromEnd;
        s_scene.pairs[i].index = i;
    }
    s_scene.numPairs = (int)n;
    qsort( s_scene.pairs, n, sizeof( DmaPair ), pair_cmp );
    return true;
}

/* one candidate gSceneTable entry: scene vrom pair must be an exact dmadata
   file, title pair must be 0/0 or an exact dmadata file too */
static bool scene_entry_valid( const u8 *p )
{
    u32 s = be32( p ), e = be32( p + 4 );
    u32 ts = be32( p + 8 ), te = be32( p + 12 );
    if( s >= e || dma_index_for_vrom( s, e ) < 0 ) return false;
    if( ts == 0 && te == 0 ) return true;
    return ts < te && dma_index_for_vrom( ts, te ) >= 0;
}

static int scene_run_length( const u8 *buf, size_t size, size_t off )
{
    int n = 0;
    while( off + SCENE_TABLE_ENTRY <= size && n < SCENE_TABLE_MAX &&
           scene_entry_valid( buf + off )) {
        n++;
        off += SCENE_TABLE_ENTRY;
    }
    return n;
}

/* scan one decompressed file for the longest entry run */
static bool find_scene_table_in( const u8 *buf, size_t size, size_t *outOff, int *outCount )
{
    size_t bestOff = 0;
    int best = 0;
    for( size_t off = 0; off + SCENE_TABLE_ENTRY <= size; off += 4 ) {
        int n = scene_run_length( buf, size, off );
        if( n > best ) { best = n; bestOff = off; }
        if( n >= SCENE_TABLE_MIN )
            off += (size_t)( n - 1 ) * SCENE_TABLE_ENTRY; /* runs can't overlap a longer one */
    }
    if( best < SCENE_TABLE_MIN ) return false;
    *outOff = bestOff;
    *outCount = best;
    return true;
}

/* locate 'code' structurally (size window + contains the scene table) and
   copy the table out; cached for the process lifetime */
static bool scene_table_init( void )
{
    size_t romSize;
    const u8 *rom = liboot_rom( &romSize );

    if( s_scene.tableReady ) return true;
    if( !rom || !build_dma_pairs( rom, romSize )) return false;

    for( u32 i = 3; i < CODE_SCAN_DMA_MAX; ++i ) {
        const u8 *dmadata = rom_find_dmadata( rom, romSize );
        LibootDmaEntry e;
        if( !dmadata || !dma_get( rom, romSize, dmadata, i, &e )) break;
        size_t size = e.vromEnd - e.vromStart;
        if( size < CODE_MIN_SIZE || size > CODE_MAX_SIZE ) continue;
        u8 *buf = rom_read_file( rom, romSize, i, &size );
        if( !buf ) continue;
        size_t off;
        int count;
        if( find_scene_table_in( buf, size, &off, &count )) {
            s_scene.table = malloc( (size_t)count * SCENE_TABLE_ENTRY );
            if( s_scene.table ) {
                memcpy( s_scene.table, buf + off, (size_t)count * SCENE_TABLE_ENTRY );
                s_scene.tableCount = count;
                s_scene.tableReady = true;
            }
            free( buf );
            return s_scene.tableReady;
        }
        free( buf );
    }
    return false;
}

/* extract the dmadata file exactly covering [vromStart, vromEnd) */
static u8 *extract_vrom_file( u32 vromStart, u32 vromEnd, size_t *outSize )
{
    size_t romSize;
    const u8 *rom = liboot_rom( &romSize );
    int idx = dma_index_for_vrom( vromStart, vromEnd );
    if( !rom || idx < 0 ) return NULL;
    return rom_read_file( rom, romSize, (u32)idx, outSize );
}

/* ---- header walks ------------------------------------------------- */
typedef LibootSceneHeader SceneHeader;

/* liboot vNEXT: read the scene's active EnvLightSettings (index 0 — the default
   entry; alternate-header / time-of-day selection is a later stage) into the
   exported OoTSceneEnvironment, and install it into the DL interpreter so lit
   geometry is shaded like the real game instead of fullbright. When the scene
   declares no light settings, reset the interpreter to its daylight default. */
static void apply_scene_environment( const u8 *blob, size_t blobSize, const SceneHeader *sh )
{
    struct OoTSceneEnvironment *e = &s_scene.env;
    memset( e, 0, sizeof( *e ));
    if( sh->numLights > 0 && blob &&
        (size_t)sh->lightListOff + 0x16u <= blobSize ) {
        const u8 *L = blob + sh->lightListOff;
        for( int k = 0; k < 3; ++k ) e->ambientColor[k] = L[k] / 255.0f;
        for( int k = 0; k < 3; ++k ) e->light1Dir[k] = (float)(s8)L[3 + k];
        for( int k = 0; k < 3; ++k ) e->light1Color[k] = L[6 + k] / 255.0f;
        for( int k = 0; k < 3; ++k ) e->light2Dir[k] = (float)(s8)L[9 + k];
        for( int k = 0; k < 3; ++k ) e->light2Color[k] = L[12 + k] / 255.0f;
        for( int k = 0; k < 3; ++k ) e->fogColor[k] = L[15 + k] / 255.0f;
        u16 bfn = be16u( L + 0x12 );
        e->fogNear = (float)( bfn & 0x3FF );
        e->fogFar = (float)be16s( L + 0x14 );
        /* export unit-length directions for the host */
        for( int l = 0; l < 2; ++l ) {
            float *d = l == 0 ? e->light1Dir : e->light2Dir;
            float len = sqrtf( d[0]*d[0] + d[1]*d[1] + d[2]*d[2] );
            if( len > 1e-6f ) { d[0]/=len; d[1]/=len; d[2]/=len; }
        }
        e->valid = 1;
    }
    if( e->valid )
        liboot_gfx_set_lights( e->ambientColor, e->light1Dir, e->light1Color,
                               e->light2Dir, e->light2Color, 2 );
    else
        liboot_gfx_set_lights( NULL, NULL, NULL, NULL, NULL, 0 );
}

typedef LibootRoomHeader RoomHeader;

static bool walk_scene_header( const u8 *s, size_t size, uint8_t layer,
                               SceneHeader *out, bool *outFallback )
{
    return liboot_scene_header_parse( s, size, layer, out, outFallback );
}

static bool walk_room_header( const u8 *r, size_t size, uint8_t layer,
                              RoomHeader *out, bool *outFallback )
{
    return liboot_room_header_parse( r, size, layer, out, outFallback );
}

/* ---- collision relocation (serialized BE -> native, field by field) ---- */
typedef struct {
    Vec3s *vtx;
    CollisionPoly *poly;
    SurfaceType *surf;
    SurfaceType *rawSurf;
    u32 numSurf;
    u8 maxExitIndex;
    WaterBox *wb;
    CollisionHeader hdr;
} RelocCol;

static u32 seg2_off( u32 w ) { return (( w >> 24 ) == 2 ) ? ( w & 0xFFFFFF ) : UINT32_MAX; }

static bool span_in_bounds( size_t off, size_t count, size_t stride, size_t size )
{
    return off <= size && stride != 0 && count <= ( size - off ) / stride;
}

static bool actor_catalog_append( struct OoTSceneActorEntry **entries,
                                  uint32_t *count, uint32_t *capacity,
                                  const u8 *blob, size_t blobSize,
                                  uint32_t listOff, int numActors,
                                  int16_t room, uint8_t source, uint8_t layer )
{
    struct OoTSceneActorEntry *grown;
    uint32_t needed;
    if( numActors <= 0 ) return true;
    if( !span_in_bounds( listOff, (size_t)numActors, 0x10u, blobSize ) ||
        (uint32_t)numActors > UINT32_MAX - *count )
        return true; /* malformed optional metadata does not reject the scene */
    needed = *count + (uint32_t)numActors;
    if( needed > *capacity ) {
        uint32_t next = *capacity != 0u ? *capacity : 16u;
        while( next < needed ) {
            if( next > UINT32_MAX / 2u ) { next = needed; break; }
            next *= 2u;
        }
        grown = realloc( *entries, (size_t)next * sizeof( **entries ));
        if( grown == NULL ) return false;
        *entries = grown;
        *capacity = next;
    }
    for( int i = 0; i < numActors; ++i ) {
        const u8 *raw = blob + listOff + (size_t)i * 0x10u;
        struct OoTSceneActorEntry *actor = &(*entries)[(*count)++];
        memset( actor, 0, sizeof( *actor ));
        actor->structSize = sizeof( *actor );
        actor->version = OOT_SCENE_ACTOR_ENTRY_VERSION;
        actor->actorId = be16s( raw );
        actor->position[0] = (float)be16s( raw + 2u );
        actor->position[1] = (float)be16s( raw + 4u );
        actor->position[2] = (float)be16s( raw + 6u );
        actor->rotation[0] = be16s( raw + 8u );
        actor->rotation[1] = be16s( raw + 10u );
        actor->rotation[2] = be16s( raw + 12u );
        actor->params = be16s( raw + 14u );
        actor->room = room;
        actor->source = source;
        actor->layer = layer;
    }
    return true;
}

static bool build_actor_catalog( const u8 *sceneBlob, size_t sceneSize,
                                 const SceneHeader *sh, const u8 *primaryBlob,
                                 size_t primarySize, const RoomHeader *primary,
                                 int roomIndex, uint8_t layer,
                                 struct OoTSceneActorEntry **outActors,
                                 uint32_t *outCount )
{
    struct OoTSceneActorEntry *actors = NULL;
    uint32_t count = 0u, capacity = 0u;
    int primaryRoom = roomIndex == -1 ? 0 : roomIndex;

    *outActors = NULL;
    *outCount = 0u;
    if( !actor_catalog_append( &actors, &count, &capacity, sceneBlob, sceneSize,
                               sh->actorListOff, sh->numActors, -1,
                               OOT_SCENE_ACTOR_SOURCE_SCENE, layer ) ||
        !actor_catalog_append( &actors, &count, &capacity, primaryBlob, primarySize,
                               primary->actorListOff, primary->numActors,
                               (int16_t)primaryRoom,
                               OOT_SCENE_ACTOR_SOURCE_ROOM, layer )) {
        free( actors );
        return false;
    }

    if( roomIndex == -1 ) {
        for( int room = 1; room < sh->numRooms; ++room ) {
            const u8 *file = sceneBlob + sh->roomListOff + (size_t)room * 8u;
            size_t roomSize = 0u;
            u8 *roomBlob = extract_vrom_file( be32( file ), be32( file + 4u ),
                                              &roomSize );
            RoomHeader rh;
            bool ok = roomBlob != NULL &&
                      walk_room_header( roomBlob, roomSize, layer, &rh, NULL );
            if( ok )
                ok = actor_catalog_append( &actors, &count, &capacity,
                                           roomBlob, roomSize, rh.actorListOff,
                                           rh.numActors, (int16_t)room,
                                           OOT_SCENE_ACTOR_SOURCE_ROOM, layer );
            free( roomBlob );
            if( !ok ) { free( actors ); return false; }
        }
    }
    *outActors = actors;
    *outCount = count;
    return true;
}

static void background_entries_free( struct OoTSceneBackground *entries,
                                     uint32_t count )
{
    if( entries == NULL ) return;
    for( uint32_t i = 0u; i < count; ++i ) {
        free((void *)entries[i].imageBytes );
        free((void *)entries[i].tlutBytes );
    }
    free( entries );
}

static void background_catalog_free( BackgroundCatalog *catalog )
{
    if( catalog == NULL ) return;
    background_entries_free( catalog->entries, catalog->count );
    memset( catalog, 0, sizeof( *catalog ));
}

static bool background_catalog_append( BackgroundCatalog *catalog,
                                       const LibootRoomImageBackground *view,
                                       int32_t roomIndex, uint8_t amountType )
{
    struct OoTSceneBackground *grown;
    uint8_t *imageCopy = NULL;
    uint8_t *tlutCopy = NULL;
    size_t added;
    if( view->imageByteCount > SIZE_MAX - view->tlutByteCount ) return false;
    added = view->imageByteCount + view->tlutByteCount;
    if( catalog->ownedBytes > BACKGROUND_CATALOG_MAX_BYTES ||
        added > BACKGROUND_CATALOG_MAX_BYTES - catalog->ownedBytes )
        return false;
    if( catalog->count == catalog->capacity ) {
        uint32_t next = catalog->capacity != 0u ? catalog->capacity * 2u : 8u;
        if( next < catalog->capacity ||
            (size_t)next > SIZE_MAX / sizeof( *catalog->entries ))
            return false;
        grown = realloc( catalog->entries,
                         (size_t)next * sizeof( *catalog->entries ));
        if( grown == NULL ) return false;
        catalog->entries = grown;
        catalog->capacity = next;
    }
    if( view->imageByteCount != 0u ) {
        imageCopy = malloc( view->imageByteCount );
        if( imageCopy == NULL ) return false;
        memcpy( imageCopy, view->imageBytes, view->imageByteCount );
    }
    if( view->tlutByteCount != 0u ) {
        tlutCopy = malloc( view->tlutByteCount );
        if( tlutCopy == NULL ) { free( imageCopy ); return false; }
        memcpy( tlutCopy, view->tlutBytes, view->tlutByteCount );
    }

    struct OoTSceneBackground *entry = &catalog->entries[catalog->count++];
    memset( entry, 0, sizeof( *entry ));
    entry->structSize = sizeof( *entry );
    entry->version = OOT_SCENE_BACKGROUND_VERSION;
    entry->roomIndex = roomIndex;
    entry->cameraIndex = view->cameraIndex;
    entry->amountType = amountType;
    entry->encoding = view->encoding;
    entry->width = view->width;
    entry->height = view->height;
    entry->format = view->format;
    entry->size = view->size;
    entry->tlutMode = view->tlutMode;
    entry->tlutCount = view->tlutCount;
    entry->entryFlags = view->entryFlags;
    entry->sourceAddress = view->sourceAddress;
    entry->tlutAddress = view->tlutAddress;
    entry->sourceMetadata = view->sourceMetadata;
    entry->imageBytes = imageCopy;
    entry->imageByteCount = view->imageByteCount;
    entry->tlutBytes = tlutCopy;
    entry->tlutByteCount = view->tlutByteCount;
    catalog->ownedBytes += added;
    return true;
}

static bool background_catalog_add_room( BackgroundCatalog *catalog,
                                         const u8 *sceneBlob, size_t sceneSize,
                                         const u8 *roomBlob, size_t roomSize,
                                         const RoomHeader *roomHeader,
                                         int32_t roomIndex )
{
    LibootRoomImageHeader image;
    if( roomHeader->shapeOff >= roomSize ) return false;
    if( roomBlob[roomHeader->shapeOff] != 1u ) return true;
    if( !liboot_room_image_header_parse( roomBlob, roomSize,
                                         roomHeader->shapeOff, &image ))
        return false;
    for( uint32_t i = 0u; i < image.numBackgrounds; ++i ) {
        LibootRoomImageBackground view;
        if( !liboot_room_image_background_get( sceneBlob, sceneSize,
                                                roomBlob, roomSize, &image, i,
                                                &view ) ||
            !background_catalog_append( catalog, &view, roomIndex,
                                         image.amountType ))
            return false;
    }
    return true;
}

static bool build_background_catalog( const u8 *sceneBlob, size_t sceneSize,
                                      const SceneHeader *sceneHeader,
                                      const u8 *primaryBlob, size_t primarySize,
                                      const RoomHeader *primaryHeader,
                                      int roomIndex, uint8_t layer,
                                      BackgroundCatalog *out )
{
    int primaryRoom = roomIndex == -1 ? 0 : roomIndex;
    memset( out, 0, sizeof( *out ));
    if( !background_catalog_add_room( out, sceneBlob, sceneSize, primaryBlob,
                                      primarySize, primaryHeader,
                                      primaryRoom ))
        goto fail;
    if( roomIndex == -1 ) {
        for( int room = 1; room < sceneHeader->numRooms; ++room ) {
            const u8 *file = sceneBlob + sceneHeader->roomListOff +
                             (size_t)room * 8u;
            size_t otherSize = 0u;
            u8 *other = extract_vrom_file( be32( file ), be32( file + 4u ),
                                           &otherSize );
            RoomHeader otherHeader;
            bool ok = other != NULL &&
                      walk_room_header( other, otherSize, layer, &otherHeader,
                                        NULL ) &&
                      background_catalog_add_room( out, sceneBlob, sceneSize,
                                                   other, otherSize,
                                                   &otherHeader, room );
            free( other );
            if( !ok ) goto fail;
        }
    }
    return true;

fail:
    background_catalog_free( out );
    return false;
}

static bool relocate_collision( const u8 *scn, size_t size, u32 colOff, RelocCol *out )
{
    memset( out, 0, sizeof( *out ));
    if( colOff == UINT32_MAX || !span_in_bounds( colOff, 1, 0x2C, size )) return false;
    const u8 *c = scn + colOff;

    u16 nv = be16u( c + 0x0C ), np = be16u( c + 0x14 ), nwb = be16u( c + 0x24 );
    u32 vtxOff = seg2_off( be32( c + 0x10 ));
    u32 polyOff = seg2_off( be32( c + 0x18 ));
    u32 surfOff = seg2_off( be32( c + 0x1C ));
    u32 wbOff = nwb ? seg2_off( be32( c + 0x28 )) : 0;

    if( nv == 0 || np == 0 || np > INT16_MAX ) return false;
    if( vtxOff == UINT32_MAX || !span_in_bounds( vtxOff, nv, 6, size )) return false;
    if( polyOff == UINT32_MAX || !span_in_bounds( polyOff, np, 0x10, size )) return false;
    if( surfOff == UINT32_MAX ) return false;
    if( nwb && ( wbOff == UINT32_MAX || !span_in_bounds( wbOff, nwb, 0x10, size ))) return false;

    Vec3s *vtx = calloc( nv, sizeof( Vec3s ));
    CollisionPoly *poly = calloc( np, sizeof( CollisionPoly ));
    if( !vtx || !poly ) { free( vtx ); free( poly ); return false; }

    for( u16 i = 0; i < nv; ++i ) {
        const u8 *p = scn + vtxOff + (size_t)i * 6;
        vtx[i].x = be16s( p );
        vtx[i].y = be16s( p + 2 );
        vtx[i].z = be16s( p + 4 );
    }

    u16 maxType = 0;
    for( u16 i = 0; i < np; ++i ) {
        const u8 *p = scn + polyOff + (size_t)i * 0x10;
        poly[i].type = be16u( p );
        /* keep xpFlags (vIA 0xE000) and the conveyor flag (vIB 0x2000) */
        poly[i].flags_vIA = be16u( p + 2 );
        poly[i].flags_vIB = be16u( p + 4 );
        poly[i].vIC = be16u( p + 6 );
        poly[i].normal.x = be16s( p + 8 );
        poly[i].normal.y = be16s( p + 10 );
        poly[i].normal.z = be16s( p + 12 );
        poly[i].dist = be16s( p + 14 );
        if( poly[i].type > maxType ) maxType = poly[i].type;
        if(( poly[i].flags_vIA & 0x1FFF ) >= nv || ( poly[i].flags_vIB & 0x1FFF ) >= nv ||
           poly[i].vIC >= nv ) {
            free( vtx ); free( poly );
            return false;
        }
    }

    /* SurfaceType count is not serialized: max(poly.type)+1 is authoritative
       (verified equal to the surf->poly list gap in probed scenes) */
    u32 nSurf = (u32)maxType + 1;
    if( !span_in_bounds( surfOff, nSurf, 8, size )) { free( vtx ); free( poly ); return false; }
    SurfaceType *surf = calloc( nSurf, sizeof( SurfaceType ));
    SurfaceType *rawSurf = calloc( nSurf, sizeof( SurfaceType ));
    if( !surf || !rawSurf ) {
        free( vtx ); free( poly ); free( surf ); free( rawSurf );
        return false;
    }
    for( u32 i = 0; i < nSurf; ++i ) {
        const u8 *p = scn + surfOff + (size_t)i * 8;
        u32 d0 = be32( p );
        u32 d1 = be32( p + 4 );
        rawSurf[i].data[0] = d0;
        rawSurf[i].data[1] = d1;
        /* ALWAYS clear exitIndex (bits 8-12): the vendored Player indexes
           play->exitList (NULL in the fake play state) every grounded frame
           a nonzero exit is underfoot -> segfault */
        d0 &= ~( 0x1Fu << 8 );
        /* void-out floorProperty values freeze Player waiting for a scene
           transition that never comes: mask 5 and 12 only, keep ledge/jump
           behaviors (6/7/8/9/11). NOTE the hardcoded y < -4000 void plane in
           z_player.c is not surface-driven and cannot be masked here — its
           transitionTrigger latch is instead dropped at every scene/world
           load and Link (re)init (see oot_scene_load / liboot_link_init). */
        u32 fp = ( d0 >> 26 ) & 0xF;
        if( fp == 5 || fp == 12 )
            d0 &= ~( 0xFu << 26 );
        surf[i].data[0] = d0;
        surf[i].data[1] = d1;  /* material/sfx, conveyor, canHookshot: keep whole */
    }
    u8 maxExitIndex = 0u;
    for( u16 i = 0; i < np; ++i ) {
        u8 exitIndex = (u8)(( rawSurf[poly[i].type].data[0] >> 8 ) & 0x1Fu );
        if( exitIndex > maxExitIndex ) maxExitIndex = exitIndex;
    }

    WaterBox *wb = NULL;
    if( nwb ) {
        wb = calloc( nwb, sizeof( WaterBox ));
        if( !wb ) {
            free( vtx ); free( poly ); free( surf ); free( rawSurf );
            return false;
        }
        for( u16 i = 0; i < nwb; ++i ) {
            const u8 *p = scn + wbOff + (size_t)i * 0x10;
            wb[i].xMin = be16s( p );
            wb[i].ySurface = be16s( p + 2 );
            wb[i].zMin = be16s( p + 4 );
            wb[i].xLength = be16s( p + 6 );
            wb[i].zLength = be16s( p + 8 );
            /* room must be "all" (fake curRoom.num = -1) and FLAG_19 clear;
               bgCam/light indices are dead with a NULL bgCamList */
            wb[i].properties = WATERBOX_PROPERTIES( 0, 0, WATERBOX_ROOM_ALL, 0 );
        }
    }

    out->vtx = vtx;
    out->poly = poly;
    out->surf = surf;
    out->rawSurf = rawSurf;
    out->numSurf = nSurf;
    out->maxExitIndex = maxExitIndex;
    out->wb = wb;

    CollisionHeader *hdr = &out->hdr;
    memset( hdr, 0, sizeof( *hdr ));
    /* header bounds verbatim: intentional superset of the vtx bounds (grid sizing) */
    hdr->minBounds.x = be16s( c );     hdr->minBounds.y = be16s( c + 2 );  hdr->minBounds.z = be16s( c + 4 );
    hdr->maxBounds.x = be16s( c + 6 ); hdr->maxBounds.y = be16s( c + 8 );  hdr->maxBounds.z = be16s( c + 10 );
    hdr->numVertices = nv;
    hdr->vtxList = vtx;
    hdr->numPolygons = np;
    hdr->polyList = poly;
    hdr->surfaceTypeList = surf;
    hdr->bgCamList = NULL;             /* every vendored reader NULL-checks */
    hdr->numWaterBoxes = nwb;
    hdr->waterBoxes = wb;
    return true;
}

static void reloc_collision_free( RelocCol *col )
{
    if( col == NULL ) return;
    free( col->vtx );
    free( col->poly );
    free( col->surf );
    free( col->rawSurf );
    free( col->wb );
    memset( col, 0, sizeof( *col ));
}

static int16_t *parse_exit_list( const u8 *blob, size_t size,
                                 const SceneHeader *header, u8 count )
{
    int16_t *exits;
    if( count == 0u ) return NULL;
    if( !header->hasExitList ||
        !span_in_bounds( header->exitListOff, count, 2u, size ))
        return NULL;
    exits = malloc( (size_t)count * sizeof( *exits ));
    if( exits == NULL ) return NULL;
    for( u8 i = 0u; i < count; ++i )
        exits[i] = be16s( blob + header->exitListOff + (size_t)i * 2u );
    return exits;
}

/* z_bgcheck uses a fixed retail-era memory budget and aborts when the static
   SSNode table is exhausted.  Count the exact nodes its subdivision walk will
   create before committing a ROM scene, and reject data that cannot fit. */
typedef struct {
    Vec3i subdivAmount;
    u32 memSize;
    u32 dynaNodeMax;
    u32 dynaPolyMax;
    u32 dynaVtxMax;
} BgCheckPlan;

static BgCheckPlan bgcheck_plan( void )
{
    BgCheckPlan plan;
    memset( &plan, 0, sizeof( plan ));
    /* The standalone PlayState intentionally uses scene 0/default camera for
       every host-loaded world (see liboot_link_init).  Keep that deterministic
       configuration: its larger generic budget also avoids 64-bit host struct
       inflation overflowing the N64's scene-specific byte budgets. */
    plan.memSize = 0x1CC00;
    plan.dynaNodeMax = 1000;
    plan.dynaPolyMax = plan.dynaVtxMax = 512;
    plan.subdivAmount.x = 16;
    plan.subdivAmount.y = 4;
    plan.subdivAmount.z = 16;
    return plan;
}

/* Internal (hidden-visibility) helper shared with the host-world loader. */
bool liboot_bgcheck_preflight( CollisionHeader *hdr )
{
    BgCheckPlan plan = bgcheck_plan();
    CollisionContext ctx;
    size_t lookupCount, fixedBytes, maxNodes, nodes = 0;

    if( hdr == NULL || hdr->vtxList == NULL || hdr->polyList == NULL ||
        hdr->numVertices == 0 || hdr->numPolygons == 0 ||
        hdr->numPolygons > INT16_MAX )
        return false;
    if( plan.subdivAmount.x <= 0 || plan.subdivAmount.y <= 0 || plan.subdivAmount.z <= 0 )
        return false;
    lookupCount = (size_t)plan.subdivAmount.x * (size_t)plan.subdivAmount.y *
                  (size_t)plan.subdivAmount.z;
    fixedBytes = lookupCount * sizeof( StaticLookup ) +
                 (size_t)hdr->numPolygons * sizeof( u8 ) +
                 (size_t)plan.dynaNodeMax * sizeof( SSNode ) +
                 (size_t)plan.dynaPolyMax * sizeof( CollisionPoly ) +
                 (size_t)plan.dynaVtxMax * sizeof( Vec3s ) +
                 sizeof( CollisionContext );
    if( fixedBytes >= plan.memSize ) return false;
    maxNodes = ( plan.memSize - fixedBytes ) / sizeof( SSNode );
    if( maxNodes == 0 || maxNodes >= UINT16_MAX ) return false;
    /* Every valid polygon occupies at least the cell containing one of its
       vertices, so this cheap lower bound rejects oversized inputs early. */
    if( hdr->numPolygons > maxNodes ) return false;

    memset( &ctx, 0, sizeof( ctx ));
    ctx.colHeader = hdr;
    ctx.subdivAmount = plan.subdivAmount;
    ctx.minBounds.x = hdr->minBounds.x;
    ctx.minBounds.y = hdr->minBounds.y;
    ctx.minBounds.z = hdr->minBounds.z;
    ctx.maxBounds.x = hdr->maxBounds.x;
    ctx.maxBounds.y = hdr->maxBounds.y;
    ctx.maxBounds.z = hdr->maxBounds.z;
    BgCheck_SetSubdivisionDimension( ctx.minBounds.x, ctx.subdivAmount.x,
                                     &ctx.maxBounds.x, &ctx.subdivLength.x,
                                     &ctx.subdivLengthInv.x );
    BgCheck_SetSubdivisionDimension( ctx.minBounds.y, ctx.subdivAmount.y,
                                     &ctx.maxBounds.y, &ctx.subdivLength.y,
                                     &ctx.subdivLengthInv.y );
    BgCheck_SetSubdivisionDimension( ctx.minBounds.z, ctx.subdivAmount.z,
                                     &ctx.maxBounds.z, &ctx.subdivLength.z,
                                     &ctx.subdivLengthInv.z );

    for( s32 poly = 0; poly < hdr->numPolygons; ++poly ) {
        s32 sxMin, syMin, szMin, sxMax, syMax, szMax;
        CollisionPoly *candidate = &hdr->polyList[poly];
        if(( candidate->flags_vIA & 0x1FFF ) >= hdr->numVertices ||
           ( candidate->flags_vIB & 0x1FFF ) >= hdr->numVertices ||
           candidate->vIC >= hdr->numVertices )
            return false;
        BgCheck_GetPolySubdivisionBounds( &ctx, hdr->vtxList, hdr->polyList,
                                          &sxMin, &syMin, &szMin,
                                          &sxMax, &syMax, &szMax, (s16)poly );
        if( sxMin < 0 || syMin < 0 || szMin < 0 || sxMax < sxMin ||
            syMax < syMin || szMax < szMin || sxMax >= ctx.subdivAmount.x ||
            syMax >= ctx.subdivAmount.y || szMax >= ctx.subdivAmount.z )
            return false;

        for( s32 sz = szMin; sz <= szMax; ++sz ) {
            Vec3f min, max;
            min.z = ctx.subdivLength.z * sz + ctx.minBounds.z - BGCHECK_SUBDIV_OVERLAP;
            max.z = min.z + ctx.subdivLength.z + 2 * BGCHECK_SUBDIV_OVERLAP;
            for( s32 sy = syMin; sy <= syMax; ++sy ) {
                min.y = ctx.subdivLength.y * sy + ctx.minBounds.y - BGCHECK_SUBDIV_OVERLAP;
                max.y = min.y + ctx.subdivLength.y + 2 * BGCHECK_SUBDIV_OVERLAP;
                for( s32 sx = sxMin; sx <= sxMax; ++sx ) {
                    min.x = ctx.subdivLength.x * sx + ctx.minBounds.x - BGCHECK_SUBDIV_OVERLAP;
                    max.x = min.x + ctx.subdivLength.x + 2 * BGCHECK_SUBDIV_OVERLAP;
                    if( BgCheck_PolyIntersectsSubdivision( &min, &max, hdr->polyList,
                                                           hdr->vtxList, (s16)poly )) {
                        if( nodes >= maxNodes ) return false;
                        nodes++;
                    }
                }
            }
        }
    }
    return true;
}

/* ---- room mesh interpretation ------------------------------------- */
/* run pass 0 = every entry's opa DL, pass 1 = every entry's xlu DL */
static bool mesh_run_pass( const u8 *room, size_t size, u32 shapeOff, int pass )
{
    u8 type = room[shapeOff];
    u32 entriesOff, stride, dlOff;
    int count;

    switch( type ) {
    case 0: /* RoomShapeNormal: {u8 type, u8 num, u32 entries, u32 end}, entry {opa, xlu} */
        if( shapeOff + 12 > size ) return false;
        count = room[shapeOff + 1];
        entriesOff = be32( room + shapeOff + 4 );
        stride = 8;
        dlOff = 0;
        break;
    case 2: /* RoomShapeCullable: entry {Vec3s center, s16 radius, u32 opa, u32 xlu};
               v1 ignores culling and draws every entry */
        if( shapeOff + 12 > size ) return false;
        count = room[shapeOff + 1];
        entriesOff = be32( room + shapeOff + 4 );
        stride = 0x10;
        dlOff = 8;
        break;
    case 1: { /* RoomShapeImage: single and multi share one real 3D DL pair;
                 their 2D background catalog is parsed independently. */
        LibootRoomImageHeader image;
        if( !liboot_room_image_header_parse( room, size, shapeOff, &image ))
            return false;
        count = 1;
        entriesOff = image.displayListEntryAddress;
        stride = 8;
        dlOff = 0;
        break;
    }
    default:
        return false;
    }

    if(( entriesOff >> 24 ) != 3 ) return false;
    entriesOff &= 0xFFFFFF;
    if( (size_t)entriesOff + (size_t)count * stride > size ) return false;

    for( int i = 0; i < count; ++i ) {
        const u8 *e = room + entriesOff + (size_t)i * stride + dlOff;
        u32 dl = be32( e + ( pass ? 4 : 0 ));
        u32 seg = dl >> 24;
        if( dl != 0 && ( seg == 2 || seg == 3 ))
            liboot_scene_dl_run( dl );
    }
    return true;
}

static bool ensure_scene_buffers( void )
{
    uint32_t capacity = sSceneGeometryCapacity;
    float *pos, *nrm, *col, *uv, *alpha;
    uint16_t *triTex;
    uint8_t *triFlags;
    struct OoTGeometryBatch *batches;

    if( s_scene.pos && s_scene.allocatedGeometryCapacity == capacity ) return true;
    pos = malloc((size_t)capacity * 9u * sizeof( float ));
    nrm = malloc((size_t)capacity * 9u * sizeof( float ));
    col = malloc((size_t)capacity * 9u * sizeof( float ));
    uv = malloc((size_t)capacity * 6u * sizeof( float ));
    triTex = malloc((size_t)capacity * sizeof( uint16_t ));
    alpha = malloc((size_t)capacity * 3u * sizeof( float ));
    triFlags = malloc((size_t)capacity * sizeof( uint8_t ));
    batches = malloc((size_t)capacity * sizeof( *batches ));
    if( !pos || !nrm || !col || !uv || !triTex || !alpha || !triFlags || !batches ) {
        free( pos ); free( nrm ); free( col ); free( uv );
        free( triTex ); free( alpha ); free( triFlags ); free( batches );
        return false;
    }
    free( s_scene.pos ); free( s_scene.nrm ); free( s_scene.col ); free( s_scene.uv );
    free( s_scene.triTex ); free( s_scene.alpha ); free( s_scene.triFlags );
    free( s_scene.batches );
    s_scene.pos = pos; s_scene.nrm = nrm; s_scene.col = col; s_scene.uv = uv;
    s_scene.triTex = triTex; s_scene.alpha = alpha; s_scene.triFlags = triFlags;
    s_scene.batches = batches;
    s_scene.allocatedGeometryCapacity = capacity;
    return true;
}

static bool interpret_room_mesh( void )
{
    RoomHeader rh;
    if( !walk_room_header( s_scene.roomBlob, s_scene.roomSize, s_scene.layer,
                           &rh, NULL ))
        return false;
    if( !ensure_scene_buffers() ) return false;

    struct OoTLinkGeometryBuffers buf = {
        s_scene.pos, s_scene.nrm, s_scene.col, s_scene.uv, s_scene.triTex, 0,
        s_scene.alpha, s_scene.triFlags, s_scene.allocatedGeometryCapacity, 0,
        s_scene.batches, s_scene.allocatedGeometryCapacity, 0
    };
    liboot_scene_dl_begin( &buf, (int)s_scene.allocatedGeometryCapacity );

    liboot_scene_dl_set_source( s_scene.curRoom, 0 );
    bool ok = mesh_run_pass( s_scene.roomBlob, s_scene.roomSize, rh.shapeOff, 0 );
    s_scene.xluStart = buf.numTrianglesUsed32;
    if( ok ) {
        liboot_scene_dl_set_source( s_scene.curRoom, 1 );
        mesh_run_pass( s_scene.roomBlob, s_scene.roomSize, rh.shapeOff, 1 );
    }
    s_scene.numTris = buf.numTrianglesUsed32;
    s_scene.numBatches = buf.numBatchesUsed;
    return ok;
}

/* liboot vNEXT: interpret EVERY room's mesh into one geometry stream for a full
   multi-room dungeon render. Opaque triangles for all rooms are emitted first,
   then translucent for all rooms, preserving the single opa/xlu split the
   scene-geometry contract exposes. Each room file is re-extracted per pass and
   registered as segment 3 only while its display lists are walked; nothing from
   the room blob is retained afterward (vertices are copied to floats and any
   textures decoded into the cache), so it is freed immediately. The caller must
   restore a valid segment 3 after this returns. */
static bool interpret_all_rooms( const u8 *scn, size_t scnSize, u32 roomListOff, int numRooms )
{
    (void)scnSize;
    if( !ensure_scene_buffers() ) return false;

    struct OoTLinkGeometryBuffers buf = {
        s_scene.pos, s_scene.nrm, s_scene.col, s_scene.uv, s_scene.triTex, 0,
        s_scene.alpha, s_scene.triFlags, s_scene.allocatedGeometryCapacity, 0,
        s_scene.batches, s_scene.allocatedGeometryCapacity, 0
    };
    liboot_scene_dl_begin( &buf, (int)s_scene.allocatedGeometryCapacity );

    for( int pass = 0; pass < 2; ++pass ) {
        if( pass == 1 ) s_scene.xluStart = buf.numTrianglesUsed32;
        for( int i = 0; i < numRooms; ++i ) {
            const u8 *rf = scn + roomListOff + (size_t)i * 8;
            size_t rs = 0;
            u8 *rb = extract_vrom_file( be32( rf ), be32( rf + 4 ), &rs );
            if( !rb ) continue;   /* skip an unreadable room, keep the rest */
            RoomHeader rh;
            if( walk_room_header( rb, rs, s_scene.layer, &rh, NULL )) {
                liboot_gfx_set_room( i );   /* key this room's seg-3 textures */
                liboot_scene_dl_set_source( i, pass );
                liboot_register_segment_span( 3, rb, rs );
                mesh_run_pass( rb, rs, rh.shapeOff, pass );
            }
            free( rb );
        }
    }
    s_scene.numTris = buf.numTrianglesUsed32;
    s_scene.numBatches = buf.numBatchesUsed;
    return s_scene.numTris > 0;
}

/* Commit the synchronous room transition to the fake PlayState exactly as the
   retail scene commands do. The serialized RoomShape is deliberately not
   installed in Room.roomShape: its embedded segmented pointers are still
   big-endian and are only consumed by liboot's display-list interpreter. */
static void apply_scene_runtime( PlayState *play, int32_t sceneIndex,
                                 int primaryRoom, const SceneHeader *sh,
                                 const RoomHeader *rh )
{
    memset( &play->roomCtx.curRoom, 0, sizeof( play->roomCtx.curRoom ));
    memset( &play->roomCtx.prevRoom, 0, sizeof( play->roomCtx.prevRoom ));
    play->roomCtx.prevRoom.num = -1;

    play->roomCtx.curRoom.num = (s8)primaryRoom;
    play->roomCtx.curRoom.segment = s_scene.roomBlob;
    play->roomCtx.curRoom.type = rh->type;
    play->roomCtx.curRoom.environmentType = rh->environmentType;
    play->roomCtx.curRoom.lensMode = rh->lensMode;
    play->roomCtx.curRoom.echo = rh->echo;
    play->msgCtx.disableWarpSongs = rh->disableWarpSongs;

    play->sceneId = sceneIndex;
    R_SCENE_CAM_TYPE = sh->sceneCamType;
    gSaveContext.worldMapArea = sh->worldMapArea;
    liboot_host_actor_sync_room();
}

/* Host-build hook called before the retail exit/void state machine mutates
 * Player. ROM surface exit/void bits stay masked in the live CollisionContext;
 * this consults the preserved originals and turns them into pollable events. */
s32 liboot_world_transition_intercept( PlayState *play, Player *player,
                                       CollisionPoly *poly, u32 bgId )
{
    uint32_t kind = 0u;
    int16_t exitIndex = 0;
    int16_t entranceIndex = -1;
    uint8_t floorProperty = 0u;

    if( play == NULL || player == NULL ||
        player->actor.category != ACTORCAT_PLAYER )
        return false;

    if( s_scene.sceneBlob != NULL && poly != NULL && bgId == BGCHECK_SCENE &&
        s_scene.colRawSurf != NULL && poly->type < s_scene.numColSurf ) {
        uint32_t raw = s_scene.colRawSurf[poly->type].data[0];
        float floorDistance = player->actor.world.pos.y - player->actor.floorHeight;
        bool grounded = ( player->actor.bgCheckFlags & BGCHECKFLAG_GROUND ) != 0;
        bool nearFloor = grounded || floorDistance <= 100.0f;
        exitIndex = (int16_t)(( raw >> 8 ) & 0x1Fu );
        floorProperty = (uint8_t)(( raw >> 26 ) & 0xFu );

        if( exitIndex > 0 && exitIndex <= s_scene.numExits && nearFloor ) {
            kind = OOT_WORLD_EVENT_SCENE_EXIT;
            entranceIndex = s_scene.exits[exitIndex - 1];
        } else if(( floorProperty == 5u || floorProperty == 12u ) &&
                  ( nearFloor || player->fallDistance > 400.0f ||
                    ( play->sceneId != SCENE_SHADOW_TEMPLE &&
                      player->fallDistance > 200.0f ))) {
            kind = OOT_WORLD_EVENT_VOID_SURFACE;
            exitIndex = 0;
        }
    }

    /* Match the two non-surface branches in Player_HandleExitsAndVoids. The
       collapsing-tower exterior starts its recovery before Link can reach the
       global -4000 plane. */
    if( kind == 0u &&
        ( player->actor.world.pos.y < -4000.0f ||
          ( play->sceneId == SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR &&
            player->fallDistance > 320.0f ))) {
        kind = OOT_WORLD_EVENT_VOID_Y;
        exitIndex = 0;
        floorProperty = 0u;
    }

    if( kind == 0u ) {
        s_worldEvents.contactKind = 0u;
        s_worldEvents.contactExitIndex = 0;
        s_worldEvents.contactFloorProperty = 0u;
        return false;
    }

    if( s_worldEvents.contactKind != kind ||
        s_worldEvents.contactExitIndex != exitIndex ||
        s_worldEvents.contactFloorProperty != floorProperty ) {
        if( world_event_push( kind, exitIndex, entranceIndex, floorProperty,
                              play, player )) {
            s_worldEvents.contactKind = kind;
            s_worldEvents.contactExitIndex = exitIndex;
            s_worldEvents.contactFloorProperty = floorProperty;
        }
    }
    return true;
}

/* ---- public API ---------------------------------------------------- */
bool oot_scene_set_geometry_capacity( uint32_t triangleCapacity )
{
    if( s_scene.sceneBlob ) return false;
    if( triangleCapacity == 0u ) triangleCapacity = OOT_SCENE_MAX_TRIANGLES;
    if( triangleCapacity > OOT_GEO_MAX_CONFIGURABLE_TRIANGLES ) return false;
    sSceneGeometryCapacity = triangleCapacity;
    return true;
}

void liboot_scene_reset_geometry_capacity( void )
{
    sSceneGeometryCapacity = OOT_SCENE_MAX_TRIANGLES;
}

int32_t oot_scene_load_ex( const struct OoTSceneLoadOptions *options )
{
    if( options == NULL ||
        options->structSize < sizeof(struct OoTSceneLoadOptions) ||
        options->layer > OOT_SCENE_LAYER_ADULT_NIGHT )
        return -10;
    int32_t sceneIndex = options->sceneIndex;
    int32_t roomIndex = options->roomIndex;
    uint8_t layer = options->layer;
    if( !scene_table_init() ) return -1;
    if( sceneIndex < 0 || sceneIndex >= s_scene.tableCount ) return -2;

    const u8 *entry = s_scene.table + (size_t)sceneIndex * SCENE_TABLE_ENTRY;
    uint8_t drawConfigId = entry[0x11u];
    size_t sceneSize, roomSize;
    u8 *sceneBlob = extract_vrom_file( be32( entry ), be32( entry + 4 ), &sceneSize );
    if( !sceneBlob ) return -3;

    SceneHeader sh;
    bool sceneFallback = false;
    if( !walk_scene_header( sceneBlob, sceneSize, layer, &sh, &sceneFallback )) {
        free( sceneBlob );
        return -4;
    }
    /* roomIndex == -1 loads the whole scene (all rooms). Room 0 is the primary
       room kept as segment 3 for gameplay; every room is interpreted below. */
    int fullScene = ( roomIndex == -1 );
    int primaryRoom = fullScene ? 0 : roomIndex;
    if( ( !fullScene && ( roomIndex < 0 || roomIndex >= sh.numRooms )) ||
        primaryRoom > INT8_MAX ||
        !span_in_bounds( sh.roomListOff, (size_t)sh.numRooms, 8, sceneSize )) {
        free( sceneBlob );
        return -5;
    }

    const u8 *rf = sceneBlob + sh.roomListOff + (size_t)primaryRoom * 8;
    u8 *roomBlob = extract_vrom_file( be32( rf ), be32( rf + 4 ), &roomSize );
    if( !roomBlob ) { free( sceneBlob ); return -6; }

    RelocCol rc;
    if( !relocate_collision( sceneBlob, sceneSize, sh.colOff, &rc )) {
        free( sceneBlob ); free( roomBlob );
        return -7;
    }
    int16_t *exits = parse_exit_list( sceneBlob, sceneSize, &sh,
                                      rc.maxExitIndex );
    if( rc.maxExitIndex != 0u && exits == NULL ) {
        reloc_collision_free( &rc );
        free( sceneBlob ); free( roomBlob );
        return -7;
    }
    if( !liboot_bgcheck_preflight( &rc.hdr )) {
        reloc_collision_free( &rc );
        free( exits );
        free( sceneBlob ); free( roomBlob );
        return -7;
    }

    /* room shape must be parsable before we commit anything */
    RoomHeader rh;
    bool roomFallback = false;
    if( !walk_room_header( roomBlob, roomSize, layer, &rh, &roomFallback )) {
        reloc_collision_free( &rc );
        free( exits );
        free( sceneBlob ); free( roomBlob );
        return -8;
    }
    struct OoTSceneActorEntry *actors = NULL;
    uint32_t numActors = 0u;
    if( !build_actor_catalog( sceneBlob, sceneSize, &sh, roomBlob, roomSize,
                              &rh, roomIndex, layer, &actors, &numActors )) {
        reloc_collision_free( &rc );
        free( exits );
        free( sceneBlob ); free( roomBlob );
        return -11;
    }
    BackgroundCatalog backgrounds;
    if( !build_background_catalog( sceneBlob, sceneSize, &sh, roomBlob,
                                   roomSize, &rh, roomIndex, layer,
                                   &backgrounds )) {
        free( actors );
        reloc_collision_free( &rc );
        free( exits );
        free( sceneBlob ); free( roomBlob );
        return -12;
    }

    /* ---- commit: replace the previous scene ---- */
    world_events_clear();
    liboot_register_segment_span( 2, sceneBlob, sceneSize );
    liboot_register_segment_span( 3, roomBlob, roomSize );
    free( s_scene.sceneBlob );
    free( s_scene.roomBlob );
    free( s_scene.actors );
    background_entries_free( s_scene.backgrounds, s_scene.numBackgrounds );
    s_scene.sceneBlob = sceneBlob; s_scene.sceneSize = sceneSize;
    s_scene.roomBlob = roomBlob;   s_scene.roomSize = roomSize;
    s_scene.actors = actors; s_scene.numActors = numActors;
    s_scene.backgrounds = backgrounds.entries;
    s_scene.numBackgrounds = backgrounds.count;
    s_scene.playerEntryOff = sh.playerEntryOff;
    s_scene.spawnListOff = sh.spawnListOff;
    s_scene.numRooms = sh.numRooms;
    s_scene.numDoors = sh.numDoors;
    s_scene.doorListOff = sh.doorListOff;
    s_scene.seqId = sh.seqId;
    s_scene.ambienceId = sh.ambienceId;
    s_scene.roomMetadataValid = rh.hasBehavior && rh.hasEcho;
    apply_scene_environment( sceneBlob, sceneSize, &sh );
    s_scene.curScene = sceneIndex;
    s_scene.curRoom = roomIndex;
    s_scene.layer = layer;
    s_scene.layerFallback = sceneFallback || roomFallback;
    s_scene.drawConfigId = drawConfigId;

    free( s_scene.colVtx ); free( s_scene.colPoly );
    free( s_scene.colSurf ); free( s_scene.colRawSurf );
    free( s_scene.colWb ); free( s_scene.exits );
    s_scene.colVtx = rc.vtx;
    s_scene.colPoly = rc.poly;
    s_scene.colSurf = rc.surf;
    s_scene.colRawSurf = rc.rawSurf;
    s_scene.numColSurf = rc.numSurf;
    s_scene.exits = exits;
    s_scene.numExits = rc.maxExitIndex;
    s_scene.colWb = rc.wb;
    s_scene.colHeader = rc.hdr;

    PlayState *play = liboot_play();
    play->sceneId = SCENE_DEKU_TREE;
    R_SCENE_CAM_TYPE = SCENE_CAM_TYPE_DEFAULT;
    liboot_reset_tha();   /* z_bgcheck is the only THA user: drop its old tables */
    BgCheck_Allocate( &play->colCtx, play, &s_scene.colHeader );
    liboot_invalidate_actor_bg_cache( play );
    liboot_dynamic_collision_rebind_all();
    /* The generic scene-0 values above are an allocation-only compatibility
       shim for liboot_bgcheck_preflight's deterministic host budget. Gameplay
       must observe the real scene and active room immediately afterward. */
    apply_scene_runtime( play, sceneIndex, primaryRoom, &sh, &rh );
    play->sceneDrawConfig = drawConfigId;
    gSaveContext.sceneLayer = layer;
    gSaveContext.save.nightFlag = ( layer == OOT_SCENE_LAYER_CHILD_NIGHT ||
                                    layer == OOT_SCENE_LAYER_ADULT_NIGHT );

    /* falling past the hardcoded y < -4000 void plane latches
       play->transitionTrigger = TRANS_TRIGGER_START (z_player.c void-out),
       which zeroes control-stick speed for the current AND every future
       Player (Player_CalcSpeedAndYawFromControlStick). liboot has no scene
       transition machinery, so a scene load IS the transition: drop the
       latch here (a still-voided Link genuinely re-latches next tick). */
    play->transitionTrigger = TRANS_TRIGGER_OFF;

    /* stale seg-2/3 texture cache entries would alias the new files */
    liboot_gfx_evict_scene();

    s_scene.numTris = s_scene.xluStart = s_scene.numBatches = 0;
    bool meshOk;
    if( fullScene ) {
        meshOk = interpret_all_rooms( s_scene.sceneBlob, s_scene.sceneSize,
                                      sh.roomListOff, sh.numRooms );
        /* interpret_all_rooms freed its transient per-room blobs, leaving seg-3
           dangling; restore the committed primary room so gameplay/actors that
           read segment 3 stay valid. */
        liboot_register_segment_span( 3, s_scene.roomBlob, s_scene.roomSize );
    } else {
        meshOk = interpret_room_mesh();
    }
    if( !meshOk )
        return -9;   /* collision is live; geometry unavailable */
    return 0;
}

int32_t oot_scene_load( int32_t sceneIndex, int32_t roomIndex )
{
    const struct OoTSceneLoadOptions options = {
        sizeof(struct OoTSceneLoadOptions),
        sceneIndex,
        roomIndex,
        OOT_SCENE_LAYER_CHILD_DAY,
        { 0u, 0u, 0u }
    };
    return oot_scene_load_ex( &options );
}

bool oot_scene_get_active_layer( uint8_t *outLayer, bool *outUsedFallback )
{
    if( outLayer != NULL ) *outLayer = OOT_SCENE_LAYER_CHILD_DAY;
    if( outUsedFallback != NULL ) *outUsedFallback = false;
    if( s_scene.sceneBlob == NULL ) return false;
    if( outLayer != NULL ) *outLayer = s_scene.layer;
    if( outUsedFallback != NULL ) *outUsedFallback = s_scene.layerFallback;
    return true;
}

int32_t oot_scene_get_background_count( void )
{
    return s_scene.sceneBlob != NULL && s_scene.numBackgrounds <= INT32_MAX
               ? (int32_t)s_scene.numBackgrounds : 0;
}

bool oot_scene_get_background( int32_t index,
                               struct OoTSceneBackground *outBackground )
{
    if( outBackground == NULL ||
        outBackground->structSize < sizeof( *outBackground ) ||
        outBackground->version != OOT_SCENE_BACKGROUND_VERSION ||
        index < 0 || (uint32_t)index >= s_scene.numBackgrounds ||
        s_scene.backgrounds == NULL )
        return false;
    *outBackground = s_scene.backgrounds[index];
    return true;
}

bool oot_scene_get_material_state( struct OoTSceneMaterialState *outState )
{
    if( outState == NULL || outState->structSize < sizeof( *outState ) ||
        outState->version != OOT_SCENE_MATERIAL_STATE_VERSION ||
        s_scene.sceneBlob == NULL )
        return false;
    memset( outState, 0, sizeof( *outState ));
    outState->structSize = sizeof( *outState );
    outState->version = OOT_SCENE_MATERIAL_STATE_VERSION;
    outState->simulationFrame = liboot_play()->gameplayFrames;
    outState->referenceCount = liboot_scene_material_reference_count();
    outState->segmentMask = liboot_scene_material_segment_mask();
    outState->drawConfigId = s_scene.drawConfigId;
    outState->referencesTruncated = liboot_scene_material_references_truncated();
    return true;
}

bool oot_scene_get_material_reference(
    int32_t index, struct OoTSceneMaterialReference *outReference )
{
    if( outReference == NULL ||
        outReference->structSize < sizeof( *outReference ) ||
        outReference->version != OOT_SCENE_MATERIAL_REFERENCE_VERSION ||
        index < 0 || s_scene.sceneBlob == NULL )
        return false;
    return liboot_scene_material_reference_get((uint32_t)index, outReference );
}

int32_t oot_scene_get_actor_count( void )
{
    return s_scene.sceneBlob != NULL && s_scene.numActors <= INT32_MAX
               ? (int32_t)s_scene.numActors : 0;
}

bool oot_scene_get_actor( int32_t index, struct OoTSceneActorEntry *outEntry )
{
    if( outEntry == NULL || outEntry->structSize < sizeof( *outEntry ) ||
        outEntry->version != OOT_SCENE_ACTOR_ENTRY_VERSION || index < 0 ||
        (uint32_t)index >= s_scene.numActors || s_scene.actors == NULL )
        return false;
    *outEntry = s_scene.actors[index];
    return true;
}

int32_t oot_scene_get_exit_count( void )
{
    return s_scene.sceneBlob != NULL ? s_scene.numExits : 0;
}

bool oot_scene_get_exit( int32_t index, int16_t *outEntranceIndex )
{
    if( outEntranceIndex != NULL ) *outEntranceIndex = -1;
    if( outEntranceIndex == NULL || s_scene.sceneBlob == NULL ||
        index < 0 || index >= s_scene.numExits || s_scene.exits == NULL )
        return false;
    *outEntranceIndex = s_scene.exits[index];
    return true;
}

bool oot_world_event_poll( struct OoTWorldEvent *outEvent )
{
    if( outEvent == NULL || outEvent->structSize < sizeof( *outEvent ) ||
        outEvent->version != OOT_WORLD_EVENT_VERSION ||
        s_worldEvents.count == 0u )
        return false;
    *outEvent = s_worldEvents.queue[s_worldEvents.head];
    s_worldEvents.head = ( s_worldEvents.head + 1u ) % WORLD_EVENT_QUEUE_CAPACITY;
    s_worldEvents.count--;
    return true;
}

void liboot_world_events_observe_pose( float y )
{
    if( y >= -4000.0f && s_worldEvents.contactKind == OOT_WORLD_EVENT_VOID_Y ) {
        s_worldEvents.contactKind = 0u;
        s_worldEvents.contactExitIndex = 0;
        s_worldEvents.contactFloorProperty = 0u;
    }
}

bool oot_scene_get_geometry( const float **position, const float **normal,
                             const float **color, const float **uv,
                             const uint16_t **triTexture,
                             uint32_t *numTriangles, uint32_t *xluStartTriangle )
{
    if( !s_scene.roomBlob || !s_scene.pos ) return false;
    if( position )    *position = s_scene.pos;
    if( normal )      *normal = s_scene.nrm;
    if( color )       *color = s_scene.col;
    if( uv )          *uv = s_scene.uv;
    if( triTexture )  *triTexture = s_scene.triTex;
    if( numTriangles )     *numTriangles = s_scene.numTris;
    if( xluStartTriangle ) *xluStartTriangle = s_scene.xluStart;
    return true;
}

bool oot_scene_get_geometry_batches( const struct OoTGeometryBatch **batches,
                                     uint32_t *numBatches )
{
    if( !s_scene.roomBlob || !s_scene.batches ) return false;
    if( batches ) *batches = s_scene.batches;
    if( numBatches ) *numBatches = s_scene.numBatches;
    return true;
}

bool oot_scene_get_triangle_flags( const uint8_t **outFlags )
{
    if( !outFlags || !s_scene.roomBlob || !s_scene.triFlags ) return false;
    *outFlags = s_scene.triFlags;
    return true;
}

/* liboot vNEXT: hidden per-vertex scene shade-alpha accessor. Internal so no
   new public raw symbol is added; the engine wrapper surfaces it through
   OoTEngineSceneGeometry.alpha. Parallel to oot_scene_get_geometry's color. */
bool liboot_scene_get_alpha( const float **alpha )
{
    if( !s_scene.roomBlob || !s_scene.pos || !s_scene.alpha ) return false;
    if( alpha ) *alpha = s_scene.alpha;
    return true;
}

/* liboot vNEXT: synchronous room transition. Collision belongs to the scene,
   not the room, and must stay allocated in place: a live Player caches pointers
   to its floor/wall CollisionPoly. Re-entering oot_scene_load here used to free
   those arrays and left the Player with dangling pointers on its next tick. */
int32_t oot_scene_set_room( int32_t roomIndex )
{
    if( !s_scene.sceneBlob || s_scene.curScene < 0 ) return -1;

    SceneHeader sh;
    bool sceneFallback = false;
    if( !walk_scene_header( s_scene.sceneBlob, s_scene.sceneSize, s_scene.layer,
                            &sh, &sceneFallback )) return -4;
    int fullScene = roomIndex == -1;
    int primaryRoom = fullScene ? 0 : roomIndex;
    if(( !fullScene && ( roomIndex < 0 || roomIndex >= sh.numRooms )) ||
       primaryRoom > INT8_MAX ||
       !span_in_bounds( sh.roomListOff, (size_t)sh.numRooms, 8, s_scene.sceneSize ))
        return -5;

    const u8 *rf = s_scene.sceneBlob + sh.roomListOff + (size_t)primaryRoom * 8;
    size_t roomSize = 0;
    u8 *roomBlob = extract_vrom_file( be32( rf ), be32( rf + 4 ), &roomSize );
    if( !roomBlob ) return -6;

    RoomHeader rh;
    bool roomFallback = false;
    if( !walk_room_header( roomBlob, roomSize, s_scene.layer, &rh,
                           &roomFallback )) {
        free( roomBlob );
        return -8;
    }
    struct OoTSceneActorEntry *actors = NULL;
    uint32_t numActors = 0u;
    if( !build_actor_catalog( s_scene.sceneBlob, s_scene.sceneSize, &sh,
                              roomBlob, roomSize, &rh, roomIndex,
                              s_scene.layer, &actors, &numActors )) {
        free( roomBlob );
        return -11;
    }
    BackgroundCatalog backgrounds;
    if( !build_background_catalog( s_scene.sceneBlob, s_scene.sceneSize, &sh,
                                   roomBlob, roomSize, &rh, roomIndex,
                                   s_scene.layer, &backgrounds )) {
        free( actors );
        free( roomBlob );
        return -12;
    }

    /* All fallible extraction/header validation completed. Replace only the
       room blob/runtime/mesh; s_scene.col* and play->colCtx remain untouched. */
    world_events_clear();
    liboot_gfx_evict_scene();
    liboot_register_segment_span( 3, roomBlob, roomSize );
    free( s_scene.roomBlob );
    free( s_scene.actors );
    background_entries_free( s_scene.backgrounds, s_scene.numBackgrounds );
    s_scene.roomBlob = roomBlob;
    s_scene.roomSize = roomSize;
    s_scene.actors = actors;
    s_scene.numActors = numActors;
    s_scene.backgrounds = backgrounds.entries;
    s_scene.numBackgrounds = backgrounds.count;
    s_scene.curRoom = roomIndex;
    s_scene.roomMetadataValid = rh.hasBehavior && rh.hasEcho;
    s_scene.layerFallback = sceneFallback || roomFallback;

    PlayState *play = liboot_play();
    apply_scene_runtime( play, s_scene.curScene, primaryRoom, &sh, &rh );
    play->sceneDrawConfig = s_scene.drawConfigId;
    play->transitionTrigger = TRANS_TRIGGER_OFF;

    s_scene.numTris = s_scene.xluStart = s_scene.numBatches = 0;
    bool meshOk;
    if( fullScene ) {
        meshOk = interpret_all_rooms( s_scene.sceneBlob, s_scene.sceneSize,
                                      sh.roomListOff, sh.numRooms );
        liboot_register_segment_span( 3, s_scene.roomBlob, s_scene.roomSize );
    } else {
        meshOk = interpret_room_mesh();
    }
    return meshOk ? 0 : -9;
}

int32_t oot_scene_get_door_count( void )
{
    return ( s_scene.sceneBlob && s_scene.doorListOff ) ? s_scene.numDoors : 0;
}

bool oot_scene_get_door( int32_t index, struct OoTDoor *outDoor )
{
    if( !outDoor || !s_scene.sceneBlob || s_scene.numDoors <= 0 ) return false;
    if( index < 0 || index >= s_scene.numDoors ) return false;
    if( !span_in_bounds( s_scene.doorListOff, (size_t)s_scene.numDoors, 0x10, s_scene.sceneSize ))
        return false;
    const u8 *e = s_scene.sceneBlob + s_scene.doorListOff + (size_t)index * 0x10;
    outDoor->frontRoom = (int8_t)e[0];   /* sides[0].room */
    outDoor->backRoom  = (int8_t)e[2];   /* sides[1].room */
    outDoor->actorId   = (int16_t)(( e[4] << 8 ) | e[5] );
    outDoor->pos[0]    = (float)(int16_t)(( e[6] << 8 ) | e[7] );
    outDoor->pos[1]    = (float)(int16_t)(( e[8] << 8 ) | e[9] );
    outDoor->pos[2]    = (float)(int16_t)(( e[10] << 8 ) | e[11] );
    outDoor->yaw       = (int16_t)(( e[12] << 8 ) | e[13] );
    return true;
}

int32_t oot_scene_get_sequence_id( void )
{
    return s_scene.sceneBlob ? s_scene.seqId : -1;
}

int32_t oot_scene_get_ambience_id( void )
{
    return s_scene.sceneBlob ? s_scene.ambienceId : -1;
}

bool oot_scene_get_environment( struct OoTSceneEnvironment *out )
{
    if( out == NULL ) return false;
    if( !s_scene.sceneBlob ) { memset( out, 0, sizeof( *out )); return false; }
    *out = s_scene.env;
    return true;
}

bool oot_scene_get_runtime( struct OoTSceneRuntime *out )
{
    if( out == NULL ) return false;
    memset( out, 0, sizeof( *out ));
    out->structSize = sizeof( *out );
    out->version = OOT_SCENE_RUNTIME_VERSION;
    out->sceneIndex = -1;
    out->activeRoomIndex = -1;
    out->geometryRoomIndex = -1;
    out->worldMapArea = -1;
    if( !s_scene.sceneBlob ) return false;

    PlayState *play = liboot_play();
    out->sceneIndex = play->sceneId;
    out->activeRoomIndex = play->roomCtx.curRoom.num;
    out->geometryRoomIndex = s_scene.curRoom;
    out->roomCount = s_scene.numRooms;
    out->worldMapArea = gSaveContext.worldMapArea;
    out->roomType = play->roomCtx.curRoom.type;
    out->environmentType = play->roomCtx.curRoom.environmentType;
    out->echo = play->roomCtx.curRoom.echo;
    out->lensMode = play->roomCtx.curRoom.lensMode;
    out->warpSongsDisabled = play->msgCtx.disableWarpSongs != 0;
    out->sceneCamType = (uint8_t)R_SCENE_CAM_TYPE;
    out->allRoomsLoaded = s_scene.curRoom == -1;
    out->roomMetadataValid = s_scene.roomMetadataValid;
    return true;
}

bool oot_scene_spawn( int32_t spawnIndex, float outPos[3], int16_t *outYaw )
{
    const u8 *scn = s_scene.sceneBlob;
    if( !scn || spawnIndex < 0 ) return false;
    if( !s_scene.spawnListOff || !s_scene.playerEntryOff ) return false;

    /* spawn count lives in the entrance table (not the scene header): the
       list is only guaranteed for index 0. Out-of-range indices are rejected
       by the actor-id gate below (player entries carry ACTOR_PLAYER = 0). */
    size_t index = (size_t)spawnIndex;
    if( s_scene.spawnListOff > s_scene.sceneSize ||
        index >= ( s_scene.sceneSize - s_scene.spawnListOff ) / 2 )
        return false;
    size_t off = s_scene.spawnListOff + index * 2;
    u8 entryIdx = scn[off];

    if( s_scene.playerEntryOff > s_scene.sceneSize ||
        (size_t)entryIdx >= ( s_scene.sceneSize - s_scene.playerEntryOff ) / 0x10 )
        return false;
    size_t pe = s_scene.playerEntryOff + (size_t)entryIdx * 0x10;
    if( be16s( scn + pe ) != 0 ) return false;   /* not a player entry */

    if( outPos ) {
        outPos[0] = be16s( scn + pe + 2 );
        outPos[1] = be16s( scn + pe + 4 );
        outPos[2] = be16s( scn + pe + 6 );
    }
    if( outYaw )
        *outYaw = be16s( scn + pe + 10 );   /* rot.y, binary angle */
    return true;
}

/* oot_global_terminate hook (called from liboot.c) */
void liboot_scene_terminate( void )
{
    world_events_clear();
    liboot_register_segment_span( 2, NULL, 0 );
    liboot_register_segment_span( 3, NULL, 0 );
    free( s_scene.table );
    free( s_scene.pairs );
    free( s_scene.sceneBlob );
    free( s_scene.roomBlob );
    free( s_scene.actors );
    background_entries_free( s_scene.backgrounds, s_scene.numBackgrounds );
    free( s_scene.colVtx );
    free( s_scene.colPoly );
    free( s_scene.colSurf );
    free( s_scene.colRawSurf );
    free( s_scene.exits );
    free( s_scene.colWb );
    free( s_scene.pos );
    free( s_scene.nrm );
    free( s_scene.col );
    free( s_scene.uv );
    free( s_scene.triTex );
    free( s_scene.alpha );
    free( s_scene.triFlags );
    free( s_scene.batches );
    memset( &s_scene, 0, sizeof( s_scene ));
}
