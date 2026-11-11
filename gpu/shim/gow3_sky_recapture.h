// SPDX-License-Identifier: GPL-2.0-or-later
// Linux fork: MediEvil's sky light, captured again when its result comes back NaN.
//
// UE4 captures the sky light when a level loads (the scene into a cube, then its diffuse lighting
// into a 9x1 RGBA16F target the game reads back). On the port that capture often came out with
// every value NaN, at random per load: pink, green or white light on Dan, the plaques and the
// bottoms of walls in the Hall of Heroes (worse after an in-game restart), the Hilltop Mausoleum
// washed out, Inside the Asylum neon green. The capture renders the scene lit by the sky light
// itself, so a NaN result also poisons the next capture. Good captures had finite values (pyro:
// every value NaN in the bad loads, finite ones with r.SkylightUpdateEveryFrame=1).
//
// So when the 9x1 result comes back to the game's memory with a NaN or infinite value, the port
// turns r.SkylightUpdateEveryFrame on until a finite result comes back (at most kMaxFrames, then
// kCooldownFrames without), then off: a good load costs nothing, a bad one a few recaptures. r.SkylightUpdateEveryFrame is a reference console variable, a plain
// int in the game's data, which run.py finds in each eboot (ue4_override.skylight_slot) and passes
// as GOW3_UE4_SKYLIGHT_CVAR. GOW3_SKY_RECAPTURE=0 turns this off (the result is left as it is).
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

extern "C" {
unsigned char* runtime_image_base(void);
}

namespace Gow3SkyRecapture {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* off = std::getenv("GOW3_SKY_RECAPTURE");
        return !off || off[0] != '0';
    }();
    return enabled;
}

inline uintptr_t Slot() {
    static const uintptr_t slot = [] {
        const char* env = std::getenv("GOW3_UE4_SKYLIGHT_CVAR");
        const uintptr_t value = env && *env ? std::strtoull(env, nullptr, 0) : 0;
        return value && value + 4 <= 0x6000000 ? value : uintptr_t{0};
    }();
    return slot;
}

inline constexpr uint64_t kMaxFrames = 150; ///< longest recapture run for one bad result
inline constexpr uint64_t kCooldownFrames = 300; ///< after giving up, NaN results are left alone

inline std::mutex targets_mutex;
inline std::set<uint64_t> targets;              ///< guest addresses of the sky's 9x1 results
inline std::atomic<uint64_t> current_frame{0};
inline std::atomic<uint64_t> recapture_until{0}; ///< recapturing until this GPU frame (0: not)
inline std::atomic<uint64_t> cooldown_until{0};  ///< no new recapture run before this frame
inline std::atomic<uint64_t> bad_results{0}, fixed_results{0};

inline void SetEveryFrame(int32_t value) {
    unsigned char* image = runtime_image_base();
    const uintptr_t slot = Slot();
    if (!image || !slot) {
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("Sky light: no game image (%p) or variable (%#llx); NaN results are only "
                        "cleared, not captured again\n",
                        static_cast<void*>(image), static_cast<unsigned long long>(slot));
        }
        return;
    }
    // The eboot's own data (not guest heap memory, which goes through the backing view), as
    // runtime_extra.c reads r.SetRes: a plain store the game thread reads at its next frame.
    __atomic_store_n(reinterpret_cast<int32_t*>(image + slot), value, __ATOMIC_RELEASE);
}

/// The GPU thread binds a 9x1 RGBA16F render target: the sky light's capture result.
inline void OnCaptureTarget(uint64_t address) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{targets_mutex};
    targets.insert(address);
}

inline bool IsTarget(uint64_t address) {
    std::scoped_lock lock{targets_mutex};
    return targets.contains(address);
}

/// A readback of `address` is about to be written to the game's memory: the first `pixels`
/// RGBA16F values of `data` (linear). NaN and infinite channels become 0; a bad result starts
/// recaptures, a finite one ends them. Returns whether anything was changed.
inline bool CheckResult(uint64_t address, void* data, uint64_t size, uint32_t pixels = 9) {
    if (!Enabled() || size < pixels * 8ull || !IsTarget(address)) {
        return false;
    }
    auto* halves = static_cast<uint16_t*>(data);
    bool bad = false;
    for (uint32_t i = 0; i < pixels * 4; ++i) {
        // Exponent all ones: infinite or NaN. Left as it is: UE4 divides by the sky light's
        // brightness, so a 0 in its place made every later capture NaN as well (pyro).
        bad |= (halves[i] & 0x7c00) == 0x7c00;
    }
    const uint64_t frame = current_frame.load(std::memory_order_relaxed);
    if (bad) {
        const uint64_t count = ++bad_results;
        if (!recapture_until.load(std::memory_order_relaxed) &&
            frame >= cooldown_until.load(std::memory_order_relaxed)) {
            SetEveryFrame(1);
            recapture_until.store(frame + kMaxFrames, std::memory_order_relaxed);
        }
        if (count <= 20 || count % 100 == 0) {
            std::printf("Sky light: capture result %#llx had NaN or infinite values (%llu so far); "
                        "capturing again\n",
                        static_cast<unsigned long long>(address),
                        static_cast<unsigned long long>(count));
        }
    } else if (recapture_until.load(std::memory_order_relaxed)) {
        SetEveryFrame(0);
        recapture_until.store(0, std::memory_order_relaxed);
        const uint64_t count = ++fixed_results;
        if (count <= 20 || count % 100 == 0) {
            std::printf("Sky light: capture %#llx came back finite; recapturing stopped (GPU frame "
                        "%llu)\n",
                        static_cast<unsigned long long>(address),
                        static_cast<unsigned long long>(frame));
        }
    }
    return bad;
}

/// Every GPU frame start.
inline void OnFrame(uint64_t frame) {
    current_frame.store(frame, std::memory_order_relaxed);
    const uint64_t until = recapture_until.load(std::memory_order_relaxed);
    if (until && frame >= until) {
        SetEveryFrame(0);
        recapture_until.store(0, std::memory_order_relaxed);
        cooldown_until.store(frame + kCooldownFrames, std::memory_order_relaxed);
        std::printf("Sky light: still NaN after %llu frames of recaptures; stopped\n",
                    static_cast<unsigned long long>(kMaxFrames));
    }
}

} // namespace Gow3SkyRecapture
