// SPDX-License-Identifier: GPL-3.0-or-later
// A story gate that another player opened leaves our game at once, not at the next area load (issue: "an NPC
// blocking the way who steps aside for one player must step aside for everyone, at once").
//
// Read from the lifted code by a research sub-agent (not by a run): a placed actor's banc entity is
// [actor+0x3d8] (its placement hash at +0x88). A placement's `Presence` is parsed by main+0x5af0e0: the flag's
// hash at entity+0xc, u32 entity+0x14 bit 1 = IsNegation, bit 2 = IsCheckLazy; the result is bit 0x2 of the
// "absent reasons" word u32 entity+0x98 (any bit of 0x800783 = not present). Only a lazy placement is checked
// again while the area stays loaded (main+0x9ca80); every other one, such as the air walls at the Kokiri Forest
// exit and on Mido's path, only when the area loads. The game's own removal (main+0x5affd0, 0x9ccb8): atomic OR
// 0x2 into entity+0x98, and when no reason was set before, main+0x6f3fd0(entity).
//
// So, once a second, for each live actor whose Presence names a shared story gate (CoopStory kind `world`) that
// now says "absent" while the actor is still there, this port does what the game does. Only that direction: a
// gate that comes back (the host loads an older save) comes back with the next area load, as in one game.
#pragma once

#include <cstdint>

namespace Presence {

inline constexpr std::uint64_t kActorEntity = 0x3d8;
inline constexpr std::uint64_t kEntityFlag = 0xc;
inline constexpr std::uint64_t kEntityBits = 0x14;
inline constexpr std::uint64_t kEntityAbsent = 0x98;
inline constexpr std::uint64_t kEntityHash = 0x88;
inline constexpr std::uint32_t kBitNegation = 0x2;
inline constexpr std::uint32_t kAbsentPresence = 0x2;
inline constexpr std::uint32_t kAbsentAny = 0x800783;
inline constexpr std::uint64_t kRemoveCall = 0x6f3fd0;  // (entity): became absent

// What a placement's Presence says now, from its flag's value.
inline bool PresentNow(std::uint32_t bits, bool flag) {
    return flag != ((bits & kBitNegation) != 0);
}

// Whether the actor should leave now: its gate says absent and the game has not marked it so.
inline bool ShouldLeave(std::uint32_t bits, std::uint32_t absent, bool flag) {
    return !PresentNow(bits, flag) && (absent & kAbsentPresence) == 0;
}

// After the OR of kAbsentPresence returned `old`: whether the game's "became absent" call is due (no other
// reason had already taken the actor out).
inline bool CallDue(std::uint32_t old) {
    return (old & kAbsentAny) == 0;
}

}  // namespace Presence
