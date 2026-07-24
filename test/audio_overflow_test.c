#include "../src/audio_math.h"

#include <stdint.h>
#include <stdio.h>

typedef struct {
    int32_t prediction;
    int32_t residual;
    int32_t expected;
} AccumulateCase;

int main( void )
{
    static const AccumulateCase cases[] = {
        { 0, 0, 0 },
        { INT32_MAX, 0, INT32_MAX },
        { INT32_MIN, 0, INT32_MIN },
        { INT32_MAX, 1, INT32_MAX },
        { INT32_MIN, -1, INT32_MIN },
        { INT32_MAX, 262144, INT32_MAX },
        { INT32_MIN, -262144, INT32_MIN },
        { INT32_MAX, -262144, INT32_MAX - 262144 },
        { INT32_MIN, 262144, INT32_MIN + 262144 },
        { 123456789, -98765432, 24691357 },
    };

    for( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); ++i ) {
        int32_t actual = liboot_audio_add_saturate_i32( cases[i].prediction, cases[i].residual );
        if( actual != cases[i].expected ) {
            fprintf( stderr, "case %zu: %d + %d yielded %d, expected %d\n", i,
                     cases[i].prediction, cases[i].residual, actual, cases[i].expected );
            return 1;
        }
    }
    return 0;
}
