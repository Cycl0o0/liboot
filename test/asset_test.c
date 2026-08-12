/* ROM-backed rendering/audio regression probe. No copyrighted assets are
 * embedded: geometry, textures and samples all come from the supplied ROM. */
#include "liboot.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float sPos[OOT_GEO_MAX_TRIANGLES * 9];
static float sNrm[OOT_GEO_MAX_TRIANGLES * 9];
static float sCol[OOT_GEO_MAX_TRIANGLES * 9];
static float sUv[OOT_GEO_MAX_TRIANGLES * 6];
static uint16_t sTriTex[OOT_GEO_MAX_TRIANGLES];
static struct OoTLinkGeometryBuffers sGeo = { sPos, sNrm, sCol, sUv, sTriTex, 0 };
static uint8_t sTextureChecked[512];
static int sLegacySfxCount, sDetailedSfxCount, sBadSfxEvent;

/* Exact pre-capacity prefix used to verify that oot_link_tick remains safe for
   binaries compiled against the historical raw geometry structure. */
struct LegacyLinkGeometryBuffers
{
    float *position;
    float *normal;
    float *color;
    float *uv;
    uint16_t *triTexture;
    uint16_t numTrianglesUsed;
    float *alpha;
    uint8_t *triFlags;
};

_Static_assert(sizeof(struct LegacyLinkGeometryBuffers) ==
                   offsetof(struct OoTLinkGeometryBuffers, triangleCapacity),
               "legacy geometry prefix layout changed");

static int geometry_capacity_and_abi_check( int32_t link )
{
    struct OoTLinkInputs input = { .camLookZ = 1.0f };
    struct OoTLinkState state;
    float position[10] = { 0 };
    float normal[10] = { 0 };
    float color[10] = { 0 };
    float uv[7] = { 0 };
    float alpha[4] = { 0 };
    uint16_t triTexture[2] = { 0, 0xA55Au };
    uint8_t triFlags[2] = { 0, 0xA5u };
    struct OoTGeometryBatch batches[2];
    struct OoTGeometryBatch batchCanary;
    struct OoTLinkGeometryBuffers geometry = {
        .position = position,
        .normal = normal,
        .color = color,
        .uv = uv,
        .triTexture = triTexture,
        .alpha = alpha,
        .triFlags = triFlags,
        .triangleCapacity = 1,
        .batches = batches,
        .batchCapacity = 1,
    };
    struct {
        struct LegacyLinkGeometryBuffers geometry;
        unsigned char canary[32];
    } legacy;
    int ok = 1;

    position[9] = 1000001.0f;
    normal[9] = 1000002.0f;
    color[9] = 1000003.0f;
    uv[6] = 1000004.0f;
    alpha[3] = 1000005.0f;
    memset( batches, 0, sizeof( batches ));
    memset( &batches[1], 0xA5, sizeof( batches[1] ));
    batchCanary = batches[1];

    oot_link_tick_sized( link, &input, &state, &geometry,
                         (uint32_t)sizeof( geometry ));
    if( geometry.numTrianglesUsed != 1u ||
        geometry.numTrianglesUsed32 != 1u ||
        geometry.numBatchesUsed != 1u ||
        oot_link_get_geometry_dropped_triangles() == 0u ) {
        fprintf( stderr,
                 "capacity-one geometry counts invalid (%u/%u batches=%u dropped=%u)\n",
                 geometry.numTrianglesUsed, geometry.numTrianglesUsed32,
                 geometry.numBatchesUsed,
                 oot_link_get_geometry_dropped_triangles() );
        ok = 0;
    } else if( batches[0].firstTriangle != 0u ||
               batches[0].numTriangles != 1u ||
               batches[0].sourceKind != OOT_GEOMETRY_SOURCE_LINK ) {
        fprintf( stderr, "capacity-one geometry batch metadata invalid\n" );
        ok = 0;
    }
    if( position[9] != 1000001.0f || normal[9] != 1000002.0f ||
        color[9] != 1000003.0f || uv[6] != 1000004.0f ||
        alpha[3] != 1000005.0f || triTexture[1] != 0xA55Au ||
        triFlags[1] != 0xA5u ||
        memcmp( &batches[1], &batchCanary, sizeof( batchCanary )) != 0 ) {
        fprintf( stderr, "capacity-one geometry wrote beyond caller buffers\n" );
        ok = 0;
    }

    memset( &legacy, 0xA5, sizeof( legacy ));
    legacy.geometry.position = sPos;
    legacy.geometry.normal = sNrm;
    legacy.geometry.color = sCol;
    legacy.geometry.uv = sUv;
    legacy.geometry.triTexture = sTriTex;
    legacy.geometry.numTrianglesUsed = 0;
    legacy.geometry.alpha = NULL;
    legacy.geometry.triFlags = NULL;
    oot_link_tick( link, &input, &state,
                   (struct OoTLinkGeometryBuffers *)&legacy.geometry );
    if( legacy.geometry.numTrianglesUsed == 0u ) {
        fprintf( stderr, "legacy geometry prefix emitted no triangles\n" );
        ok = 0;
    }
    for( size_t i = 0; i < sizeof( legacy.canary ); ++i ) {
        if( legacy.canary[i] != 0xA5u ) {
            fprintf( stderr, "legacy geometry call wrote into appended fields\n" );
            ok = 0;
            break;
        }
    }
    return ok;
}

