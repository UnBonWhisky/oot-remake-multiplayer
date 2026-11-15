// SPDX-License-Identifier: GPL-3.0-or-later
// One talker per NPC. While a player talks to an NPC, another player's "talk" to the same NPC (same placement
// hash, same scene) is refused in that player's game, with the line "<name> is already talking to this NPC"
// ("<nom> parle déjà à ce personnage" in a French game) instead of a second conversation.
//
// Read from the lifted code by a research sub-agent (not by a run): an NPC's talk goes through main+0xbd1670
// (x0 = the NPC's event entry component, owner actor at [x0+0x18]; w1 = the entry type, 2 = OnPlayerTalk; x2 = the
// other actor); it builds the event's name and asks for the event (0x4aa890 -> 0xd79f10, 0xf61460, 0xd7a820) and
// returns whether it was asked. Its talk call site ignores the answer, so leaving the call out (answer 0) starts
// nothing. A talk lasts while our Link is in an event (the reading's event flag).
//
// Our talk is said as PLAYER_EXTRA kind 5 (u8 talking, u64 the NPC's placement hash, the scene's name), again every
// 2 s while it lasts and once more as 0 when it ends; another player's talk counts for 6 s unless said again.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace CoopNx2navi::TalkLock {

inline constexpr std::uint8_t kExtraTalk = 5;
inline constexpr std::uint64_t kAgainUs = 2'000'000;
inline constexpr std::uint64_t kHoldUs = 6'000'000;
// After our talk request, the reading must show the event within this long, or the talk did not start.
inline constexpr std::uint64_t kStartUs = 3'000'000;

struct Note {
    std::string scene;
    std::uint64_t hash = 0;
    bool talking = false;
};

std::vector<std::uint8_t> Encode(const Note& n);
std::optional<Note> Decode(const std::vector<std::uint8_t>& data);

// The refusal's text in the game's language.
std::string Refusal(const std::string& name, bool french);

class Lock {
public:
    // Our scene; a new one forgets everything.
    void SetScene(const std::string& scene);
    // The hook (game thread): our Link asks to talk to the NPC `hash`. Returns the number of the player who talks
    // to it now (the call is then left out), or 0 when the talk may start (it is then ours).
    std::uint8_t Request(std::uint64_t hash, std::uint64_t now_us);
    // The frame: whether our Link is in an event now. Ends our talk when the event ends.
    void Event(bool running, std::uint64_t now_us);
    // Another player's note.
    void Remote(std::uint8_t player, const Note& n, std::uint64_t now_us);
    // What to send now.
    std::vector<Note> TakeSends(std::uint64_t now_us);
    // Players whose talk refused ours since the last ask (one entry per refusal, at most one a second).
    std::vector<std::uint8_t> TakeRefusals();
    std::vector<std::string> TakeLines();
    std::uint64_t Talking() const;  // the NPC we talk to, 0 none
    // The NPCs (placement hashes) another player talks to now.
    std::vector<std::uint64_t> Others(std::uint64_t now_us) const;

private:
    mutable std::mutex mutex_;
    std::string scene_;
    std::uint64_t ours_ = 0;          // our NPC's hash while our talk lasts (or is starting)
    std::uint64_t asked_us_ = 0;      // when our talk was asked
    bool event_seen_ = false;         // our event began after the ask
    std::uint64_t sent_us_ = 0;
    bool said_ = false;
    std::uint64_t ended_ = 0;         // a talk that ended, to say 0 for
    std::map<std::uint64_t, std::map<std::uint8_t, std::uint64_t>> remote_;  // hash -> player -> until
    std::vector<std::uint8_t> refusals_;
    std::uint64_t refused_us_ = 0;
    std::vector<std::string> lines_;
};

} // namespace CoopNx2navi::TalkLock
