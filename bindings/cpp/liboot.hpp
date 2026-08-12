/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#ifndef LIBOOT_CPP_LIBOOT_HPP
#define LIBOOT_CPP_LIBOOT_HPP

#include <liboot_engine.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

namespace liboot {

inline std::uint32_t api_version() noexcept {
    return oot_engine_api_version();
}

inline const char *result_string(OoTResult result) noexcept {
    return oot_engine_result_string(result);
}

class Error : public std::runtime_error {
public:
    explicit Error(OoTResult result)
        : std::runtime_error(result_string(result)), result_(result) {}

    OoTResult result() const noexcept { return result_; }

private:
    OoTResult result_;
};

inline void check(OoTResult result) {
    if (result != OOT_ENGINE_RESULT_OK) {
        throw Error(result);
    }
}

inline OoTEngineConfig default_config() {
    OoTEngineConfig config;
    check(oot_engine_config_init_sized(&config,
                                       static_cast<std::uint32_t>(sizeof(config)),
                                       OOT_ENGINE_API_VERSION));
    return config;
}

inline OoTEngineLimits limits() {
    OoTEngineLimits value = OOT_ENGINE_LIMITS_INIT;
    check(oot_engine_get_limits(&value));
    return value;
}

struct AdvanceResult {
    std::uint32_t steps;
    const OoTEngineFrame *frame;
};

struct SceneSpawn {
    std::array<float, 3> position;
    std::int16_t yaw;
};

struct SceneLayerSelection {
    OoTSceneLayer layer;
    bool used_fallback;
};

namespace audio {
enum class Player : std::uint8_t {
    Main = OOT_AUDIO_PLAYER_MAIN,
    Fanfare = OOT_AUDIO_PLAYER_FANFARE,
    Sfx = OOT_AUDIO_PLAYER_SFX,
    Sub = OOT_AUDIO_PLAYER_SUB,
};
} // namespace audio

struct EnemyBgmState {
    bool enabled;
    bool active;
    float distance;
};

/*
 * Exclusive RAII owner for one engine-neutral API instance. Shared-library
 * builds isolate native writable state and support multiple Engine objects;
 * static archives expose PROCESS_SINGLETON instead. Frame pointers are
 * borrowed and become stale after the next mutating call on their Engine.
 */
class Engine {
public:
    Engine(const std::uint8_t *rom, std::size_t romSize) {
        OoTEngineConfig config = default_config();
        config.romData = rom;
        config.romSize = romSize;
        check(oot_engine_create(&config, &engine_));
    }

    explicit Engine(const OoTEngineConfig &config) {
        check(oot_engine_create(&config, &engine_));
    }

    ~Engine() noexcept { destroy_or_terminate(); }

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    Engine(Engine &&other) noexcept : engine_(other.engine_) {
        other.engine_ = nullptr;
    }

    Engine &operator=(Engine &&other) noexcept {
        if (this != &other) {
            destroy_or_terminate();
            engine_ = other.engine_;
            other.engine_ = nullptr;
        }
        return *this;
    }

    OoTEngine *native_handle() const noexcept { return engine_; }

    /*
     * Explicit fallible teardown. On failure the handle remains owned so the
     * caller can catch Error and retry (notably after OOT_ENGINE_RESULT_BUSY).
     */
    void close() {
        if (engine_ == nullptr) {
            return;
        }
        OoTResult result = oot_engine_destroy(engine_);
        if (result == OOT_ENGINE_RESULT_OK) {
            engine_ = nullptr;
        }
        check(result);
    }

    /*
     * Callbacks are borrowed. Their functions and user-data objects must stay
     * alive until they are replaced, cleared, or this Engine is destroyed.
     * A callback must not call back into this Engine.
     */
    void set_callbacks(OoTEngineDebugCallback debugCallback,
                       void *debugUserData,
                       OoTEngineSfxCallback sfxCallback,
                       void *sfxUserData) {
        check(oot_engine_set_callbacks(require_engine(), debugCallback,
                                       debugUserData, sfxCallback, sfxUserData));
    }

    void create_link(float x, float y, float z) {
        check(oot_engine_link_create(require_engine(), x, y, z));
    }

    void delete_link() {
        check(oot_engine_link_delete(require_engine()));
    }

    void set_age(std::uint8_t age) {
        check(oot_engine_link_set_age(require_engine(), age));
    }

