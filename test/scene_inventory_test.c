/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include "liboot.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_COUNT ((int)OOT_SCENE_OUTSIDE_GANONS_CASTLE + 1)
#define ROOM_COUNT 128
#define BACKGROUND_KEY_CAPACITY 4096u

struct BackgroundKey
{
    int32_t scene;
    int32_t room;
    int16_t camera;
    uint8_t amount;
    uint8_t encoding;
    uint16_t width;
    uint16_t height;
    uint8_t format;
    uint8_t size;
    uint16_t tlutMode;
    uint16_t tlutCount;
    uint16_t flags;
    uint32_t source;
    uint32_t tlut;
    uint32_t metadata;
    size_t imageBytes;
    size_t tlutBytes;
};

static struct BackgroundKey s_backgroundKeys[BACKGROUND_KEY_CAPACITY];
static uint32_t s_numBackgroundKeys;
static bool s_seenRoomShape[SCENE_COUNT][ROOM_COUNT][3];

static bool background_is_new( int scene, const struct OoTSceneBackground *bg )
{
    struct BackgroundKey key;
    memset( &key, 0, sizeof( key ));
    key.scene = scene;
    key.room = bg->roomIndex;
    key.camera = bg->cameraIndex;
    key.amount = bg->amountType;
    key.encoding = bg->encoding;
    key.width = bg->width;
    key.height = bg->height;
    key.format = bg->format;
    key.size = bg->size;
    key.tlutMode = bg->tlutMode;
    key.tlutCount = bg->tlutCount;
    key.flags = bg->entryFlags;
    key.source = bg->sourceAddress;
    key.tlut = bg->tlutAddress;
    key.metadata = bg->sourceMetadata;
    key.imageBytes = bg->imageByteCount;
    key.tlutBytes = bg->tlutByteCount;
    for( uint32_t i = 0u; i < s_numBackgroundKeys; ++i )
        if( memcmp( &s_backgroundKeys[i], &key, sizeof( key )) == 0 )
            return false;
    if( s_numBackgroundKeys >= BACKGROUND_KEY_CAPACITY ) return false;
    s_backgroundKeys[s_numBackgroundKeys++] = key;
    return true;
}

static unsigned char *read_file( const char *path, size_t *outSize )
{
    FILE *file = fopen( path, "rb" );
    unsigned char *data;
    long length;
    if( file == NULL ) return NULL;
    if( fseek( file, 0, SEEK_END ) != 0 || ( length = ftell( file )) <= 0 ||
        fseek( file, 0, SEEK_SET ) != 0 ) {
        fclose( file );
        return NULL;
    }
    data = malloc((size_t)length );
    if( data == NULL || fread( data, 1u, (size_t)length, file ) != (size_t)length ) {
        free( data ); fclose( file ); return NULL;
    }
    fclose( file );
    *outSize = (size_t)length;
    return data;
}

