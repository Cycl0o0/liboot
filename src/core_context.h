/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#ifndef LIBOOT_CORE_CONTEXT_H
#define LIBOOT_CORE_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct LibootCoreContext LibootCoreContext;

/* The checked engine wrapper serializes entry into the decompiled core.  A
 * shared-library build can additionally preserve the module's writable data
 * as one image per engine, allowing otherwise process-global game state to be
 * switched safely.  Static archives deliberately report unavailable: their
 * data sections are merged with the host executable and cannot be copied
 * without also copying unrelated application globals. */
bool liboot_core_context_lock( void );
void liboot_core_context_unlock( void );
bool liboot_core_context_available( void );
/* Fresh process-wide nonzero 24-bit token for an engine-returned handle
 * lifecycle. Tokens are never recycled; zero reports namespace exhaustion.
 * Must be called with the context lock held and never belongs to a switched
 * image. */
uint32_t liboot_core_context_next_handle_namespace( void );

/* Texture revisions are host cache identities, so they must remain monotonic
 * across engine switches and terminate/re-init cycles. This allocator lives
 * in the unswitched manager and is safe for the legacy raw entry points too. */
uint32_t liboot_core_context_next_texture_revision( void );

/* Callback dispatch follows the engine call currently holding the core lock.
 * Windows uses its native TLS API here so MinGW static consumers do not gain
 * an otherwise unnecessary libwinpthread dependency. */
bool liboot_core_context_set_thread_owner( const void *owner );
const void *liboot_core_context_get_thread_owner( void );

/* Switch to an existing image. Passing NULL saves the active image (if any)
 * and restores the clean module baseline. All calls require the context lock. */
bool liboot_core_context_activate( LibootCoreContext *context );
/* Validate an opaque engine owner without dereferencing it, then activate the
 * context registered for that owner. This keeps stale/foreign handles from
 * becoming arbitrary pointer reads in the public engine API. */
bool liboot_core_context_activate_owner( const void *owner,
                                         LibootCoreContext **outContext );

/* Capture the currently loaded native state after a successful initialization,
 * then mark that image as active without another restore. */
LibootCoreContext *liboot_core_context_capture( const void *owner );
bool liboot_core_context_adopt( LibootCoreContext *context );

/* The caller must terminate the native core before discarding its active
 * image. This restores the clean baseline and releases the saved copy. */
void liboot_core_context_discard( LibootCoreContext *context );

#endif
