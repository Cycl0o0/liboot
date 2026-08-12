/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#ifndef LIBOOT_SCENE_HEADER_H
#define LIBOOT_SCENE_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The retail command executor allows a selected alternate header to replace
 * the remainder of its parent command stream. Keep this parser independent of
 * the native decompilation types: ROM commands are always big-endian bytes. */
#define LIBOOT_SCENE_HEADER_MAX_COMMANDS 64u
#define LIBOOT_SCENE_HEADER_MAX_DEPTH 8u

typedef struct LibootSceneHeader
{
    uint32_t colOff;
    int numRooms;
    uint32_t roomListOff;
    uint32_t playerEntryOff;
    uint32_t spawnListOff;
    int numActors;
    uint32_t actorListOff;
    int numDoors;
    uint32_t doorListOff;
    int seqId;
    int ambienceId;
    int numLights;
    uint32_t lightListOff;
    uint32_t exitListOff;
    uint8_t sceneCamType;
    int16_t worldMapArea;
    bool hasExitList;
} LibootSceneHeader;

typedef struct LibootRoomHeader
{
    uint32_t shapeOff;
    uint8_t type;
    uint8_t environmentType;
    uint8_t lensMode;
    uint8_t disableWarpSongs;
    int8_t echo;
    int numActors;
    uint32_t actorListOff;
    bool hasBehavior;
    bool hasEcho;
} LibootRoomHeader;

/* layer is one of the four non-cutscene SceneLayer values, 0..3. The fallback
 * result is true only when adult-night selected the retail adult-day fallback. */
bool liboot_scene_header_parse( const uint8_t *blob, size_t size, uint8_t layer,
                                LibootSceneHeader *out, bool *outFallback );
bool liboot_room_header_parse( const uint8_t *blob, size_t size, uint8_t layer,
                               LibootRoomHeader *out, bool *outFallback );

#endif
