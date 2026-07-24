#include "fake_play.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "dma.h"
#include "item.h"
#include "object.h"
#include "player.h"
#include "regs.h"
#include "save.h"
#include "segment_symbols.h"
#include "segmented_address.h"
#include "sys_matrix.h"
#include "ultra64.h"

typedef struct LibootSegmentSpan {
    u8* base;
    size_t size;
} LibootSegmentSpan;

static PlayState sFakePlay;
static Player sBootDataPlayer;
static RegEditor sRegEditor;
static LibootSegmentSpan sSegmentSpans[NUM_SEGMENTS];

s16 liboot_cam_yaw;
s32 liboot_reset_requested;

SaveContext gSaveContext;
RegEditor* gRegEditor = &sRegEditor;
uintptr_t gSegments[NUM_SEGMENTS];

static void Liboot_InitCamera(PlayState* play, Camera* camera, s16 camId) {
    memset(camera, 0, sizeof(*camera));
    camera->play = play;
    camera->camId = camId;
    camera->status = CAM_STAT_ACTIVE;
    camera->setting = CAM_SET_NORMAL0;
    camera->mode = CAM_MODE_NORMAL;
    camera->parentCamId = CAM_ID_NONE;
    camera->childCamId = CAM_ID_NONE;
}

PlayState* liboot_get_fake_play_state(void) {
    return &sFakePlay;
}

PlayState* liboot_play(void) {
    return liboot_get_fake_play_state();
}

/* Backing storage for the GameState TwoHeadArena: BgCheck carves its poly
   lookup tables out of the tail end (8 MiB covers any reasonable user mesh). */
static u8 sThaHeap[8 * 1024 * 1024] __attribute__((aligned(16)));

void liboot_fake_play_reset(void) {
    s32 i;

    memset(&sFakePlay, 0, sizeof(sFakePlay));
    memset(&gSaveContext, 0, sizeof(gSaveContext));
    memset(&sRegEditor, 0, sizeof(sRegEditor));
    memset(&sBootDataPlayer, 0, sizeof(sBootDataPlayer));

    Liboot_InitCamera(&sFakePlay, &sFakePlay.mainCamera, CAM_ID_MAIN);
    sFakePlay.cameraPtrs[CAM_ID_MAIN] = &sFakePlay.mainCamera;
    for (i = CAM_ID_SUB_FIRST; i < NUM_CAMS; i++) {
        Liboot_InitCamera(&sFakePlay, &sFakePlay.subCameras[i - CAM_ID_SUB_FIRST], i);
        sFakePlay.cameraPtrs[i] = &sFakePlay.subCameras[i - CAM_ID_SUB_FIRST];
    }
    sFakePlay.activeCamId = CAM_ID_MAIN;
    sFakePlay.nextCamId = CAM_ID_NONE;

    THA_Init(&sFakePlay.state.tha, sThaHeap, sizeof(sThaHeap));
    Matrix_Init(&sFakePlay.state);

    /* liboot v0.4: slot 1 mirrors segment 4 (gameplay_keep) so real actors
       whose profile wants OBJECT_GAMEPLAY_KEEP (EnElf/Navi) pass the
       Object_GetSlot/Object_IsLoaded checks in Actor_Spawn/Actor_Init. */
    sFakePlay.objectCtx.numEntries = 2;
    sFakePlay.objectCtx.numPersistentEntries = 2;
    sFakePlay.objectCtx.slots[0].id = OBJECT_LINK_BOY;
    sFakePlay.objectCtx.slots[0].segment = sSegmentSpans[6].base;
    sFakePlay.objectCtx.slots[1].id = OBJECT_GAMEPLAY_KEEP;
    sFakePlay.objectCtx.slots[1].segment = sSegmentSpans[4].base;
    sFakePlay.roomCtx.curRoom.num = -1;
    sFakePlay.roomCtx.prevRoom.num = -1;

    gSaveContext.save.entranceIndex = 0;
    gSaveContext.save.linkAge = LINK_AGE_ADULT;
    gSaveContext.save.cutsceneIndex = CS_INDEX_NONE;
    gSaveContext.save.dayTime = 0x8000;
    gSaveContext.save.info.playerData.healthCapacity = 0x30;
    gSaveContext.save.info.playerData.health = 0x30;
    memset(gSaveContext.save.info.inventory.items, ITEM_NONE,
           sizeof(gSaveContext.save.info.inventory.items));
    memset(gSaveContext.save.info.equips.buttonItems, ITEM_NONE,
           sizeof(gSaveContext.save.info.equips.buttonItems));
    memset(gSaveContext.save.info.equips.cButtonSlots, SLOT_NONE,
           sizeof(gSaveContext.save.info.equips.cButtonSlots));
    gSaveContext.save.info.equips.equipment =
        (EQUIP_VALUE_TUNIC_KOKIRI << 8) | (EQUIP_VALUE_BOOTS_KOKIRI << 12);
    gSaveContext.save.info.inventory.equipment = (1 << (8 + EQUIP_INV_TUNIC_KOKIRI)) |
                                                 (1 << (12 + EQUIP_INV_BOOTS_KOKIRI));
    gSaveContext.dogParams = 0;
    gSaveContext.magicState = MAGIC_STATE_IDLE;
    gSaveContext.nextCutsceneIndex = NEXT_CS_INDEX_NONE;
    gSaveContext.language = LANGUAGE_ENG;

    SREG(30) = 3;
    sBootDataPlayer.currentBoots = PLAYER_BOOTS_KOKIRI;
    Player_SetBootData(&sFakePlay, &sBootDataPlayer);

    liboot_cam_yaw = 0;
    liboot_reset_requested = 0;
}

