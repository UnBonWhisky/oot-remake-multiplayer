// SPDX-License-Identifier: GPL-3.0-or-later
//
// NPC talk progress shared (docs/flags.md, "Quests and major unlocks"): what an
// NPC says next depends on the saved Bools `HaveTalked_Special.<event>` its
// event flows set (402 names in Event/EventFlow). They are the members of the
// GameData Struct `HaveTalked_Special` (0xab601492), but each member is also a
// plain saved Bool of the flag store, keyed by the Murmur3 of its full dotted
// name (checked against GameDataList: all 402 are Bools with SaveFileIndex 0).
// So sharing them needs no struct access: they join the shared flags, and a
// talk one player had moves every game's NPC on to its next lines.
//
// On in a build that defines LICHII_COOP_SHARE_TALKS=1 (the NX2navi build);
// the emulator and Hookshot builds are unchanged. LICHII_COOP_TALKS=0 turns it
// off. Tested in nx2navi/tests/nx2navi_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "coop/host/flags.hpp"

namespace CoopTalks {

struct Talk {
    std::uint32_t hash;
    const char* name;
};

// The 402 talk flags, sorted by hash.
std::span<const Talk> Talks();

// Adds them to `shared` when `on`; returns how many it added.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// The build's default (LICHII_COOP_SHARE_TALKS) unless LICHII_COOP_TALKS says 0 or 1.
bool On();

// Merge, logged.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

}  // namespace CoopTalks