static void legacy_sfx( uint16_t sfxId )
{
    (void)sfxId;
    ++sLegacySfxCount;
}

static void detailed_sfx( const struct OoTSfxEvent *event )
{
    if( event && event->action == OOT_SFX_PLAY ) ++sDetailedSfxCount;
    if( !event || event->action > OOT_SFX_STOP_POSITION ||
        ( event->action == OOT_SFX_PLAY &&
          ( !isfinite( event->freqScale ) || event->freqScale <= 0.0f ||
            !isfinite( event->volume ) || event->volume < 0.0f )) ||
        !isfinite( event->position[0] ) || !isfinite( event->position[1] ) ||
        !isfinite( event->position[2] ))
        sBadSfxEvent = 1;
}

static int tick_and_check( int32_t link, const char *label )
{
    struct OoTLinkInputs input = { .camLookZ = 1.0f };
    struct OoTLinkState state;
    oot_link_tick( link, &input, &state, &sGeo );
    int textureCount = oot_get_texture_count();
    if( sGeo.numTrianglesUsed == 0 || textureCount == 0 ) {
        fprintf( stderr, "%s: no geometry/textures (%u/%d)\n", label,
                 sGeo.numTrianglesUsed, textureCount );
        return 0;
    }
    for( uint32_t tri = 0; tri < sGeo.numTrianglesUsed; ++tri ) {
        uint16_t texture = sTriTex[tri];
        if( texture != UINT16_MAX && texture >= textureCount ) {
            fprintf( stderr, "%s: triangle %u has invalid texture %u/%d\n",
                     label, tri, texture, textureCount );
            return 0;
        }
        if( texture != UINT16_MAX && texture < sizeof( sTextureChecked ) && !sTextureChecked[texture] ) {
            struct OoTTextureInfo info;
            const uint8_t *rgba;
            if( !oot_get_texture( texture, &info, &rgba ) || !rgba || !info.width || !info.height ||
                info.width > 1024 || info.height > 1024 || info.wrapS > 2 || info.wrapT > 2 ||
                info.revision == 0 ) {
                fprintf( stderr, "%s: texture %u has invalid decoded data\n", label, texture );
                return 0;
            }
            sTextureChecked[texture] = 1;
        }
        for( int v = 0; v < 3; ++v ) {
            uint32_t i = tri * 3 + (uint32_t)v;
            for( int axis = 0; axis < 3; ++axis ) {
                if( !isfinite( sPos[i * 3 + axis] ) || !isfinite( sNrm[i * 3 + axis] ) ||
                    !isfinite( sCol[i * 3 + axis] )) {
                    fprintf( stderr, "%s: triangle %u has non-finite vertex data\n", label, tri );
                    return 0;
                }
            }
            if( !isfinite( sUv[i * 2] ) || !isfinite( sUv[i * 2 + 1] )) {
                fprintf( stderr, "%s: triangle %u has non-finite UVs\n", label, tri );
                return 0;
            }
        }
    }
    printf( "%s: triangles=%u textures=%d\n", label, sGeo.numTrianglesUsed, textureCount );
    return 1;
}

