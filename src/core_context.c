/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#if defined(LIBOOT_MULTI_INSTANCE) && defined(__ELF__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "core_context.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(LIBOOT_MULTI_INSTANCE) && defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/loader.h>
#elif defined(LIBOOT_MULTI_INSTANCE) && defined(__ELF__)
#include <link.h>
#endif

#define LIBOOT_CONTEXT_MAX_REGIONS 16u
#define LIBOOT_CONTEXT_MAX_BYTES (128u * 1024u * 1024u)

typedef struct LibootContextRegion
{
    unsigned char *address;
    size_t size;
    size_t imageOffset;
} LibootContextRegion;

struct LibootCoreContext
{
    unsigned char *image;
    const void *owner;
    struct LibootCoreContext *next;
};

typedef struct LibootContextManager
{
    atomic_flag guard;
    bool prepared;
    bool available;
    uint32_t regionCount;
    size_t imageSize;
    LibootContextRegion regions[LIBOOT_CONTEXT_MAX_REGIONS];
    unsigned char *baseline;
    LibootCoreContext *active;
    LibootCoreContext *contexts;
    uint32_t nextHandleNamespace;
    atomic_uint_least32_t nextTextureRevision;
#if defined(_WIN32)
    DWORD threadOwnerIndex;
#endif
} LibootContextManager;

#if !defined(_WIN32)
static _Thread_local const void *sThreadOwner;
#endif

/* Only this pointer lives inside the writable image. Every baseline and engine
 * image captures the same value; the lock and active-image bookkeeping it
 * points to are heap-resident and therefore never context-switched. */
static _Atomic(LibootContextManager *) sContextManager;

static LibootContextManager *context_manager( void )
{
    LibootContextManager *manager = atomic_load_explicit(
        &sContextManager, memory_order_acquire );
    if( manager != NULL ) return manager;

    LibootContextManager *fresh = calloc( 1u, sizeof( *fresh ));
    if( fresh == NULL ) return NULL;
    atomic_flag_clear_explicit( &fresh->guard, memory_order_relaxed );
    atomic_init( &fresh->nextTextureRevision, 0u );
#if defined(_WIN32)
    fresh->threadOwnerIndex = TlsAlloc();
    if( fresh->threadOwnerIndex == TLS_OUT_OF_INDEXES ) {
        free( fresh );
        return NULL;
    }
#endif
    LibootContextManager *expected = NULL;
    if( !atomic_compare_exchange_strong_explicit(
            &sContextManager, &expected, fresh,
            memory_order_release, memory_order_acquire )) {
#if defined(_WIN32)
        (void)TlsFree( fresh->threadOwnerIndex );
#endif
        free( fresh );
        manager = expected;
    } else {
        manager = fresh;
    }
    return manager;
}

static bool context_add_region_raw( LibootContextManager *manager,
                                    void *address, size_t size )
{
    if( address == NULL || size == 0u ) return true;
    if( manager->regionCount >= LIBOOT_CONTEXT_MAX_REGIONS ||
        size > LIBOOT_CONTEXT_MAX_BYTES - manager->imageSize )
        return false;
    LibootContextRegion *region = &manager->regions[manager->regionCount++];
    region->address = address;
    region->size = size;
    region->imageOffset = manager->imageSize;
    manager->imageSize += size;
    return true;
}

/* The manager is deliberately outside every saved image. In particular, do
 * not rely on its pointer having the same bytes in the baseline: a limits
 * query can prepare the baseline before the engine wrapper allocates any of
 * its own process-wide bookkeeping. Splitting the containing section also
 * avoids memcpy writing through a live C11 atomic object. */
static bool context_add_region( LibootContextManager *manager,
                                void *address, size_t size )
{
    uintptr_t first = (uintptr_t)address;
    uintptr_t last;
    uintptr_t excludedFirst = (uintptr_t)&sContextManager;
    uintptr_t excludedLast = excludedFirst + sizeof( sContextManager );

    if( address == NULL || size == 0u ) return true;
    if( size > UINTPTR_MAX - first ) return false;
    last = first + size;
    if( last <= excludedFirst || first >= excludedLast )
        return context_add_region_raw( manager, address, size );
    if( first < excludedFirst &&
        !context_add_region_raw( manager, (void *)first,
                                 (size_t)( excludedFirst - first )))
        return false;
    if( last > excludedLast &&
        !context_add_region_raw( manager, (void *)excludedLast,
                                 (size_t)( last - excludedLast )))
        return false;
    return true;
}

