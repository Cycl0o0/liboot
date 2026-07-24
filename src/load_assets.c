/* Runtime asset binding + Player lifecycle glue. The strong definitions here
   override the weak bring-up hooks in src/shim/stubs.c. */
#include "liboot_assets.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ultra64.h"
#include "animation.h"
#include "player.h"
#include "play_state.h"
#include "scene.h"
#include "segmented_address.h"
#include "save.h"
#include "regs.h"
#include "sys_matrix.h"
#include "liboot.h"

extern FlexSkeletonHeader gLinkAdultSkel;
extern FlexSkeletonHeader gLinkChildSkel;

/* Anchor for the decomp's linker-script symbol: z_skelanime computes
   animation "ROM" addresses relative to this array; the shim's DMA service
   maps any address >= this anchor back into the segment-7 blob. */
u8 _link_animetionSegmentRomStart[1] __attribute__(( aligned( 16 )));

extern void Player_Init( Actor *thisx, PlayState *play );
extern void Actor_Init( Actor *actor, PlayState *play );
extern void Player_Update( Actor *thisx, PlayState *play );
extern void Player_Draw( Actor *thisx, PlayState *play );

/* liboot v0.3: attention/Z-targeting wiring (z_actor.c internals without
   header prototypes in the decomp) */
#include "sfx.h"
#include "z_lib.h"
extern void Attention_Init( Attention *attention, Actor *actor, PlayState *play );
extern void Attention_Update( Attention *attention, Player *player, Actor *playerFocusActor, PlayState *play );

/* liboot v0.4: real Navi (vendored EnElf) */
#include "z_actor_dlftbls.h"
#include "object.h"
extern ActorProfile En_Elf_Profile;
extern void ZeldaArena_Free( void *ptr );

/* liboot v0.6: real projectiles (vendored EnArrow/EnBom/EnBoom/ArmsHook) */
#include "overlays/actors/ovl_Arms_Hook/z_arms_hook.h"
extern ActorProfile En_Arrow_Profile;
extern ActorProfile En_Bom_Profile;
extern ActorProfile En_Boom_Profile;
extern ActorProfile Arms_Hook_Profile;
extern void ArmsHook_Wait( ArmsHook *thisx, PlayState *play );
extern Actor *Actor_Delete( ActorContext *actorCtx, Actor *actor, PlayState *play );
extern int Player_HoldsHookshot( Player *player );

static u32 be32at( const u8 *p ) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static s16 be16at( const u8 *p ) { return (s16)(( (u16)p[0] << 8 ) | p[1] ); }

#define LINK_LIMB_COUNT 21
#define ASSET_LIMB_MAX  32

static LodLimb sAdultLimbs[LINK_LIMB_COUNT];
static void *sAdultLimbTable[LINK_LIMB_COUNT];
static LodLimb sChildLimbs[LINK_LIMB_COUNT];
static void *sChildLimbTable[LINK_LIMB_COUNT];
static bool sAdultValid;
static bool sChildValid;

static bool asset_span_valid( size_t size, u32 offset, size_t length )
{
    return offset <= size && length <= size - offset;
}

static bool asset_token_span_valid( u32 token, u8 segment, size_t size,
                                    size_t length, u32 alignment )
{
    u32 offset = token & 0x00FFFFFFu;

    return ( token >> 24 ) == segment &&
           ( alignment == 0 || ( offset % alignment ) == 0 ) &&
           asset_span_valid( size, offset, length );
}

static bool asset_dlist_token_valid( u32 token, u8 segment, size_t size )
{
    /* A null display list is valid.  A non-null one must name the beginning
       of at least one aligned Gfx command in the asset's own segment. */
    return token == 0 || asset_token_span_valid( token, segment, size, 8, 8 );
}

static bool limb_graph_visit( const u8 *children, const u8 *siblings, u8 count,
                              u8 node, u8 depth, u8 *state, u8 *visited )
{
    if( node >= count || depth >= count || state[node] != 0 ) return false;

    state[node] = 1;
    ( *visited )++;
    if( children[node] != 0xFF &&
        !limb_graph_visit( children, siblings, count, children[node],
                           (u8)( depth + 1 ), state, visited )) return false;
    if( siblings[node] != 0xFF &&
        !limb_graph_visit( children, siblings, count, siblings[node],
                           (u8)( depth + 1 ), state, visited )) return false;
    state[node] = 2;
    return true;
}

/* The game's walkers enter limb zero explicitly and recurse only through its
   child.  Reject root siblings, out-of-range edges, cycles, repeated nodes,
   and disconnected limbs before publishing any skeleton to the decomp. */
static bool limb_graph_valid( const u8 *children, const u8 *siblings, u8 count )
{
    u8 state[ASSET_LIMB_MAX] = { 0 };
    u8 visited = 0;

    if( count == 0 || count > ASSET_LIMB_MAX || siblings[0] != 0xFF ) return false;
    for( u8 i = 0; i < count; ++i ) {
        if(( children[i] != 0xFF && children[i] >= count ) ||
           ( siblings[i] != 0xFF && siblings[i] >= count )) return false;
    }
    return limb_graph_visit( children, siblings, count, 0, 0, state, &visited ) &&
           visited == count;
}

/* Parse the big-endian serialized FlexSkeletonHeader + LodLimbs out of an
   extracted link object into native mirrors (ANALYSIS §5). The limb display
   lists stay segment-6 tokens for the DL interpreter. */
