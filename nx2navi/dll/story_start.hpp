// SPDX-License-Identifier: GPL-3.0-or-later
// A story cutscene for every player, wherever they are (the user's rule, 2026-11-14): lichii/hook
// coop_shared_scenes.h "everyone mode" (LICHII_COOP_SHARED_SCENES_EVERYONE=1 in this build).
//
// The hook sends its own story cutscene start as DUNGEON_EVENT, which the server relays to the starter's scene
// only. So this port also tells every game: PLAYER_EXTRA kind 6 (relayed as it comes), data = scene, a 0 byte,
// the event flow's name. A game in another scene (or too far) is then brought to the starter by this port's trip
// to another player (teleport.hpp GoTo), which arrives where the starter stands (after their scene, if it moved
// them), and plays the scene there; the hook waits while our game is busy or in a boss fight (up to 180 s).
// `OOT_COOP_SCENES_EVERYONE=0`: the hook's same-scene rule again.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CoopNx2navi::StoryStart {

inline constexpr std::uint8_t kExtraStory = 6;  // PLAYER_EXTRA kind (1 to 5 are taken: puppet_fx, step_switch, talk)
inline constexpr std::size_t kMaxText = 96;

struct Start {
    std::string scene;
    std::string name;
};

inline std::vector<std::uint8_t> Encode(const Start& s) {
    std::vector<std::uint8_t> out(s.scene.begin(), s.scene.end());
    out.push_back(0);
    out.insert(out.end(), s.name.begin(), s.name.end());
    return out;
}

inline std::optional<Start> Decode(const std::vector<std::uint8_t>& data) {
    if (data.size() < 3 || data.size() > 2 * kMaxText) {
        return std::nullopt;
    }
    std::size_t zero = 0;
    while (zero < data.size() && data[zero] != 0) {
        ++zero;
    }
    if (zero == 0 || zero + 1 >= data.size() || zero > kMaxText) {
        return std::nullopt;
    }
    Start s;
    s.scene.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(zero));
    s.name.assign(data.begin() + static_cast<std::ptrdiff_t>(zero) + 1, data.end());
    for (const char c : s.scene + s.name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return std::nullopt;
        }
    }
    return s;
}

}  // namespace CoopNx2navi::StoryStart
