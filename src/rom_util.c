#include "rom_util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define ROM_MAX_FILE_SIZE (256u * 1024u * 1024u)

static uint32_t be32( const uint8_t *p )
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool rom_normalize( uint8_t *rom, size_t romSize )
{
    if( rom == NULL || romSize < 0x1060 ) return false;
    uint32_t magic = be32( rom );
    if( magic == 0x37804012 ) { /* .v64: swap every byte pair */
        for( size_t i = 0; i + 1 < romSize; i += 2 ) {
            uint8_t t = rom[i]; rom[i] = rom[i+1]; rom[i+1] = t;
        }
    } else if( magic == 0x40123780 ) { /* .n64: swap 32-bit words */
        for( size_t i = 0; i + 3 < romSize; i += 4 ) {
            uint8_t t0 = rom[i], t1 = rom[i+1];
            rom[i] = rom[i+3]; rom[i+1] = rom[i+2]; rom[i+2] = t1; rom[i+3] = t0;
        }
    }
    if( be32( rom ) != 0x80371240 ) return false;
    /* game code "?ZL?" at 0x3B..0x3E: NZL (jp/us) or NZP (pal) etc. */
    return rom[0x3C] == 'Z' && ( rom[0x3D] == 'L' || rom[0x3D] == 'P' );
}

const uint8_t *rom_find_dmadata( const uint8_t *rom, size_t romSize )
{
    /* First entry always describes makerom: vrom 0..0x1060, rom 0, romEnd 0,
       and the second entry starts at vrom 0x1060. */
    static const uint8_t sig[20] = {
        0,0,0,0, 0,0,0x10,0x60, 0,0,0,0, 0,0,0,0, 0,0,0x10,0x60
    };
    if( rom == NULL || romSize < sizeof( sig )) return NULL;
    for( size_t i = 0; i <= romSize - sizeof( sig ); i += 16 ) {
        if( !memcmp( rom + i, sig, sizeof( sig )))
            return rom + i;
    }
    return NULL;
}

bool dma_get( const uint8_t *rom, size_t romSize, const uint8_t *dmadata,
              uint32_t index, LibootDmaEntry *out )
{
    uintptr_t romAddress;
    uintptr_t tableAddress;
    size_t tableOffset;
    size_t available;
    const uint8_t *p;

    if( rom == NULL || dmadata == NULL || out == NULL ) return false;
    romAddress = (uintptr_t)rom;
    tableAddress = (uintptr_t)dmadata;
    if( tableAddress < romAddress ) return false;
    if( tableAddress - romAddress > romSize ) return false;
    tableOffset = (size_t)( tableAddress - romAddress );
    available = romSize - tableOffset;
    if((uint64_t)index >= (uint64_t)( available / 16u )) return false;

    p = dmadata + (size_t)index * 16u;
    out->vromStart = be32( p );
    out->vromEnd   = be32( p + 4 );
    out->romStart  = be32( p + 8 );
    out->romEnd    = be32( p + 12 );
    return !( out->vromStart == 0 && out->vromEnd == 0 );
}

bool yaz0_decode( const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t dstSize )
{
    /* src points at the "Yaz0" header */
    if( src == NULL || ( dst == NULL && dstSize != 0u ) || srcSize < 16u ) return false;
    const uint8_t *in = src + 16;
    const uint8_t *inEnd = src + srcSize;
    uint32_t outPos = 0;
    uint8_t codeByte = 0;
    int codeBitsLeft = 0;

    while( outPos < dstSize ) {
        if( codeBitsLeft == 0 ) {
            if( in >= inEnd ) return false;
            codeByte = *in++;
            codeBitsLeft = 8;
        }
        if( codeByte & 0x80 ) {
            if( in >= inEnd ) return false;
            dst[outPos++] = *in++;
        } else {
            if((size_t)( inEnd - in ) < 2u ) return false;
            uint8_t b1 = *in++, b2 = *in++;
            uint32_t dist = (((uint32_t)b1 & 0xF) << 8 | b2) + 1;
            uint32_t len = (uint32_t)b1 >> 4;
            if( len == 0 ) {
                if( in >= inEnd ) return false;
                len = (uint32_t)*in++ + 0x12;
            } else {
                len += 2;
            }
            if( dist > outPos ) return false;
            while( len-- && outPos < dstSize ) {
                dst[outPos] = dst[outPos - dist];
                outPos++;
            }
        }
        codeByte <<= 1;
        codeBitsLeft--;
    }
    return true;
}

uint8_t *rom_read_file( const uint8_t *rom, size_t romSize, uint32_t index, size_t *outSize )
{
    const uint8_t *dmadata;
    LibootDmaEntry e;
    size_t size;
    uint8_t *buf;

    if( outSize == NULL ) return NULL;
    *outSize = 0u;
    if( rom == NULL ) return NULL;
    dmadata = rom_find_dmadata( rom, romSize );
    if( !dmadata || !dma_get( rom, romSize, dmadata, index, &e )) return NULL;
    if( e.romStart == 0xFFFFFFFF ) return NULL; /* file absent in this version */
    if( e.vromEnd <= e.vromStart ) return NULL;

    size = (size_t)e.vromEnd - e.vromStart;
    if( size == 0u || size > ROM_MAX_FILE_SIZE || size > UINT32_MAX ) return NULL;

    if( e.romEnd != 0 ) { /* compressed */
        size_t start = e.romStart;
        size_t end = e.romEnd;
        if( start > romSize || end > romSize || end < start || end - start < 16u ||
            memcmp( rom + start, "Yaz0", 4 ) != 0 ||
            be32( rom + start + 4u ) != (uint32_t)size ) return NULL;
    } else {
        size_t start = e.romStart;
        if( start > romSize || size > romSize - start ) return NULL;
    }

    buf = malloc( size );
    if( !buf ) return NULL;
    if( e.romEnd != 0 ) {
        size_t start = e.romStart;
        size_t compressedSize = (size_t)e.romEnd - start;
        if( !yaz0_decode( rom + start, compressedSize, buf, (uint32_t)size )) {
            free( buf );
            return NULL;
        }
    } else {
        memcpy( buf, rom + (size_t)e.romStart, size );
    }
    *outSize = size;
    return buf;
}