static bool relocate_link_skeleton( const u8 *blob, size_t size, u32 skelOff,
                                    LodLimb *limbs, void **limbTableOut,
                                    FlexSkeletonHeader *outSkel )
{
    u8 children[LINK_LIMB_COUNT];
    u8 siblings[LINK_LIMB_COUNT];

    memset( limbs, 0, sizeof( *limbs ) * LINK_LIMB_COUNT );
    memset( limbTableOut, 0, sizeof( *limbTableOut ) * LINK_LIMB_COUNT );
    memset( outSkel, 0, sizeof( *outSkel ));
    if( !blob || !asset_span_valid( size, skelOff, 9 )) return false;

    const u8 *hdr = blob + skelOff;
    u32 limbTableTok = be32at( hdr );
    u8 limbCount = hdr[4];
    u8 dListCount = hdr[8];

    if( limbCount != LINK_LIMB_COUNT || dListCount == 0 || dListCount > limbCount ||
        !asset_token_span_valid( limbTableTok, 0x06, size,
                                 4u * LINK_LIMB_COUNT, 4 )) return false;
    const u8 *limbTable = blob + ( limbTableTok & 0x00FFFFFF );

    for( int i = 0; i < LINK_LIMB_COUNT; ++i ) {
        u32 limbTok = be32at( limbTable + i * 4 );
        if( !asset_token_span_valid( limbTok, 0x06, size, 16, 4 )) goto fail;
        u32 limbOff = limbTok & 0x00FFFFFF;
        const u8 *limb = blob + limbOff;
        u32 dListNear = be32at( limb + 8 );
        u32 dListFar = be32at( limb + 12 );
        if( !asset_dlist_token_valid( dListNear, 0x06, size ) ||
            !asset_dlist_token_valid( dListFar, 0x06, size )) goto fail;
        limbs[i].jointPos.x = be16at( limb + 0 );
        limbs[i].jointPos.y = be16at( limb + 2 );
        limbs[i].jointPos.z = be16at( limb + 4 );
        limbs[i].child      = limb[6];
        limbs[i].sibling    = limb[7];
        limbs[i].dLists[0]  = (Gfx *)(uintptr_t)dListNear;
        limbs[i].dLists[1]  = (Gfx *)(uintptr_t)dListFar;
        limbTableOut[i] = &limbs[i];
        children[i] = limb[6];
        siblings[i] = limb[7];
    }
    if( !limb_graph_valid( children, siblings, LINK_LIMB_COUNT )) goto fail;

    outSkel->sh.segment = limbTableOut;
    outSkel->sh.limbCount = LINK_LIMB_COUNT;
    outSkel->dListCount = dListCount;
    return true;

fail:
    memset( limbs, 0, sizeof( *limbs ) * LINK_LIMB_COUNT );
    memset( limbTableOut, 0, sizeof( *limbTableOut ) * LINK_LIMB_COUNT );
    memset( outSkel, 0, sizeof( *outSkel ));
    return false;
}

/* ---- Navi / gameplay_keep fairy assets (liboot v0.4) -------------------- */

/* PAL (gc-eu) gameplay_keep offsets, ROM-verified; the header fields are
   validated before use so an unexpected layout just disables Navi. */
#define FAIRY_SKEL_OFF  0x16A48
#define FAIRY_ANIM_OFF  0x14BA4
#define FAIRY_LIMB_COUNT 14

/* Referenced by the vendored z_en_elf.c (the only two asset symbols it
   needs); filled from the extracted segment-4 blob at bind time. The limb
   display lists stay segment-4 tokens for the DL interpreter. */
SkeletonHeader gFairySkel;
AnimationHeader gFairyAnim;

static StandardLimb sFairyLimbs[FAIRY_LIMB_COUNT];
static void *sFairyLimbTable[FAIRY_LIMB_COUNT];
static s16 sFairyFrameData[64];
static JointIndex sFairyJointIndices[FAIRY_LIMB_COUNT + 1];
static bool sFairyValid;

static bool animation_indices_valid( const JointIndex *indices, u32 indexCount,
                                     u32 frameDataCount, u16 staticIndexMax,
                                     u16 frameCount )
{
    if( staticIndexMax > frameDataCount ) return false;
    for( u32 i = 0; i < indexCount; ++i ) {
        const u16 component[3] = { indices[i].x, indices[i].y, indices[i].z };
        for( int axis = 0; axis < 3; ++axis ) {
            size_t last = component[axis];
            if( component[axis] >= staticIndexMax ) last += (size_t)frameCount - 1u;
            if( last >= frameDataCount ) return false;
        }
    }
    return true;
}

