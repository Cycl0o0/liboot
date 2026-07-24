/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 *
 * ROM-free contract test for the proximity-driven enemy/battle BGM API.
 * The config and query functions touch only process-local state, so they are
 * exercisable without a ROM. The audible play/stop path is driven by OoT's own
 * Audio_SetBgmEnemyVolume call site inside oot_link_tick and is confirmed live
 * in the playground combat arena with a real ROM.
 */
#include <stdbool.h>
#include <stdio.h>

#include "liboot.h"

static int failures = 0;

static void check(const char *label, int condition)
{
    if (!condition) {
        fprintf(stderr, "enemy bgm: FAIL: %s\n", label);
        ++failures;
    }
}

int main(void)
{
    bool active = true;
    float dist = -1.0f;

    /* Default: feature disabled, nothing active, distance at max battle range. */
    check("default disabled", oot_audio_get_enemy_bgm(&active, &dist) == false);
    check("default inactive", active == false);
    check("default distance is range", dist > 400.0f && dist <= 500.0f);

    /* Enable with defaults (0xFF player -> SUB, 0 seqId -> NA_BGM_ENEMY). */
    check("enable ok", oot_audio_set_enemy_bgm(true, 0xFF, 0, 400) == true);
    check("enabled reported", oot_audio_get_enemy_bgm(NULL, NULL) == true);
    /* No tick has run, so nothing is playing yet. */
    oot_audio_get_enemy_bgm(&active, NULL);
    check("still inactive before any tick", active == false);

    /* Validation: player out of range and sequence out of range are rejected. */
    check("reject bad player", oot_audio_set_enemy_bgm(true, 250, 0, 400) == false);
    check("reject bad seqId", oot_audio_set_enemy_bgm(true, 0, 60000, 400) == false);

    /* Explicit valid config: SFX player carrying NA_BGM_ENEMY. */
    check("explicit config ok", oot_audio_set_enemy_bgm(true, 2, 0x1A, 500) == true);

    /* NULL out pointers must be tolerated. */
    (void)oot_audio_get_enemy_bgm(NULL, NULL);

    /* Disable turns the feature back off. */
    check("disable ok", oot_audio_set_enemy_bgm(false, 0xFF, 0, 0) == true);
    check("disabled reported", oot_audio_get_enemy_bgm(NULL, NULL) == false);

    if (failures == 0) {
        printf("enemy bgm: PASS\n");
        return 0;
    }
    fprintf(stderr, "enemy bgm: %d check(s) failed\n", failures);
    return 1;
}