int main( int argc, char **argv )
{
    unsigned char *rom;
    size_t romSize;
    uint32_t loads = 0u, failed = 0u, singleRooms = 0u, multiRooms = 0u;
    uint32_t geometryUnavailable = 0u;
    uint32_t backgrounds = 0u, jpeg = 0u, raw = 0u, withTlut = 0u;
    uint32_t uniqueJpeg = 0u, uniqueRaw = 0u, uniqueTlut = 0u;
    uint32_t materialRefs = 0u;
    uint16_t materialSegments = 0u;
    if( argc != 2 ) {
        fprintf( stderr, "usage: %s path/to/pal-1.1-rom\n", argv[0] );
        return 2;
    }
    rom = read_file( argv[1], &romSize );
    if( rom == NULL ) return 2;
    oot_global_init( rom, romSize, NULL );

    for( int scene = 0; scene <= OOT_SCENE_OUTSIDE_GANONS_CASTLE; ++scene ) {
        for( int layer = OOT_SCENE_LAYER_CHILD_DAY;
             layer <= OOT_SCENE_LAYER_ADULT_NIGHT; ++layer ) {
            struct OoTSceneLoadOptions options = OOT_SCENE_LOAD_OPTIONS_INIT(
                scene, -1, layer );
            int result = oot_scene_load_ex( &options );
            loads++;
            /* -9 is a committed scene with no interpretable 3D triangles.
               Its background/material catalogs remain valid and must still be
               inventoried (the retail cutscene-map scene exercises this). */
            if( result == -9 ) {
                geometryUnavailable++;
            } else if( result != 0 ) {
                fprintf( stderr, "scene inventory: scene=%d layer=%d error=%d\n",
                         scene, layer, result );
                failed++;
                continue;
            }
            int count = oot_scene_get_background_count();
            int lastRoom = -1;
            const float *position;
            uint32_t sceneTriangles = 0u;
            if( !oot_scene_get_geometry( &position, NULL, NULL, NULL, NULL,
                                         &sceneTriangles, NULL ) ||
                ( sceneTriangles != 0u && position == NULL )) {
                fprintf( stderr, "scene inventory: invalid geometry scene=%d layer=%d\n",
                         scene, layer );
                failed++;
                continue;
            }
            for( int i = 0; i < count; ++i ) {
                struct OoTSceneBackground bg = {
                    .structSize = sizeof( bg ),
                    .version = OOT_SCENE_BACKGROUND_VERSION
                };
                if( !oot_scene_get_background( i, &bg ) || bg.imageBytes == NULL ||
                    bg.imageByteCount == 0u ) {
                    fprintf( stderr, "scene inventory: invalid background scene=%d layer=%d index=%d\n",
                             scene, layer, i );
                    failed++;
                    continue;
                }
                backgrounds++;
                jpeg += bg.encoding == OOT_SCENE_BACKGROUND_JPEG;
                raw += bg.encoding == OOT_SCENE_BACKGROUND_RAW;
                withTlut += bg.tlutByteCount != 0u;
                if( bg.encoding == OOT_SCENE_BACKGROUND_JPEG &&
                    ( bg.imageByteCount < 4u || bg.imageBytes[0] != 0xFFu ||
                      bg.imageBytes[1] != 0xD8u ||
                      bg.imageBytes[bg.imageByteCount - 2u] != 0xFFu ||
                      bg.imageBytes[bg.imageByteCount - 1u] != 0xD9u )) {
                    fprintf( stderr, "scene inventory: invalid JPEG bounds scene=%d layer=%d index=%d\n",
                             scene, layer, i );
                    failed++;
                }
                if( bg.tlutByteCount != (size_t)bg.tlutCount * 2u ||
                    (( bg.amountType == 1u ) != ( bg.cameraIndex == -1 ))) {
                    fprintf( stderr, "scene inventory: invalid background metadata scene=%d layer=%d index=%d\n",
                             scene, layer, i );
                    failed++;
                }
                if( background_is_new( scene, &bg )) {
                    uniqueJpeg += bg.encoding == OOT_SCENE_BACKGROUND_JPEG;
                    uniqueRaw += bg.encoding == OOT_SCENE_BACKGROUND_RAW;
                    uniqueTlut += bg.tlutByteCount != 0u;
                }
                if( bg.roomIndex != lastRoom ) {
                    singleRooms += bg.amountType == 1u;
                    multiRooms += bg.amountType == 2u;
                    lastRoom = bg.roomIndex;
                    if( bg.roomIndex >= 0 && bg.roomIndex < ROOM_COUNT &&
                        bg.amountType <= 2u )
                        s_seenRoomShape[scene][bg.roomIndex][bg.amountType] = true;
                }
            }
            struct OoTSceneMaterialState state = {
                .structSize = sizeof( state ),
                .version = OOT_SCENE_MATERIAL_STATE_VERSION
            };
            if( !oot_scene_get_material_state( &state ) ||
                state.drawConfigId > 52u || state.referencesTruncated ) {
                fprintf( stderr, "scene inventory: invalid material state scene=%d layer=%d\n",
                         scene, layer );
                failed++;
            } else {
                materialRefs += state.referenceCount;
                materialSegments |= state.segmentMask;
                for( uint32_t i = 0u; i < state.referenceCount; ++i ) {
                    struct OoTSceneMaterialReference ref = {
                        .structSize = sizeof( ref ),
                        .version = OOT_SCENE_MATERIAL_REFERENCE_VERSION
                    };
                    if( !oot_scene_get_material_reference((int32_t)i, &ref ) ||
                        ref.segment < 8u || ref.segment > 15u ||
                        ref.roomIndex < 0 ||
                        ref.firstTriangle > sceneTriangles ||
                        ref.numTriangles > sceneTriangles - ref.firstTriangle ||
                        ( ref.kind != OOT_SCENE_MATERIAL_TEXTURE_IMAGE &&
                          ref.numTriangles != 0u )) {
                        fprintf( stderr, "scene inventory: invalid material ref scene=%d layer=%d index=%" PRIu32 "\n",
                                 scene, layer, i );
                        failed++;
                    }
                }
            }
        }
    }

    uint32_t uniqueSingleRooms = 0u, uniqueMultiRooms = 0u;
    for( int scene = 0; scene < SCENE_COUNT; ++scene ) {
        for( int room = 0; room < ROOM_COUNT; ++room ) {
            uniqueSingleRooms += s_seenRoomShape[scene][room][1];
            uniqueMultiRooms += s_seenRoomShape[scene][room][2];
        }
    }

    printf( "scene inventory: loads=%" PRIu32 " failed=%" PRIu32
            " geometry-unavailable=%" PRIu32
            " single-room-loads=%" PRIu32 " multi-room-loads=%" PRIu32
            " backgrounds=%" PRIu32 " jpeg=%" PRIu32 " raw=%" PRIu32
            " tlut=%" PRIu32 " unique-single-rooms=%" PRIu32
            " unique-multi-rooms=%" PRIu32 " unique-backgrounds=%" PRIu32
            " unique-jpeg=%" PRIu32 " unique-raw=%" PRIu32
            " unique-tlut=%" PRIu32 " material-refs=%" PRIu32
            " material-segments=0x%04X\n",
            loads, failed, geometryUnavailable, singleRooms, multiRooms,
            backgrounds, jpeg, raw,
            withTlut, uniqueSingleRooms, uniqueMultiRooms, s_numBackgroundKeys,
            uniqueJpeg, uniqueRaw, uniqueTlut, materialRefs, materialSegments );
    oot_global_terminate();
    free( rom );
    return failed == 0u ? 0 : 1;
}
