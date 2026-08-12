# Notices and provenance

## liboot code

Copyright (C) 2026 Cycl0o0.

The original liboot integration code, build scripts, tests, examples, and
documentation authored by Cycl0o0 are made available under the GNU Affero
General Public License, version 3 or (at your option) any later version
(`AGPL-3.0-or-later`). See [LICENSE](LICENSE).

## Vendored Ocarina of Time decompilation sources

Except for the two placeholder headers identified below, files under
`src/decomp/` are selected source and header files vendored from the
[zeldaret/oot](https://github.com/zeldaret/oot) decompilation project at
upstream commit `269d03016cd0e3d7a0b8925e02b97a319c1d0e8d`. They are third-party
material and are not relicensed by the liboot project.
At the time of publication, the upstream repository did not declare a
repository-wide software license. Consult upstream before redistributing or
using that material, and retain any notices that upstream later adds.

`tools/vendor-list.txt` records the selected upstream translation units and
`tools/vendor.sh` documents how those units are refreshed. The vendored
header/asset-declaration closure is preserved under `src/decomp/`. Local
integration patches are marked in the source with `liboot` comments where
practical.

The vendored subtree includes local integration changes in
`include/libc/stddef.h`, `include/libc/stdint.h`,
`include/segmented_address.h`, `include/ultra64/controller.h`,
`include/ultra64/gbi.h`, `include/ultra64/libc.h`,
`include/ultra64/ultratypes.h`, `src/code/sys_matrix.c`, and
`src/code/z_actor.c`, plus
`src/overlays/actors/ovl_player_actor/z_player.c`. The unmodified upstream dynamic-collision helpers
`src/code/z_bg_collect.c` and `src/code/z_bg_item.c` are also included.

The following liboot-authored placeholder headers are stored under that subtree
only because upstream include paths require those locations. They are original
Cycl0o0 code covered by `AGPL-3.0-or-later`, not vendored third-party files:

- `assets/objects/gameplay_dangeon_keep/gameplay_dangeon_keep.h`
- `assets/objects/object_bdoor/object_bdoor.h`

## Game assets and trademarks

This repository does not contain an Ocarina of Time ROM or extracted game
assets. Models, textures, animation data, and audio are read at runtime from a
ROM supplied by the user. Users are responsible for supplying a legally
obtained compatible ROM and complying with applicable law.

The Legend of Zelda, Ocarina of Time, Link, Nintendo 64, and Nintendo are
trademarks or properties of Nintendo and their respective owners. liboot is
an independent compatibility project and is not affiliated with, endorsed by,
or sponsored by Nintendo.

The architecture is inspired by the public
[libsm64](https://github.com/libsm64/libsm64) project; liboot does not include
libsm64 source code.

## LibUltraShip audio mixer arithmetic

`src/audio_mixer.c` adapts the N64 ABI four-tap resampling coefficients and
fixed-point command arithmetic from the portable LibUltraShip mixer authored
by Kenix3 and later moved into Ship of Harkinian in commit
`2e4a19c0ee120870ef34e3f211e15ca51461b2b2`. That material is available
under the MIT License; its notice is reproduced in
`LICENSES/LibUltraShip-MIT.txt`. liboot's sequence runtime and the surrounding
cursor, ramp, pan, saturation, and reverb integration are separate work. This
does not claim that liboot executes a retail RSP command stream bit-for-bit.