void liboot_play_init(void) {
    liboot_fake_play_reset();
}

void liboot_register_segment_span(int seg, void* base, size_t size) {
    if ((seg < 0) || (seg >= NUM_SEGMENTS)) {
        return;
    }

    sSegmentSpans[seg].base = base;
    sSegmentSpans[seg].size = (base != NULL) ? size : 0;
    gSegments[seg] = (base != NULL) ? ((uintptr_t)base - K0BASE) : 0;

    if (seg == 6) {
        sFakePlay.objectCtx.slots[0].segment = base;
        gObjectTable[OBJECT_LINK_BOY].vromStart = (uintptr_t)base;
        gObjectTable[OBJECT_LINK_BOY].vromEnd = (uintptr_t)base + sSegmentSpans[seg].size;
    }

    /* liboot v0.4: gameplay_keep rides object slot 1 */
    if (seg == 4) {
        sFakePlay.objectCtx.slots[1].segment = base;
        gObjectTable[OBJECT_GAMEPLAY_KEEP].vromStart = (uintptr_t)base;
        gObjectTable[OBJECT_GAMEPLAY_KEEP].vromEnd = (uintptr_t)base + sSegmentSpans[seg].size;
    }
}

void liboot_reset_tha(void) {
    THA_Init(&sFakePlay.state.tha, sThaHeap, sizeof(sThaHeap));
    Matrix_Init(&sFakePlay.state); /* the matrix stack lives at the arena head */
}

const u8* liboot_segment_base(int seg, size_t* outSize) {
    if ((seg < 0) || (seg >= NUM_SEGMENTS) || (sSegmentSpans[seg].base == NULL)) {
        if (outSize != NULL) {
            *outSize = 0;
        }
        return NULL;
    }
    if (outSize != NULL) {
        *outSize = sSegmentSpans[seg].size;
    }
    return sSegmentSpans[seg].base;
}

