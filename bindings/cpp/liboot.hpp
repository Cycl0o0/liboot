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

class Error : public std::runtime_error {
public:
    explicit Error(OoTResult result)
        : std::runtime_error(oot_engine_result_string(result)), result_(result) {}

    OoTResult result() const noexcept { return result_; }

private:
    OoTResult result_;
};

inline void check(OoTResult result) {
    if (result != OOT_ENGINE_RESULT_OK) {
        throw Error(result);
    }
}

struct AdvanceResult {
    std::uint32_t steps;
    const OoTEngineFrame *frame;
};

/*
 * Exclusive RAII owner for the engine-neutral API. The native core currently
 * allows only one Engine in a process. Frame pointers are borrowed and become
 * stale after the next mutating call.
 */
class Engine {
public:
    Engine(const std::uint8_t *rom, std::size_t romSize) {
        OoTEngineConfig config;
        check(oot_engine_config_init(&config));
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

    void load_scene(std::int32_t scene, std::int32_t room = 0) {
        std::int32_t nativeResult = 0;
        check(oot_engine_scene_load(require_engine(), scene, room,
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
    check(oot_engine_input_init(&input));
    return input;
}

/*
 * ROM-backed Zelda AudioSeq playback. Audio state is process-global and is
 * initialized and torn down with Engine, so keep an Engine alive while using
 * these helpers. Serialize control calls against the host audio callback.
 */
namespace audio {

enum class Player : std::uint8_t {
    Main = OOT_AUDIO_PLAYER_MAIN,
    Fanfare = OOT_AUDIO_PLAYER_FANFARE,
    Sfx = OOT_AUDIO_PLAYER_SFX,
    Sub = OOT_AUDIO_PLAYER_SUB,
};

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

inline bool sequence_prewarm(std::uint16_t sequenceId) noexcept {
    return oot_audio_sequence_prewarm(sequenceId);
}

inline bool sequence_play(Player player, std::uint16_t sequenceId,
                          std::uint16_t fadeInMs = 0u) noexcept {
    return oot_audio_sequence_play(native_player(player), sequenceId,
                                   fadeInMs);
}

inline bool nature_play(Player player, std::uint8_t ambienceId,
                        std::uint16_t fadeInMs = 0u) noexcept {
    return oot_audio_nature_play(native_player(player), ambienceId, fadeInMs);
}

inline void sequence_stop(Player player,
                          std::uint16_t fadeOutMs = 0u) noexcept {
    oot_audio_sequence_stop(native_player(player), fadeOutMs);
}

inline void sequence_pause(Player player, bool paused) noexcept {
    oot_audio_sequence_pause(native_player(player), paused);
}

inline void sequence_set_volume(Player player, float volume) noexcept {
    oot_audio_sequence_set_volume(native_player(player), volume);
}

inline void sequence_set_io(Player player, std::uint8_t port,
                            std::int8_t value) noexcept {
    oot_audio_sequence_set_io(native_player(player), port, value);
}

inline void channel_set_io(Player player, std::uint8_t channel,
                           std::uint8_t port, std::int8_t value) noexcept {
    oot_audio_channel_set_io(native_player(player), channel, port, value);
}

inline bool sequence_state(Player player, OoTAudioState &state) noexcept {
    state = OoTAudioState{};
    state.structSize = sizeof(state);
    state.version = OOT_AUDIO_STATE_VERSION;
    return oot_audio_sequence_get_state(native_player(player), &state);
}

inline void set_master_volume(float volume) noexcept {
    oot_audio_set_master_volume(volume);
}

inline void stop_all(std::uint16_t fadeOutMs = 0u) noexcept {
    oot_audio_stop_all(fadeOutMs);
}

inline std::uint32_t render_f32(float *interleavedStereo,
                                std::uint32_t frames,
                                std::uint32_t sampleRate) noexcept {
    return oot_audio_render_f32(interleavedStereo, frames, sampleRate);
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

inline bool sfx_play(std::uint16_t sfxId, float pan = 0.0f,
                     float volume = 1.0f) noexcept {
    return oot_audio_sfx_play(sfxId, pan, volume);
}

inline void sfx_stop(std::uint16_t sfxId) noexcept {
    oot_audio_sfx_stop(sfxId);
}

inline void sfx_stop_all() noexcept {
    oot_audio_sfx_stop_all();
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
