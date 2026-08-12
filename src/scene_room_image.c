/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#include "scene_room_image.h"

#include <limits.h>
#include <string.h>

#define ROOM_SHAPE_IMAGE_SINGLE 1u
#define ROOM_SHAPE_IMAGE_MULTI 2u
#define ROOM_SHAPE_IMAGE_MULTI_ENTRY_SIZE 0x1Cu
#define N64_IMAGE_FORMAT_CI 2u

static uint16_t read_be16( const uint8_t *p )
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1] );
}

static uint32_t read_be32( const uint8_t *p )
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static bool span_in_bounds( size_t offset, size_t count, size_t stride,
                            size_t size )
{
    return stride != 0u && offset <= size && count <= ( size - offset ) / stride;
}

static bool segmented_view( uint32_t address,
                            const uint8_t *scene, size_t sceneSize,
                            const uint8_t *room, size_t roomSize,
                            const uint8_t **outData, size_t *outAvailable )
{
    const uint8_t *blob;
    size_t size;
    uint32_t segment = address >> 24;
    size_t offset = address & 0x00FFFFFFu;
    if( segment == 2u ) {
        blob = scene;
        size = sceneSize;
    } else if( segment == 3u ) {
        blob = room;
        size = roomSize;
    } else {
        return false;
    }
    if( blob == NULL || offset > size ) return false;
    *outData = blob + offset;
    *outAvailable = size - offset;
    return true;
}

static bool raw_image_size( uint16_t width, uint16_t height, uint8_t size,
                            size_t *outSize )
{
    uint64_t pixels = (uint64_t)width * (uint64_t)height;
    uint64_t bytes;
    if( pixels == 0u || size > 3u ) return false;
    switch( size ) {
    case 0u: bytes = ( pixels + 1u ) / 2u; break;
    case 1u: bytes = pixels; break;
    case 2u: bytes = pixels * 2u; break;
    default: bytes = pixels * 4u; break;
    }
    if( bytes > SIZE_MAX ) return false;
    *outSize = (size_t)bytes;
    return true;
}

bool liboot_room_image_jpeg_size( const uint8_t *data, size_t available,
                                  size_t *outSize )
{
    size_t pos = 2u;
    bool inScan = false;
    bool sawScan = false;
    if( outSize != NULL ) *outSize = 0u;
    /* This is the exact signature Room_DecodeJpeg recognizes in retail. */
    if( data == NULL || outSize == NULL || available < 4u ||
        data[0] != 0xFFu || data[1] != 0xD8u ||
        data[2] != 0xFFu || data[3] != 0xE0u )
        return false;

    while( pos < available ) {
        uint8_t marker;
        if( !inScan ) {
            if( data[pos++] != 0xFFu ) return false;
            while( pos < available && data[pos] == 0xFFu ) pos++;
            if( pos >= available ) return false;
            marker = data[pos++];
            if( marker == 0x00u ) return false;
        } else {
            while( pos < available && data[pos] != 0xFFu ) pos++;
            if( pos >= available ) return false;
            while( pos < available && data[pos] == 0xFFu ) pos++;
            if( pos >= available ) return false;
            marker = data[pos++];
            if( marker == 0x00u || ( marker >= 0xD0u && marker <= 0xD7u ))
                continue; /* escaped 0xFF or an entropy restart marker */
            inScan = false;
        }

        if( marker == 0xD9u ) {
            if( !sawScan ) return false;
            *outSize = pos;
            return true;
        }
        if( marker == 0xD8u ) return false; /* nested SOI */
        if( marker == 0x01u || ( marker >= 0xD0u && marker <= 0xD7u ))
            continue; /* standalone markers */
        if( pos > available || available - pos < 2u ) return false;
        uint16_t length = read_be16( data + pos );
        if( length < 2u || (size_t)length > available - pos ) return false;
        pos += length;
        if( marker == 0xDAu ) {
            sawScan = true;
            inScan = true;
        }
    }
    return false;
}

