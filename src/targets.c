/* Host-controlled actor bridge.
 *
 * A host actor is deliberately smaller in scope than a retail actor overlay:
 * it is a native Actor list entry used by Attention/Navi, while the host owns
 * behavior, health and rendering.  The original oot_target_* API is retained
 * as a compatibility facade over the same pool.
 */
#include "liboot.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "ultra64.h"
#include "actor.h"
#include "player.h"
#include "play_state.h"
#include "sys_math.h"
#include "z_lib.h"
#include "overlays/actors/ovl_Arms_Hook/z_arms_hook.h"
#include "overlays/actors/ovl_En_Arrow/z_en_arrow.h"
#include "overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "overlays/actors/ovl_En_Boom/z_en_boom.h"

extern PlayState *liboot_play( void );
extern void Actor_Init( Actor *actor, PlayState *play );

#define LIBOOT_CONTACT_QUEUE_CAPACITY 256u
#define LIBOOT_HOST_ACTOR_KNOWN_FLAGS \
    ( OOT_HOST_ACTOR_ENABLED | OOT_HOST_ACTOR_TARGETABLE | \
      OOT_HOST_ACTOR_HOSTILE | OOT_HOST_ACTOR_HURT )

typedef struct {
    Actor actor;                    /* first: pool ownership uses Actor * */
    struct OoTHostActorState state;
    uint8_t contactOverlapMask;
    bool inUse;
    bool legacyTarget;
} LibootHostActor;

static LibootHostActor sActors[OOT_HOST_ACTOR_MAX];
static struct OoTHostActorContact sContacts[LIBOOT_CONTACT_QUEUE_CAPACITY];
static uint32_t sContactHead;
static uint32_t sContactCount;

static bool actor_active_in_room( const LibootHostActor *slot, const PlayState *play );

static Player *runtime_player( PlayState *play )
{
    Actor *actor;
    if( play == NULL ) return NULL;
    actor = play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
    if( actor == NULL || actor->id != ACTOR_PLAYER ) return NULL;
    return (Player *)actor;
}

static void LibootHostActor_Noop( Actor *actor, PlayState *play )
{
    LibootHostActor *slot = (LibootHostActor *)actor;
    actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    if( actor_active_in_room( slot, play ) &&
        ( slot->state.flags & OOT_HOST_ACTOR_TARGETABLE ) != 0u )
        actor->flags |= ACTOR_FLAG_ATTENTION_ENABLED;
}

static int actor_index( const LibootHostActor *slot )
{
    return (int)( slot - &sActors[0] );
}

static LibootHostActor *actor_slot( int32_t actorId )
{
    if( actorId < 0 || (uint32_t)actorId >= OOT_HOST_ACTOR_MAX ||
        !sActors[actorId].inUse )
        return NULL;
    return &sActors[actorId];
}

static bool finite3( const float v[3] )
{
    return v != NULL && isfinite( v[0] ) && isfinite( v[1] ) && isfinite( v[2] );
}

static bool state_valid( const struct OoTHostActorState *state )
{
    return state != NULL && state->structSize >= sizeof( *state ) &&
           state->version == OOT_HOST_ACTOR_STATE_VERSION &&
           ( state->flags & ~LIBOOT_HOST_ACTOR_KNOWN_FLAGS ) == 0u &&
           finite3( state->position ) && finite3( state->focusOffset ) &&
           isfinite( state->hurtRadius ) && state->hurtRadius >= 0.0f &&
           isfinite( state->hurtHeight ) && state->hurtHeight >= 0.0f &&
           isfinite( state->hurtYOffset ) &&
           state->room >= -1 && state->room <= INT8_MAX &&
           state->attentionRange < ATTENTION_RANGE_MAX;
}

static bool actor_is_referenced( PlayState *play, Actor *actor )
{
    Player *player = runtime_player( play );
    Attention *attention = &play->actorCtx.attention;
    return ( player != NULL && player->focusActor == actor ) ||
           attention->naviHoverActor == actor || attention->arrowHoverActor == actor ||
           attention->reticleActor == actor || attention->forcedLockOnActor == actor ||
           attention->bgmEnemy == actor;
}

static void unlink_actor( PlayState *play, Actor *actor )
{
    ActorListEntry *list = &play->actorCtx.actorLists[actor->category];
    if( actor->prev != NULL ) actor->prev->next = actor->next;
    else if( list->head == actor ) list->head = actor->next;
    if( actor->next != NULL ) actor->next->prev = actor->prev;
    actor->prev = actor->next = NULL;
    if( list->length > 0 ) list->length--;
}

