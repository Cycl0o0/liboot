/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include "scene_header.h"

#include <limits.h>
#include <string.h>

typedef struct HeaderWalk
{
    const uint8_t *blob;
    size_t size;
    uint32_t stack[LIBOOT_SCENE_HEADER_MAX_DEPTH];
    size_t commandCount;
    uint8_t segment;
    uint8_t layer;
    uint8_t depth;
    bool fallbackUsed;
} HeaderWalk;

static uint32_t read_be32( const uint8_t *p )
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static bool header_enter( HeaderWalk *walk, uint32_t offset )
{
    if( walk->depth >= LIBOOT_SCENE_HEADER_MAX_DEPTH ||
        offset > walk->size || walk->size - offset < 8u )
        return false;
    for( uint8_t i = 0u; i < walk->depth; ++i )
        if( walk->stack[i] == offset )
            return false;
    walk->stack[walk->depth++] = offset;
    return true;
}

/* Returns -1 for malformed data, 0 when the parent should continue, and 1
 * when target receives an alternate command-stream offset. */
static int header_alternate_target( HeaderWalk *walk, uint32_t listAddress,
                                    uint32_t *target )
{
    uint32_t listOffset;
    uint32_t address;
    size_t index;

    if( walk->layer == 0u )
        return 0;
    if(( listAddress >> 24 ) != walk->segment )
        return -1;
    listOffset = listAddress & 0x00FFFFFFu;
    index = (size_t)walk->layer - 1u;
    /* Written without addition so a hostile offset cannot wrap size_t. */
    if( listOffset > walk->size )
        return -1;
    if( index >= ( walk->size - listOffset ) / 4u )
        return -1;
    address = read_be32( walk->blob + listOffset + index * 4u );

    if( address == 0u && walk->layer == 3u ) {
        /* Retail adult-night fallback: altHeaders[adult day - 1]. */
        if(( walk->size - listOffset ) / 4u <= 1u )
            return -1;
        address = read_be32( walk->blob + listOffset + 4u );
        if( address != 0u )
            walk->fallbackUsed = true;
    }
    if( address == 0u )
        return 0;
    if(( address >> 24 ) != walk->segment )
        return -1;
    *target = address & 0x00FFFFFFu;
    if( *target > walk->size || walk->size - *target < 8u )
        return -1;
    return 1;
}

static bool walk_scene( HeaderWalk *walk, uint32_t headerOffset,
                        LibootSceneHeader *out )
{
    bool result = false;
    if( !header_enter( walk, headerOffset ))
        return false;

    for( size_t off = headerOffset; off <= walk->size && walk->size - off >= 8u;
         off += 8u ) {
        uint8_t cmd;
        uint8_t data1;
        uint32_t data2;
        uint32_t target;
        int alternate;

        if( walk->commandCount++ >= LIBOOT_SCENE_HEADER_MAX_COMMANDS )
            goto done;
        cmd = walk->blob[off];
        data1 = walk->blob[off + 1u];
        data2 = read_be32( walk->blob + off + 4u );
        switch( cmd ) {
        case 0x14: /* END */
            result = out->colOff != UINT32_MAX && out->numRooms > 0;
            goto done;
        case 0x00: /* PLAYER_ENTRY_LIST */
            if(( data2 >> 24 ) == 2u ) out->playerEntryOff = data2 & 0xFFFFFFu;
            break;
        case 0x01: /* ACTOR_ENTRY_LIST */
            if(( data2 >> 24 ) == 2u ) {
                out->numActors = data1;
                out->actorListOff = data2 & 0xFFFFFFu;
            }
            break;
        case 0x03: /* COLLISION_HEADER */
            if(( data2 >> 24 ) == 2u ) out->colOff = data2 & 0xFFFFFFu;
            break;
        case 0x04: /* ROOM_LIST */
            if(( data2 >> 24 ) == 2u ) {
                out->numRooms = data1;
                out->roomListOff = data2 & 0xFFFFFFu;
            }
            break;
        case 0x06: /* SPAWN_LIST */
            if(( data2 >> 24 ) == 2u ) out->spawnListOff = data2 & 0xFFFFFFu;
            break;
        case 0x0E: /* TRANSITION_ACTOR_LIST */
            if(( data2 >> 24 ) == 2u ) {
                out->numDoors = data1;
                out->doorListOff = data2 & 0xFFFFFFu;
            }
            break;
        case 0x0F: /* LIGHT_SETTINGS_LIST */
            if(( data2 >> 24 ) == 2u ) {
                out->numLights = data1;
                out->lightListOff = data2 & 0xFFFFFFu;
            }
            break;
        case 0x13: /* EXIT_LIST */
            if(( data2 >> 24 ) == 2u ) {
                out->exitListOff = data2 & 0xFFFFFFu;
                out->hasExitList = true;
            }
            break;
        case 0x15: /* SOUND_SETTINGS */
            out->seqId = (int)( data2 & 0xFFu );
            out->ambienceId = (int)(( data2 >> 8 ) & 0xFFu );
            break;
        case 0x18: /* ALTERNATE_HEADER_LIST */
            alternate = header_alternate_target( walk, data2, &target );
            if( alternate < 0 )
                goto done;
            if( alternate > 0 ) {
                result = walk_scene( walk, target, out );
                goto done; /* selected alternate suppresses the parent tail */
            }
            break;
        case 0x19: /* MISC_SETTINGS */
            out->sceneCamType = data1;
            out->worldMapArea = (int16_t)data2;
            break;
        default:
            break;
        }
    }

done:
    walk->depth--;
    return result;
}