#if defined(LIBOOT_MULTI_INSTANCE) && defined(__APPLE__)
static bool context_discover_regions( LibootContextManager *manager )
{
    Dl_info info;
    const struct mach_header_64 *header;
    const struct load_command *command;
    uintptr_t slide = 0u;
    bool haveText = false;

    if( dladdr((const void *)&liboot_core_context_available, &info ) == 0 ||
        info.dli_fbase == NULL )
        return false;
    header = (const struct mach_header_64 *)info.dli_fbase;
    if( header->magic != MH_MAGIC_64 ) return false;

    command = (const struct load_command *)( header + 1 );
    for( uint32_t i = 0u; i < header->ncmds; ++i ) {
        if( command->cmdsize < sizeof( *command )) return false;
        if( command->cmd == LC_SEGMENT_64 ) {
            const struct segment_command_64 *segment =
                (const struct segment_command_64 *)command;
            if( strncmp( segment->segname, "__TEXT", 16u ) == 0 ) {
                slide = (uintptr_t)header - (uintptr_t)segment->vmaddr;
                haveText = true;
                break;
            }
        }
        command = (const struct load_command *)((const unsigned char *)command +
                                                 command->cmdsize );
    }
    if( !haveText ) return false;

    command = (const struct load_command *)( header + 1 );
    for( uint32_t i = 0u; i < header->ncmds; ++i ) {
        if( command->cmdsize < sizeof( *command )) return false;
        if( command->cmd == LC_SEGMENT_64 ) {
            const struct segment_command_64 *segment =
                (const struct segment_command_64 *)command;
            /* Apple linkers may split mutable globals into __DATA_DIRTY.
               __DATA_CONST stays excluded because it is read-only after dyld
               fixups and must never be a memcpy destination. */
            if( strncmp( segment->segname, "__DATA", 16u ) == 0 ||
                strncmp( segment->segname, "__DATA_DIRTY", 16u ) == 0 ) {
                const struct section_64 *section =
                    (const struct section_64 *)( segment + 1 );
                if( command->cmdsize < sizeof( *segment ) +
                    (size_t)segment->nsects * sizeof( *section ))
                    return false;
                for( uint32_t s = 0u; s < segment->nsects; ++s ) {
                    if(( strncmp( section[s].sectname, "__data", 16u ) == 0 ||
                         strncmp( section[s].sectname, "__bss", 16u ) == 0 ||
                         strncmp( section[s].sectname, "__common", 16u ) == 0 ) &&
                       !context_add_region(
                           manager, (void *)( slide + (uintptr_t)section[s].addr ),
                           (size_t)section[s].size ))
                        return false;
                }
            }
        }
        command = (const struct load_command *)((const unsigned char *)command +
                                                 command->cmdsize );
    }
    return manager->regionCount != 0u;
}
#elif defined(LIBOOT_MULTI_INSTANCE) && defined(_WIN32)
/* MinGW's linker provides this PE image-base symbol. Declare it as an
 * incomplete byte array so bounds diagnostics do not mistake the symbol for
 * a standalone 64-byte IMAGE_DOS_HEADER while we walk the complete image. */
extern unsigned char __ImageBase[];

static bool context_discover_regions( LibootContextManager *manager )
{
    unsigned char *base = __ImageBase;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if( dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ) return false;
    const IMAGE_NT_HEADERS *nt =
        (const IMAGE_NT_HEADERS *)( base + (size_t)dos->e_lfanew );
    if( nt->Signature != IMAGE_NT_SIGNATURE ) return false;
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION( nt );
    for( uint16_t i = 0u; i < nt->FileHeader.NumberOfSections; ++i ) {
        char name[9] = { 0 };
        memcpy( name, section[i].Name, 8u );
        if(( strcmp( name, ".data" ) == 0 || strcmp( name, ".bss" ) == 0 ) &&
           !context_add_region( manager, base + section[i].VirtualAddress,
                                (size_t)section[i].Misc.VirtualSize ))
            return false;
    }
    return manager->regionCount != 0u;
}
#elif defined(LIBOOT_MULTI_INSTANCE) && defined(__ELF__)
/* Discover this DSO by address rather than using __data_start/_end: those
 * names are interposable and can resolve to the host executable, which would
 * make a shared build copy unrelated application memory. Writable PT_LOAD
 * ranges contain this module's .data/.bss. GNU_RELRO is omitted because the
 * loader has made it read-only after applying relocations. */
typedef struct LibootElfDiscovery
{
    LibootContextManager *manager;
    uintptr_t needle;
    bool found;
    bool ok;
} LibootElfDiscovery;

