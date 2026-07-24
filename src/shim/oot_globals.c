#include "audio.h"
#include "camera.h"
#include "inventory.h"
#include "object.h"
#include "scene.h"
#include "sfx.h"
#include "z_actor_dlftbls.h"

ActorOverlay gActorOverlayTable[ACTOR_ID_MAX];
RomFile gObjectTable[OBJECT_ID_MAX];
EntranceInfo gEntranceTable[ENTR_MAX];

s32 gDebugCamEnabled;

u32 gBitFlags[32] = {
    0x00000001, 0x00000002, 0x00000004, 0x00000008,
    0x00000010, 0x00000020, 0x00000040, 0x00000080,
    0x00000100, 0x00000200, 0x00000400, 0x00000800,
    0x00001000, 0x00002000, 0x00004000, 0x00008000,
    0x00010000, 0x00020000, 0x00040000, 0x00080000,
    0x00100000, 0x00200000, 0x00400000, 0x00800000,
    0x01000000, 0x02000000, 0x04000000, 0x08000000,
    0x10000000, 0x20000000, 0x40000000, 0x80000000,
};

u16 gEquipMasks[EQUIP_TYPE_MAX] = { 0x000F, 0x00F0, 0x0F00, 0xF000 };
u8 gEquipShifts[EQUIP_TYPE_MAX] = { 0, 4, 8, 12 };
u32 gUpgradeMasks[UPG_MAX] = {
    0x00000007, 0x00000038, 0x000001C0, 0x00000E00,
    0x00003000, 0x0001C000, 0x000E0000, 0x00700000,
};
u8 gUpgradeShifts[UPG_MAX] = { 0, 3, 6, 9, 12, 14, 17, 20 };

u8 gItemSlots[56] = {
    0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13,
    14, 15, 16, 17,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
};

Vec3f gSfxDefaultPos = { 0.0f, 0.0f, 0.0f };
f32 gSfxDefaultFreqAndVolScale = 1.0f;
s8 gSfxDefaultReverb = 0;
