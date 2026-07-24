#ifndef LIBOOT_ASSETS_H
#define LIBOOT_ASSETS_H

#include "ultra64.h"

/* Runtime binding of a generated placeholder array to real data inside an
   extracted ROM segment. RAW = plain copy; S16 = big-endian 16-bit decode. */
typedef enum { LIBOOT_BIND_RAW, LIBOOT_BIND_S16 } LibootBindMode;

typedef struct {
    const char *name;
    void *dst;
    u32 segment;
    u32 offset;
    u32 size;
    LibootBindMode mode;
} LibootAssetBind;

extern const LibootAssetBind gLibootAssetBinds[];
extern const u32 gLibootAssetBindCount;

/* shim-provided segment span lookup (fake_play.c) */
const u8 *liboot_segment_base( int seg, size_t *outSize );

#endif
