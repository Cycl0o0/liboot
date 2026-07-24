/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * Minimal headless liboot host: initialize from a user-provided ROM, create a
 * floor, spawn Link, run one second of input, and inspect renderer-neutral
 * geometry. A real engine performs oot_link_tick at a fixed 20 Hz and uploads
 * changed textures through oot_get_texture().
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "liboot.h"

static float sPosition[OOT_GEO_MAX_TRIANGLES * 9];
static float sNormal[OOT_GEO_MAX_TRIANGLES * 9];
static float sColor[OOT_GEO_MAX_TRIANGLES * 9];
static float sUv[OOT_GEO_MAX_TRIANGLES * 6];
static uint16_t sTexture[OOT_GEO_MAX_TRIANGLES];

static int read_file( const char *path, uint8_t **outData, size_t *outSize )
{
    FILE *file = fopen( path, "rb" );
    if( !file ) {
        fprintf( stderr, "%s: %s\n", path, strerror( errno ));
        return 0;
    }
    if( fseek( file, 0, SEEK_END ) != 0 ) { fclose( file ); return 0; }
    long length = ftell( file );
    if( length <= 0 || fseek( file, 0, SEEK_SET ) != 0 ) { fclose( file ); return 0; }
    uint8_t *data = malloc((size_t)length );
    if( !data ) { fclose( file ); return 0; }
    if( fread( data, 1, (size_t)length, file ) != (size_t)length ) {
        free( data );
        fclose( file );
        return 0;
    }
    fclose( file );
    *outData = data;
    *outSize = (size_t)length;
    return 1;
}

int main( int argc, char **argv )
{
    if( argc != 2 ) {
        fprintf( stderr, "usage: %s <legally-obtained-oot-rom.z64>\n", argv[0] );
        return 2;
    }

    uint8_t *rom = NULL;
    size_t romSize = 0;
    if( !read_file( argv[1], &rom, &romSize )) return 1;

    oot_global_init( rom, romSize, NULL );
    free( rom ); /* initialization extracts everything it needs synchronously */

    const struct OoTSurface floor[2] = {
        { 0, {{ -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 }} },
        { 0, {{ -1000, 0, -1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 }} },
    };
    oot_static_surfaces_load( floor, 2 );

    int32_t link = oot_link_create( 0, 0, 0 );
    if( link < 0 ) {
        fprintf( stderr, "Link creation failed; verify that the ROM is compatible.\n" );
        oot_global_terminate();
        return 1;
    }
    oot_link_set_equipment( link, OOT_SWORD_MASTER, OOT_SHIELD_HYLIAN,
                            OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI );

    struct OoTLinkInputs input = { .camLookZ = 1.0f };
    struct OoTLinkState state = { 0 };
    struct OoTLinkGeometryBuffers geometry = {
        sPosition, sNormal, sColor, sUv, sTexture, 0
    };

    for( int tick = 0; tick < 20; ++tick ) {
        input.stickY = tick < 12 ? 1.0f : 0.0f;
        input.buttonA = tick == 14; /* one-tick A press */
        oot_link_tick( link, &input, &state, &geometry );
    }

    printf( "Link position: %.2f %.2f %.2f\n", state.position[0],
            state.position[1], state.position[2] );
    printf( "Geometry: %u triangles, %d cached textures\n",
            geometry.numTrianglesUsed, oot_get_texture_count());

    oot_link_delete( link );
    oot_global_terminate();
    return 0;
}