static void relocate_fairy_assets( const u8 *blob, size_t size )
{
    u8 children[FAIRY_LIMB_COUNT];
    u8 siblings[FAIRY_LIMB_COUNT];

    sFairyValid = false;
    memset( sFairyLimbs, 0, sizeof( sFairyLimbs ));
    memset( sFairyLimbTable, 0, sizeof( sFairyLimbTable ));
    memset( sFairyFrameData, 0, sizeof( sFairyFrameData ));
    memset( sFairyJointIndices, 0, sizeof( sFairyJointIndices ));
    memset( &gFairySkel, 0, sizeof( gFairySkel ));
    memset( &gFairyAnim, 0, sizeof( gFairyAnim ));
    if( !blob || !asset_span_valid( size, FAIRY_SKEL_OFF, 8 )) return;

    /* standard (non-flex) SkeletonHeader: limb table token + limbCount */
    const u8 *hdr = blob + FAIRY_SKEL_OFF;
    u32 limbTableTok = be32at( hdr );
    if( hdr[4] != FAIRY_LIMB_COUNT ||
        !asset_token_span_valid( limbTableTok, 0x04, size,
                                 4u * FAIRY_LIMB_COUNT, 4 )) return;
    u32 limbTableOff = limbTableTok & 0x00FFFFFF;

    for( int i = 0; i < FAIRY_LIMB_COUNT; ++i ) {
        u32 limbTok = be32at( blob + limbTableOff + i * 4 );
        if( !asset_token_span_valid( limbTok, 0x04, size, 12, 4 )) goto fail;
        u32 limbOff = limbTok & 0x00FFFFFF;
        const u8 *limb = blob + limbOff;
        u32 dList = be32at( limb + 8 );
        if( !asset_dlist_token_valid( dList, 0x04, size )) goto fail;
        sFairyLimbs[i].jointPos.x = be16at( limb + 0 );
        sFairyLimbs[i].jointPos.y = be16at( limb + 2 );
        sFairyLimbs[i].jointPos.z = be16at( limb + 4 );
        sFairyLimbs[i].child      = limb[6];
        sFairyLimbs[i].sibling    = limb[7];
        sFairyLimbs[i].dList      = (Gfx *)(uintptr_t)dList;
        sFairyLimbTable[i] = &sFairyLimbs[i];
        children[i] = limb[6];
        siblings[i] = limb[7];
    }
    if( !limb_graph_valid( children, siblings, FAIRY_LIMB_COUNT )) goto fail;

    /* standard AnimationHeader; frameData/jointIndices are S16-swapped into
       native arrays (SEGMENTED_TO_VIRTUAL passes native pointers through) */
    if( !asset_span_valid( size, FAIRY_ANIM_OFF, 14 )) goto fail;
    const u8 *ah = blob + FAIRY_ANIM_OFF;
    s16 frameCount = be16at( ah );
    u32 fdTok = be32at( ah + 4 ), jiTok = be32at( ah + 8 );
    u16 staticIndexMax = (u16)(( ah[12] << 8 ) | ah[13] );
    if( frameCount <= 0 || !asset_token_span_valid( fdTok, 0x04, size, 2, 2 ) ||
        !asset_token_span_valid( jiTok, 0x04, size,
                                 6u * ( FAIRY_LIMB_COUNT + 1 ), 2 )) goto fail;
    u32 fdOff = fdTok & 0x00FFFFFF, jiOff = jiTok & 0x00FFFFFF;
    if( jiOff <= fdOff || (( jiOff - fdOff ) & 1u ) != 0 ) goto fail;
    u32 fdCount = ( jiOff - fdOff ) / 2;
    if( fdCount == 0 || fdCount > 64 ) goto fail;

    for( u32 j = 0; j < fdCount; ++j )
        sFairyFrameData[j] = be16at( blob + fdOff + j * 2 );
    for( int j = 0; j < FAIRY_LIMB_COUNT + 1; ++j ) {
        sFairyJointIndices[j].x = (u16)be16at( blob + jiOff + j * 6 + 0 );
        sFairyJointIndices[j].y = (u16)be16at( blob + jiOff + j * 6 + 2 );
        sFairyJointIndices[j].z = (u16)be16at( blob + jiOff + j * 6 + 4 );
    }
    if( !animation_indices_valid( sFairyJointIndices, FAIRY_LIMB_COUNT + 1,
                                  fdCount, staticIndexMax, (u16)frameCount )) goto fail;
    gFairySkel.segment = sFairyLimbTable;
    gFairySkel.limbCount = FAIRY_LIMB_COUNT;
    gFairyAnim.common.frameCount = frameCount;
    gFairyAnim.frameData = sFairyFrameData;
    gFairyAnim.jointIndices = sFairyJointIndices;
    gFairyAnim.staticIndexMax = staticIndexMax;
    sFairyValid = true;
    return;

fail:
    memset( sFairyLimbs, 0, sizeof( sFairyLimbs ));
    memset( sFairyLimbTable, 0, sizeof( sFairyLimbTable ));
    memset( sFairyFrameData, 0, sizeof( sFairyFrameData ));
    memset( sFairyJointIndices, 0, sizeof( sFairyJointIndices ));
    memset( &gFairySkel, 0, sizeof( gFairySkel ));
    memset( &gFairyAnim, 0, sizeof( gFairyAnim ));
}

/* ---- EnArrow gameplay_keep assets (liboot v0.6) ------------------------- */

/* PAL (gc-eu) offsets; the whole block sits below the ~0xC878 per-version
   layout fork in gameplay_keep so they are version-independent. Header
   fields are validated before use so an unexpected layout just keeps
   EnArrow cut (bow works, no arrow actor). */
#define ARROW_SKEL_OFF   0x6010
#define ARROW_ANIM1_OFF  0x4310
#define ARROW_ANIM2_OFF  0x436C
#define ARROW_LIMB_COUNT 4
#define ARROW_FRAME_DATA_CAP 256

/* Referenced by the vendored z_en_arrow.c (LodLimb variant of the fairy
   relocation); filled from the extracted segment-4 blob at bind time. The
   limb display lists stay segment-4 tokens for the DL interpreter. */
SkeletonHeader gArrowSkel;
AnimationHeader gArrow1_Anim;
AnimationHeader gArrow2_Anim;

static LodLimb sArrowLimbs[ARROW_LIMB_COUNT];
static void *sArrowLimbTable[ARROW_LIMB_COUNT];
static s16 sArrowFrameData[2][ARROW_FRAME_DATA_CAP];
static JointIndex sArrowJointIndices[2][ARROW_LIMB_COUNT + 1];
static bool sArrowValid;