static void link_actor( PlayState *play, Actor *actor, uint8_t category )
{
    ActorListEntry *list = &play->actorCtx.actorLists[category];
    actor->category = category;
    actor->prev = NULL;
    actor->next = list->head;
    if( list->head != NULL ) list->head->prev = actor;
    list->head = actor;
    list->length++;
}

static bool actor_active_in_room( const LibootHostActor *slot, const PlayState *play )
{
    return ( slot->state.flags & OOT_HOST_ACTOR_ENABLED ) != 0u &&
           ( slot->state.room < 0 || slot->state.room == play->roomCtx.curRoom.num );
}

static void apply_state( LibootHostActor *slot, PlayState *play,
                         const struct OoTHostActorState *state )
{
    Actor *actor = &slot->actor;
    uint8_t category = ( state->flags & OOT_HOST_ACTOR_HOSTILE ) != 0u
                           ? ACTORCAT_ENEMY : ACTORCAT_NPC;

    if( slot->inUse && actor->category != category ) {
        unlink_actor( play, actor );
        link_actor( play, actor, category );
    } else {
        actor->category = category;
    }

    slot->state = *state;
    slot->state.structSize = sizeof( slot->state );
    slot->state.version = OOT_HOST_ACTOR_STATE_VERSION;

    actor->world.pos.x = actor->home.pos.x = actor->prevPos.x = state->position[0];
    actor->world.pos.y = actor->home.pos.y = actor->prevPos.y = state->position[1];
    actor->world.pos.z = actor->home.pos.z = actor->prevPos.z = state->position[2];
    actor->world.rot.x = actor->home.rot.x = actor->shape.rot.x = state->rotation[0];
    actor->world.rot.y = actor->home.rot.y = actor->shape.rot.y = state->rotation[1];
    actor->world.rot.z = actor->home.rot.z = actor->shape.rot.z = state->rotation[2];
    actor->focus.pos.x = state->position[0] + state->focusOffset[0];
    actor->focus.pos.y = state->position[1] + state->focusOffset[1];
    actor->focus.pos.z = state->position[2] + state->focusOffset[2];
    actor->focus.rot = actor->world.rot;
    actor->attentionRangeType = state->attentionRange;
    actor->room = -1; /* host room filtering is non-destructive */
    actor->flags &= ~( ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE |
                       ACTOR_FLAG_FRIENDLY );
    if( actor_active_in_room( slot, play ) &&
        ( state->flags & OOT_HOST_ACTOR_TARGETABLE ) != 0u )
        actor->flags |= ACTOR_FLAG_ATTENTION_ENABLED;
    actor->flags |= ( state->flags & OOT_HOST_ACTOR_HOSTILE ) != 0u
                        ? ACTOR_FLAG_HOSTILE : ACTOR_FLAG_FRIENDLY;
    actor->update = LibootHostActor_Noop;
}

static void purge_contacts( int actorId )
{
    struct OoTHostActorContact retained[LIBOOT_CONTACT_QUEUE_CAPACITY];
    uint32_t retainedCount = 0u;
    for( uint32_t i = 0u; i < sContactCount; ++i ) {
        uint32_t at = ( sContactHead + i ) % LIBOOT_CONTACT_QUEUE_CAPACITY;
        if( sContacts[at].actorId != actorId )
            retained[retainedCount++] = sContacts[at];
    }
    if( retainedCount != 0u )
        memcpy( sContacts, retained, retainedCount * sizeof( retained[0] ));
    sContactHead = 0u;
    sContactCount = retainedCount;
}

static void queue_contact( LibootHostActor *slot, uint32_t source,
                           uint32_t sourceActorId, uint32_t gameplayFrame,
                           const Vec3f *position )
{
    uint32_t tail;
    struct OoTHostActorContact *event;
    if( sContactCount == LIBOOT_CONTACT_QUEUE_CAPACITY ) {
        sContactHead = ( sContactHead + 1u ) % LIBOOT_CONTACT_QUEUE_CAPACITY;
        sContactCount--;
    }
    tail = ( sContactHead + sContactCount ) % LIBOOT_CONTACT_QUEUE_CAPACITY;
    event = &sContacts[tail];
    memset( event, 0, sizeof( *event ));
    event->structSize = sizeof( *event );
    event->version = OOT_HOST_ACTOR_CONTACT_VERSION;
    event->actorId = actor_index( slot );
    event->source = source;
    event->userTag = slot->state.userTag;
    event->sourceActorId = sourceActorId;
    event->gameplayFrame = gameplayFrame;
    event->position[0] = position->x;
    event->position[1] = position->y;
    event->position[2] = position->z;
    sContactCount++;
}