    void set_equipment(std::uint8_t sword, std::uint8_t shield,
                       std::uint8_t tunic, std::uint8_t boots) {
        check(oot_engine_link_set_equipment(require_engine(), sword, shield,
                                            tunic, boots));
    }

    void use_item(std::uint8_t item) {
        check(oot_engine_link_use_item(require_engine(), item));
    }

    void set_health(std::int16_t health, std::int16_t capacity) {
        check(oot_engine_link_set_health(require_engine(), health, capacity));
    }

    void damage(std::int16_t amount) {
        check(oot_engine_link_damage(require_engine(), amount));
    }

    void set_magic(std::uint8_t level, std::int16_t amount) {
        check(oot_engine_link_set_magic(require_engine(), level, amount));
    }

    // liboot vNEXT: reposition Link in place (pair with freeze for a clean warp).
    void set_pose(float x, float y, float z, std::int16_t yaw) {
        check(oot_engine_link_set_pose(require_engine(), x, y, z, yaw));
    }

    void freeze(bool frozen) {
        check(oot_engine_link_freeze(require_engine(), frozen ? 1u : 0u));
    }

    void set_invincible(std::int8_t frames) {
        check(oot_engine_link_set_invincible(require_engine(), frames));
    }

    // Raycast the live collision world; throws Error(NOT_AVAILABLE) if no floor.
    OoTSurfaceInfo query_surface(float x, float y, float z) {
        OoTSurfaceInfo info;
        check(oot_engine_scene_query_surface(require_engine(), x, y, z, &info));
        return info;
    }

    // liboot vNEXT: door-driven room transitions. set_room swaps the active
    // room (roomIndex -1 = whole scene); the door list lets a host detect a
    // crossing and pick the target room.
    void set_room(std::int32_t roomIndex) {
        std::int32_t nativeResult = 0;
        check(oot_engine_scene_set_room(require_engine(), roomIndex, &nativeResult));
    }

    std::uint32_t door_count() {
        std::uint32_t count = 0;
        check(oot_engine_scene_get_door_count(require_engine(), &count));
        return count;
    }

    OoTDoor door(std::uint32_t index) {
        OoTDoor d;
        check(oot_engine_scene_get_door(require_engine(), index, &d));
        return d;
    }

    // liboot vNEXT: the loaded scene's sound settings (cmd 0x15). Both are -1
    // when the scene declares none. Feed them to liboot::audio below or to a
    // host sequenced-audio player.
    std::int32_t sequence_id() {
        std::int32_t seqId = -1;
        check(oot_engine_scene_get_sequence_id(require_engine(), &seqId));
        return seqId;
    }

    std::int32_t ambience_id() {
        std::int32_t ambienceId = -1;
        check(oot_engine_scene_get_ambience_id(require_engine(), &ambienceId));
        return ambienceId;
    }

    // liboot vNEXT: the scene's active light/fog settings. liboot already bakes
    // this shade into emitted vertex colors; use it to drive host lighting/fog.
    OoTSceneEnvironment environment() {
        OoTSceneEnvironment env;
        check(oot_engine_scene_get_environment(require_engine(), &env));
        return env;
    }

    OoTSceneRuntime scene_runtime() {
        OoTSceneRuntime runtime{};
        check(oot_engine_scene_get_runtime(require_engine(), &runtime));
        return runtime;
    }

    SceneLayerSelection active_scene_layer() const {
        std::uint8_t layer = OOT_SCENE_LAYER_CHILD_DAY;
        std::uint8_t fallback = 0u;
        check(oot_engine_scene_get_active_layer(require_engine(), &layer,
                                                &fallback));
        return {static_cast<OoTSceneLayer>(layer), fallback != 0u};
    }

    std::uint32_t scene_exit_count() const {
        std::uint32_t count = 0u;
        check(oot_engine_scene_get_exit_count(require_engine(), &count));
        return count;
    }

    std::int16_t scene_exit(std::uint32_t index) const {
        std::int16_t entrance = -1;
        check(oot_engine_scene_get_exit(require_engine(), index, &entrance));
        return entrance;
    }

    bool poll_world_event(OoTWorldEvent &event) {
        event = {};
        event.structSize = sizeof(event);
        event.version = OOT_WORLD_EVENT_VERSION;
        const OoTResult result =
            oot_engine_world_event_poll(require_engine(), &event);
        if (result == OOT_ENGINE_RESULT_NOT_AVAILABLE) {
            return false;
        }
        check(result);
        return true;
    }

