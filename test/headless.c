/* Headless smoke test: load ROM, flat ground, spawn Link, hold the stick
   forward for 100 ticks and print his position each frame. */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include "../src/liboot.h"

static void crash_handler( int sig )
{
    void *frames[32];
    int n = backtrace( frames, 32 );
    fprintf( stderr, "\n*** signal %d ***\n", sig );
    backtrace_symbols_fd( frames, n, STDERR_FILENO );
    _exit( 139 );
}

static void debug_print( const char *message )
{
    if( message ) puts( message );
}

static int read_file( const char *path, uint8_t **outData, size_t *outSize )
{
    FILE *file = fopen( path, "rb" );
    long length;
    uint8_t *data;

    *outData = NULL;
    *outSize = 0;
    if( !file ) { perror( "rom" ); return 0; }
    if( fseek( file, 0, SEEK_END ) != 0 || ( length = ftell( file )) <= 0 ||
        (uintmax_t)length > SIZE_MAX || fseek( file, 0, SEEK_SET ) != 0 ) {
        fclose( file );
        return 0;
    }
    data = malloc((size_t)length );
    if( !data || fread( data, 1, (size_t)length, file ) != (size_t)length ) {
        free( data );
        fclose( file );
        return 0;
    }
    if( fclose( file ) != 0 ) { free( data ); return 0; }
    *outData = data;
    *outSize = (size_t)length;
    return 1;
}

int main( int argc, char **argv )
{
    signal( SIGSEGV, crash_handler );
    if( argc < 2 ) {
        fprintf( stderr, "usage: %s <oot-rom.z64>\n", argv[0] );
        return 1;
    }

    uint8_t *rom = NULL;
    size_t romSize = 0;
    if( !read_file( argv[1], &rom, &romSize )) return 1;

    oot_set_debug_print_function( debug_print );
    fprintf( stderr, "[T] global_init...\n" ); oot_global_init( rom, romSize, NULL ); fprintf( stderr, "[T] global_init done\n" );
    free( rom );

    /* 2000x2000 ground plane at y=0: two triangles */
    struct OoTSurface ground[2] = {
        { 0, {{ -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 }} },
        { 0, {{ -1000, 0, -1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 }} },
    };
    fprintf( stderr, "[T] surfaces...\n" ); oot_static_surfaces_load( ground, 2 ); fprintf( stderr, "[T] surfaces done\n" );

    fprintf( stderr, "[T] link_create...\n" ); int32_t link = oot_link_create( 0, 0, 0 ); fprintf( stderr, "[T] link_create done -> %d\n", link );
    if( link < 0 ) {
        fprintf( stderr, "link_create failed\n" );
        oot_global_terminate();
        return 1;
    }

    struct OoTLinkInputs inputs = { 0 };
    struct OoTLinkState state = { 0 };
    inputs.camLookZ = 1.0f;

    for( int i = 0; i < 100; ++i ) {
        inputs.stickY = ( i >= 10 && i < 60 ) ? 1.0f : 0.0f; /* run frames 10-59, stop, roll at 70 */
        inputs.buttonA = ( i == 70 );
        oot_link_tick( link, &inputs, &state, NULL ); fflush( stdout );
        printf( "%3d pos=(%8.2f %8.2f %8.2f) speed=%6.2f f1=%08x f2=%08x anim=%.1f\n",
                i, state.position[0], state.position[1], state.position[2],
                state.linearVelocity, state.stateFlags1, state.stateFlags2, state.animFrame );
    }

    puts( "-- switching to child --" );
    if( !oot_link_set_age( link, OOT_AGE_CHILD ))
        puts( "child switch unavailable" );
    for( int i = 100; i < 130; ++i ) {
        inputs.stickY = ( i >= 110 ) ? 1.0f : 0.0f;
        inputs.buttonA = 0;
        oot_link_tick( link, &inputs, &state, NULL );
        if( i % 5 == 0 )
            printf( "%3d age=%d pos=(%7.2f %7.2f %7.2f) speed=%5.2f hp=%d/%d\n",
                    i, state.age, state.position[0], state.position[1], state.position[2],
                    state.linearVelocity, state.health, state.healthCapacity );
    }

    puts( "-- damage test (adult again) --" );
    oot_link_set_age( link, OOT_AGE_ADULT );
    oot_link_set_health( link, 0x30, 0x60 );
    oot_link_damage( link, 8 );
    for( int i = 130; i < 150; ++i ) {
        inputs.stickY = 0;
        oot_link_tick( link, &inputs, &state, NULL );
        if( i % 4 == 0 )
            printf( "%3d hp=%d/%d dead=%d f1=%08x speed=%5.2f\n",
                    i, state.health, state.healthCapacity, state.isDead, state.stateFlags1, state.linearVelocity );
    }

    oot_link_delete( link );
    oot_global_terminate();
    puts( "done" );
    return 0;
}