bool liboot_target_owns( Actor *actor )
{
    uintptr_t address = (uintptr_t)actor;
    uintptr_t begin = (uintptr_t)&sActors[0];
    uintptr_t end = (uintptr_t)&sActors[OOT_HOST_ACTOR_MAX];
    return address >= begin && address < end &&
           ( address - begin ) % sizeof( LibootHostActor ) == 0u;
}

void liboot_target_release( Actor *actor )
{
    LibootHostActor *slot = (LibootHostActor *)actor;
    int index = actor_index( slot );
    actor->update = NULL;
    slot->inUse = false;
    slot->legacyTarget = false;
    slot->contactOverlapMask = 0u;
    purge_contacts( index );
}

int32_t oot_host_actor_create( const struct OoTHostActorState *state )
{
    PlayState *play = liboot_play();
    if( !state_valid( state ) || runtime_player( play ) == NULL ) return -1;

    for( int32_t i = 0; (uint32_t)i < OOT_HOST_ACTOR_MAX; ++i ) {
        LibootHostActor *slot = &sActors[i];
        if( slot->inUse || actor_is_referenced( play, &slot->actor )) continue;

        purge_contacts( i );
        memset( slot, 0, sizeof( *slot ));
        Actor *actor = &slot->actor;
        actor->id = ACTOR_EN_TEST; /* neutral compiled profile identity */
        actor->init = LibootHostActor_Noop;
        actor->update = LibootHostActor_Noop;
        actor->home.pos.x = state->position[0];
        actor->home.pos.y = state->position[1];
        actor->home.pos.z = state->position[2];
        actor->home.rot.x = state->rotation[0];
        actor->home.rot.y = state->rotation[1];
        actor->home.rot.z = state->rotation[2];
        Actor_Init( actor, play );
        apply_state( slot, play, state );
        slot->inUse = true;
        link_actor( play, actor, actor->category );
        return i;
    }
    return -1;
}

bool oot_host_actor_update( int32_t actorId, const struct OoTHostActorState *state )
{
    LibootHostActor *slot = actor_slot( actorId );
    if( slot == NULL || !state_valid( state )) return false;
    apply_state( slot, liboot_play(), state );
    if(( state->flags & OOT_HOST_ACTOR_HURT ) == 0u )
        slot->contactOverlapMask = 0u;
    return true;
}

bool oot_host_actor_get( int32_t actorId, struct OoTHostActorState *outState )
{
    LibootHostActor *slot = actor_slot( actorId );
    if( slot == NULL || outState == NULL || outState->structSize < sizeof( *outState ) ||
        outState->version != OOT_HOST_ACTOR_STATE_VERSION )
        return false;
    *outState = slot->state;
    outState->structSize = sizeof( *outState );
    outState->version = OOT_HOST_ACTOR_STATE_VERSION;
    return true;
}

bool oot_host_actor_remove( int32_t actorId )
{
    LibootHostActor *slot = actor_slot( actorId );
    if( slot == NULL ) return false;
    Actor *actor = &slot->actor;
    unlink_actor( liboot_play(), actor );
    actor->update = NULL;
    slot->inUse = false;
    slot->legacyTarget = false;
    slot->contactOverlapMask = 0u;
    purge_contacts( actorId );
    return true;
}

void oot_host_actor_clear( void )
{
    for( int32_t i = 0; (uint32_t)i < OOT_HOST_ACTOR_MAX; ++i )
        if( sActors[i].inUse ) (void)oot_host_actor_remove( i );
    sContactHead = sContactCount = 0u;
}

/* Re-evaluate non-destructive host room membership synchronously when a host
   world or ROM room is committed. This prevents an actor from remaining
   targetable for the one frame between the room swap and its next update. */
void liboot_host_actor_sync_room( void )
{
    PlayState *play = liboot_play();
    if( play == NULL ) return;
    for( uint32_t i = 0u; i < OOT_HOST_ACTOR_MAX; ++i ) {
        LibootHostActor *slot = &sActors[i];
        if( !slot->inUse ) continue;
        LibootHostActor_Noop( &slot->actor, play );
        if( !actor_active_in_room( slot, play )) slot->contactOverlapMask = 0u;
    }
}