    std::uint32_t scene_background_count() {
        std::uint32_t count = 0u;
        check(oot_engine_scene_get_background_count(require_engine(), &count));
        return count;
    }

    OoTSceneBackground scene_background(std::uint32_t index) {
        OoTSceneBackground background{};
        background.structSize = sizeof(background);
        background.version = OOT_SCENE_BACKGROUND_VERSION;
        check(oot_engine_scene_get_background(require_engine(), index,
                                               &background));
        return background;
    }

    OoTSceneMaterialState scene_material_state() {
        OoTSceneMaterialState state{};
        state.structSize = sizeof(state);
        state.version = OOT_SCENE_MATERIAL_STATE_VERSION;
        check(oot_engine_scene_get_material_state(require_engine(), &state));
        return state;
    }

    OoTSceneMaterialReference scene_material_reference(std::uint32_t index) {
        OoTSceneMaterialReference reference{};
        reference.structSize = sizeof(reference);
        reference.version = OOT_SCENE_MATERIAL_REFERENCE_VERSION;
        check(oot_engine_scene_get_material_reference(require_engine(), index,
                                                      &reference));
        return reference;
    }

    /* Borrowed view; scene/world changes can invalidate its array pointers. */
    const OoTEngineSceneGeometry &scene_geometry() const {
        const OoTEngineSceneGeometry *geometry = nullptr;
        check(oot_engine_scene_get_geometry(require_engine(), &geometry));
        if (geometry == nullptr) {
            throw Error(OOT_ENGINE_RESULT_NOT_AVAILABLE);
        }
        return *geometry;
    }

    std::uint32_t scene_dropped_triangles() const {
        std::uint32_t count = 0u;
        check(oot_engine_scene_get_dropped_triangles(require_engine(), &count));
        return count;
    }

    SceneSpawn scene_spawn(std::int32_t spawnIndex) const {
        SceneSpawn spawn{};
        check(oot_engine_scene_get_spawn(require_engine(), spawnIndex,
                                         spawn.position.data(), &spawn.yaw));
        return spawn;
    }

    std::uint32_t scene_actor_count() const {
        std::uint32_t count = 0u;
        check(oot_engine_scene_get_actor_count(require_engine(), &count));
        return count;
    }

    OoTSceneActorEntry scene_actor(std::uint32_t index) const {
        OoTSceneActorEntry actor{};
        actor.structSize = sizeof(actor);
        actor.version = OOT_SCENE_ACTOR_ENTRY_VERSION;
        check(oot_engine_scene_get_actor(require_engine(), index, &actor));
        return actor;
    }

    const OoTEngineFrame &step(const OoTEngineInput *input = nullptr) {
        const OoTEngineFrame *frame = nullptr;
        check(oot_engine_step(require_engine(), input, &frame));
        if (frame == nullptr) {
            throw Error(OOT_ENGINE_RESULT_NO_FRAME);
        }
        return *frame;
    }

    AdvanceResult advance(float elapsedSeconds,
                          const OoTEngineInput *input = nullptr) {
        AdvanceResult result{0u, nullptr};
        check(oot_engine_advance(require_engine(), elapsedSeconds, input,
                                 &result.steps, &result.frame));
        return result;
    }

    const OoTEngineFrame &frame() const {
        const OoTEngineFrame *value = nullptr;
        check(oot_engine_get_frame(require_engine(), &value));
        if (value == nullptr) {
            throw Error(OOT_ENGINE_RESULT_NO_FRAME);
        }
        return *value;
    }

    void reset_clock() {
        check(oot_engine_reset_clock(require_engine()));
    }

    void set_render_flags(std::uint32_t flags) {
        check(oot_engine_set_render_flags(require_engine(), flags));
    }

    std::uint32_t render_flags() const {
        std::uint32_t flags = 0u;
        check(oot_engine_get_render_flags(require_engine(), &flags));
        return flags;
    }

    void load_world(const OoTSurface *surfaces, std::uint32_t surfaceCount,
                    const OoTWaterBox *waterBoxes = nullptr,
                    std::uint32_t waterBoxCount = 0u) {
        check(oot_engine_static_world_load(require_engine(), surfaces,
                                           surfaceCount, waterBoxes,
                                           waterBoxCount));
    }