int main( int argc, char **argv )
{
    if( argc != 2 ) {
        fprintf( stderr, "usage: %s ROM\n", argv[0] );
        return 2;
    }
    FILE *file = fopen( argv[1], "rb" );
    if( !file ) return 1;
    if( fseek( file, 0, SEEK_END ) != 0 ) { fclose( file ); return 1; }
    long romSize = ftell( file );
    if( romSize <= 0 || (uintmax_t)romSize > SIZE_MAX ||
        fseek( file, 0, SEEK_SET ) != 0 ) { fclose( file ); return 1; }
    uint8_t *rom = malloc((size_t)romSize );
    if( !rom || fread( rom, 1, (size_t)romSize, file ) != (size_t)romSize ) {
        free( rom ); fclose( file ); return 1;
    }
    if( fclose( file ) != 0 ) { free( rom ); return 1; }

    oot_global_init( rom, (size_t)romSize, NULL );
    free( rom );
    struct OoTSurface ground[2] = {
        { 0, { { -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 } } },
        { 0, { { -1000, 0, -1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 } } },
    };
    oot_static_surfaces_load( ground, 2 );
    int32_t link = oot_link_create( 0, 0, 0 );
    if( link < 0 ) { oot_global_terminate(); return 1; }
    oot_set_sfx_callback( legacy_sfx );
    oot_set_sfx_callback_ex( detailed_sfx );

    int ok = geometry_capacity_and_abi_check( link );
    for( uint8_t age = OOT_AGE_ADULT; age <= OOT_AGE_CHILD; ++age ) {
        if( !oot_link_set_age( link, age )) { ok = 0; continue; }
        uint8_t swordMax = age == OOT_AGE_ADULT ? OOT_SWORD_BIGGORON : OOT_SWORD_KOKIRI;
        uint8_t shieldMax = age == OOT_AGE_ADULT ? OOT_SHIELD_MIRROR : OOT_SHIELD_HYLIAN;
        for( uint8_t sword = OOT_SWORD_NONE; sword <= swordMax; ++sword ) {
            for( uint8_t shield = OOT_SHIELD_NONE; shield <= shieldMax; ++shield ) {
                char label[64];
                snprintf( label, sizeof( label ), "age=%u sword=%u shield=%u", age, sword, shield );
                oot_link_set_equipment( link, sword, shield, OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI );
                ok &= tick_and_check( link, label );
            }
        }
    }

    const int16_t *pcm;
    uint32_t count, rate;
    if( oot_get_voice_sample( 0x6840, &pcm, &count, &rate )) {
        fprintf( stderr, "retail-silent Navi id 0x6840 unexpectedly has a sample\n" );
        ok = 0;
    }
    if( !oot_get_voice_sample( 0x6800, &pcm, &count, &rate ) || !pcm || !count || !rate ) {
        fprintf( stderr, "Link voice 0x6800 did not decode\n" );
        ok = 0;
    }
    if( !oot_get_voice_sample( 0x6000, &pcm, &count, &rate ) || !pcm || !count || !rate ) {
        fprintf( stderr, "continuous Link voice alias 0x6000 did not decode\n" );
        ok = 0;
    }
    if( oot_get_voice_sample( 0x6100, &pcm, &count, &rate )) {
        fprintf( stderr, "invalid voice alias 0x6100 unexpectedly decoded\n" );
        ok = 0;
    }
    if( !oot_get_voice_sample( 0x680E, &pcm, &count, &rate ) ||
        count <= (uint64_t)rate * 7u ) {
        fprintf( stderr, "timed adult sneeze sequence is missing its seq_0 delays\n" );
        ok = 0;
    }
    if( !oot_get_voice_sample( 0x6844, &pcm, &count, &rate ) || !pcm || !count || !rate ) {
        fprintf( stderr, "fixed Navi voice 0x6844 did not decode\n" );
        ok = 0;
    }

    /* Walk far enough to produce at least one real Player audio request and
       ensure both callback APIs observe the same events. */
    for( int i = 0; i < 90; ++i ) {
        struct OoTLinkInputs input = { .stickY = 1.0f, .camLookZ = 1.0f };
        struct OoTLinkState state;
        oot_link_tick( link, &input, &state, &sGeo );
    }
    if( sLegacySfxCount == 0 || sLegacySfxCount != sDetailedSfxCount || sBadSfxEvent ) {
        fprintf( stderr, "SFX callbacks invalid (legacy=%d detailed=%d bad=%d)\n",
                 sLegacySfxCount, sDetailedSfxCount, sBadSfxEvent );
        ok = 0;
    }

    oot_link_delete( link );
    oot_global_terminate();
    return ok ? 0 : 1;
}
