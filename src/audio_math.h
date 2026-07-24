#ifndef LIBOOT_AUDIO_MATH_H
#define LIBOOT_AUDIO_MATH_H

#include <stdint.h>

/* VADPCM prediction and residual values are individually signed 32-bit, but
 * their sum can exceed that range for malformed or extreme predictor books.
 * Add in a wide type and saturate before storing the decoder state. */
static inline int32_t liboot_audio_add_saturate_i32( int32_t prediction, int32_t residual )
{
    int64_t sum = (int64_t)prediction + (int64_t)residual;
    if( sum < INT32_MIN ) return INT32_MIN;
    if( sum > INT32_MAX ) return INT32_MAX;
    return (int32_t)sum;
}

#endif