    OoTDynamicCollision create_dynamic_collision(
        const OoTSurface *surfaces, std::uint32_t surfaceCount,
        const OoTDynamicCollisionTransform &transform,
        std::uint32_t flags = OOT_DYNAMIC_COLLISION_CARRY_POSITION |
                              OOT_DYNAMIC_COLLISION_CARRY_ROTATION_Y) {
        OoTDynamicCollision handle = OOT_DYNAMIC_COLLISION_INVALID;
        check(oot_engine_dynamic_collision_create(require_engine(), surfaces,
                                                   surfaceCount, &transform,
                                                   flags, &handle));
        return handle;
    }

    void set_dynamic_collision_transform(
        OoTDynamicCollision handle,
        const OoTDynamicCollisionTransform &transform) {
        check(oot_engine_dynamic_collision_set_transform(require_engine(), handle,
                                                          &transform));
    }

    void set_dynamic_collision_enabled(OoTDynamicCollision handle, bool enabled) {
        check(oot_engine_dynamic_collision_set_enabled(require_engine(), handle,
                                                        enabled ? 1u : 0u));
    }

    OoTDynamicCollisionState dynamic_collision_state(
        OoTDynamicCollision handle) const {
        OoTDynamicCollisionState state{};
        state.structSize = static_cast<std::uint32_t>(sizeof(state));
        check(oot_engine_dynamic_collision_get_state(require_engine(), handle,
                                                      &state));
        return state;
    }

    void delete_dynamic_collision(OoTDynamicCollision handle) {
        check(oot_engine_dynamic_collision_delete(require_engine(), handle));
    }

    void load_scene(std::int32_t scene, std::int32_t room = 0) {
        std::int32_t nativeResult = 0;
        check(oot_engine_scene_load(require_engine(), scene, room,
                                    &nativeResult));
    }

    void load_scene(std::int32_t scene, std::int32_t room,
                    OoTSceneLayer layer) {
        OoTSceneLoadOptions options{};
        options.structSize = sizeof(options);
        options.sceneIndex = scene;
        options.roomIndex = room;
        options.layer = static_cast<std::uint8_t>(layer);
        std::int32_t nativeResult = 0;
        check(oot_engine_scene_load_ex(require_engine(), &options,
                                       &nativeResult));
    }

    OoTEngineTarget create_target(float x, float y, float z,
                                  float focusHeight) {
        OoTEngineTarget target = OOT_ENGINE_INVALID_TARGET;
        check(oot_engine_target_create(require_engine(), x, y, z,
                                       focusHeight, &target));
        return target;
    }

    void move_target(OoTEngineTarget target, float x, float y, float z) {
        check(oot_engine_target_move(require_engine(), target, x, y, z));
    }

    void remove_target(OoTEngineTarget target) {
        check(oot_engine_target_remove(require_engine(), target));
    }

    void clear_targets() {
        check(oot_engine_targets_clear(require_engine()));
    }

    OoTEngineHostActor create_host_actor(const OoTHostActorState &state) {
        OoTEngineHostActor actor = OOT_ENGINE_INVALID_HOST_ACTOR;
        check(oot_engine_host_actor_create(require_engine(), &state, &actor));
        return actor;
    }

    void update_host_actor(OoTEngineHostActor actor,
                           const OoTHostActorState &state) {
        check(oot_engine_host_actor_update(require_engine(), actor, &state));
    }

    OoTHostActorState host_actor(OoTEngineHostActor actor) const {
        OoTHostActorState state{};
        state.structSize = sizeof(state);
        state.version = OOT_HOST_ACTOR_STATE_VERSION;
        check(oot_engine_host_actor_get(require_engine(), actor, &state));
        return state;
    }

    void remove_host_actor(OoTEngineHostActor actor) {
        check(oot_engine_host_actor_remove(require_engine(), actor));
    }

    void clear_host_actors() {
        check(oot_engine_host_actors_clear(require_engine()));
    }

    bool poll_host_actor_contact(OoTEngineActorContact &contact) {
        contact = {};
        contact.structSize = sizeof(contact);
        contact.version = OOT_HOST_ACTOR_CONTACT_VERSION;
        OoTResult result = oot_engine_host_actor_poll_contact(require_engine(),
                                                               &contact);
        if (result == OOT_ENGINE_RESULT_NOT_AVAILABLE) {
            return false;
        }
        check(result);
        return true;
    }