static bool relocate_arrow_anim( const u8 *blob, size_t size, u32 animOff,
                                 s16 *frameData, JointIndex *jointIndices,
                                 AnimationHeader *out )
{
    memset( out, 0, sizeof( *out ));
    if( !blob || !asset_span_valid( size, animOff, 16 )) return false;
    const u8 *ah = blob + animOff;
    s16 frameCount = be16at( ah );
    u32 fdTok = be32at( ah + 4 ), jiTok = be32at( ah + 8 );
    u16 staticIndexMax = (u16)(( ah[12] << 8 ) | ah[13] );
    if( frameCount <= 0 || !asset_token_span_valid( fdTok, 0x04, size, 2, 2 ) ||
        !asset_token_span_valid( jiTok, 0x04, size,
                                 6u * ( ARROW_LIMB_COUNT + 1 ), 2 )) return false;
    u32 fdOff = fdTok & 0x00FFFFFF, jiOff = jiTok & 0x00FFFFFF;
    if( jiOff <= fdOff || (( jiOff - fdOff ) & 1u ) != 0 ) return false;
    u32 fdCount = ( jiOff - fdOff ) / 2;
    if( fdCount == 0 || fdCount > ARROW_FRAME_DATA_CAP ) return false;

    for( u32 j = 0; j < fdCount; ++j )
        frameData[j] = be16at( blob + fdOff + j * 2 );
    for( int j = 0; j < ARROW_LIMB_COUNT + 1; ++j ) {
        jointIndices[j].x = (u16)be16at( blob + jiOff + j * 6 + 0 );
        jointIndices[j].y = (u16)be16at( blob + jiOff + j * 6 + 2 );
        jointIndices[j].z = (u16)be16at( blob + jiOff + j * 6 + 4 );
    }
    if( !animation_indices_valid( jointIndices, ARROW_LIMB_COUNT + 1,
                                  fdCount, staticIndexMax, (u16)frameCount )) return false;
    out->common.frameCount = frameCount;
    out->frameData = frameData;
    out->jointIndices = jointIndices;
    out->staticIndexMax = staticIndexMax;
    return true;
}

static void relocate_arrow_assets( const u8 *blob, size_t size )
{
    u8 children[ARROW_LIMB_COUNT];
    u8 siblings[ARROW_LIMB_COUNT];

    sArrowValid = false;
    memset( sArrowLimbs, 0, sizeof( sArrowLimbs ));
    memset( sArrowLimbTable, 0, sizeof( sArrowLimbTable ));
    memset( sArrowFrameData, 0, sizeof( sArrowFrameData ));
    memset( sArrowJointIndices, 0, sizeof( sArrowJointIndices ));
    memset( &gArrowSkel, 0, sizeof( gArrowSkel ));
    memset( &gArrow1_Anim, 0, sizeof( gArrow1_Anim ));
    memset( &gArrow2_Anim, 0, sizeof( gArrow2_Anim ));
    if( !blob || !asset_span_valid( size, ARROW_SKEL_OFF, 8 )) return;

    /* standard (non-flex) SkeletonHeader whose limbs are LodLimbs */
    const u8 *hdr = blob + ARROW_SKEL_OFF;
    u32 limbTableTok = be32at( hdr );
    if( hdr[4] != ARROW_LIMB_COUNT ||
        !asset_token_span_valid( limbTableTok, 0x04, size,
                                 4u * ARROW_LIMB_COUNT, 4 )) return;
    u32 limbTableOff = limbTableTok & 0x00FFFFFF;

    for( int i = 0; i < ARROW_LIMB_COUNT; ++i ) {
        u32 limbTok = be32at( blob + limbTableOff + i * 4 );
        if( !asset_token_span_valid( limbTok, 0x04, size, 16, 4 )) goto fail;
        u32 limbOff = limbTok & 0x00FFFFFF;
        const u8 *limb = blob + limbOff;
        u32 dListNear = be32at( limb + 8 );
        u32 dListFar = be32at( limb + 12 );
        if( !asset_dlist_token_valid( dListNear, 0x04, size ) ||
            !asset_dlist_token_valid( dListFar, 0x04, size )) goto fail;
        sArrowLimbs[i].jointPos.x = be16at( limb + 0 );
        sArrowLimbs[i].jointPos.y = be16at( limb + 2 );
        sArrowLimbs[i].child      = limb[6];
        sArrowLimbs[i].jointPos.z = be16at( limb + 4 );
        sArrowLimbs[i].sibling    = limb[7];
        sArrowLimbs[i].dLists[0]  = (Gfx *)(uintptr_t)dListNear;
        sArrowLimbs[i].dLists[1]  = (Gfx *)(uintptr_t)dListFar;
        sArrowLimbTable[i] = &sArrowLimbs[i];
        children[i] = limb[6];
        siblings[i] = limb[7];
    }
    if( !limb_graph_valid( children, siblings, ARROW_LIMB_COUNT )) goto fail;

    if( !relocate_arrow_anim( blob, size, ARROW_ANIM1_OFF, sArrowFrameData[0],
                              sArrowJointIndices[0], &gArrow1_Anim )) goto fail;
    if( !relocate_arrow_anim( blob, size, ARROW_ANIM2_OFF, sArrowFrameData[1],
                              sArrowJointIndices[1], &gArrow2_Anim )) goto fail;
    gArrowSkel.segment = sArrowLimbTable;
    gArrowSkel.limbCount = ARROW_LIMB_COUNT;
    sArrowValid = true;
    return;

fail:
    memset( sArrowLimbs, 0, sizeof( sArrowLimbs ));
    memset( sArrowLimbTable, 0, sizeof( sArrowLimbTable ));
    memset( sArrowFrameData, 0, sizeof( sArrowFrameData ));
    memset( sArrowJointIndices, 0, sizeof( sArrowJointIndices ));
    memset( &gArrowSkel, 0, sizeof( gArrowSkel ));
    memset( &gArrow1_Anim, 0, sizeof( gArrow1_Anim ));
    memset( &gArrow2_Anim, 0, sizeof( gArrow2_Anim ));
}

