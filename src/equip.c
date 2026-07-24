#include "liboot.h"

#include "shim/fake_play.h"

#include "inventory.h"
#include "item.h"
#include "ocarina.h"
#include "player.h"
#include "save.h"
#include "z_actor_dlftbls.h"

/* These Player entry points are global in z_player.c but are not part of the
 * subset exposed by the vendored player.h. */
extern void Player_UseItem(PlayState* play, Player* player, s32 item);
extern s8 Player_ItemToItemAction(s32 item);
extern void Player_InitItemActionWithAnim(PlayState* play, Player* player, s8 itemAction);
extern void func_80834644(PlayState* play, Player* player);

/* Minimal ocarina lifecycle supplied by the standalone message shim. */
extern void liboot_message_prepare_ocarina(PlayState* play);
extern void liboot_message_stop_ocarina(PlayState* play);

static Player* liboot_get_link(int32_t linkId, PlayState** outPlay) {
    PlayState* play;
    Player* player;

    if (linkId != 0) {
        return NULL;
    }

    play = liboot_play();
    player = GET_PLAYER(play);
    if (player == NULL) {
        return NULL;
    }

    if (outPlay != NULL) {
        *outPlay = play;
    }
    return player;
}

static u8 liboot_clamp_range(u8 value, u8 min, u8 max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static u8 liboot_clamp_sword(u8 sword) {
    if (sword == OOT_SWORD_NONE) {
        return OOT_SWORD_NONE;
    }
    if (LINK_IS_CHILD) {
        return OOT_SWORD_KOKIRI;
    }
    return liboot_clamp_range(sword, OOT_SWORD_MASTER, OOT_SWORD_BIGGORON);
}

static u8 liboot_clamp_shield(u8 shield) {
    if (shield == OOT_SHIELD_NONE) {
        return OOT_SHIELD_NONE;
    }
    if (LINK_IS_CHILD) {
        return liboot_clamp_range(shield, OOT_SHIELD_DEKU, OOT_SHIELD_HYLIAN);
    }
    return liboot_clamp_range(shield, OOT_SHIELD_HYLIAN, OOT_SHIELD_MIRROR);
}

static void liboot_set_equip_value(s32 type, u8 value) {
    gSaveContext.save.info.equips.equipment =
        (gSaveContext.save.info.equips.equipment & ~gEquipMasks[type]) |
        ((u16)value << gEquipShifts[type]);
}

static void liboot_give_equipment(s32 type, u8 inventoryValue) {
    gSaveContext.save.info.inventory.equipment |=
        (u16)(gBitFlags[inventoryValue] << gEquipShifts[type]);
}

void oot_link_set_equipment(int32_t linkId, uint8_t sword, uint8_t shield, uint8_t tunic, uint8_t boots) {
    PlayState* play;
    Player* player = liboot_get_link(linkId, &play);
    u8 swordItem;

    if (player == NULL) {
        return;
    }

    sword = liboot_clamp_sword(sword);
    shield = liboot_clamp_shield(shield);
    if (LINK_IS_CHILD) {
        tunic = OOT_TUNIC_KOKIRI;
        boots = OOT_BOOTS_KOKIRI;
    } else {
        tunic = liboot_clamp_range(tunic, OOT_TUNIC_KOKIRI, OOT_TUNIC_ZORA);
        boots = liboot_clamp_range(boots, OOT_BOOTS_KOKIRI, OOT_BOOTS_HOVER);
    }

    liboot_set_equip_value(EQUIP_TYPE_SWORD, sword);
    liboot_set_equip_value(EQUIP_TYPE_SHIELD, shield);
    liboot_set_equip_value(EQUIP_TYPE_TUNIC, tunic + EQUIP_VALUE_TUNIC_KOKIRI);
    liboot_set_equip_value(EQUIP_TYPE_BOOTS, boots + EQUIP_VALUE_BOOTS_KOKIRI);

    if (sword != OOT_SWORD_NONE) {
        liboot_give_equipment(EQUIP_TYPE_SWORD, sword - OOT_SWORD_KOKIRI);
    }
    if (shield != OOT_SHIELD_NONE) {
        liboot_give_equipment(EQUIP_TYPE_SHIELD, shield - OOT_SHIELD_DEKU);
    }
    liboot_give_equipment(EQUIP_TYPE_TUNIC, tunic);
    liboot_give_equipment(EQUIP_TYPE_BOOTS, boots);

    switch (sword) {
        case OOT_SWORD_KOKIRI:
            swordItem = ITEM_SWORD_KOKIRI;
            break;
        case OOT_SWORD_MASTER:
            swordItem = ITEM_SWORD_MASTER;
            break;
        case OOT_SWORD_BIGGORON:
            swordItem = ITEM_SWORD_BIGGORON;
            gSaveContext.save.info.playerData.bgsFlag = true;
            gSaveContext.save.info.playerData.swordHealth = 8;
            break;
        default:
            swordItem = ITEM_NONE;
            break;
    }

    gSaveContext.save.info.equips.buttonItems[0] = swordItem;
    gSaveContext.buttonStatus[0] = BTN_ENABLED;

    Player_SetEquipmentData(play, player);
    /* Player_SetEquipmentData currently calls this too; keep the explicit call
     * because boot register refresh is part of this public API's contract. */
    Player_SetBootData(play, player);
}

static void liboot_set_upgrade(u8 upgrade, u8 value) {
    gSaveContext.save.info.inventory.upgrades =
        (gSaveContext.save.info.inventory.upgrades & ~gUpgradeMasks[upgrade]) |
        ((u32)value << gUpgradeShifts[upgrade]);
}

static void liboot_give_item(u8 item) {
    s32 slot = SLOT(item);

    if ((slot >= 0) && (slot < (s32)sizeof(gSaveContext.save.info.inventory.items))) {
        gSaveContext.save.info.inventory.items[slot] = item;
    }

    switch (item) {
        case ITEM_DEKU_STICK:
            liboot_set_upgrade(UPG_DEKU_STICKS, 1);
            AMMO(ITEM_DEKU_STICK) = 10;
            break;
        case ITEM_BOW:
            liboot_set_upgrade(UPG_QUIVER, 1);
            AMMO(ITEM_BOW) = 30;
            break;
        case ITEM_BOMB:
            liboot_set_upgrade(UPG_BOMB_BAG, 1);
            AMMO(ITEM_BOMB) = 20;
            break;
        default:
            break;
    }
}

static u8 liboot_item_for_age(u8 item) {
    switch (item) {
        case OOT_ITEM_OCARINA:
            return LINK_IS_CHILD ? ITEM_OCARINA_FAIRY : ITEM_OCARINA_OF_TIME;
        case OOT_ITEM_BOTTLE:
            return ITEM_BOTTLE_EMPTY;
        case OOT_ITEM_HAMMER:
            return LINK_IS_ADULT ? ITEM_HAMMER : ITEM_NONE;
        case OOT_ITEM_DEKU_STICK:
            return LINK_IS_CHILD ? ITEM_DEKU_STICK : ITEM_NONE;
        case OOT_ITEM_BOOMERANG:
            return LINK_IS_CHILD ? ITEM_BOOMERANG : ITEM_NONE;
        case OOT_ITEM_BOW:
            return LINK_IS_ADULT ? ITEM_BOW : ITEM_NONE;
        case OOT_ITEM_HOOKSHOT:
            return LINK_IS_ADULT ? ITEM_HOOKSHOT : ITEM_NONE;
        case OOT_ITEM_BOMB:
            return ITEM_BOMB;
        default:
            return ITEM_NONE;
    }
}

void oot_link_use_item(int32_t linkId, uint8_t item) {
    PlayState* play;
    Player* player = liboot_get_link(linkId, &play);
    u8 gameItem;
    s8 itemAction;
    s32 wasUsingOcarina;

    if (player == NULL) {
        return;
    }

    wasUsingOcarina = (player->itemAction == PLAYER_IA_OCARINA_FAIRY) ||
                      (player->itemAction == PLAYER_IA_OCARINA_OF_TIME);

    if (item == OOT_ITEM_NONE) {
        /* v0.6: clear the item button too, so held-item button semantics
         * (Player_ProcessItemButtons via the C-left input) release with it */
        gSaveContext.save.info.equips.buttonItems[1] = ITEM_NONE;
        Player_UseItem(play, player, ITEM_NONE);

        if (wasUsingOcarina) {
            /* Player_UseItem cannot normally interrupt the ocarina cutscene
             * action. Arrange for its existing OCARINA_MODE_04 exit branch to
             * restore the no-item model when the end animation starts. */
            player->heldItemId = ITEM_NONE;
            player->heldItemAction = PLAYER_IA_NONE;
            player->nextModelGroup = Player_ActionToModelGroup(player, PLAYER_IA_NONE);
            liboot_message_stop_ocarina(play);
        }
        return;
    }

    gameItem = liboot_item_for_age(item);
    if (gameItem == ITEM_NONE) {
        gSaveContext.save.info.equips.buttonItems[1] = ITEM_NONE;
        Player_UseItem(play, player, ITEM_NONE);
        return;
    }

    liboot_give_item(gameItem);

    /* liboot v0.6: mirror the item onto C-left (buttonItems[1] is what
     * C_BTN_ITEM(0) reads) so the OoTLinkInputs item button drives the real
     * Player_ProcessItemButtons press/hold/release semantics — bow nock and
     * fire, hookshot trigger, repeat boomerang throws all key off
     * sUseHeldItem/sHeldItemButtonIsHeldDown, which only that path sets. */
    gSaveContext.save.info.equips.buttonItems[1] = gameItem;
    gSaveContext.buttonStatus[1] = BTN_ENABLED;
    if ((gameItem == ITEM_OCARINA_FAIRY) || (gameItem == ITEM_OCARINA_OF_TIME)) {
        liboot_message_prepare_ocarina(play);
    }

    itemAction = Player_ItemToItemAction(gameItem);
    Player_UseItem(play, player, gameItem);

    /* v0.6: the boomerang-throw guard that used to live here is gone — the
     * vendored EnBoom registers a profile, so the auto-use throw after the
     * item change reaches func_808359FC with a real Actor_Spawn result (the
     * path that stores &boomerang->actor before its NULL check). The guard
     * survives ONLY as a fallback for ROMs whose gameplay_keep assets did
     * not bind (profile still NULL => Actor_Spawn returns NULL). */
    if ((gameItem == ITEM_BOOMERANG) &&
        (gActorOverlayTable[ACTOR_EN_BOOM].profile == NULL) &&
        (player->stateFlags1 & PLAYER_STATE1_START_CHANGING_HELD_ITEM)) {
        player->stateFlags1 &= ~PLAYER_STATE1_START_CHANGING_HELD_ITEM;
        player->heldItemId = gameItem;
        player->nextModelGroup = Player_ActionToModelGroup(player, itemAction);
        Player_InitItemActionWithAnim(play, player, itemAction);
        func_80834644(play, player);
    }
}
