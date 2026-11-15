// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the present-time frame of the hook spends its time, by part. Off
// unless the build defines LICHII_COOP_FRAME_PROFILE=1 (the NX2navi port
// does): then each part's time is summed, and the port logs the sums. In
// every other build the marks compile to nothing.
//
// Use: a Lap at the start of a frame, then lap.Mark(part) after each part;
// the time since the previous mark is that part's.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace CoopFrameProfile {

enum Part : int {
    kRead,        // the player reader (Read)
    kFlags,       // the flag stores, mends, catch-ups
    kSession,     // session_->frame (flag watching, presence, sends)
    kEvents,      // live events, shared scenes (FindRunningEvent)
    kNotices,     // notices, the save, chests, one-way restores
    kPuppet,      // puppets, voice, the Ocarina, Navi, rides
    kObjects,     // CoopObjects::Share (the object walk)
    kHud,         // hearts
    kSpawn,       // spawns, drops, time
    kBoss,        // boss rooms
    kPort,        // the port's own frame (CoopTeleport::Frame: hooks.frame)
    kPvp,         // PvP
    kPlace,       // markers and name tags (Place)
    kObjectsPre,  // CoopObjects::OnFrame, CoopTestInput::OnFrame
    kCount
};

inline const char* Name(int part) {
    constexpr const char* kNames[kCount] = {"read",  "flags", "session", "events", "notices",
                                            "puppet", "objects", "hud",  "spawn",  "boss",
                                            "port",  "pvp",   "place",   "objects-pre"};
    return part >= 0 && part < kCount ? kNames[part] : "?";
}

#if defined(LICHII_COOP_FRAME_PROFILE) && LICHII_COOP_FRAME_PROFILE

struct Sums {
    std::array<std::atomic<std::uint64_t>, kCount> ns{};
    std::atomic<std::uint64_t> frames{0};
};

inline Sums& TheSums() {
    static Sums sums;
    return sums;
}

inline constexpr bool kOn = true;

class Lap {
public:
    Lap() : last_(std::chrono::steady_clock::now()) {}
    void Mark(Part part) {
        const auto now = std::chrono::steady_clock::now();
        TheSums().ns[part].fetch_add(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_).count()),
            std::memory_order_relaxed);
        last_ = now;
    }
    void Frame() { TheSums().frames.fetch_add(1, std::memory_order_relaxed); }

private:
    std::chrono::steady_clock::time_point last_;
};

// The sums since the last Take, in nanoseconds, and the frames counted; zeroed.
struct Taken {
    std::array<std::uint64_t, kCount> ns{};
    std::uint64_t frames = 0;
};
inline Taken Take() {
    Taken t;
    for (int i = 0; i < kCount; ++i) {
        t.ns[i] = TheSums().ns[i].exchange(0, std::memory_order_relaxed);
    }
    t.frames = TheSums().frames.exchange(0, std::memory_order_relaxed);
    return t;
}

#else

inline constexpr bool kOn = false;

class Lap {
public:
    void Mark(Part) {}
    void Frame() {}
};

struct Taken {
    std::array<std::uint64_t, kCount> ns{};
    std::uint64_t frames = 0;
};
inline Taken Take() { return {}; }

#endif

} // namespace CoopFrameProfile