    std::uint32_t texture_count() const {
        std::uint32_t count = 0u;
        check(oot_engine_texture_count(require_engine(), &count));
        return count;
    }

    OoTEngineTexture texture(std::uint32_t index) const {
        OoTEngineTexture value{};
        check(oot_engine_texture_get(require_engine(), index, &value));
        return value;
    }

    /* Returned sample pointers are borrowed and live until Engine teardown. */
    OoTEnginePcm voice_pcm(std::uint16_t sfxId) const {
        OoTEnginePcm value{};
        check(oot_engine_voice_get(require_engine(), sfxId, &value));
        return value;
    }

    OoTEnginePcm ocarina_note_pcm(std::uint8_t noteIndex) const {
        OoTEnginePcm value{};
        check(oot_engine_ocarina_note_get(require_engine(), noteIndex, &value));
        return value;
    }

    void audio_sequence_prewarm(std::uint16_t sequenceId) {
        check(oot_engine_audio_sequence_prewarm(require_engine(), sequenceId));
    }

    void audio_sequence_play(audio::Player player, std::uint16_t sequenceId,
                             std::uint16_t fadeInMs = 0u) {
        check(oot_engine_audio_sequence_play(
            require_engine(), static_cast<std::uint8_t>(player), sequenceId,
            fadeInMs));
    }

    void audio_nature_play(audio::Player player, std::uint8_t ambienceId,
                           std::uint16_t fadeInMs = 0u) {
        check(oot_engine_audio_nature_play(
            require_engine(), static_cast<std::uint8_t>(player), ambienceId,
            fadeInMs));
    }

    void audio_sequence_stop(audio::Player player,
                             std::uint16_t fadeOutMs = 0u) {
        check(oot_engine_audio_sequence_stop(
            require_engine(), static_cast<std::uint8_t>(player), fadeOutMs));
    }

    void audio_sequence_pause(audio::Player player, bool paused) {
        check(oot_engine_audio_sequence_pause(
            require_engine(), static_cast<std::uint8_t>(player),
            paused ? 1u : 0u));
    }

    void audio_sequence_set_volume(audio::Player player, float volume) {
        check(oot_engine_audio_sequence_set_volume(
            require_engine(), static_cast<std::uint8_t>(player), volume));
    }

    void audio_sequence_set_io(audio::Player player, std::uint8_t port,
                               std::int8_t value) {
        check(oot_engine_audio_sequence_set_io(
            require_engine(), static_cast<std::uint8_t>(player), port, value));
    }

    void audio_channel_set_io(audio::Player player, std::uint8_t channel,
                              std::uint8_t port, std::int8_t value) {
        check(oot_engine_audio_channel_set_io(
            require_engine(), static_cast<std::uint8_t>(player), channel, port,
            value));
    }

    OoTAudioState audio_sequence_state(audio::Player player) const {
        OoTAudioState state{};
        state.structSize = sizeof(state);
        state.version = OOT_AUDIO_STATE_VERSION;
        check(oot_engine_audio_sequence_get_state(
            require_engine(), static_cast<std::uint8_t>(player), &state));
        return state;
    }

    void audio_set_master_volume(float volume) {
        check(oot_engine_audio_set_master_volume(require_engine(), volume));
    }

    void audio_stop_all(std::uint16_t fadeOutMs = 0u) {
        check(oot_engine_audio_stop_all(require_engine(), fadeOutMs));
    }

    std::uint32_t audio_render_f32(float *interleavedStereo,
                                   std::uint32_t frames,
                                   std::uint32_t sampleRate) {
        std::uint32_t rendered = 0u;
        check(oot_engine_audio_render_f32(require_engine(), interleavedStereo,
                                          frames, sampleRate, &rendered));
        return rendered;
    }

    std::uint32_t audio_render_s16(std::int16_t *interleavedStereo,
                                   std::uint32_t frames,
                                   std::uint32_t sampleRate) {
        std::uint32_t rendered = 0u;
        check(oot_engine_audio_render_s16(require_engine(), interleavedStereo,
                                          frames, sampleRate, &rendered));
        return rendered;
    }

    void audio_sfx_play(std::uint16_t sfxId, float pan = 0.0f,
                        float volume = 1.0f) {
        check(oot_engine_audio_sfx_play(require_engine(), sfxId, pan, volume));
    }