static bool walk_room( HeaderWalk *walk, uint32_t headerOffset,
                       LibootRoomHeader *out )
{
    bool result = false;
    if( !header_enter( walk, headerOffset ))
        return false;

    for( size_t off = headerOffset; off <= walk->size && walk->size - off >= 8u;
         off += 8u ) {
        uint8_t cmd;
        uint32_t data2;
        uint32_t target;
        int alternate;

        if( walk->commandCount++ >= LIBOOT_SCENE_HEADER_MAX_COMMANDS )
            goto done;
        cmd = walk->blob[off];
        data2 = read_be32( walk->blob + off + 4u );
        switch( cmd ) {
        case 0x01: /* ACTOR_ENTRY_LIST */
            if(( data2 >> 24 ) == 3u ) {
                out->numActors = walk->blob[off + 1u];
                out->actorListOff = data2 & 0xFFFFFFu;
            }
            break;
        case 0x08: /* ROOM_BEHAVIOR */
            out->type = walk->blob[off + 1u];
            out->environmentType = (uint8_t)data2;
            out->lensMode = (uint8_t)(( data2 >> 8 ) & 1u );
            out->disableWarpSongs = (uint8_t)(( data2 >> 10 ) & 1u );
            out->hasBehavior = true;
            break;
        case 0x0A: /* ROOM_SHAPE */
            if(( data2 >> 24 ) == 3u && ( data2 & 0xFFFFFFu ) < walk->size )
                out->shapeOff = data2 & 0xFFFFFFu;
            break;
        case 0x14: /* END */
            result = out->shapeOff != UINT32_MAX;
            goto done;
        case 0x16: /* ECHO_SETTINGS */
            out->echo = (int8_t)(uint8_t)data2;
            out->hasEcho = true;
            break;
        case 0x18: /* ALTERNATE_HEADER_LIST */
            alternate = header_alternate_target( walk, data2, &target );
            if( alternate < 0 )
                goto done;
            if( alternate > 0 ) {
                result = walk_room( walk, target, out );
                goto done;
            }
            break;
        default:
            break;
        }
    }

done:
    walk->depth--;
    return result;
}

bool liboot_scene_header_parse( const uint8_t *blob, size_t size, uint8_t layer,
                                LibootSceneHeader *out, bool *outFallback )
{
    HeaderWalk walk;
    bool result;
    if( outFallback != NULL ) *outFallback = false;
    if( blob == NULL || out == NULL || layer > 3u )
        return false;
    memset( out, 0, sizeof( *out ));
    out->colOff = UINT32_MAX;
    out->exitListOff = UINT32_MAX;
    out->seqId = -1;
    out->ambienceId = -1;
    memset( &walk, 0, sizeof( walk ));
    walk.blob = blob;
    walk.size = size;
    walk.segment = 2u;
    walk.layer = layer;
    result = walk_scene( &walk, 0u, out );
    if( outFallback != NULL ) *outFallback = walk.fallbackUsed;
    return result;
}

bool liboot_room_header_parse( const uint8_t *blob, size_t size, uint8_t layer,
                               LibootRoomHeader *out, bool *outFallback )
{
    HeaderWalk walk;
    bool result;
    if( outFallback != NULL ) *outFallback = false;
    if( blob == NULL || out == NULL || layer > 3u )
        return false;
    memset( out, 0, sizeof( *out ));
    out->shapeOff = UINT32_MAX;
    memset( &walk, 0, sizeof( walk ));
    walk.blob = blob;
    walk.size = size;
    walk.segment = 3u;
    walk.layer = layer;
    result = walk_room( &walk, 0u, out );
    if( outFallback != NULL ) *outFallback = walk.fallbackUsed;
    return result;
}