static const u8 *sChildBlob; static size_t sChildBlobSize;

/* Adult and child assets both live in "segment 6" objects, but only one can
   be mapped at a time; child-named symbols read from the child blob. */
void liboot_set_child_blob( const void *blob, size_t size )
{
    sChildBlob = blob;
    sChildBlobSize = size;
}

bool liboot_link_skeleton_valid( u8 age )
{
    return age == OOT_AGE_ADULT ? sAdultValid :
           age == OOT_AGE_CHILD ? sChildValid : false;
}

bool liboot_bind_assets( void )
{
    for( u32 i = 0; i < gLibootAssetBindCount; ++i ) {
        const LibootAssetBind *b = &gLibootAssetBinds[i];
        size_t segSize;
        const u8 *base;
        if( strncmp( b->name, "gLinkChild", 10 ) == 0 ) {
            base = sChildBlob;
            segSize = sChildBlobSize;
        } else {
            base = liboot_segment_base( (int)b->segment, &segSize );
        }
        if( !base || b->offset + b->size > segSize ) continue;
        if( b->mode == LIBOOT_BIND_S16 ) {
            s16 *dst = b->dst;
            for( u32 j = 0; j + 1 < b->size; j += 2 )
                *dst++ = be16at( base + b->offset + j );
        } else {
            memcpy( b->dst, base + b->offset, b->size );
        }
    }

    size_t seg6Size;
    const u8 *seg6 = liboot_segment_base( 6, &seg6Size );
    sAdultValid = relocate_link_skeleton( seg6, seg6Size, 0x377F4,
                                          sAdultLimbs, sAdultLimbTable, &gLinkAdultSkel );
    sChildValid = relocate_link_skeleton( sChildBlob, sChildBlobSize, 0x2CF6C,
                                          sChildLimbs, sChildLimbTable, &gLinkChildSkel );

    /* liboot v0.4: fairy (Navi) skeleton + animation from gameplay_keep */
    size_t seg4Size;
    const u8 *seg4 = liboot_segment_base( 4, &seg4Size );
    relocate_fairy_assets( seg4, seg4Size );

    /* liboot v0.6: arrow skeleton + animations from gameplay_keep */
    relocate_arrow_assets( seg4, seg4Size );
    return sAdultValid;
}

/* ---- Player lifecycle -------------------------------------------------- */

static SceneTableEntry sFakeScene; /* zero titleFile -> Player_Init skips title card */

/* liboot v0.4 (generalized in v0.6): remove every spawned actor except the
   Player — called when the Player is re-initialized (age switch) or deleted.
   The per-tick reap in liboot_link_update only collects Actor_Kill'd actors;
   this drops live ones (Navi, held projectiles, ...) through their own
   destroy hooks before the Player they point at goes away.

   Attention targets (targets.c) also sit in these lists but live in a static
   pool, not the arena: unlink them and release their slot instead of freeing
   (freeing static memory is a glibc abort), and skip the total-- they never
   counted toward. Host-side target ids die here; oot_target_move/remove on a
   released id are safe no-ops. */
bool liboot_target_owns( Actor *actor );
void liboot_target_release( Actor *actor );

void liboot_despawn_actors( PlayState *play )
{
    for( int cat = 0; cat < ACTORCAT_MAX; cat++ ) {
        ActorListEntry *list = &play->actorCtx.actorLists[cat];
        Actor *it = list->head;
        while( it != NULL ) {
            Actor *next = it->next;
            if( it->id != ACTOR_PLAYER ) {
                if( it->destroy )
                    it->destroy( it, play );
                if( it->prev != NULL ) it->prev->next = it->next;
                else list->head = it->next;
                if( it->next != NULL ) it->next->prev = it->prev;
                it->prev = it->next = NULL;
                list->length--;
                if( liboot_target_owns( it )) {
                    liboot_target_release( it );
                } else {
                    if( play->actorCtx.total > 0 ) play->actorCtx.total--;
                    ZeldaArena_Free( it );
                }
            }
            it = next;
        }
    }
    play->actorCtx.attention.naviHoverActor = NULL;
    play->actorCtx.attention.arrowHoverActor = NULL;
    play->actorCtx.attention.reticleActor = NULL;
    play->actorCtx.attention.forcedLockOnActor = NULL;
    play->actorCtx.attention.bgmEnemy = NULL;
}

/* liboot v0.4: bodyPartsPos is normally written by the draw path
   (Player_PostLimbDrawGameplay), which liboot never runs. Navi hovers over
   bodyPartsPos[PLAYER_BODYPART_HAT], so approximate every part with the
   posed joint world positions (limb-table index = PLAYER_LIMB_* - 1). */