bool oot_host_actor_poll_contact( struct OoTHostActorContact *outContact )
{
    if( outContact == NULL || outContact->structSize < sizeof( *outContact ) ||
        outContact->version != OOT_HOST_ACTOR_CONTACT_VERSION || sContactCount == 0u )
        return false;
    *outContact = sContacts[sContactHead];
    sContactHead = ( sContactHead + 1u ) % LIBOOT_CONTACT_QUEUE_CAPACITY;
    sContactCount--;
    return true;
}

/* Compatibility target API. Targets are enabled, hostile, targetable host
   actors without a hurt volume; legacy ids remain safe no-op inputs. */
int32_t oot_target_create( float x, float y, float z, float radius )
{
    struct OoTHostActorState state;
    int32_t actorId;
    uint32_t targetCount = 0u;
    for( uint32_t i = 0u; i < OOT_HOST_ACTOR_MAX; ++i )
        if( sActors[i].inUse && sActors[i].legacyTarget ) targetCount++;
    if( targetCount >= 16u ) return -1;

    memset( &state, 0, sizeof( state ));
    state.structSize = sizeof( state );
    state.version = OOT_HOST_ACTOR_STATE_VERSION;
    state.flags = OOT_HOST_ACTOR_ENABLED | OOT_HOST_ACTOR_TARGETABLE |
                  OOT_HOST_ACTOR_HOSTILE;
    state.position[0] = x;
    state.position[1] = y;
    state.position[2] = z;
    state.focusOffset[1] = radius;
    state.room = -1;
    state.attentionRange = ATTENTION_RANGE_3;
    actorId = oot_host_actor_create( &state );
    if( actorId >= 0 ) sActors[actorId].legacyTarget = true;
    return actorId;
}

void oot_target_move( int32_t targetId, float x, float y, float z )
{
    LibootHostActor *slot = actor_slot( targetId );
    if( slot == NULL || !slot->legacyTarget ) return;
    struct OoTHostActorState state = slot->state;
    state.position[0] = x;
    state.position[1] = y;
    state.position[2] = z;
    (void)oot_host_actor_update( targetId, &state );
}

void oot_target_remove( int32_t targetId )
{
    LibootHostActor *slot = actor_slot( targetId );
    if( slot != NULL && slot->legacyTarget ) (void)oot_host_actor_remove( targetId );
}

static bool segment_hits_hurt( const LibootHostActor *slot, const Vec3f *a,
                               const Vec3f *b, Vec3f *outPosition )
{
    const struct OoTHostActorState *state = &slot->state;
    float low = state->position[1] + state->hurtYOffset;
    float high = low + state->hurtHeight;
    float t0 = 0.0f, t1 = 1.0f;
    float dy = b->y - a->y;
    float t;
    float dx, dz, denom;

    if( state->hurtRadius <= 0.0f || state->hurtHeight <= 0.0f ) return false;
    if( fabsf( dy ) < 0.000001f ) {
        if( a->y < low || a->y > high ) return false;
    } else {
        float enter = ( low - a->y ) / dy;
        float leave = ( high - a->y ) / dy;
        if( enter > leave ) { float swap = enter; enter = leave; leave = swap; }
        if( enter > t0 ) t0 = enter;
        if( leave < t1 ) t1 = leave;
        if( t0 > t1 || t1 < 0.0f || t0 > 1.0f ) return false;
        if( t0 < 0.0f ) t0 = 0.0f;
        if( t1 > 1.0f ) t1 = 1.0f;
    }

    dx = b->x - a->x;
    dz = b->z - a->z;
    denom = dx * dx + dz * dz;
    if( denom > 0.000001f )
        t = (( state->position[0] - a->x ) * dx +
             ( state->position[2] - a->z ) * dz ) / denom;
    else
        t = t0;
    if( t < t0 ) t = t0;
    if( t > t1 ) t = t1;

    outPosition->x = a->x + dx * t;
    outPosition->y = a->y + dy * t;
    outPosition->z = a->z + dz * t;
    dx = outPosition->x - state->position[0];
    dz = outPosition->z - state->position[2];
    return dx * dx + dz * dz <= state->hurtRadius * state->hurtRadius;
}

static bool point_hits_hurt( const LibootHostActor *slot, const Vec3f *point,
                             float extraRadius )
{
    const struct OoTHostActorState *state = &slot->state;
    float low = state->position[1] + state->hurtYOffset;
    float high = low + state->hurtHeight;
    float closestY = point->y < low ? low : ( point->y > high ? high : point->y );
    float dx = point->x - state->position[0];
    float dz = point->z - state->position[2];
    float horizontal = sqrtf( dx * dx + dz * dz ) - state->hurtRadius;
    float vertical = point->y - closestY;
    if( horizontal < 0.0f ) horizontal = 0.0f;
    return horizontal * horizontal + vertical * vertical <= extraRadius * extraRadius;
}

