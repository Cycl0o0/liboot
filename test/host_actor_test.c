/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "liboot.h"

static int expect(const char *label, int value)
{
    if (!value) fprintf(stderr, "host actor: FAIL: %s\n", label);
    return value;
}

int main(void)
{
    struct OoTHostActorState state;
    struct OoTHostActorState snapshot;
    struct OoTHostActorContact contact;
    int ok = 1;

    memset(&state, 0, sizeof(state));
    state.structSize = sizeof(state);
    state.version = OOT_HOST_ACTOR_STATE_VERSION;
    state.flags = OOT_HOST_ACTOR_ENABLED | OOT_HOST_ACTOR_TARGETABLE;
    state.room = -1;
    state.attentionRange = 3u;
    ok &= expect("requires live Link", oot_host_actor_create(&state) == -1);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.structSize = sizeof(snapshot);
    snapshot.version = OOT_HOST_ACTOR_STATE_VERSION;
    ok &= expect("invalid id get", !oot_host_actor_get(-1, &snapshot));
    ok &= expect("invalid id update", !oot_host_actor_update(-1, &state));
    ok &= expect("invalid id remove", !oot_host_actor_remove(-1));

    memset(&contact, 0, sizeof(contact));
    contact.structSize = sizeof(contact);
    contact.version = OOT_HOST_ACTOR_CONTACT_VERSION;
    ok &= expect("empty contact queue", !oot_host_actor_poll_contact(&contact));

    state.structSize = sizeof(state) - 1u;
    ok &= expect("short state", oot_host_actor_create(&state) == -1);
    state.structSize = sizeof(state);
    state.version++;
    ok &= expect("state version", oot_host_actor_create(&state) == -1);
    ok &= expect("host capacity constant", OOT_HOST_ACTOR_MAX == 64u);
    ok &= expect("state ABI", sizeof(state) == 72u);
    ok &= expect("contact ABI", sizeof(contact) == 48u);

    if (!ok) return 1;
    puts("host actor: PASS");
    return 0;
}
