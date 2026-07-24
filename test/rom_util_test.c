/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * Synthetic malformed-ROM regression tests. No game data is embedded.
 */
#include "rom_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_SIZE 0x2000u
#define DMA_OFFSET 0x1000u
#define DATA_OFFSET 0x1800u

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "rom_util_test:%d: %s\n", __LINE__, #condition); \
            return 0; \
        } \
    } while (0)

static void put_be32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static void put_entry(uint8_t *table, uint32_t index, uint32_t vromStart,
                      uint32_t vromEnd, uint32_t romStart, uint32_t romEnd)
{
    uint8_t *entry = table + (size_t)index * 16u;
    put_be32(entry, vromStart);
    put_be32(entry + 4u, vromEnd);
    put_be32(entry + 8u, romStart);
    put_be32(entry + 12u, romEnd);
}

static uint8_t *make_rom(void)
{
    uint8_t *rom = (uint8_t *)calloc(ROM_SIZE, 1u);
    uint8_t *table;
    if (rom == NULL) {
        return NULL;
    }
    put_be32(rom, 0x80371240u);
    rom[0x3Cu] = 'Z';
    rom[0x3Du] = 'L';
    table = rom + DMA_OFFSET;
    put_entry(table, 0u, 0u, 0x1060u, 0u, 0u);
    put_entry(table, 1u, 0x1060u, 0x1063u, DATA_OFFSET, 0u);
    memcpy(rom + DATA_OFFSET, "abc", 3u);
    return rom;
}

static int test_dma_bounds(void)
{
    uint8_t *rom = make_rom();
    const uint8_t *table;
    LibootDmaEntry entry;
    uint8_t *file;
    size_t size = 99u;

    CHECK(rom != NULL);
    CHECK(rom_normalize(rom, ROM_SIZE));
    table = rom_find_dmadata(rom, ROM_SIZE);
    CHECK(table == rom + DMA_OFFSET);
    CHECK(dma_get(rom, ROM_SIZE, table, 0u, &entry));
    CHECK(dma_get(rom, ROM_SIZE, table, 1u, &entry));
    CHECK(!dma_get(rom, ROM_SIZE, table, 2u, &entry));
    CHECK(!dma_get(rom, ROM_SIZE, table, UINT32_MAX, &entry));
    CHECK(!dma_get(rom, ROM_SIZE, NULL, 0u, &entry));
    CHECK(!dma_get(rom, ROM_SIZE,
                   (const uint8_t *)((uintptr_t)rom + ROM_SIZE + 1u),
                   0u, &entry));

    file = rom_read_file(rom, ROM_SIZE, 1u, &size);
    CHECK(file != NULL && size == 3u && memcmp(file, "abc", 3u) == 0);
    free(file);
    CHECK(rom_read_file(rom, ROM_SIZE, 1u, NULL) == NULL);

    /* Reversed VROM ranges and physical spans beyond the ROM are rejected
       before subtraction, allocation, header reads, or decompression. */
    put_entry(rom + DMA_OFFSET, 1u, 0x2000u, 0x1000u, DATA_OFFSET, 0u);
    CHECK(rom_read_file(rom, ROM_SIZE, 1u, &size) == NULL && size == 0u);
    put_entry(rom + DMA_OFFSET, 1u, 0x1060u, 0x1063u,
              ROM_SIZE - 1u, ROM_SIZE);
    CHECK(rom_read_file(rom, ROM_SIZE, 1u, &size) == NULL && size == 0u);
    put_entry(rom + DMA_OFFSET, 1u, 0x1060u, 0x1063u,
              DATA_OFFSET, ROM_SIZE + 1u);
    CHECK(rom_read_file(rom, ROM_SIZE, 1u, &size) == NULL && size == 0u);

    free(rom);
    return 1;
}

static int test_yaz0(void)
{
    uint8_t *rom = make_rom();
    uint8_t *source;
    uint8_t *file;
    uint8_t output[3] = { 0u, 0u, 0u };
    uint8_t badBackref[19] = { 'Y', 'a', 'z', '0' };
    size_t size = 0u;

    CHECK(rom != NULL);
    source = rom + DATA_OFFSET;
    memset(source, 0, 20u);
    memcpy(source, "Yaz0", 4u);
    put_be32(source + 4u, 3u);
    source[16] = 0xE0u;
    memcpy(source + 17u, "xyz", 3u);
    put_entry(rom + DMA_OFFSET, 1u, 0x1060u, 0x1063u,
              DATA_OFFSET, DATA_OFFSET + 20u);
    file = rom_read_file(rom, ROM_SIZE, 1u, &size);
    CHECK(file != NULL && size == 3u && memcmp(file, "xyz", 3u) == 0);
    free(file);

    put_be32(source + 4u, 4u);
    CHECK(rom_read_file(rom, ROM_SIZE, 1u, &size) == NULL && size == 0u);
    CHECK(!yaz0_decode(NULL, 0u, output, sizeof(output)));
    CHECK(!yaz0_decode(badBackref, sizeof(badBackref), output, sizeof(output)));

    free(rom);
    return 1;
}

int main(void)
{
    uint8_t shortRom[32] = { 0u };
    if (!test_dma_bounds() || !test_yaz0()) {
        return 1;
    }
    if (rom_normalize(NULL, 0u) || rom_find_dmadata(NULL, 0u) != NULL ||
        rom_find_dmadata(shortRom, sizeof(shortRom)) != NULL) {
        fprintf(stderr, "rom_util_test: null/short input validation failed\n");
        return 1;
    }
    puts("rom util: PASS");
    return 0;
}
