// SPDX-License-Identifier: GPL-2.0-or-later
// Linux fork: MediEvil's sky light, captured again shortly after it is first captured.
//
// UE4 captures the sky light once when a level loads (the scene into a cube, then its lighting
// into a 9x1 RGBA16F target). On the port that first capture often came out wrong, at random per
// load: pink, green or white light on Dan, the plaques and the bottoms of walls in the Hall of
// Heroes (and an in-game restart made it worse). Captures after it come out right: with
// r.SkylightUpdateEveryFrame=1 every load was right, at about 9 FPS for as long as it is on.
//
// So when the game draws into a 9x1 RGBA16F target (a sky capture: only at a level load), the
// port turns r.SkylightUpdateEveryFrame on for GOW3_SKY_RECAPTURE_FRAMES GPU frames (90) and off
// again: the level starts with a few seconds of recaptures, then the last, right capture stays.
// r.SkylightUpdateEveryFrame is a reference console variable: the engine reads a plain int in the
// game's data, which run.py finds in each eboot (ue4_override.skylight_slot) and passes as
// GOW3_UE4_SKYLIGHT_CVAR. GOW3_SKY_RECAPTURE=0 turns this off.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
unsigned char* runtime_image_base(void);
}

namespace Gow3SkyRecapture {

inline uintptr_t Slot() {
    static const uintptr_t slot = [] {
        const char* off = std::getenv("GOW3_SKY_RECAPTURE");
        if (off && off[0] == '0') {
            return uintptr_t{0};
        }
        const char* env = std::getenv("GOW3_UE4_SKYLIGHT_CVAR");
        const uintptr_t value = env && *env ? std::strtoull(env, nullptr, 0) : 0;
        return value && value + 4 <= 0x6000000 ? value : uintptr_t{0};
    }();
    return slot;
}

inline uint32_t WindowFrames() {
    static const uint32_t frames = [] {
        const char* env = std::getenv("GOW3_SKY_RECAPTURE_FRAMES");
        const unsigned long value = env && *env ? std::strtoul(env, nullptr, 10) : 90;
        return static_cast<uint32_t>(value ? value : 90);
    }();
    return frames;
}

inline std::atomic<uint64_t> current_frame{0};
inline std::atomic<uint64_t> window_end{0};      ///< recapturing until this GPU frame (0: not)
inline std::atomic<uint64_t> cooldown_until{0};  ///< captures before this frame are our own

inline bool SetEveryFrame(int32_t value) {
    unsigned char* image = runtime_image_base();
    const uintptr_t slot = Slot();
    if (!image || !slot) {
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("Sky light: no game image (%p) or variable (%#llx); no recapture\n",
                        static_cast<void*>(image), static_cast<unsigned long long>(slot));
        }
        return false;
    }
    // The eboot's own data (not guest heap memory, which goes through the backing view), as
    // runtime_extra.c reads r.SetRes: a plain store the game thread reads at its next frame.
    __atomic_store_n(reinterpret_cast<int32_t*>(image + slot), value, __ATOMIC_RELEASE);
    return true;
}

/// The GPU thread binds a 9x1 RGBA16F render target: the game is capturing the sky light.
inline void OnCaptureTarget() {
    if (!Slot()) {
        return;
    }
    const uint64_t frame = current_frame.load(std::memory_order_relaxed);
    if (window_end.load(std::memory_order_relaxed) || frame < cooldown_until.load()) {
        return; // our own recaptures
    }
    if (SetEveryFrame(1)) {
        window_end.store(frame + WindowFrames(), std::memory_order_relaxed);
        std::printf("Sky light: captured at a level load (GPU frame %llu); capturing again for %u "
                    "frames\n",
                    static_cast<unsigned long long>(frame), WindowFrames());
    }
}

/// Every GPU frame start.
inline void OnFrame(uint64_t frame) {
    current_frame.store(frame, std::memory_order_relaxed);
    const uint64_t end = window_end.load(std::memory_order_relaxed);
    if (end && frame >= end) {
        SetEveryFrame(0);
        window_end.store(0, std::memory_order_relaxed);
        cooldown_until.store(frame + 30, std::memory_order_relaxed);
        std::printf("Sky light: recaptures done (GPU frame %llu)\n",
                    static_cast<unsigned long long>(frame));
    }
}

} // namespace Gow3SkyRecapture