static bool context_add_elf_load( LibootContextManager *manager,
                                  const struct dl_phdr_info *info,
                                  const ElfW(Phdr) *load )
{
    uintptr_t first = (uintptr_t)info->dlpi_addr + (uintptr_t)load->p_vaddr;
    uintptr_t last;
    uintptr_t cursor;
    if(( load->p_flags & PF_W ) == 0u || load->p_memsz == 0u ) return true;
    if((uint64_t)load->p_memsz > UINTPTR_MAX - first ) return false;
    last = first + (uintptr_t)load->p_memsz;
    cursor = first;

    while( cursor < last ) {
        uintptr_t nextRelroFirst = last;
        uintptr_t nextRelroLast = last;
        for( ElfW(Half) i = 0; i < info->dlpi_phnum; ++i ) {
            const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
            uintptr_t relroFirst;
            uintptr_t relroLast;
            if( ph->p_type != PT_GNU_RELRO || ph->p_memsz == 0u ) continue;
            relroFirst = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
            if((uint64_t)ph->p_memsz > UINTPTR_MAX - relroFirst ) return false;
            relroLast = relroFirst + (uintptr_t)ph->p_memsz;
            if( relroLast <= cursor || relroFirst >= last ) continue;
            if( relroFirst < nextRelroFirst ) {
                nextRelroFirst = relroFirst;
                nextRelroLast = relroLast;
            }
        }
        if( nextRelroFirst > cursor &&
            !context_add_region( manager, (void *)cursor,
                                 (size_t)( nextRelroFirst - cursor )))
            return false;
        if( nextRelroFirst == last ) break;
        cursor = nextRelroLast > cursor ? nextRelroLast : cursor;
        if( cursor > last ) cursor = last;
    }
    return true;
}

static int context_elf_visit( struct dl_phdr_info *info, size_t size, void *opaque )
{
    LibootElfDiscovery *discovery = (LibootElfDiscovery *)opaque;
    bool containsNeedle = false;
    (void)size;

    for( ElfW(Half) i = 0; i < info->dlpi_phnum; ++i ) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t first;
        uintptr_t last;
        if( ph->p_type != PT_LOAD || ph->p_memsz == 0u ) continue;
        first = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
        if((uint64_t)ph->p_memsz > UINTPTR_MAX - first ) continue;
        last = first + (uintptr_t)ph->p_memsz;
        if( discovery->needle >= first && discovery->needle < last ) {
            containsNeedle = true;
            break;
        }
    }
    if( !containsNeedle ) return 0;

    discovery->found = true;
    discovery->ok = true;
    for( ElfW(Half) i = 0; i < info->dlpi_phnum; ++i ) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if( ph->p_type == PT_LOAD &&
            !context_add_elf_load( discovery->manager, info, ph )) {
            discovery->ok = false;
            break;
        }
    }
    return 1;
}

static bool context_discover_regions( LibootContextManager *manager )
{
    LibootElfDiscovery discovery = {
        manager, (uintptr_t)&liboot_core_context_available, false, false
    };
    (void)dl_iterate_phdr( context_elf_visit, &discovery );
    return discovery.found && discovery.ok && manager->regionCount != 0u;
}
#else
static bool context_discover_regions( LibootContextManager *manager )
{
    (void)manager;
    return false;
}
#endif

static void context_copy_from_module( const LibootContextManager *manager,
                                      unsigned char *image )
{
    for( uint32_t i = 0u; i < manager->regionCount; ++i ) {
        const LibootContextRegion *region = &manager->regions[i];
        memcpy( image + region->imageOffset, region->address, region->size );
    }
}

static void context_copy_to_module( const LibootContextManager *manager,
                                    const unsigned char *image )
{
    for( uint32_t i = 0u; i < manager->regionCount; ++i ) {
        const LibootContextRegion *region = &manager->regions[i];
        memcpy( region->address, image + region->imageOffset, region->size );
    }
}

static bool context_prepare( LibootContextManager *manager )
{
    if( manager->prepared ) return manager->available;
    manager->prepared = true;
    if( !context_discover_regions( manager ) || manager->imageSize == 0u )
        return false;
    manager->baseline = malloc( manager->imageSize );
    if( manager->baseline == NULL ) return false;
    context_copy_from_module( manager, manager->baseline );
    manager->available = true;
    return true;
}

bool liboot_core_context_lock( void )
{
    LibootContextManager *manager = context_manager();
    return manager != NULL &&
           !atomic_flag_test_and_set_explicit( &manager->guard,
                                               memory_order_acquire );
}

void liboot_core_context_unlock( void )
{
    LibootContextManager *manager = context_manager();
    if( manager != NULL )
        atomic_flag_clear_explicit( &manager->guard, memory_order_release );
}