static bool weapon_hits_hurt( const LibootHostActor *slot, const WeaponInfo *weapon,
                              Vec3f *outPosition )
{
    return weapon != NULL && weapon->active &&
           segment_hits_hurt( slot, &weapon->posA, &weapon->posB, outPosition );
}

/* Called after native actors update, when weapon attachment positions are
   current. Contacts are edge-triggered per source class, so one swing or
   projectile overlap emits one event instead of one per simulation tick. */
void liboot_host_actor_contacts_tick( PlayState *play, Player *player )
{
    for( uint32_t i = 0u; i < OOT_HOST_ACTOR_MAX; ++i ) {
        LibootHostActor *slot = &sActors[i];
        uint8_t overlaps = 0u;
        Vec3f contact = slot->actor.focus.pos;

        if( !slot->inUse || !actor_active_in_room( slot, play ) ||
            ( slot->state.flags & OOT_HOST_ACTOR_HURT ) == 0u ) {
            if( slot->inUse ) slot->contactOverlapMask = 0u;
            continue;
        }

        if( player->meleeWeaponState != 0 ) {
            bool meleeHit = false;
            for( int weapon = 0; weapon < 3; ++weapon ) {
                if( weapon_hits_hurt( slot, &player->meleeWeaponInfo[weapon], &contact )) {
                    meleeHit = true;
                    break;
                }
            }
            /* On a host renderer the draw callback that normally seeds swept
               weapon segments may be unavailable for the first active frame.
               Fall back to a short facing segment rather than dropping the
               gameplay contact entirely. */
            if( !meleeHit ) {
                Vec3f base = player->actor.world.pos;
                Vec3f tip = base;
                base.y += 25.0f;
                tip.y += 45.0f;
                float yaw = player->actor.shape.rot.y * ( 3.14159265358979323846f / 32768.0f );
                tip.x += sinf( yaw ) * 90.0f;
                tip.z += cosf( yaw ) * 90.0f;
                meleeHit = segment_hits_hurt( slot, &base, &tip, &contact );
            }
            if( meleeHit ) {
                overlaps |= 1u << 0;
                if(( slot->contactOverlapMask & ( 1u << 0 )) == 0u )
                    queue_contact( slot, OOT_HOST_CONTACT_MELEE, ACTOR_PLAYER,
                                   play->gameplayFrames, &contact );
            }
        }

        for( int category = 0; category < ACTORCAT_MAX; ++category ) {
            for( Actor *actor = play->actorCtx.actorLists[category].head;
                 actor != NULL; actor = actor->next ) {
                uint8_t bit = 0u;
                uint32_t source = 0u;
                bool hit = false;
                if( actor->id == ACTOR_EN_ARROW ) {
                    source = OOT_HOST_CONTACT_ARROW; bit = 1u << 1;
                    hit = weapon_hits_hurt( slot, &((EnArrow *)actor)->weaponInfo, &contact );
                } else if( actor->id == ACTOR_EN_BOOM ) {
                    source = OOT_HOST_CONTACT_BOOMERANG; bit = 1u << 2;
                    hit = weapon_hits_hurt( slot, &((EnBoom *)actor)->weaponInfo, &contact );
                } else if( actor->id == ACTOR_ARMS_HOOK ) {
                    source = OOT_HOST_CONTACT_HOOKSHOT; bit = 1u << 3;
                    hit = weapon_hits_hurt( slot, &((ArmsHook *)actor)->weaponInfo, &contact );
                } else if( actor->id == ACTOR_EN_BOM && actor->params == BOMB_EXPLOSION ) {
                    EnBom *bomb = (EnBom *)actor;
                    float radius = bomb->explosionCollider.elements != NULL
                                       ? bomb->explosionCollider.elements[0].dim.worldSphere.radius
                                       : 0.0f;
                    source = OOT_HOST_CONTACT_BOMB; bit = 1u << 4;
                    contact = actor->world.pos;
                    hit = radius > 0.0f && point_hits_hurt( slot, &contact, radius );
                }
                if( hit ) {
                    if(( overlaps & bit ) == 0u && ( slot->contactOverlapMask & bit ) == 0u )
                        queue_contact( slot, source, (uint32_t)(uint16_t)actor->id,
                                       play->gameplayFrames, &contact );
                    overlaps |= bit;
                }
            }
        }
        slot->contactOverlapMask = overlaps;
    }
}
