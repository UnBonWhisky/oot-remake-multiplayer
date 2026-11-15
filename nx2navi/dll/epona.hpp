// SPDX-License-Identifier: GPL-3.0-or-later
// Epona for every adult: the horse itself, not only her name (docs/flags.md, "Epona: the owned horse").
//
// The hook (lichii/hook coop_hook.cpp MendOwnedHorse) writes `Animal_Epona_Adult` into OwnedHorseList.ActorName[0]
// in an adult game whose Get_Epona came Complete from another player. Run ep16 showed it is not enough: the song
// played and no horse came. Read from the lifted code and the event flows by a research sub-agent (not by a run):
// - Epona's Song (Event/EventFlow/Dm_SY_Ocarina_EponaSong) asks "has an owned horse" (the name: now true), "may
//   ride here", the player's state, and then `EventCreateAndCallOwnedHorse`, which calls only a horse ACTOR the
//   OwnedHorseMgr links at +0x70 (+0x78 actor, u32 +0x80 its id, -1 none). Without one it does nothing, silently.
// - That actor is made only when an area (banc) loads (Sequence/OnLoadBancParam: SeqOneShotInitializeOwnedHorseMgr,
//   main+0x32d5b00 -> 0x32a2280), from the saved name. So after the name is written, the next area load brings
//   Epona; until then the song is silent.
// - In the same session: the manager's update (main+0x32a0170, at 0x32a0898) makes `Animal_Epona_Adult` (instance
//   `Animal_Epona_Adult_Owned`) at the place in the 3 floats at mgr+0x50c when the first is not NaN, registers it
//   (main+0x329d990) and sets +0x50c back to NaN. It is reached while +0x3d6 == 1 and the flags word +0x2a0 has
//   neither 0x100 (creation pending) nor 0x8. The floats are read without a lock: write +0x510, +0x514, then +0x50c.
// - The manager: X = [main+0x4e97648], L = [X+0x1f0], u32 [L+0x50] >= 0x12, mgr = [[L+0x58]+0x88]. Its +0x4d8 holds
//   the hash of `OwnedHorseList` (0x4920b64f) and +0x4e0 that of `ActorName` (0xa1ddf1de): checked before any write.
//
// So the port (dll/epona_step.inc) asks for the horse that way, once, beside our Link, when the owned horse's
// name was empty at the start of this session and is set now (the hook wrote it), we are adult, and no horse is
// linked. `OOT_COOP_EPONA_NOW=0` leaves it to the next area load.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Epona {

inline constexpr std::uint64_t kGlobal = 0x4e97648;
inline constexpr std::uint64_t kList = 0x1f0;
inline constexpr std::uint64_t kListCount = 0x50;
inline constexpr std::uint64_t kListArray = 0x58;
inline constexpr std::uint64_t kSlot = 0x88;  // slot 17
inline constexpr std::uint64_t kLinkedId = 0x80;
inline constexpr std::uint64_t kFlags = 0x2a0;
inline constexpr std::uint64_t kReady = 0x3d6;
inline constexpr std::uint64_t kStructHash = 0x4d8;
inline constexpr std::uint64_t kMemberHash = 0x4e0;
inline constexpr std::uint64_t kMakeAt = 0x50c;
inline constexpr std::uint32_t kOwnedHorseList = 0x4920b64fu;
inline constexpr std::uint32_t kActorName = 0xa1ddf1deu;
inline constexpr std::uint32_t kNoHorse = 0xffffffffu;
inline constexpr std::uint32_t kPending = 0x100;
inline constexpr std::uint32_t kBusy = 0x8;
inline constexpr float kBeside = 3.0f;  // metres from our Link

struct Mgr {
    std::uint32_t struct_hash = 0;
    std::uint32_t member_hash = 0;
    std::uint32_t linked = kNoHorse;
    std::uint32_t flags = 0;
    std::uint8_t ready = 0;
    float make_at = NAN;
};

inline bool IsManager(const Mgr& m) { return m.struct_hash == kOwnedHorseList && m.member_hash == kActorName; }

// Whether to ask the manager for the horse now. `name_was_empty`: the owned horse's name was empty when this
// session first read it (so the race never ran in this save and the hook wrote it); `owned`: it is set now.
inline bool ShouldMake(const Mgr& m, bool name_was_empty, bool owned, bool adult) {
    return IsManager(m) && name_was_empty && owned && adult && m.linked == kNoHorse && m.ready == 1 &&
           (m.flags & (kPending | kBusy)) == 0 && std::isnan(m.make_at);
}

// Where the horse is made: kBeside metres from our Link along x, at his height.
inline void PlaceBeside(const float link[3], float out[3]) {
    out[0] = link[0] + kBeside;
    out[1] = link[1];
    out[2] = link[2];
}

}  // namespace Epona
