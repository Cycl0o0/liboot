#ifndef LIBOOT_FAKE_PLAY_H
#define LIBOOT_FAKE_PLAY_H

#include <stddef.h>

#include "liboot.h"
#include "play_state.h"

extern s16 liboot_cam_yaw;
extern s32 liboot_reset_requested;

/* Internal runtime helpers.  These intentionally have hidden visibility and
 * are not part of liboot's public ABI. */
PlayState* liboot_get_fake_play_state(void);
void liboot_fake_play_reset(void);
void liboot_register_segment_span(int seg, void* base, size_t size);

/* Compatibility names used by the in-progress public loader. */
PlayState* liboot_play(void);
void liboot_play_init(void);

#endif
