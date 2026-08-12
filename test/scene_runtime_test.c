/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include "liboot.h"

#include <stdio.h>
#include <stdlib.h>

static int expect( const char *label, int condition )
{
    if( condition ) return 1;
    fprintf( stderr, "FAIL: %s\n", label );
    return 0;
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
    data = malloc( (size_t)length );
    if( data == NULL || fread( data, 1u, (size_t)length, file ) != (size_t)length ) {
        free( data );
        fclose( file );
        return NULL;
    }
    fclose( file );
    *outSize = (size_t)length;
    return data;
}

int main( int argc, char **argv )
{
    unsigned char *rom;
    size_t romSize;
    int ok = 1;
    if( argc != 2 ) {
        fprintf( stderr, "usage: %s path/to/pal-1.1-rom\n", argv[0] );
        return 2;
    }
    rom = read_file( argv[1], &romSize );
    if( rom == NULL ) {
        fprintf( stderr, "unable to read ROM\n" );
        return 2;
    }
    oot_global_init( rom, romSize, NULL );

    struct OoTWorldEvent event = { 0 };
    event.structSize = sizeof( event );
    event.version = OOT_WORLD_EVENT_VERSION;
    ok &= expect( "empty event queue", !oot_world_event_poll( &event ) );
    ok &= expect( "invalid layer rejected transactionally",
                  oot_scene_load_ex( &(struct OoTSceneLoadOptions){
                      sizeof(struct OoTSceneLoadOptions), OOT_SCENE_HYRULE_FIELD,
                      0, 4u, { 0u, 0u, 0u }
                  } ) == -10 );
    ok &= expect( "short options rejected",
                  oot_scene_load_ex( &(struct OoTSceneLoadOptions){
                      sizeof(uint32_t), OOT_SCENE_HYRULE_FIELD, 0,
                      OOT_SCENE_LAYER_CHILD_DAY, { 0u, 0u, 0u }
                  } ) == -10 );

    const struct OoTSceneLoadOptions layers[] = {
        OOT_SCENE_LOAD_OPTIONS_INIT( OOT_SCENE_HYRULE_FIELD, 0,
                                     OOT_SCENE_LAYER_CHILD_DAY ),
        OOT_SCENE_LOAD_OPTIONS_INIT( OOT_SCENE_HYRULE_FIELD, 0,
                                     OOT_SCENE_LAYER_CHILD_NIGHT ),
        OOT_SCENE_LOAD_OPTIONS_INIT( OOT_SCENE_HYRULE_FIELD, 0,
                                     OOT_SCENE_LAYER_ADULT_DAY ),
        OOT_SCENE_LOAD_OPTIONS_INIT( OOT_SCENE_HYRULE_FIELD, 0,
                                     OOT_SCENE_LAYER_ADULT_NIGHT )
    };
    for( unsigned i = 0; i < sizeof( layers ) / sizeof( layers[0] ); ++i ) {
        uint8_t active = 0xFFu;
        bool fallback = true;
        int result = oot_scene_load_ex( &layers[i] );
        if( result != 0 ) {
            fprintf( stderr, "FAIL: Hyrule Field layer %u returned %d\n", i, result );
            ok = 0;
            continue;
        }
        ok &= expect( "active layer getter",
                      oot_scene_get_active_layer( &active, &fallback ) &&
                      active == i );
        ok &= expect( "exit list discovered", oot_scene_get_exit_count() > 0 &&
                      oot_scene_get_exit_count() <= 31 );
        for( int exit = 0; exit < oot_scene_get_exit_count(); ++exit ) {
            int16_t entrance = -1;
            ok &= expect( "exit entry readable", oot_scene_get_exit( exit, &entrance ) );
        }
        int16_t invalid = 0;
        ok &= expect( "exit bounds", !oot_scene_get_exit( -1, &invalid ) && invalid == -1 );
    }

    ok &= expect( "legacy load succeeds",
                  oot_scene_load( OOT_SCENE_HYRULE_FIELD, 0 ) == 0 );
    {
        uint8_t active = 0xFFu;
        bool fallback = true;
        ok &= expect( "legacy load is child-day",
                      oot_scene_get_active_layer( &active, &fallback ) &&
                      active == OOT_SCENE_LAYER_CHILD_DAY && !fallback );
    }

    /* The hard void plane must be converted to an event without entering the
       retail transition state machine. Repositioning clears contact and allows
       a later crossing to emit a new sequence number. */
    ok &= expect( "Link create for void test", oot_link_create( 0.0f, -4100.0f, 0.0f ) == 0 );
    struct OoTLinkInputs input = { 0 };
    struct OoTLinkState state;
    oot_link_tick( 0, &input, &state, NULL );
    event.structSize = sizeof(uint32_t);
    event.version = OOT_WORLD_EVENT_VERSION;
    ok &= expect( "short event output preserves queue",
                  !oot_world_event_poll( &event ) );
    event.structSize = sizeof( event );
    event.version = OOT_WORLD_EVENT_VERSION + 1u;
    ok &= expect( "wrong event version preserves queue",
                  !oot_world_event_poll( &event ) );
    event.version = OOT_WORLD_EVENT_VERSION;
    ok &= expect( "hard void event", oot_world_event_poll( &event ) &&
                  event.version == OOT_WORLD_EVENT_VERSION &&
                  event.kind == OOT_WORLD_EVENT_VOID_Y &&
                  event.sceneIndex == OOT_SCENE_HYRULE_FIELD &&
                  event.roomIndex == 0 &&
                  event.layer == OOT_SCENE_LAYER_CHILD_DAY &&
                  event.entranceIndex == -1 && event.position[1] < -4000.0f );
    uint64_t firstSequence = event.sequence;
    oot_link_tick( 0, &input, &state, NULL );
    event.structSize = sizeof( event );
    ok &= expect( "continuous hard void deduplicated", !oot_world_event_poll( &event ) );
    ok &= expect( "Link still repositionable", oot_link_set_pose( 0, 0.0f, 100.0f, 0.0f, 0 ) );
    oot_link_tick( 0, &input, &state, NULL );
    oot_link_tick( 0, &input, &state, NULL );
    event.structSize = sizeof( event );
    while( oot_world_event_poll( &event ))
        event.structSize = sizeof( event );
    ok &= expect( "Link can move after event", oot_link_set_pose( 0, 0.0f, -4100.0f, 0.0f, 0 ) );
    oot_link_tick( 0, &input, &state, NULL );
    oot_link_tick( 0, &input, &state, NULL );
    event.structSize = sizeof( event );
    ok &= expect( "hard void retriggers after leaving", oot_world_event_poll( &event ) &&
                  event.kind == OOT_WORLD_EVENT_VOID_Y &&
                  event.sequence > firstSequence );

    oot_link_delete( 0 );

    /* The collapsing-tower exterior has a retail recovery threshold at a
       320-unit fall, well above the global -4000 plane. Keep this path covered
       so the host receives the event before the original transition code can
       latch Player into a transition state. */
    ok &= expect( "collapse exterior load",
                  oot_scene_load( OOT_SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR, 0 ) == 0 );
    ok &= expect( "Link create for tower fall",
                  oot_link_create( 10000.0f, 1000.0f, 10000.0f ) == 0 );
    int towerEvent = 0;
    for( int tick = 0; tick < 300 && !towerEvent; ++tick ) {
        oot_link_tick( 0, &input, &state, NULL );
        event.structSize = sizeof( event );
        event.version = OOT_WORLD_EVENT_VERSION;
        if( oot_world_event_poll( &event )) {
            towerEvent = event.kind == OOT_WORLD_EVENT_VOID_Y &&
                         event.sceneIndex == OOT_SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR &&
                         event.position[1] > -4000.0f;
        }
    }
    ok &= expect( "collapse exterior fall event", towerEvent );

    oot_link_delete( 0 );
    oot_global_terminate();
    free( rom );
    if( !ok ) return 1;
    puts( "scene runtime tests: ok" );
    return 0;
}