static const u8* Liboot_FindDmaSource(uintptr_t vrom, size_t size, s32* segmentOut) {
    s32 seg;

    if (vrom <= UINT32_MAX) {
        seg = SEGMENT_NUMBER(vrom);
        if ((seg >= 0) && (seg < NUM_SEGMENTS) && (sSegmentSpans[seg].base != NULL)) {
            size_t offset = SEGMENT_OFFSET(vrom);
            if ((offset <= sSegmentSpans[seg].size) && (size <= sSegmentSpans[seg].size - offset)) {
                *segmentOut = seg;
                return sSegmentSpans[seg].base + offset;
            }
        }
    }

    for (seg = 0; seg < NUM_SEGMENTS; seg++) {
        uintptr_t base = (uintptr_t)sSegmentSpans[seg].base;
        if ((base != 0) && (vrom >= base)) {
            size_t offset = vrom - base;
            if ((offset <= sSegmentSpans[seg].size) && (size <= sSegmentSpans[seg].size - offset)) {
                *segmentOut = seg;
                return sSegmentSpans[seg].base + offset;
            }
        }
    }

    if (sSegmentSpans[7].base != NULL) {
        uintptr_t animationBase = (uintptr_t)_link_animetionSegmentRomStart;
        if (vrom >= animationBase) {
            size_t offset = vrom - animationBase;
            if ((offset <= sSegmentSpans[7].size) && (size <= sSegmentSpans[7].size - offset)) {
                *segmentOut = 7;
                return sSegmentSpans[7].base + offset;
            }
        }
    }

    return NULL;
}

static s32 Liboot_DmaCopy(void* ram, uintptr_t vrom, size_t size) {
    const u8* source;
    s32 segment = -1;
    size_t i;

    if ((ram == NULL) && (size != 0)) {
        return -1;
    }

    source = Liboot_FindDmaSource(vrom, size, &segment);
    if (source == NULL) {
        if (ram != NULL) {
            memset(ram, 0, size);
        }
        return -1;
    }

    if (segment == 7) {
        u8* destination = ram;
        for (i = 0; i + 1 < size; i += 2) {
            u16 word = ((u16)source[i] << 8) | source[i + 1];
            memcpy(destination + i, &word, sizeof(word));
        }
        if (i < size) {
            destination[i] = source[i];
        }
    } else {
        memcpy(ram, source, size);
    }
    return 0;
}

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count) {
    if (mq == NULL) {
        return;
    }
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = (count > 0) ? count : 0;
    mq->msg = msg;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    s32 index;

    (void)flag;
    if ((mq == NULL) || (mq->msg == NULL) || (mq->msgCount <= 0) ||
        (mq->validCount >= mq->msgCount)) {
        return -1;
    }
    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    (void)flag;
    if ((mq == NULL) || (mq->msg == NULL) || (mq->validCount <= 0) || (mq->msgCount <= 0)) {
        return -1;
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

s32 DmaMgr_RequestSync(void* ram, uintptr_t vrom, size_t size) {
    return Liboot_DmaCopy(ram, vrom, size);
}

s32 DmaMgr_RequestAsync(DmaRequest* req, void* ram, uintptr_t vrom, size_t size, u32 unk5,
                        OSMesgQueue* queue, OSMesg msg) {
    s32 result;

    if (req != NULL) {
        memset(req, 0, sizeof(*req));
        req->vromAddr = vrom;
        req->dramAddr = ram;
        req->size = size;
        req->unk_14 = unk5;
        req->notifyQueue = queue;
        req->notifyMsg = msg;
    }
    result = Liboot_DmaCopy(ram, vrom, size);
    if (queue != NULL) {
        osSendMesg(queue, msg, OS_MESG_NOBLOCK);
    }
    return result;
}

Camera* Play_GetCamera(PlayState* play, s16 camId) {
    if (play == NULL) {
        play = &sFakePlay;
    }
    if (camId == CAM_ID_NONE) {
        camId = play->activeCamId;
    }
    if ((camId < 0) || (camId >= NUM_CAMS) || (play->cameraPtrs[camId] == NULL)) {
        return &sFakePlay.mainCamera;
    }
    return play->cameraPtrs[camId];
}

s32 Play_GetActiveCamId(PlayState* play) {
    return (play != NULL) ? play->activeCamId : CAM_ID_MAIN;
}

int Play_CamIsNotFixed(PlayState* play) {
    (void)play;
    return true;
}

static void Liboot_FakePlayConstructor(void) __attribute__((constructor));

static void Liboot_FakePlayConstructor(void) {
    liboot_fake_play_reset();
}