static const u8 sBodyPartLimbIndex[PLAYER_BODYPART_MAX] = {
    1,          /* WAIST      */
    3, 4, 5,    /* R leg      */
    6, 7, 8,    /* L leg      */
    10, 11, 12, /* HEAD HAT COLLAR */
    13, 14, 15, /* L arm      */
    16, 17, 18, /* R arm      */
    19, 20,     /* SHEATH TORSO */
};

bool oot_link_get_skeleton_impl( Player *player, struct OoTSkeletonPose *out );

static void liboot_fill_body_parts( Player *player )
{
    struct OoTSkeletonPose pose;
    if( !oot_link_get_skeleton_impl( player, &pose )) return;
    for( int i = 0; i < PLAYER_BODYPART_MAX; ++i ) {
        u8 limb = sBodyPartLimbIndex[i];
        if( limb >= pose.numJoints ) continue;
        player->bodyPartsPos[i].x = pose.jointPos[limb][0];
        player->bodyPartsPos[i].y = pose.jointPos[limb][1];
        player->bodyPartsPos[i].z = pose.jointPos[limb][2];
    }
}

void liboot_link_init( PlayState *play, Player *player, float x, float y, float z )
{
    play->loadedScene = &sFakeScene;

    /* liboot v0.4: make the vendored EnElf spawnable before Player_Init runs
       (its Player_SpawnFairy path auto-spawns Navi through the real
       Actor_Spawn; vramStart NULL takes the non-overlay branch). Actor_Spawn
       keeps EnElf cut while the profile is missing (fairy assets absent).
       liboot v0.6: same registry, extended to the projectile actors — a
       profile is only registered while its assets bound, so a previous
       oot_global_init against a ROM without them re-cuts the id (Actor_Spawn
       returns NULL, callers treat it as "spawn failed"). */
    {
        size_t keepSize = 0;
        bool keep = liboot_segment_base( 4, &keepSize ) != NULL;

        gActorOverlayTable[ACTOR_EN_ELF].profile = sFairyValid ? &En_Elf_Profile : NULL;
        gActorOverlayTable[ACTOR_EN_ARROW].profile = ( keep && sArrowValid ) ? &En_Arrow_Profile : NULL;
        gActorOverlayTable[ACTOR_EN_BOM].profile = keep ? &En_Bom_Profile : NULL;
        gActorOverlayTable[ACTOR_EN_BOOM].profile = keep ? &En_Boom_Profile : NULL;
        /* ArmsHook only needs object_link_boy (slot 0), present whenever the
           library initialized; as child the slot holds LINK_CHILD so the
           spawn correctly fails through Object_GetSlot. */
        gActorOverlayTable[ACTOR_ARMS_HOOK].profile = &Arms_Hook_Profile;
    }

    /* a re-init (age switch) leaves the previous Navi/projectiles in the
       actor lists; drop them through their destroy hooks before respawning */
    liboot_despawn_actors( play );

    Actor *actor = &player->actor;
    actor->id = ACTOR_PLAYER;
    actor->category = ACTORCAT_PLAYER;
    actor->params = PLAYER_PARAMS( PLAYER_START_MODE_IDLE, 0 );
    actor->world.pos.x = actor->home.pos.x = actor->prevPos.x = x;
    actor->world.pos.y = actor->home.pos.y = actor->prevPos.y = y;
    actor->world.pos.z = actor->home.pos.z = actor->prevPos.z = z;
    actor->update = Player_Update;
    actor->init = Player_Init;
    /* liboot v0.6: never called by liboot itself (liboot_render_link walks
       the skeleton directly), but ArmsHook_Draw keys its chain/tip render on
       player->actor.draw != NULL, matching the real spawn-path assignment. */
    actor->draw = Player_Draw;

    /* make GET_PLAYER(play) work */
    play->actorCtx.actorLists[ACTORCAT_PLAYER].head = actor;

    /* the standard spawn-path defaults (scale 0.01, focus, colChkInfo, ...) —
       normally applied by Actor_Spawn, which liboot cuts. Calls Player_Init
       through actor->init like the real spawn path. */
    Actor_Init( actor, play );

    /* Player_Init leaves Link in the scene-entrance transit action; drop him
       straight into the normal controllable idle instead (same routine the
       game uses to restore control after cutscenes). */
    if( play->func_11D54 )
        play->func_11D54( player, play );
    player->stateFlags1 &= ~( PLAYER_STATE1_29 | PLAYER_STATE1_31 );

    /* a previous Link's y < -4000 void-out latched play->transitionTrigger
       = TRANS_TRIGGER_START, which zeroes stick speed for every Player while
       set (Player_CalcSpeedAndYawFromControlStick) — new Link, new session:
       drop the latch so the fresh Player is controllable. */
    play->transitionTrigger = TRANS_TRIGGER_OFF;

    /* liboot v0.3: attention system boot — the real game does this at the end
       of Actor_InitContext with actorLists[ACTORCAT_PLAYER].head. */
    Attention_Init( &play->actorCtx.attention, actor, play );

    /* liboot has no real view/projection pipeline. Attention needs
       Actor_ProjectPos results with z > 0 and |x*invW| < 1 or it treats every
       actor as off-screen and drops the reticle. This constant matrix
       projects every world position to clip (0,0,1,w=1): always on-screen,
       always in front. Hosts that want the genuine off-screen release can
       overwrite play->viewProjectionMtxF with their own VP matrix per tick. */
    memset( &play->viewProjectionMtxF, 0, sizeof( play->viewProjectionMtxF ));
    play->viewProjectionMtxF.zw = 1.0f;
    play->viewProjectionMtxF.ww = 1.0f;

    /* liboot v0.6: billboardMtxF is normally rebuilt from the camera each
       frame; the zeroed matrix would collapse geometry in draws that
       Matrix_ReplaceRotation with it (EnBom, arrow sparkles), so seed
       identity = axis-aligned billboards. MREG(95) is the arrow LOD split
       depth: projectedPos.z stays 0 in liboot, 0 < 1 selects the near LOD. */
    memset( &play->billboardMtxF, 0, sizeof( play->billboardMtxF ));
    play->billboardMtxF.xx = play->billboardMtxF.yy =
        play->billboardMtxF.zz = play->billboardMtxF.ww = 1.0f;
    MREG( 95 ) = 1;

    /* liboot v0.4: seed bodyPartsPos so Navi's first update doesn't read the
       world origin (refined every tick once animation tasks run) */
    liboot_fill_body_parts( player );
}

