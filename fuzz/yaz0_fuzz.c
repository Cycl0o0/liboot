/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * libFuzzer target for the bounded Yaz0 decoder in src/rom_util.c.
 */
#include "rom_util.h"

#include <stdint.h>
#include <stdlib.h>

#define FUZZ_MAX_INPUT_SIZE (4u * 1024u * 1024u)
#define FUZZ_MAX_OUTPUT_SIZE (64u * 1024u)

static uint32_t fuzz_u32(const uint8_t *data, size_t size, size_t offset)
{
    uint32_t value = 0u;
    size_t i;

    for (i = 0u; i < 4u && offset + i < size; ++i) {
        value = (value << 8) | data[offset + i];
    }
    return value;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint32_t outputSize;
    uint8_t *output;

    if (size > FUZZ_MAX_INPUT_SIZE || (data == NULL && size != 0u)) {
        return 0;
    }

    /* Callers supply the decompressed size after reading the Yaz0 header.
     * Bound it here so a four-byte fuzz field cannot exhaust the runner. */
    outputSize = 1u + fuzz_u32(data, size, 4u) % FUZZ_MAX_OUTPUT_SIZE;
    output = (uint8_t *)malloc(outputSize);
    if (output == NULL) {
        return 0;
    }
    (void)yaz0_decode(data, size, output, outputSize);
    free(output);

    /* Keep the zero-length output contract under coverage as well. */
    (void)yaz0_decode(data, size, NULL, 0u);
    return 0;
}
