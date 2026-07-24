#ifndef ROM_UTIL_H
#define ROM_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t vromStart, vromEnd; /* uncompressed address space */
    uint32_t romStart,  romEnd;  /* physical rom; romEnd != 0 -> yaz0 file */
} LibootDmaEntry;

/* Byteswap .v64/.n64 images to .z64 in place; returns false if not an OoT rom. */
bool rom_normalize( uint8_t *rom, size_t romSize );

/* Locate the dmadata table (scan for the makerom first-entry signature). */
const uint8_t *rom_find_dmadata( const uint8_t *rom, size_t romSize );

/* Read entry i of the table (big-endian). The table pointer must point inside
   rom; returns false at the terminator or before any out-of-range read. */
bool dma_get( const uint8_t *rom, size_t romSize, const uint8_t *dmadata,
              uint32_t index, LibootDmaEntry *out );

/* Extract file with dma index `index` into a malloc'd buffer (decompressing
   yaz0 as needed). Returns NULL on failure; *outSize = decompressed size. */
uint8_t *rom_read_file( const uint8_t *rom, size_t romSize, uint32_t index, size_t *outSize );

/* returns false if src is exhausted/corrupt before dstSize bytes are produced */
bool yaz0_decode( const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t dstSize );

#endif
