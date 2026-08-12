/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include "scene_room_image.h"

#include <stdio.h>
#include <string.h>

static int expect( const char *label, int condition )
{
    if( condition ) return 1;
    fprintf( stderr, "FAIL: %s\n", label );
    return 0;
}

static void put_be16( unsigned char *p, unsigned value )
{
    p[0] = (unsigned char)( value >> 8 );
    p[1] = (unsigned char)value;
}

static void put_be32( unsigned char *p, unsigned value )
{
    p[0] = (unsigned char)( value >> 24 );
    p[1] = (unsigned char)( value >> 16 );
    p[2] = (unsigned char)( value >> 8 );
    p[3] = (unsigned char)value;
}

static size_t put_jpeg( unsigned char *p )
{
    static const unsigned char jpeg[] = {
        0xFF, 0xD8,
        0xFF, 0xE0, 0x00, 0x04, 0x12, 0x34,
        0xFF, 0xDB, 0x00, 0x04, 0x56, 0x78,
        0xFF, 0xDA, 0x00, 0x08, 1, 2, 3, 4, 5, 6,
        0x11, 0xFF, 0x00, 0x22, 0xFF, 0xD0, 0x33,
        0xFF, 0xD9,
        0xAA, 0xBB /* trailing room data is not part of the encoded span */
    };
    memcpy( p, jpeg, sizeof( jpeg ));
    return sizeof( jpeg ) - 2u;
}

static int test_single_raw( void )
{
    unsigned char room[0x100];
    LibootRoomImageHeader header;
    LibootRoomImageBackground background;
    int ok = 1;
    memset( room, 0, sizeof( room ));
    room[0] = 1u;
    room[1] = 1u;
    put_be32( room + 4, 0x03000040u );
    put_be32( room + 8, 0x03000080u );
    put_be32( room + 0x0C, 0x11223344u );
    put_be16( room + 0x14, 3u );
    put_be16( room + 0x16, 2u );
    room[0x18] = 0u; /* RGBA */
    room[0x19] = 2u; /* 16b */

    ok &= expect( "single header",
                  liboot_room_image_header_parse( room, sizeof( room ), 0u,
                                                  &header ));
    ok &= expect( "single shared display list", header.amountType == 1u &&
                  header.numBackgrounds == 1u &&
                  header.displayListEntryOffset == 0x40u );
    ok &= expect( "single raw background",
                  liboot_room_image_background_get( NULL, 0u, room,
                                                     sizeof( room ), &header,
                                                     0u, &background ));
    ok &= expect( "single raw span", background.cameraIndex == -1 &&
                  background.encoding == LIBOOT_ROOM_IMAGE_ENCODING_RAW &&
                  background.imageBytes == room + 0x80 &&
                  background.imageByteCount == 12u &&
                  background.sourceMetadata == 0x11223344u );
    return ok;
}

static int test_multi_ci_and_jpeg( void )
{
    unsigned char room[0x200];
    LibootRoomImageHeader header;
    LibootRoomImageBackground background;
    size_t jpegSize;
    int ok = 1;
    memset( room, 0, sizeof( room ));
    room[0] = 1u;
    room[1] = 2u;
    put_be32( room + 4, 0x03000040u );
    room[8] = 2u;
    put_be32( room + 0x0C, 0x03000060u );

    put_be16( room + 0x60, 0xCAFEu );
    room[0x62] = 7u;
    put_be32( room + 0x64, 0x030000C0u );
    put_be32( room + 0x68, 0x12345678u );
    put_be32( room + 0x6C, 0x030000D0u );
    put_be16( room + 0x70, 3u );
    put_be16( room + 0x72, 1u );
    room[0x74] = 2u; /* CI */
    room[0x75] = 0u; /* 4b */
    put_be16( room + 0x76, 0x8000u );
    put_be16( room + 0x78, 16u );

    room[0x7C + 2] = 9u;
    put_be32( room + 0x7C + 4, 0x03000100u );
    put_be16( room + 0x7C + 0x10, 320u );
    put_be16( room + 0x7C + 0x12, 240u );
    room[0x7C + 0x14] = 0u;
    room[0x7C + 0x15] = 2u;
    jpegSize = put_jpeg( room + 0x100 );

    ok &= expect( "multi header",
                  liboot_room_image_header_parse( room, sizeof( room ), 0u,
                                                  &header ) &&
                  header.amountType == 2u && header.numBackgrounds == 2u );
    ok &= expect( "multi CI entry",
                  liboot_room_image_background_get( NULL, 0u, room,
                                                     sizeof( room ), &header,
                                                     0u, &background ));
    ok &= expect( "CI sizes and metadata", background.cameraIndex == 7 &&
                  background.entryFlags == 0xCAFEu &&
                  background.imageByteCount == 2u &&
                  background.tlutBytes == room + 0xD0 &&
                  background.tlutByteCount == 32u &&
                  background.tlutMode == 0x8000u );
    ok &= expect( "multi JPEG entry",
                  liboot_room_image_background_get( NULL, 0u, room,
                                                     sizeof( room ), &header,
                                                     1u, &background ));
    ok &= expect( "JPEG exact EOI span", background.cameraIndex == 9 &&
                  background.encoding == LIBOOT_ROOM_IMAGE_ENCODING_JPEG &&
                  background.imageByteCount == jpegSize &&
                  background.imageBytes[jpegSize - 2u] == 0xFFu &&
                  background.imageBytes[jpegSize - 1u] == 0xD9u );
    return ok;
}

static int test_rejections( void )
{
    unsigned char room[0x180];
    LibootRoomImageHeader header;
    LibootRoomImageBackground background;
    int ok = 1;
    memset( room, 0, sizeof( room ));
    room[0] = 1u;
    room[1] = 1u;
    put_be32( room + 4, 0x03000040u );
    put_be32( room + 8, 0x03000100u );
    put_be16( room + 0x14, 320u );
    put_be16( room + 0x16, 240u );
    room[0x18] = 0u;
    room[0x19] = 2u;
    put_jpeg( room + 0x100 );
    room[0x100 + 31] = 0u; /* destroy the EOI marker */
    ok &= expect( "valid rejection header",
                  liboot_room_image_header_parse( room, sizeof( room ), 0u,
                                                  &header ));
    ok &= expect( "JPEG without EOI rejected",
                  !liboot_room_image_background_get( NULL, 0u, room,
                                                      sizeof( room ), &header,
                                                      0u, &background ));

    put_be32( room + 4, 0x02000040u );
    ok &= expect( "scene-segment DL entry rejected",
                  !liboot_room_image_header_parse( room, sizeof( room ), 0u,
                                                   &header ));
    put_be32( room + 4, 0x03000040u );
    room[1] = 3u;
    ok &= expect( "unknown amount type rejected",
                  !liboot_room_image_header_parse( room, sizeof( room ), 0u,
                                                   &header ));
    room[1] = 2u;
    room[8] = 2u;
    put_be32( room + 0x0C, 0x03000170u );
    ok &= expect( "truncated multi table rejected",
                  !liboot_room_image_header_parse( room, sizeof( room ), 0u,
                                                   &header ));
    return ok;
}

int main( void )
{
    int ok = test_single_raw() & test_multi_ci_and_jpeg() & test_rejections();
    if( !ok ) return 1;
    puts( "scene room image tests: ok" );
    return 0;
}
