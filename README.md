# liboot

[![License: AGPL v3 or later](https://img.shields.io/badge/License-AGPL_v3_or_later-blue.svg)](LICENSE)
[![Status: experimental](https://img.shields.io/badge/status-experimental-orange.svg)](#current-limits)

Link from *Ocarina of Time*, extracted out of the [zeldaret/oot](https://github.com/zeldaret/oot)
decompilation into a standalone shared library — the same way
[libsm64](https://github.com/libsm64/libsm64) extracted Mario from the SM64 decomp.

The **real** Player actor (`z_player.c`, 16k lines), the real collision engine
(`z_bgcheck.c`), the real skeleton/animation system (`z_skelanime.c`) and the
supporting math core run largely intact on PC; a thin shim fakes the N64 OS
and the rest of the game around them. Models, textures, animations, and audio
are extracted at runtime from a user-supplied OoT ROM — no game asset or ROM
ships in this repo.

liboot is a pre-1.0 compatibility SDK for embedding the original OoT Player
gameplay in another engine. It is not a PC port, emulator, complete game
engine, or source of game assets.

## Documentation

- [Getting started](docs/GETTING_STARTED.md) — build, minimal C host, fixed
  simulation step, rendering, collision, and audio.
- [Universal SDK](docs/UNIVERSAL_SDK.md) — versioned engine wrapper, ownership,
  ABI rules, current instance model, and roadmap.
- [Engine integration guide](docs/ENGINE_INTEGRATION.md) — Unity, Godot,
  Unreal, C/C++, C#, Rust, Python, and custom-engine architecture.
- [Fidelity traces](docs/FIDELITY.md) — deterministic record/compare runner,
  numeric/hash trace format, and the distinction between a regression baseline
  and an OoT reference oracle.
- [Engine-neutral C API](src/liboot_engine.h) — recommended versioned API with
  explicit results and owned frame buffers.
- [Low-level C API](src/liboot.h) — advanced compatibility layer.
- [Starter bindings](bindings/README.md) — C++ RAII and C#/Unity P/Invoke.
- [Feature playground](#feature-playground) — interactive validation lab and
  deterministic ROM-backed test modes.
- [Notices and provenance](NOTICE.md) — AGPL scope, vendored-source status,
  ROM policy, and third-party attribution.

## Status

Working core (v0.8):

- `oot_global_init(rom, size, NULL)` — accepts `.z64/.v64/.n64`, locates
  `link_animetion`, `object_link_boy` **and `object_link_child`** via the
  ROM's own DMADATA instead of fixed file indices, Yaz0-decompresses,
  relocates both flex skeletons, and binds assets. PAL Europe Rev 1 is the
  current fully exercised ROM; a broader revision matrix is a roadmap item.
- `oot_static_surfaces_load` / `oot_static_world_load` — build a real
  `CollisionHeader` from caller triangles and optional water boxes; the real
  `z_bgcheck` handles floors, walls, ceilings, raycasts, swimming, diving and
  iron-boots movement.
- `oot_link_create/tick/delete` — the genuine Player state machine: idle
  (with fidget anims), camera-relative run at the authentic caps (adult 6.0,
  child 5.5 u/f) and accel/decel curves, real animation task queue.
- `oot_link_set_age` — adult/child switch at runtime (re-inits the Player on
  the right skeleton/model group/object segment, position preserved).
- `oot_link_set_health/set_magic/damage` — save-context backed; damage goes
  through the real `Player_InflictDamage`.
- `oot_link_set_equipment` / `oot_link_use_item` — swords, shields, tunics,
  boots, Ocarina and the real projectile actors: arrows, bombs, hookshot and
  boomerang. `oot_actor_query` exposes live actor transforms and
  `oot_actor_set_render` can append their ROM meshes to the geometry buffer.
- `oot_target_create/move/remove` — host-owned targets backed by OoT's real
  Attention system, including Z-lock, strafing, auto-facing and lock release.
  The real EnElf actor supplies Navi's position/colors; its wing mesh is an
  opt-in part of the geometry output.
- `oot_link_tick` fills `OoTLinkGeometryBuffers` with Link's real mesh: an
  F3DZEX2 display-list interpreter walks the flex skeleton with the game's own
  limb-draw overrides, so the model follows equipment (swords, shields, hands)
  and age.
- `oot_link_get_skeleton` — Link's animated pose in world space (21 joints,
  limb-indexed), for callers that render him themselves.
- **Real textures**: the interpreter emulates the texture pipeline (SETTIMG /
  SETTILE / LOADBLOCK / LOADTLUT, formats rgba16, ci4/ci8+palette, ia4/8/16,
  i4/i8) and decodes every texture Link's display lists reference straight
  from the caller's ROM into an RGBA cache (`oot_get_texture*`). Per-triangle
  texture indices land in the geometry buffers, including animation-driven
  eye and mouth texture changes.
- **SFX events + mapped PCM**: every sound request the real Player code makes is
  forwarded via `oot_set_sfx_callback` (or `oot_set_sfx_callback_ex` for the
  original pitch, volume, reverb, and position). Despite its compatibility
  name, `oot_get_voice_sample` serves mapped VADPCM-decoded gameplay SFX as
  well as Link/Navi voice clips, including the ROM sequence timing for chained
  grunts. `oot_get_ocarina_note` exposes the five ROM instrument samples, rates
  and loop points.
- **Native ROM AudioSeq playback**: `oot_audio_render_f32` mixes the ROM's 110
  sequences through the four retail players using its 38 soundfonts and seven
  sample banks. The API includes fades, pause/volume/player/channel IO, all 19
  nature-ambience presets, and a named 1,259-entry SFX selector. Rendering is
  allocation-free and produces interleaved stereo F32 at the host's rate;
  `oot_audio_sequence_prewarm` moves first-use decoding before device start.
- **ROM scenes**: `oot_scene_load` installs a scene's real collision, surface
  data and water, while `oot_scene_get_geometry` exposes opaque/translucent
  room triangles through the shared texture cache. `oot_scene_spawn` returns
  the scene entrance position. The full retail scene-index table is exposed
  (`OoTSceneIndex`), and passing `roomIndex = -1` renders every room of a
  multi-room dungeon at once. `oot_scene_get_door_count`/`oot_scene_get_door`
  surface the scene's transition actors and `oot_scene_set_room` swaps the
  active room (unloading the previous one, loading the next) for door-driven
  transitions. `oot_scene_get_sequence_id`/`oot_scene_get_ambience_id` report
  the scene's sound-settings command (background-music sequence id + nature
  ambience id) so a host can call `oot_audio_sequence_play` and
  `oot_audio_nature_play` (or drive its own player).
  `oot_scene_get_runtime` exposes the live scene/room behavior copied into the
  Player runtime: room type and environment, echo, Lens mode, warp restriction,
  scene-camera type and map area. In an all-room render, geometry remains `-1`
  while gameplay correctly uses room 0 as its active room.
- **Gameplay state + queries**: `oot_scene_query_surface` reports the floor
  under a point (height, floor type, mapped material, hookshot-attachability);
  `oot_link_set_pose`/`oot_link_freeze`/`oot_link_set_invincible` reposition and
  gate Link; `oot_actor_spawn` places supported actors. `OoTLinkState` also
  carries the high-level action, stable animation id + frame, aim pitch/yaw,
  floor-sfx group, attack-swing id, and the submerged **underwater/drown
  timer** (`underwaterTimer`, 0..300).
- **Ocarina songs**: `oot_ocarina_song_notes` returns the canonical note
  pattern for each of the twelve songs (the six warp/teleport songs first) and
  `oot_ocarina_match` recognizes a played note sequence via longest-tail match.

## Universal engine API

`liboot_engine.h` is the preferred foundation for new game-engine plugins. It
wraps the raw process-global calls in a checked opaque `OoTEngine` and adds:

- `structSize`/API-version configuration and explicit `OoTResult` errors;
- owned Link geometry, actors, skeleton, Navi, and copied scene frame views;
- exact single-tick stepping or a capped 20 Hz elapsed-time accumulator;
- userdata-aware debug and SFX callbacks with re-entry protection;
- checked world, scene, target, equipment, texture, mapped PCM, and Ocarina
  access.

```c
if (oot_engine_api_version() != OOT_ENGINE_API_VERSION) {
    fail("liboot API version mismatch");
}

OoTEngineConfig config;
OoTResult result = oot_engine_config_init(&config);
if (result != OOT_ENGINE_RESULT_OK) {
    fail(oot_engine_result_string(result));
}
config.romData = rom;
config.romSize = romSize;

OoTEngine *engine = NULL;
result = oot_engine_create(&config, &engine);
/* Load collision or a ROM scene before oot_engine_link_create(). */
```

The convenience initializer above is a C macro that passes the structure size
and expected API version to the result-returning exported
`oot_engine_config_init_sized` function. Compatible ROM buffers are at least
`OOT_ENGINE_MIN_ROM_SIZE` (`0x1060`) bytes and at most
`OOT_ENGINE_MAX_ROM_SIZE` (256 MiB). Configured fixed-step intervals must be in
`[0.001, 1.0]` seconds and `maxSubsteps` in `[1, 1000]`. A custom world accepts
at most `OOT_ENGINE_MAX_STATIC_SURFACES` (2730) triangles and
`OOT_ENGINE_MAX_WATER_BOXES` (65,535) water boxes. Setting either scheduling
field to zero selects its initialized default.

This first universal layer is engine-neutral, not yet multi-instance: the
decompiled core still uses globals, so one `OoTEngine` and one Link may exist
per process. See the [Universal SDK roadmap](docs/UNIVERSAL_SDK.md) and the
[engine integration guide](docs/ENGINE_INTEGRATION.md) for exact lifecycle,
rendering, audio, Unity, Godot, Unreal, Rust, and Python guidance.

## Current limits

Current limits: one active engine/Link, no general enemy/actor runtime or
dynamic collision, and a first-pass scene loader (main
headers only; no exits, alternate
day/age headers or animated materials; prerendered JPEG backgrounds are not
drawn). Geometry is a triangle stream with per-vertex shade alpha and
per-triangle cull/alpha-test/decal flags, but without entity, material, pass,
general blend, or depth metadata. Gameplay behavior is
compiled against the NTSC 1.2 decomp paths even when structurally compatible
assets are read from another retail ROM. See the public headers for exact
contracts; do not claim unlisted ROM compatibility without testing it.

## Build

```sh
make                        # -> liboot.so + C/C++ public headers in dist/
make -C examples            # -> examples/engine + examples/basic (raw API)
make -C test playground     # needs SDL2 and OpenGL development headers
./examples/engine path/to/oot.z64
```

The CMake build supports shared or static libraries, installable
`liboot::oot` package metadata, and `pkg-config`. See
[Getting started](docs/GETTING_STARTED.md). A ROM is always supplied by the
user at runtime and must never be committed to an application repository.

## Feature playground

```sh
./test/playground path/to/oot.z64
```

The playground is a feature workbench rather than a single combat demo. It
shows live health/magic, inputs, loadout, actors and geometry counts; the
diagnostics panel adds state flags, animation/velocity data and the recent SFX
event/PCM log. Press **F9** or **Tab** to open the workbench, then use Tab to
change category, arrows to select/change values, Enter to activate and Esc to
close. Its Play, Link, Items, World, Audio and Render pages cover pause/step/
time scale, age and equipment, every public item, world loading/teleports,
automatic per-scene music/ambience, manual access to every sequence and SFX,
ROM-audio replay and render/debug overlays. F12 opens the built-in help page.

Core controls:

- WASD moves; Space is A, J is B/attack, K is Z-target, L is shield and I is
  the item button. Hold/release I to aim/fire the bow or hookshot; use it to
  place bombs or throw the child boomerang.
- E toggles the Ocarina; while it is out, J and the arrow keys play its five
  notes. T switches adult/child, H inflicts damage, M fills magic and R
  respawns Link.
- Right-drag rotates the orbit camera, the wheel zooms and C cycles orbit,
  chase and target cameras. P pauses, N advances one 20 Hz simulation tick,
  F10 toggles the HUD and F11 toggles diagnostics. A game controller is also
  supported.
- 1-4 select swords, 5-8 select bow/bomb/hookshot/boomerang, Shift+5-7 select
  shields, Shift+8/9/0 select tunics and F1-F3 select boots. V switches between
  the textured model and skeleton view.
- F5 switches between the arena and a ROM scene; F6 cycles Deku Tree, Kokiri
  Forest, Link's House, Temple of Time, Hyrule Field, Kakariko Village,
  Zora's Domain and Outside Ganon's Castle.

The arena has a central Stalchild combat ring, a north-west stair/platform
course, a north-east deep-water and iron-boots basin with a west exit ramp, a
south-west slope/ledge course, and a south-east projectile range with a
hookshot tower. A narrow balance beam provides another collision test.

Headless modes run before SDL/OpenGL initialization, so they are useful in CI:

```sh
./test/playground rom.z64 --frames 3000        # scripted arena/combat loop
./test/playground rom.z64 --scene-frames 1600  # walk through all eight scenes
./test/playground rom.z64 --suite 1000         # full API checks + N stress ticks
```

`--suite` validates finite/bounded Link geometry and textures, skeleton state,
voice and Ocarina extraction, every projectile actor, water/boots behavior and
all configured scene meshes/spawns. `--ui-frames N` is an optional graphical smoke
run that opens the workbench and diagnostics, checks GL errors, then exits.

Other tests: `make -C test headless && ./test/headless rom.z64`
(movement/age/damage), and `make -C test equip_test && ./test/equip_test
rom.z64` (equipment/items). `./test/engine_init_test`,
`./test/rom_util_test`, `./test/audio_overflow_test`, and
`./test/fidelity_runner --self-test` are copyright-free ABI/parser/arithmetic/
trace regressions that do not require a ROM. Record or replay a deterministic
ROM-backed numeric/hash trace with:

```sh
./test/fidelity_runner rom.z64 --record local.trace
./test/fidelity_runner rom.z64 --compare local.trace
```

A trace recorded by liboot is an anti-regression baseline, not proof that it
matches OoT. See [the fidelity guide](docs/FIDELITY.md) for the reference-oracle
workflow and trace-data policy.

ROM-backed audio regressions are available separately:

```sh
./test/audio_catalog_test rom.z64
./test/audio_sequence_test rom.z64
```

## Layout

- `src/liboot_engine.{c,h}` — recommended versioned engine-neutral wrapper.
- `src/liboot.{c,h}` — low-level compatibility API;
  `src/rom_util.c` — DMADATA/Yaz0/byteswap.
- `src/load_assets.c` — runtime skeleton relocation + asset binding,
  Player lifecycle glue.
- `src/shim/` — fake PlayState/camera/save + no-op subsystems (audio, message,
  effects...). Rule: *real types, fake instances; real math, fake IO*.
- `src/decomp/` — pinned decomp source/header closure with the local patch set
  listed in `NOTICE.md` (patches marked `// liboot:` where practical).
- `src/gen/` — generated from the decomp's asset XMLs by
  `tools/gen_anim_headers.py` (561 animation headers as segment-7 tokens) and
  `tools/gen_asset_symbols.py` (155 model symbols + runtime bind manifest).
- `bindings/` — starter C++ and C# adapters; `cmake/` and `CMakeLists.txt` —
  install/package support.

Current behavior is defined by the public headers and the guides under `docs/`.

## License, attribution, and contributor

Original liboot code and documentation authored by **Cycl0o0**, the sole
current contributor, are licensed under the
[GNU Affero General Public License v3.0 or later](LICENSE).

The `src/decomp/` subtree is vendored from `zeldaret/oot`, which does not
currently declare a repository-wide license; it is not relicensed by liboot.
Read [NOTICE.md](NOTICE.md) before redistribution. No Nintendo ROM, model,
texture, animation, or audio asset is included. liboot is unaffiliated with
Nintendo.

## How it works, in one paragraph

The pinned decomp header closure and 22 selected game translation units compile
on a normal host toolchain with a small, documented integration patch set,
including exact-width N64 types, segmented/native pointer handling, host GBI
callbacks, matrix alignment, and the bounded actor slice. Undefined symbols
were closed in three moves: animation headers generated from XML with
segment-7 tokens instead of pointers; model
symbols generated as one-command `gsSPBranchList(0x06xxxxxx)` display lists
plus a bind manifest the loader fills from the ROM; and ~180 engine
functions stubbed or minimally implemented (synchronous message queues, a DMA
service that resolves segment tokens and big-endian-decodes animation frames,
a fixed follow camera whose yaw is fed from the public API). A patched
`SEGMENTED_TO_VIRTUAL` distinguishes 32-bit segment tokens from native
pointers, which lets the same decomp code walk both ROM data and host structs.