bool liboot_room_image_header_parse( const uint8_t *room, size_t roomSize,
                                     uint32_t shapeOffset,
                                     LibootRoomImageHeader *out )
{
    uint32_t entryAddress;
    size_t entryOffset;
    if( room == NULL || out == NULL ||
        !span_in_bounds( shapeOffset, 1u, 8u, roomSize ) ||
        room[shapeOffset] != 1u )
        return false;
    memset( out, 0, sizeof( *out ));
    entryAddress = read_be32( room + shapeOffset + 4u );
    if(( entryAddress >> 24 ) != 3u ) return false;
    entryOffset = entryAddress & 0x00FFFFFFu;
    if( !span_in_bounds( entryOffset, 1u, 8u, roomSize )) return false;
    out->displayListEntryAddress = entryAddress;
    out->displayListEntryOffset = (uint32_t)entryOffset;
    out->amountType = room[shapeOffset + 1u];

    if( out->amountType == ROOM_SHAPE_IMAGE_SINGLE ) {
        if( !span_in_bounds( shapeOffset, 1u, 0x20u, roomSize )) return false;
        out->numBackgrounds = 1u;
        out->backgroundsOffset = shapeOffset + 8u;
        return true;
    }
    if( out->amountType == ROOM_SHAPE_IMAGE_MULTI ) {
        uint32_t backgroundsAddress;
        if( !span_in_bounds( shapeOffset, 1u, 0x10u, roomSize )) return false;
        out->numBackgrounds = room[shapeOffset + 8u];
        backgroundsAddress = read_be32( room + shapeOffset + 0x0Cu );
        if( out->numBackgrounds == 0u ||
            ( backgroundsAddress >> 24 ) != 3u )
            return false;
        out->backgroundsOffset = backgroundsAddress & 0x00FFFFFFu;
        return span_in_bounds( out->backgroundsOffset, out->numBackgrounds,
                               ROOM_SHAPE_IMAGE_MULTI_ENTRY_SIZE, roomSize );
    }
    return false;
}

bool liboot_room_image_background_get(
    const uint8_t *scene, size_t sceneSize,
    const uint8_t *room, size_t roomSize,
    const LibootRoomImageHeader *header, uint32_t index,
    LibootRoomImageBackground *out )
{
    const uint8_t *entry;
    const uint8_t *source;
    size_t available;
    size_t imageSize;
    if( room == NULL || header == NULL || out == NULL ||
        index >= header->numBackgrounds )
        return false;
    memset( out, 0, sizeof( *out ));
    out->cameraIndex = -1;

    if( header->amountType == ROOM_SHAPE_IMAGE_SINGLE ) {
        size_t shapeOffset = header->backgroundsOffset - 8u;
        if( !span_in_bounds( shapeOffset, 1u, 0x20u, roomSize )) return false;
        entry = room + shapeOffset;
        out->sourceAddress = read_be32( entry + 0x08u );
        out->sourceMetadata = read_be32( entry + 0x0Cu );
        out->tlutAddress = read_be32( entry + 0x10u );
        out->width = read_be16( entry + 0x14u );
        out->height = read_be16( entry + 0x16u );
        out->format = entry[0x18u];
        out->size = entry[0x19u];
        out->tlutMode = read_be16( entry + 0x1Au );
        out->tlutCount = read_be16( entry + 0x1Cu );
    } else if( header->amountType == ROOM_SHAPE_IMAGE_MULTI ) {
        size_t offset = header->backgroundsOffset +
                        (size_t)index * ROOM_SHAPE_IMAGE_MULTI_ENTRY_SIZE;
        if( !span_in_bounds( offset, 1u, ROOM_SHAPE_IMAGE_MULTI_ENTRY_SIZE,
                             roomSize ))
            return false;
        entry = room + offset;
        out->entryFlags = read_be16( entry );
        out->cameraIndex = entry[2u];
        out->sourceAddress = read_be32( entry + 0x04u );
        out->sourceMetadata = read_be32( entry + 0x08u );
        out->tlutAddress = read_be32( entry + 0x0Cu );
        out->width = read_be16( entry + 0x10u );
        out->height = read_be16( entry + 0x12u );
        out->format = entry[0x14u];
        out->size = entry[0x15u];
        out->tlutMode = read_be16( entry + 0x16u );
        out->tlutCount = read_be16( entry + 0x18u );
    } else {
        return false;
    }

    if( out->format > 4u || out->size > 3u ) return false;
    if( out->sourceAddress != 0u ) {
        if( out->width == 0u || out->height == 0u ||
            !segmented_view( out->sourceAddress, scene, sceneSize, room,
                             roomSize, &source, &available ))
            return false;
        if( available >= 4u && source[0] == 0xFFu && source[1] == 0xD8u &&
            source[2] == 0xFFu && source[3] == 0xE0u ) {
            if( !liboot_room_image_jpeg_size( source, available, &imageSize ))
                return false;
            out->encoding = LIBOOT_ROOM_IMAGE_ENCODING_JPEG;
        } else {
            if( !raw_image_size( out->width, out->height, out->size,
                                 &imageSize ) || imageSize > available )
                return false;
            out->encoding = LIBOOT_ROOM_IMAGE_ENCODING_RAW;
        }
        out->imageBytes = source;
        out->imageByteCount = imageSize;
    }

    if( out->format == N64_IMAGE_FORMAT_CI ) {
        size_t tlutSize;
        const uint8_t *tlut;
        if( out->tlutCount == 0u || out->tlutCount > 256u ||
            out->tlutAddress == 0u )
            return false;
        tlutSize = (size_t)out->tlutCount * 2u;
        if( !segmented_view( out->tlutAddress, scene, sceneSize, room,
                             roomSize, &tlut, &available ) ||
            tlutSize > available )
            return false;
        out->tlutBytes = tlut;
        out->tlutByteCount = tlutSize;
    }
    return true;
}
