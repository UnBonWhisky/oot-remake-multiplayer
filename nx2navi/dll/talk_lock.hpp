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
//
// One authority (run tk13: two A presses 33 ms apart, before either game had the other's note, both started a
// talk). The host's game (the server's HOST, Client::host_number) decides who talks to an NPC. Its own talk is
// decided at once against what it granted. Another game asks (state 2) and holds the game's talk call for up to
// kGrantWaitUs, until the host's game answers: granted (3) or refused (4, with the player who talks). Without an
// answer in that time (an older build, a slow link) the talk goes ahead as before. The ask and the answers carry
// u8 target (the asker) and u8 holder before the scene; an older build's Decode refuses them (state above 1).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
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
// How long a game other than the host's holds its talk call waiting for the host's answer.
inline constexpr std::uint64_t kGrantWaitUs = 150'000;

enum Kind : std::uint8_t { kNote = 0, kAsk = 2, kGrant = 3, kDeny = 4 };

struct Note {
    std::string scene;
    std::uint64_t hash = 0;
    bool talking = false;      // kNote: talks now, or stopped
    std::uint8_t kind = kNote;
    std::uint8_t target = 0;   // kGrant, kDeny: the player who asked
    std::uint8_t holder = 0;   // kDeny: the player who talks to it
};

std::vector<std::uint8_t> Encode(const Note& n);
std::optional<Note> Decode(const std::vector<std::uint8_t>& data);

// The refusal's text in the game's language.
std::string Refusal(const std::string& name, bool french);

class Lock {
public:
    // Our scene; a new one forgets everything.
    void SetScene(const std::string& scene);
    // Our player number and the host's (0: not known). The host's game decides every talk.
    void SetRoles(std::uint8_t self, std::uint8_t authority);
    // How an ask reaches the host's game at once (any thread; never called with the lock held). Unset: no asks.
    void SetSender(std::function<void(const Note&)> send);
    // The hook (game thread): our Link asks to talk to the NPC `hash`. Returns the number of the player who talks
    // to it now (the call is then left out), or 0 when the talk may start (it is then ours). In a game other than
    // the host's it asks the host's game first and waits for its answer, kGrantWaitUs at most (`wait_us`: tests).
    std::uint8_t Request(std::uint64_t hash, std::uint64_t now_us, std::uint64_t wait_us = kGrantWaitUs);
    // The frame: whether our Link is in an event now. Ends our talk when the event ends.
    void Event(bool running, std::uint64_t now_us);
    // Another player's note. Returns the answer to send when it is an ask and we are the host's game.
    std::optional<Note> Remote(std::uint8_t player, const Note& n, std::uint64_t now_us);
    // What to send now.
    std::vector<Note> TakeSends(std::uint64_t now_us);
    // Players whose talk refused ours since the last ask (one entry per refusal, at most one a second).
    std::vector<std::uint8_t> TakeRefusals();
    std::vector<std::string> TakeLines();
    std::uint64_t Talking() const;  // the NPC we talk to, 0 none
    // The NPCs (placement hashes) another player talks to now.
    std::vector<std::uint64_t> Others(std::uint64_t now_us) const;

private:
    // Who talks to `hash` in `scene` by what the host's game knows, other than `except`; 0 nobody.
    std::uint8_t HolderLocked(const std::string& scene, std::uint64_t hash, std::uint8_t except,
                              std::uint64_t now_us) const;
    mutable std::mutex mutex_;
    std::condition_variable answered_;
    std::uint8_t self_ = 0, authority_ = 0;
    std::function<void(const Note&)> send_;
    std::uint64_t asking_ = 0;        // the NPC our pending ask is for
    std::uint8_t answer_ = 0;         // kGrant, kDeny, 0 none yet
    std::uint8_t answer_holder_ = 0;
    // The host's game: who talks to which NPC in every scene (scene, hash) -> (player, until).
    std::map<std::pair<std::string, std::uint64_t>, std::pair<std::uint8_t, std::uint64_t>> granted_;
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
