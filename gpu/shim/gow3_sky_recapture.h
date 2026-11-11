// SPDX-License-Identifier: GPL-2.0-or-later
// Linux fork: MediEvil's sky light, captured again after it is first captured at a level load.
//
// UE4 captures the sky light once when a level loads (the scene into a cube, then its lighting
// into a 9x1 RGBA16F target). On the port that first capture often came out wrong, at random per
// load: pink, green or white light on Dan, the plaques and the bottoms of walls in the Hall of
// Heroes (worse after an in-game restart), and the Hilltop Mausoleum washed out. Captures after it
// come out right: with r.SkylightUpdateEveryFrame=1 every load was right (pyro), at about 9 FPS
// for as long as it is on.
//
// So when the game draws into a 9x1 RGBA16F target (a sky capture: only at a level load), the
// port turns r.SkylightUpdateEveryFrame on for short bursts (kBursts: the first 3 seconds, then
// 1 second after 10 and after 30 seconds) and off again: the last capture, taken once the level
// has settled, stays. r.SkylightUpdateEveryFrame is a reference console variable: the engine
// reads a plain int in the game's data, which run.py finds in each eboot
// (ue4_override.skylight_slot) and passes as GOW3_UE4_SKYLIGHT_CVAR. GOW3_SKY_RECAPTURE=0 turns
// this off.
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

/// Recapture bursts after a level load's capture, in GPU frames from it: [start, end). The first
/// covers the level's opening seconds; the later ones take a capture once the level has settled
/// (a single 90-frame burst still left one load of twelve wrong on pyro).
struct Burst {
    uint64_t start, end;
};
inline constexpr Burst kBursts[] = {{0, 90}, {300, 330}, {900, 930}};
inline constexpr uint64_t kScheduleEnd = 930 + 30; ///< then a load's capture is noticed again

inline std::atomic<uint64_t> current_frame{0};
inline std::atomic<uint64_t> schedule_start{0}; ///< GPU frame of the load's capture (0: idle)
inline std::atomic<int32_t> written{0};         ///< the value last stored
inline std::atomic<uint64_t> last_off{0};       ///< GPU frame a burst ended

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
    written.store(value, std::memory_order_relaxed);
    return true;
}

/// The GPU thread binds a 9x1 RGBA16F render target: the game is capturing the sky light.
inline void OnCaptureTarget() {
    if (!Slot()) {
        return;
    }
    const uint64_t frame = current_frame.load(std::memory_order_relaxed);
    // Our own recaptures: during a burst, or the few frames the game takes to see it end. A
    // capture outside them is another level load (a restart soon after the last one).
    if (schedule_start.load(std::memory_order_relaxed) &&
        (written.load(std::memory_order_relaxed) ||
         frame < last_off.load(std::memory_order_relaxed) + 10)) {
        return;
    }
    schedule_start.store(frame ? frame : 1, std::memory_order_relaxed);
    std::printf("Sky light: captured at a level load (GPU frame %llu); capturing again now and "
                "after 10 and 30 seconds\n",
                static_cast<unsigned long long>(frame));
}

/// Every GPU frame start.
inline void OnFrame(uint64_t frame) {
    current_frame.store(frame, std::memory_order_relaxed);
    const uint64_t start = schedule_start.load(std::memory_order_relaxed);
    if (!start || frame < start) {
        return;
    }
    const uint64_t at = frame - start;
    int32_t want = 0;
    for (const Burst& burst : kBursts) {
        want |= at >= burst.start && at < burst.end;
    }
    if (want != written.load(std::memory_order_relaxed)) {
        SetEveryFrame(want);
        if (!want) {
            last_off.store(frame, std::memory_order_relaxed);
        }
    }
    if (at >= kScheduleEnd) {
        schedule_start.store(0, std::memory_order_relaxed);
        std::printf("Sky light: recaptures done (GPU frame %llu)\n",
                    static_cast<unsigned long long>(frame));
    }
}

} // namespace Gow3SkyRecapture
