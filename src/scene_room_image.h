/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#ifndef LIBOOT_SCENE_ROOM_IMAGE_H
#define LIBOOT_SCENE_ROOM_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum LibootRoomImageEncoding
{
    LIBOOT_ROOM_IMAGE_ENCODING_NONE = 0,
    LIBOOT_ROOM_IMAGE_ENCODING_RAW = 1,
    LIBOOT_ROOM_IMAGE_ENCODING_JPEG = 2
};

typedef struct LibootRoomImageHeader
{
    uint8_t amountType;
    uint8_t numBackgrounds;
    uint32_t displayListEntryAddress;
    uint32_t displayListEntryOffset;
    uint32_t backgroundsOffset;
} LibootRoomImageHeader;

typedef struct LibootRoomImageBackground
{
    int16_t cameraIndex;
    uint16_t entryFlags;
    uint8_t encoding;
    uint8_t format;
    uint8_t size;
    uint16_t width;
    uint16_t height;
    uint16_t tlutMode;
    uint16_t tlutCount;
    uint32_t sourceAddress;
    uint32_t sourceMetadata;
    uint32_t tlutAddress;
    const uint8_t *imageBytes;
    size_t imageByteCount;
    const uint8_t *tlutBytes;
    size_t tlutByteCount;
} LibootRoomImageBackground;

/* Parses the type-1 room-shape header and its shared 3D display-list entry.
 * Serialized pointers are accepted only from the scene/room segments used by
 * retail room images, and all returned views are bounded by their source blob. */
bool liboot_room_image_header_parse( const uint8_t *room, size_t roomSize,
                                     uint32_t shapeOffset,
                                     LibootRoomImageHeader *out );
bool liboot_room_image_background_get(
    const uint8_t *scene, size_t sceneSize,
    const uint8_t *room, size_t roomSize,
    const LibootRoomImageHeader *header, uint32_t index,
    LibootRoomImageBackground *out );

/* Returns the exact encoded span from SOI through EOI. This is deliberately a
 * marker parser rather than a byte search: marker payload and entropy escaping
 * are observed, so an embedded 0xFFD9 cannot truncate the borrowed view. */
bool liboot_room_image_jpeg_size( const uint8_t *data, size_t available,
                                  size_t *outSize );

#endif
