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

// --- Since the evening of 2026-11-14: every banc entity, not only the live actors, both ways ---
//
// The Kokiri Forest exit wall (`FldObj_AirWall_Player_GenerateNavMesh`, Presence on
// KokiriForest_Npc_Kokiri_007_Child_Blocker_00) has no actor: its collision is a body of the area's static compound,
// so the actor walk above never saw it. Read from the lifted code by a research sub-agent (not by a run):
// - every banc entity of the loaded areas: A = [main+0x4e91940]; two arrays of records (count s32 [A+0xc8], array
//   [A+0xd0]; count s32 [A+0xd8], array [A+0xe0]), a record per banc file, stride 0x58, loaded when
//   (u32 [rec+0x50] & 0x27) == 1; its entities inline at [rec+8], stride 0x100, min(s32 [rec], u32 [rec+0x14]) of
//   them. Entity: +0x8 name hash, +0xc Presence flag, +0x14 bits, +0x18 place (3 floats), +0x60 actor (bit 1:
//   being deleted), +0x88 placement hash, +0x98 absent reasons.
// - main+0x2c2f880(entity, bit, set) is the game's own "absent reason" toggle: an atomic OR / AND of 1 << bit into
//   +0x98, then "became absent" main+0x6f3fd0 (takes the static compound's bodies out of the world, hides the
//   placement's render object, tells the listeners) or "became present" main+0x6f1ce0 (the mirror), when the
//   state changed. Both take the static compound's lock themselves; the game calls them from the calc thread.
// So the port now walks the entities and toggles bit 1 (Presence) as the shared gate says, both ways: a gate
// opened by anyone goes in every game, and a gate the group's flags (the host's save) have closed comes back.
inline constexpr std::uint64_t kEntityTable = 0x4e91940;
inline constexpr std::uint64_t kToggleCall = 0x2c2f880;  // (entity, bit, set)
inline constexpr std::uint32_t kPresenceBit = 1;         // 1 << 1 == kAbsentPresence
inline constexpr std::uint64_t kRecordSize = 0x58;
inline constexpr std::uint64_t kEntitySize = 0x100;
inline constexpr std::uint64_t kEntityActor = 0x60;
inline constexpr std::uint64_t kEntityPlace = 0x18;
inline constexpr std::uint32_t kBitLazy = 0x4;
inline constexpr float kBlockerRange = 6.0f;  // a blocker NPC stands on its wall (Kokiri 007: 0.6 m from it)

inline bool RecordLoaded(std::uint32_t state) { return (state & 0x27) == 1; }

enum class Gate { keep, leave, back };

// What to do with a placement whose Presence names a shared story gate, from the gate's value now. A lazy
// placement is left to the game (it re-checks those itself while the area stays loaded).
inline Gate GateAction(std::uint32_t bits, std::uint32_t absent, bool flag) {
    if ((bits & kBitLazy) != 0) {
        return Gate::keep;
    }
    const bool present = PresentNow(bits, flag);
    if (!present && (absent & kAbsentPresence) == 0) {
        return Gate::leave;
    }
    if (present && (absent & kAbsentPresence) != 0) {
        return Gate::back;
    }
    return Gate::keep;
}

// A blocker NPC (no Presence of its own, its schedule picks "stand in the way" or "stand aside" from the gate's
// flag) that stands on a wall whose gate just changed: it is made again (its absent reason set, then cleared a
// second later), and the new actor reads its schedule from the flags as they are now.
inline bool IsBlockerNear(const float wall[3], const float npc[3]) {
    const float dx = wall[0] - npc[0], dy = wall[1] - npc[1], dz = wall[2] - npc[2];
    return dx * dx + dy * dy + dz * dz <= kBlockerRange * kBlockerRange;
}

}  // namespace Presence