/* liboot v0.6: the real game positions held projectiles from the posed hand
   matrices inside Player_PostLimbDrawGameplay (z_player_lib.c), a draw pass
   liboot never runs. Replicate the three attachments that gameplay reads
   back, from the same bodyPartsPos the draw pass would have produced:
     - hookshot muzzle player->unk_3C8 (ArmsHook retracts toward it; zeroed
       it would reel in toward the world origin),
     - the un-fired ArmsHook riding the hand,
     - the nocked arrow pos/aim (spawn leaves it at Link's feet, rot.x 0),
     - the carried bomb at the hands' midpoint.
   Runs at end-of-tick, so actors consume it next tick exactly like the
   game's update -> draw cadence. */
static void liboot_replicate_draw_attachments( PlayState *play, Player *player )
{
    Actor *held = player->heldActor;
    (void)play;

    if( Player_HoldsHookshot( player )) {
        player->unk_3C8 = player->bodyPartsPos[PLAYER_BODYPART_R_HAND];
        if( held != NULL && held->id == ACTOR_ARMS_HOOK &&
            (( ArmsHook * )held )->actionFunc == ArmsHook_Wait ) {
            held->world.pos = player->unk_3C8;
            held->world.rot.x = player->actor.focus.rot.x;
            held->world.rot.y = player->actor.shape.rot.y;
            held->shape.rot = held->world.rot;
            (( ArmsHook * )held )->unk_1E8 = held->world.pos;
        }
    } else if(( player->stateFlags1 & PLAYER_STATE1_9 ) &&
              held != NULL && held->id == ACTOR_EN_ARROW ) {
        held->world.pos = player->bodyPartsPos[PLAYER_BODYPART_L_HAND];
        held->world.rot.x = player->actor.focus.rot.x;
        held->world.rot.y = player->actor.shape.rot.y;
        held->shape.rot = held->world.rot;
    } else if(( player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR ) && held != NULL ) {
        held->world.pos.x = ( player->bodyPartsPos[PLAYER_BODYPART_R_HAND].x +
                              player->bodyPartsPos[PLAYER_BODYPART_L_HAND].x ) * 0.5f;
        held->world.pos.y = ( player->bodyPartsPos[PLAYER_BODYPART_R_HAND].y +
                              player->bodyPartsPos[PLAYER_BODYPART_L_HAND].y ) * 0.5f;
        held->world.pos.z = ( player->bodyPartsPos[PLAYER_BODYPART_R_HAND].z +
                              player->bodyPartsPos[PLAYER_BODYPART_L_HAND].z ) * 0.5f;
        held->world.rot.y = held->shape.rot.y =
            (s16)( player->actor.shape.rot.y + player->unk_3BC.y );
    }
}

void liboot_link_update( PlayState *play, Player *player )
{
    AnimTaskQueue_Reset( &play->animTaskQueue );

    if( player->actor.update )
        player->actor.update( &player->actor, play );

    /* liboot v0.3: non-player actors — the per-actor bookkeeping the real
       Actor_UpdateAll does before calling each update (the attention scan
       reads these fields on the following lines). liboot v0.6: `next` is
       saved before the update runs (updates may unlink/spawn), and
       Actor_Kill'd actors (update==NULL) are reaped through the real
       Actor_Delete, mirroring the deletion pass in Actor_UpdateAll —
       without it zombies accumulate and Player_UseItem's "3 explosives"
       cap blocks bombs forever. */
    for( int cat = 0; cat < ACTORCAT_MAX; cat++ ) {
        if( cat == ACTORCAT_PLAYER ) continue;
        Actor *a = play->actorCtx.actorLists[cat].head;
        while( a != NULL ) {
            Actor *next = a->next;
            if( a->update == NULL ) {
                Actor_Delete( &play->actorCtx, a, play );
                a = next;
                continue;
            }
            a->prevPos = a->world.pos;
            a->xzDistToPlayer = Actor_WorldDistXZToActor( a, &player->actor );
            a->yDistToPlayer = Actor_HeightDiff( a, &player->actor );
            a->xyzDistToPlayerSq = SQ( a->xzDistToPlayer ) + SQ( a->yDistToPlayer );
            a->yawTowardsPlayer = Actor_WorldYawTowardActor( a, &player->actor );
            a->isLockedOn = ( a == player->focusActor );
            if(( a->attentionPriority != 0 ) && ( player->focusActor == NULL ))
                a->attentionPriority = 0;
            a->update( a, play );
            a = next;
        }
    }

    /* liboot v0.3: tail of Actor_UpdateAll (z_actor.c:2525-2539) — reticle
       release + Attention_Update. Runs after Player_Update so next tick's
       Player_UpdateZTargeting consumes this frame's scan, like the real
       game loop. */
    {
        Actor *focus = player->focusActor;

        if(( focus != NULL ) && ( focus->update == NULL )) {
            focus = NULL;
            Player_ReleaseLockOn( player );
        }
        if(( focus == NULL ) || ( player->zTargetActiveTimer < 5 )) {
            if( play->actorCtx.attention.reticleSpinCounter != 0 ) {
                play->actorCtx.attention.reticleSpinCounter = 0;
                Sfx_PlaySfxCentered( NA_SE_SY_LOCK_OFF );
            }
            focus = NULL;
        }
        Attention_Update( &play->actorCtx.attention, player, focus, play );
    }

    AnimTaskQueue_Update( play, &play->animTaskQueue );

    /* liboot v0.4: publish this frame's posed joint positions the way the
       real draw path would; Navi (and anything else reading bodyPartsPos)
       consumes them next tick, matching the game's update->draw cadence. */
    liboot_fill_body_parts( player );

    /* liboot v0.6: held-projectile attachments the draw pass would set */
    liboot_replicate_draw_attachments( play, player );

    if( getenv( "LIBOOT_TRACE" ))
        {
        LinkAnimationHeader *ah = (LinkAnimationHeader*)player->skelAnime.animation;
        fprintf( stderr, "[L] joint1=(%d,%d,%d) joint5=(%d,%d,%d) frame=%.1f\n",
                 player->skelAnime.jointTable[1].x, player->skelAnime.jointTable[1].y, player->skelAnime.jointTable[1].z,
                 player->skelAnime.jointTable[5].x, player->skelAnime.jointTable[5].y, player->skelAnime.jointTable[5].z,
                 player->skelAnime.curFrame );
    }

    play->gameplayFrames++;
    play->state.frames++;
}