bool liboot_core_context_available( void )
{
    LibootContextManager *manager = context_manager();
    return manager != NULL && context_prepare( manager );
}

uint32_t liboot_core_context_next_handle_namespace( void )
{
    LibootContextManager *manager = context_manager();
    if( manager == NULL ) return 0u;
    if( manager->nextHandleNamespace >= 0xFFFFFFu ) return 0u;
    return ++manager->nextHandleNamespace;
}

uint32_t liboot_core_context_next_texture_revision( void )
{
    LibootContextManager *manager = context_manager();
    uint32_t value;
    if( manager == NULL ) return 0u;
    value = atomic_fetch_add_explicit( &manager->nextTextureRevision, 1u,
                                       memory_order_relaxed ) + 1u;
    /* Zero is the uninitialized public revision. Skip it if the 32-bit
     * counter ever wraps; as with the historical counter, practical
     * monotonicity is bounded by the public field width. */
    if( value == 0u )
        value = atomic_fetch_add_explicit( &manager->nextTextureRevision, 1u,
                                           memory_order_relaxed ) + 1u;
    return value;
}

bool liboot_core_context_set_thread_owner( const void *owner )
{
#if defined(_WIN32)
    LibootContextManager *manager = context_manager();
    return manager != NULL &&
           TlsSetValue( manager->threadOwnerIndex, (LPVOID)owner ) != 0;
#else
    sThreadOwner = owner;
    return true;
#endif
}

const void *liboot_core_context_get_thread_owner( void )
{
#if defined(_WIN32)
    LibootContextManager *manager = context_manager();
    return manager != NULL ? TlsGetValue( manager->threadOwnerIndex ) : NULL;
#else
    return sThreadOwner;
#endif
}

bool liboot_core_context_activate( LibootCoreContext *context )
{
    LibootContextManager *manager = context_manager();
    if( manager == NULL || !context_prepare( manager )) return false;
    if( manager->active == context ) {
        /* NULL also denotes the scratch image used while a new core is being
         * initialized. Force a real baseline restore so a failed creation
         * cannot leak partially initialized globals into the next engine. */
        if( context == NULL )
            context_copy_to_module( manager, manager->baseline );
        return true;
    }
    if( manager->active != NULL )
        context_copy_from_module( manager, manager->active->image );
    context_copy_to_module( manager,
                            context != NULL ? context->image : manager->baseline );
    manager->active = context;
    return true;
}

bool liboot_core_context_activate_owner( const void *owner,
                                         LibootCoreContext **outContext )
{
    LibootContextManager *manager = context_manager();
    LibootCoreContext *context;
    if( outContext != NULL ) *outContext = NULL;
    if( manager == NULL || owner == NULL || !context_prepare( manager ))
        return false;
    for( context = manager->contexts; context != NULL; context = context->next ) {
        if( context->owner == owner ) {
            if( !liboot_core_context_activate( context )) return false;
            if( outContext != NULL ) *outContext = context;
            return true;
        }
    }
    return false;
}

LibootCoreContext *liboot_core_context_capture( const void *owner )
{
    LibootContextManager *manager = context_manager();
    LibootCoreContext *context;
    if( manager == NULL || owner == NULL || !context_prepare( manager ) ||
        manager->active != NULL )
        return NULL;
    for( context = manager->contexts; context != NULL; context = context->next )
        if( context->owner == owner ) return NULL;
    context = calloc( 1u, sizeof( *context ));
    if( context == NULL ) return NULL;
    context->image = malloc( manager->imageSize );
    if( context->image == NULL ) {
        free( context );
        return NULL;
    }
    context_copy_from_module( manager, context->image );
    context->owner = owner;
    context->next = manager->contexts;
    manager->contexts = context;
    return context;
}

bool liboot_core_context_adopt( LibootCoreContext *context )
{
    LibootContextManager *manager = context_manager();
    if( manager == NULL || context == NULL || context->image == NULL ||
        !context_prepare( manager ) ||
        manager->active != NULL )
        return false;
    manager->active = context;
    return true;
}

void liboot_core_context_discard( LibootCoreContext *context )
{
    LibootContextManager *manager = context_manager();
    LibootCoreContext **cursor;
    if( context == NULL ) return;
    if( manager != NULL && manager->available && manager->active == context ) {
        context_copy_to_module( manager, manager->baseline );
        manager->active = NULL;
    }
    if( manager != NULL ) {
        cursor = &manager->contexts;
        while( *cursor != NULL && *cursor != context )
            cursor = &( *cursor )->next;
        if( *cursor == context ) *cursor = context->next;
    }
    free( context->image );
    free( context );
}
