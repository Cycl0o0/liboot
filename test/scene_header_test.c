/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include "scene_header.h"

#include <stdio.h>
#include <string.h>

static int expect( const char *label, int condition )
{
    if( condition ) return 1;
    fprintf( stderr, "FAIL: %s\n", label );
    return 0;
}

static void put_be32( unsigned char *p, unsigned value )
{
    p[0] = (unsigned char)( value >> 24 );
    p[1] = (unsigned char)( value >> 16 );
    p[2] = (unsigned char)( value >> 8 );
    p[3] = (unsigned char)value;
}

static void put_cmd( unsigned char *blob, size_t off, unsigned char code,
                     unsigned char data1, unsigned data2 )
{
    blob[off] = code;
    blob[off + 1u] = data1;
    put_be32( blob + off + 4u, data2 );
}

static int test_scene_layers( void )
{
    unsigned char blob[0x300];
    LibootSceneHeader header;
    bool fallback = false;
    int ok = 1;
    memset( blob, 0, sizeof( blob ));

    put_cmd( blob, 0x00, 0x03, 0, 0x020001D0u );
    put_cmd( blob, 0x08, 0x04, 2, 0x020001E0u );
    put_cmd( blob, 0x10, 0x19, 1, 11u );
    put_cmd( blob, 0x18, 0x01, 2, 0x020001F0u );
    put_cmd( blob, 0x20, 0x18, 0, 0x02000080u );
    put_cmd( blob, 0x28, 0x15, 0, 0x00001221u );
    put_cmd( blob, 0x30, 0x14, 0, 0u );
    put_be32( blob + 0x80, 0x020000A0u );
    put_be32( blob + 0x84, 0x020000C0u );
    put_be32( blob + 0x88, 0u );

    put_cmd( blob, 0xA0, 0x19, 2, 22u );
    put_cmd( blob, 0xA8, 0x01, 4, 0x02000200u );
    put_cmd( blob, 0xB0, 0x15, 0, 0x00002332u );
    put_cmd( blob, 0xB8, 0x14, 0, 0u );
    put_cmd( blob, 0xC0, 0x19, 3, 33u );
    put_cmd( blob, 0xC8, 0x01, 5, 0x02000220u );
    put_cmd( blob, 0xD0, 0x15, 0, 0x00003443u );
    put_cmd( blob, 0xD8, 0x14, 0, 0u );

    ok &= expect( "child-day parses",
                  liboot_scene_header_parse( blob, sizeof( blob ), 0, &header,
                                             &fallback ));
    ok &= expect( "child-day parent tail", header.sceneCamType == 1 &&
                  header.worldMapArea == 11 && header.seqId == 0x21 &&
                  header.ambienceId == 0x12 && header.numActors == 2 &&
                  !fallback );
    ok &= expect( "child-night parses",
                  liboot_scene_header_parse( blob, sizeof( blob ), 1, &header,
                                             &fallback ));
    ok &= expect( "child alternate replaces tail", header.colOff == 0x1D0u &&
                  header.numRooms == 2 && header.sceneCamType == 2 &&
                  header.worldMapArea == 22 && header.seqId == 0x32 &&
                  header.numActors == 4 && !fallback );
    ok &= expect( "adult-day parses",
                  liboot_scene_header_parse( blob, sizeof( blob ), 2, &header,
                                             &fallback ));
    ok &= expect( "adult-day selected", header.sceneCamType == 3 &&
                  header.worldMapArea == 33 && header.numActors == 5 &&
                  !fallback );
    ok &= expect( "adult-night fallback parses",
                  liboot_scene_header_parse( blob, sizeof( blob ), 3, &header,
                                             &fallback ));
    ok &= expect( "adult-night uses adult-day", header.sceneCamType == 3 &&
                  header.seqId == 0x43 && fallback );

    put_be32( blob + 0x80, 0u );
    ok &= expect( "missing child alternate continues parent",
                  liboot_scene_header_parse( blob, sizeof( blob ), 1, &header,
                                             &fallback ) &&
                  header.sceneCamType == 1 && header.seqId == 0x21 && !fallback );
    put_be32( blob + 0x80, 0x030000A0u );
    ok &= expect( "selected wrong-segment alternate rejected",
                  !liboot_scene_header_parse( blob, sizeof( blob ), 1, &header,
                                              &fallback ));
    ok &= expect( "child-day never dereferences alternate list",
                  liboot_scene_header_parse( blob, sizeof( blob ), 0, &header,
                                             &fallback ));
    return ok;
}