/* ---- Skeleton pose export (playground/renderer support) ----------------- */

static struct OoTSkeletonPose *sPoseOut;
static u8 sPoseIndex;
static u8 sPoseLimbCount;
static u8 sPoseWalkCount;
static u32 sPoseVisited;
static bool sPoseInvalid;

/* joints are stored at their limb-table index (PLAYER_LIMB_* - 1), so callers
   can attach things to semantically known joints (hands, head, ...) */
static void pose_walk_limb( void **skeleton, Vec3s *jointTable, u8 limbIndex,
                            u8 parentIndex, u8 depth )
{
    if( sPoseInvalid || limbIndex >= sPoseLimbCount || depth >= sPoseLimbCount ||
        sPoseWalkCount >= sPoseLimbCount ||
        ( sPoseVisited & ( 1u << limbIndex )) != 0 || skeleton[limbIndex] == NULL ) {
        sPoseInvalid = true;
        return;
    }
    sPoseVisited |= 1u << limbIndex;
    sPoseWalkCount++;

    LodLimb *limb = skeleton[limbIndex];
    Vec3f pos = { limb->jointPos.x, limb->jointPos.y, limb->jointPos.z };
    Vec3s rot = jointTable[limbIndex + 1];

    if( limbIndex == 0 ) {
        pos.x = jointTable[0].x;
        pos.y = jointTable[0].y;
        pos.z = jointTable[0].z;
    }

    Matrix_Push();
    Matrix_TranslateRotateZYX( &pos, &rot );

    if( limbIndex < OOT_SKELETON_MAX_JOINTS ) {
        Vec3f zero = { 0, 0, 0 }, world;
        Matrix_MultVec3f( &zero, &world );
        sPoseOut->jointPos[limbIndex][0] = world.x;
        sPoseOut->jointPos[limbIndex][1] = world.y;
        sPoseOut->jointPos[limbIndex][2] = world.z;
        sPoseOut->parent[limbIndex] = parentIndex;
        sPoseIndex++;
    }

    if( limb->child != 0xFF )
        pose_walk_limb( skeleton, jointTable, limb->child, limbIndex, (u8)( depth + 1 ));

    Matrix_Pop();

    if( limb->sibling != 0xFF )
        pose_walk_limb( skeleton, jointTable, limb->sibling, parentIndex, (u8)( depth + 1 ));
}

bool oot_link_get_skeleton_impl( Player *player, struct OoTSkeletonPose *out )
{
    if( !out ) return false;
    memset( out, 0, sizeof( *out ));
    if( !player ) return false;
    SkelAnime *sk = &player->skelAnime;
    void **skeleton = sk->skeleton;
    if( !skeleton || !sk->jointTable || sk->limbCount != LINK_LIMB_COUNT + 1 ) return false;

    Actor *actor = &player->actor;
    Matrix_SetTranslateRotateYXZ( actor->world.pos.x,
                                  actor->world.pos.y + actor->shape.yOffset * actor->scale.y,
                                  actor->world.pos.z, &actor->shape.rot );
    Matrix_Scale( actor->scale.x, actor->scale.y, actor->scale.z, MTXMODE_APPLY );

    sPoseOut = out;
    sPoseIndex = 0;
    sPoseLimbCount = LINK_LIMB_COUNT;
    sPoseWalkCount = 0;
    sPoseVisited = 0;
    sPoseInvalid = false;
    pose_walk_limb( skeleton, sk->jointTable, 0, 0xFF, 0 );
    if( sPoseInvalid || sPoseWalkCount != sPoseLimbCount ) {
        memset( out, 0, sizeof( *out ));
        return false;
    }
    out->numJoints = sPoseIndex;
    return sPoseIndex == LINK_LIMB_COUNT;
}
