/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * libFuzzer target for ROM byte-order normalization, dmadata parsing, and
 * bounded file extraction. It uses synthetic data only.
 */
#include "liboot_engine.h"
#include "rom_util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_INPUT_SIZE (4u * 1024u * 1024u)
#define FUZZ_MAX_EXTRACT_SIZE (64u * 1024u)
#define SYNTHETIC_ROM_SIZE 0x4000u
#define SYNTHETIC_DMA_OFFSET 0x1000u
#define SYNTHETIC_DATA_OFFSET 0x1800u

static uint32_t fuzz_u32(const uint8_t *data, size_t size, size_t offset)
{
    uint32_t value = 0u;
    size_t i;

    for (i = 0u; i < 4u && offset + i < size; ++i) {
        value |= (uint32_t)data[offset + i] << (i * 8u);
    }
    return value;
}

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

static void exercise_dma(uint8_t *rom, size_t romSize,
                         const uint8_t *controls, size_t controlSize)
{
    const uint8_t *dmadata = rom_find_dmadata(rom, romSize);
    LibootDmaEntry entry;
    size_t candidateOffset;
    uint32_t probe;

    candidateOffset = romSize == SIZE_MAX
        ? 0u
        : (size_t)fuzz_u32(controls, controlSize, 0u) % (romSize + 1u);
    for (probe = 0u; probe < 8u; ++probe) {
        uint32_t index = probe < 4u
            ? probe
            : fuzz_u32(controls, controlSize, (size_t)probe * 4u);
        (void)dma_get(rom, romSize, rom + candidateOffset, index, &entry);

        if (dmadata != NULL && dma_get(rom, romSize, dmadata, index, &entry) &&
            entry.vromEnd > entry.vromStart &&
            entry.vromEnd - entry.vromStart <= FUZZ_MAX_EXTRACT_SIZE) {
            size_t extractedSize = 0u;
            uint8_t *extracted = rom_read_file(rom, romSize, index,
                                                &extractedSize);
            free(extracted);
        }
    }
}

/* Build a small valid dmadata envelope around fuzzed Yaz0 or raw file bytes.
 * This reaches extraction paths without requiring a copyrighted seed ROM. */
static void exercise_synthetic_file(const uint8_t *data, size_t size)
{
    uint8_t *rom = (uint8_t *)calloc(SYNTHETIC_ROM_SIZE, 1u);
    uint8_t *table;
    uint8_t *source;
    uint32_t outputSize;
    size_t payloadOffset;
    size_t payloadSize;
    size_t sourceCapacity;
    int compressed;

    if (rom == NULL) {
        return;
    }

    put_be32(rom, 0x80371240u);
    rom[0x3Cu] = 'Z';
    rom[0x3Du] = 'L';
    table = rom + SYNTHETIC_DMA_OFFSET;
    source = rom + SYNTHETIC_DATA_OFFSET;
    outputSize = 1u + fuzz_u32(data, size, 0u) % FUZZ_MAX_EXTRACT_SIZE;
    compressed = (int)(fuzz_u32(data, size, 4u) & 1u);
    payloadOffset = size < 8u ? size : 8u;
    payloadSize = size - payloadOffset;
    sourceCapacity = SYNTHETIC_ROM_SIZE - SYNTHETIC_DATA_OFFSET;

    put_entry(table, 0u, 0u, OOT_ENGINE_MIN_ROM_SIZE, 0u, 0u);
    if (compressed) {
        size_t capacity = sourceCapacity - 16u;
        if (payloadSize > capacity) {
            payloadSize = capacity;
        }
        memcpy(source, "Yaz0", 4u);
        put_be32(source + 4u, outputSize);
        if (payloadSize != 0u) {
            memcpy(source + 16u, data + payloadOffset, payloadSize);
        }
        put_entry(table, 1u, OOT_ENGINE_MIN_ROM_SIZE,
                  OOT_ENGINE_MIN_ROM_SIZE + outputSize,
                  SYNTHETIC_DATA_OFFSET,
                  SYNTHETIC_DATA_OFFSET + 16u + (uint32_t)payloadSize);
    } else {
        size_t copySize = payloadSize;
        if (outputSize > sourceCapacity) {
            outputSize = (uint32_t)sourceCapacity;
        }
        if (copySize > outputSize) {
            copySize = outputSize;
        }
        if (copySize != 0u) {
            memcpy(source, data + payloadOffset, copySize);
        }
        put_entry(table, 1u, OOT_ENGINE_MIN_ROM_SIZE,
                  OOT_ENGINE_MIN_ROM_SIZE + outputSize,
                  SYNTHETIC_DATA_OFFSET, 0u);
    }
    put_entry(table, 2u, 0u, 0u, 0u, 0u);

    if (rom_normalize(rom, SYNTHETIC_ROM_SIZE)) {
        exercise_dma(rom, SYNTHETIC_ROM_SIZE, data, size);
    }
    free(rom);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t allocationSize;
    uint8_t *rom;

    if (size > FUZZ_MAX_INPUT_SIZE || (data == NULL && size != 0u)) {
        return 0;
    }

    allocationSize = size < OOT_ENGINE_MIN_ROM_SIZE
        ? OOT_ENGINE_MIN_ROM_SIZE
        : size;
    rom = (uint8_t *)calloc(allocationSize, 1u);
    if (rom == NULL) {
        return 0;
    }
    if (size != 0u) {
        memcpy(rom, data, size);
    }

    /* The exact-size call covers short-input rejection. Padding a second call
     * to the public minimum lets small corpus entries exercise byte swapping. */
    exercise_dma(rom, size, data, size);
    (void)rom_normalize(rom, size);
    exercise_dma(rom, size, data, size);
    if (size < OOT_ENGINE_MIN_ROM_SIZE) {
        (void)rom_normalize(rom, allocationSize);
        exercise_dma(rom, allocationSize, data, size);
    }

    free(rom);
    exercise_synthetic_file(data, size);
    return 0;
}
