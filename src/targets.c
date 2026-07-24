/* liboot v0.3: Z-targeting attention targets.

   Each target is a fake-spawned but otherwise real hostile Actor kept in a
   static pool and linked into play->actorCtx.actorLists[ACTORCAT_ENEMY], so
   the vendored Attention system (Attention_Update / Attention_FindActor /
   Attention_ShouldReleaseLockOn, all compiled from the real z_actor.c) scans
   and locks onto it exactly like an in-game enemy. liboot cuts Actor_Spawn,
   so the spawn-path defaults are applied here by calling Actor_Init directly,
   the same way liboot_link_init fakes the Player spawn. */
#include "liboot.h"

#include <string.h>

#include "ultra64.h"
#include "actor.h"
#include "player.h"
#include "play_state.h"

extern PlayState *liboot_play( void );
extern void Actor_Init( Actor *actor, PlayState *play );

#define LIBOOT_MAX_TARGETS 16

typedef struct {
    Actor actor;
    float focusHeight;
    bool inUse;
} LibootTarget;

static LibootTarget sTargets[LIBOOT_MAX_TARGETS];

/* Pool ownership test for liboot_despawn_actors (load_assets.c): target
   actors are static memory linked into actorLists[ACTORCAT_ENEMY], so the
   despawn walk must not ZeldaArena_Free() them (glibc "free(): invalid
   pointer" abort). Actor is the first member, so the actor pointer and the
   slot pointer coincide; the modulo check rejects interior pointers. */
bool liboot_target_owns( Actor *actor )
{
    LibootTarget *t = (LibootTarget *)actor;
    return t >= &sTargets[0] && t < &sTargets[LIBOOT_MAX_TARGETS] &&
           (size_t)( (char *)actor - (char *)sTargets ) % sizeof( LibootTarget ) == 0;
}

/* Called by liboot_despawn_actors after it unlinks a pool target: give the
   slot back so the pool does not leak across age switches / deletes. */
void liboot_target_release( Actor *actor )
{
    LibootTarget *t = (LibootTarget *)actor;
    t->actor.update = NULL; /* Actor_Kill semantics, matches oot_target_remove */
    t->inUse = false;
}

/* Actor_Init calls init unconditionally (object slot 0 is always "loaded"),
   and a NULL update means dead/untargetable, so both must be real no-ops. */
static void LibootTarget_Noop( Actor *actor, PlayState *play ) { (void)actor; (void)play; }

int32_t oot_target_create( float x, float y, float z, float radius )
{
    PlayState *play = liboot_play();
    Player *player = (Player *)play->actorCtx.actorLists[ACTORCAT_PLAYER].head;

    for( int32_t i = 0; i < LIBOOT_MAX_TARGETS; ++i ) {
        LibootTarget *t = &sTargets[i];
        if( t->inUse ) continue;
        /* a just-removed slot can stay the player's focusActor until the
           release paths run next tick; don't recycle it out from under them */
        if( player != NULL && player->focusActor == &t->actor ) continue;

        Actor *a = &t->actor;
        memset( a, 0, sizeof( *a ));
        a->id = ACTOR_EN_TEST;          /* anything but ACTOR_EN_BOOM */
        a->category = ACTORCAT_ENEMY;
        a->flags = ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE;
        a->init = LibootTarget_Noop;    /* consumed by Actor_Init */
        a->update = LibootTarget_Noop;  /* non-NULL = alive = targetable */
        /* the real Actor_Spawn sets home; Actor_Init copies home -> world */
        a->home.pos.x = a->world.pos.x = x;
        a->home.pos.y = a->world.pos.y = y;
        a->home.pos.z = a->world.pos.z = z;
        Actor_Init( a, play );          /* scale .01, ATTENTION_RANGE_3, focus, colChkInfo */
        Actor_SetFocus( a, radius );    /* lock-on point; keeps the LOS ray off the floor */

        ActorListEntry *list = &play->actorCtx.actorLists[ACTORCAT_ENEMY];
        a->prev = NULL;
        a->next = list->head;
        if( list->head != NULL ) list->head->prev = a;
        list->head = a;
        list->length++;

        t->focusHeight = radius;
        t->inUse = true;
        return i;
    }
    return -1;
}

void oot_target_move( int32_t targetId, float x, float y, float z )
{
    if( targetId < 0 || targetId >= LIBOOT_MAX_TARGETS || !sTargets[targetId].inUse ) return;

    Actor *a = &sTargets[targetId].actor;
    a->world.pos.x = a->home.pos.x = x;
    a->world.pos.y = a->home.pos.y = y;
    a->world.pos.z = a->home.pos.z = z;
    Actor_SetFocus( a, sTargets[targetId].focusHeight );
}

void oot_target_remove( int32_t targetId )
{
    if( targetId < 0 || targetId >= LIBOOT_MAX_TARGETS || !sTargets[targetId].inUse ) return;

    PlayState *play = liboot_play();
    Actor *a = &sTargets[targetId].actor;

    /* Actor_Kill semantics: update == NULL makes both release paths fire on
       the next tick (Actor_UpdateAll-tail check and the real
       Attention_ShouldReleaseLockOn's dead-actor branch). The pool slot stays
       resident so the lingering focusActor pointer remains valid until then. */
    a->update = NULL;

    ActorListEntry *list = &play->actorCtx.actorLists[ACTORCAT_ENEMY];
    if( a->prev != NULL ) a->prev->next = a->next;
    else if( list->head == a ) list->head = a->next;
    if( a->next != NULL ) a->next->prev = a->prev;
    a->prev = a->next = NULL;
    list->length--;

    sTargets[targetId].inUse = false;
}