    void audio_sfx_stop(std::uint16_t sfxId) {
        check(oot_engine_audio_sfx_stop(require_engine(), sfxId));
    }

    void audio_sfx_stop_all() {
        check(oot_engine_audio_sfx_stop_all(require_engine()));
    }

    EnemyBgmState enemy_bgm() const {
        std::uint8_t enabled = 0u;
        std::uint8_t active = 0u;
        float distance = 0.0f;
        check(oot_engine_get_enemy_bgm(require_engine(), &enabled, &active,
                                       &distance));
        return {enabled != 0u, active != 0u, distance};
    }

    void set_enemy_bgm(bool enabled) {
        check(oot_engine_set_enemy_bgm(require_engine(), enabled ? 1u : 0u));
    }

private:
    OoTEngine *require_engine() const {
        if (engine_ == nullptr) {
            throw Error(OOT_ENGINE_RESULT_NOT_INITIALIZED);
        }
        return engine_;
    }

    void destroy_or_terminate() noexcept {
        if (engine_ != nullptr) {
            if (oot_engine_destroy(engine_) != OOT_ENGINE_RESULT_OK) {
                /* A noexcept teardown path cannot preserve ownership for retry. */
                std::terminate();
            }
            engine_ = nullptr;
        }
    }

    OoTEngine *engine_ = nullptr;
};

inline OoTEngineInput default_input() {
    OoTEngineInput input;
    check(oot_engine_input_init_sized(&input,
                                      static_cast<std::uint32_t>(sizeof(input)),
                                      OOT_ENGINE_API_VERSION));
    return input;
}

/*
 * Immutable audio constants and raw ROM-catalog views. A live Engine must have
 * initialized the catalog, and these raw queries have no handle with which to
 * select among different engine contexts. Mutable playback, cache, mixer, SFX,
 * and state operations are therefore exposed only as guarded Engine methods.
 */
namespace audio {

constexpr std::uint16_t SequenceCount = OOT_AUDIO_SEQUENCE_COUNT;
constexpr std::uint16_t NoMusic = OOT_AUDIO_NO_MUSIC;
constexpr std::uint16_t NatureRain = OOT_AUDIO_NATURE_RAIN;
constexpr std::uint8_t NatureCount = OOT_AUDIO_NATURE_COUNT;
constexpr std::uint8_t NatureNone = OOT_AUDIO_NATURE_NONE;

inline std::uint8_t native_player(Player player) noexcept {
    return static_cast<std::uint8_t>(player);
}

inline std::int32_t sequence_count() noexcept {
    return oot_audio_sequence_count();
}

inline const char *sequence_name(std::uint16_t sequenceId) noexcept {
    return oot_audio_sequence_name(sequenceId);
}

inline bool sequence_info(std::uint16_t sequenceId,
                          OoTSequenceInfo &info) noexcept {
    info = OoTSequenceInfo{};
    info.structSize = sizeof(info);
    info.version = OOT_SEQUENCE_INFO_VERSION;
    return oot_audio_sequence_get_info(sequenceId, &info);
}

inline std::int32_t sfx_catalog_count() noexcept {
    return oot_audio_sfx_catalog_count();
}

inline bool sfx_info(std::int32_t catalogIndex, OoTSfxInfo &info) noexcept {
    info = OoTSfxInfo{};
    info.structSize = sizeof(info);
    info.version = OOT_SFX_INFO_VERSION;
    return oot_audio_sfx_catalog_get(catalogIndex, &info);
}

} // namespace audio

/*
 * Stateless ocarina-song helpers. These wrap pure lookup functions in the raw
 * API, so they need no Engine. `song` is an OoTOcarinaSong value; the pattern
 * holds `count` valid note indices (0..4, C-button order) in `notes`.
 */
struct OcarinaSongPattern {
    std::array<std::uint8_t, 8> notes;
    std::int32_t count;
};

inline OcarinaSongPattern ocarina_song_notes(OoTOcarinaSong song) {
    OcarinaSongPattern pattern{};
    oot_ocarina_song_notes(static_cast<std::int32_t>(song), pattern.notes.data(),
                           &pattern.count);
    return pattern;
}

/* Returns the matching OoTOcarinaSong, or -1 when the tail matches no song. */
inline std::int32_t ocarina_match(const std::uint8_t *notes, std::int32_t count) {
    return oot_ocarina_match(notes, count);
}

} // namespace liboot

#endif