static int test_room_layers( void )
{
    unsigned char blob[0x240];
    LibootRoomHeader header;
    bool fallback = false;
    int ok = 1;
    memset( blob, 0, sizeof( blob ));

    put_cmd( blob, 0x00, 0x08, 1, 5u | ( 1u << 8 ) | ( 1u << 10 ));
    put_cmd( blob, 0x08, 0x01, 1, 0x03000160u );
    put_cmd( blob, 0x10, 0x18, 0, 0x03000080u );
    put_cmd( blob, 0x18, 0x16, 0, 7u );
    put_cmd( blob, 0x20, 0x0A, 0, 0x03000180u );
    put_cmd( blob, 0x28, 0x14, 0, 0u );
    put_be32( blob + 0x80, 0x030000A0u );
    put_be32( blob + 0x84, 0x030000E0u );
    put_be32( blob + 0x88, 0u );

    put_cmd( blob, 0xA0, 0x08, 2, 9u );
    put_cmd( blob, 0xA8, 0x01, 4, 0x030001A0u );
    put_cmd( blob, 0xB0, 0x16, 0, 3u );
    put_cmd( blob, 0xB8, 0x0A, 0, 0x03000190u );
    put_cmd( blob, 0xC0, 0x14, 0, 0u );
    put_cmd( blob, 0xE0, 0x08, 3, 12u );
    put_cmd( blob, 0xE8, 0x01, 5, 0x030001C0u );
    put_cmd( blob, 0xF0, 0x16, 0, 4u );
    put_cmd( blob, 0xF8, 0x0A, 0, 0x030001A0u );
    put_cmd( blob, 0x100, 0x14, 0, 0u );

    ok &= expect( "main room parses",
                  liboot_room_header_parse( blob, sizeof( blob ), 0, &header,
                                            &fallback ));
    ok &= expect( "main room facts", header.type == 1 &&
                  header.environmentType == 5 && header.lensMode == 1 &&
                  header.disableWarpSongs == 1 && header.echo == 7 &&
                  header.shapeOff == 0x180u && header.numActors == 1 );
    ok &= expect( "child room alternate parses",
                  liboot_room_header_parse( blob, sizeof( blob ), 1, &header,
                                            &fallback ));
    ok &= expect( "child room alternate facts", header.type == 2 &&
                  header.environmentType == 9 && header.echo == 3 &&
                  header.shapeOff == 0x190u && header.numActors == 4 &&
                  !fallback );
    ok &= expect( "adult-night room fallback parses",
                  liboot_room_header_parse( blob, sizeof( blob ), 3, &header,
                                            &fallback ));
    ok &= expect( "adult-night room uses adult-day", header.type == 3 &&
                  header.echo == 4 && header.shapeOff == 0x1A0u &&
                  header.numActors == 5 && fallback );
    return ok;
}

static int test_guards( void )
{
    unsigned char cycle[0x100];
    unsigned char longHeader[65u * 8u];
    LibootSceneHeader header;
    bool fallback;
    int ok = 1;
    memset( cycle, 0, sizeof( cycle ));
    put_cmd( cycle, 0x00, 0x03, 0, 0x020000E0u );
    put_cmd( cycle, 0x08, 0x04, 1, 0x020000E8u );
    put_cmd( cycle, 0x10, 0x18, 0, 0x02000040u );
    put_be32( cycle + 0x40, 0x02000060u );
    put_cmd( cycle, 0x60, 0x18, 0, 0x02000050u );
    put_be32( cycle + 0x50, 0x02000060u );
    ok &= expect( "alternate cycle rejected",
                  !liboot_scene_header_parse( cycle, sizeof( cycle ), 1,
                                              &header, &fallback ));

    memset( longHeader, 0, sizeof( longHeader ));
    put_cmd( longHeader, 0, 0x03, 0, 0x02000100u );
    put_cmd( longHeader, 8, 0x04, 1, 0x02000120u );
    for( size_t i = 2u; i < 64u; ++i )
        put_cmd( longHeader, i * 8u, 0x19, 0, (unsigned)i );
    put_cmd( longHeader, 64u * 8u, 0x14, 0, 0u );
    ok &= expect( "global command budget enforced",
                  !liboot_scene_header_parse( longHeader, sizeof( longHeader ), 0,
                                              &header, &fallback ));
    ok &= expect( "truncated command rejected",
                  !liboot_scene_header_parse( cycle, 7u, 0, &header, &fallback ));
    ok &= expect( "unsupported cutscene layer rejected",
                  !liboot_scene_header_parse( cycle, sizeof( cycle ), 4,
                                              &header, &fallback ));
    return ok;
}

int main( void )
{
    int ok = test_scene_layers() & test_room_layers() & test_guards();
    if( !ok ) return 1;
    puts( "scene header tests: ok" );
    return 0;
}
