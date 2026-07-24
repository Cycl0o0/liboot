#ifndef SEGMENTED_ADDRESS_H
#define SEGMENTED_ADDRESS_H

#include "ultra64.h"
#include "stdint.h"

extern uintptr_t gSegments[NUM_SEGMENTS];

/* liboot: the decomp code passes BOTH 32-bit segment tokens (0x0Sxxxxxx) and
   already-native pointers through this macro. On a 64-bit host we resolve
   only values that look like a token with a registered segment and pass
   everything else through untouched. gSegments[] holds (hostBase - K0BASE)
   so the original arithmetic still lands on the host allocation. */
static inline void* liboot_segmented_to_virtual(uintptr_t addr)
{
    uintptr_t seg = addr >> 24;
    if (addr != 0 && addr <= 0xFFFFFFFFu && seg >= 1 && seg < NUM_SEGMENTS && gSegments[seg] != 0) {
        return (void*)(gSegments[seg] + (addr & 0x00FFFFFF) + K0BASE);
    }
    return (void*)addr;
}

#define SEGMENTED_TO_VIRTUAL(addr) liboot_segmented_to_virtual((uintptr_t)(addr))

#endif
