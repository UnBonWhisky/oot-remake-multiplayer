// SPDX-License-Identifier: GPL-3.0-or-later
// Another player's Ocarina notes, heard at their puppet (dll/ocarina_play_step.inc).
//
// Run ep16: the notes came (`ocarina: P2's note 16`) but none was played on the puppet. Read from the game's sound
// resource and the lifted code by a research sub-agent (not by a run):
// - Only the sound user `Ocarina` holds the notes (`Ocarina_LowB` ... `Ocarina_HighF`, 19 of them; bone
//   `Performer_Head`); `Player_Adult`, `Player_Child` and `Player` have none, so a search in the puppet's own user
//   finds nothing. Each note is a looping stream: the game stops it when the button is let go.
// - `Ocarina` is one global user, made at boot by the game's Ocarina sound module (found by its vtable, as
//   lichii/hook coop_puppet_sound.cpp FindOcarinaModule does). The module's +0x40 is a wrapper R; the game's own
//   song player gets the user with U = [[R]+0x20](R), and the accessor is [[U+0x58]+0x18].
// - The emit (main+0x440630) uses only the asset pointer, not the user's own tables: an asset found in U's
//   resource can be emitted on the puppet's sound user, and the game's 3D sound system then places it at the
//   puppet, with the user's own distance attenuation.
// - A note is stopped with main+0x7cc120(&handle).
// So: the note's asset is searched once in U's resource (main+0x43db00), emitted on the puppet's user with a
// handle of that player's, and stopped by that handle when the player lets go (note 0), plays the next note, or
// sends nothing for kHoldMs. `OOT_COOP_OCARINA_PLAY=0` turns it off.
#pragma once

#include <cstdint>
#include <optional>

namespace OcarinaPlay {

inline constexpr std::uint64_t kModuleWrapper = 0x40;
inline constexpr std::uint64_t kWrapperGetUser = 0x20;  // vtable slot: U = f(R)
inline constexpr std::uint64_t kSearchAsset = 0x43db00;
inline constexpr std::uint64_t kEmit = 0x440630;
inline constexpr std::uint64_t kKill = 0x7cc120;
inline constexpr int kNotes = 19;
inline constexpr int kPlayers = 17;
// The scratch block: the asset name, the 19 assets once found, one handle per player.
inline constexpr std::uint64_t kText = 0x00;
inline constexpr std::uint64_t kAssets = 0x40;
inline constexpr std::uint64_t kHandles = 0x100;
inline constexpr std::uint64_t kBlock = 0x200;
inline constexpr std::uint64_t kHoldMs = 4000;  // a note held longer than any song's with no word is let go

inline std::uint64_t AssetSlot(int note) { return kAssets + 8 * static_cast<std::uint64_t>(note - 1); }
inline std::uint64_t HandleSlot(int player) { return kHandles + 8 * static_cast<std::uint64_t>(player); }

// What one player's note asks of the sound system now.
struct Action {
    bool stop = false;  // stop the note this player holds
    int play = 0;       // then play this note (1..kNotes), 0 none
};

// The notes held per player: what an incoming note (0 = let go) does.
class Held {
public:
    Action Note(int player, int note, std::uint64_t now_ms) {
        Action a;
        if (player <= 0 || player >= kPlayers) {
            return a;
        }
        a.stop = held_[player] != 0;
        held_[player] = 0;
        if (note >= 1 && note <= kNotes) {
            a.play = note;
            held_[player] = note;
            since_[player] = now_ms;
        }
        return a;
    }
    // A held note whose player sent nothing for kHoldMs: the player to stop, or nothing.
    std::optional<int> Stale(std::uint64_t now_ms) {
        for (int p = 1; p < kPlayers; ++p) {
            if (held_[p] != 0 && now_ms - since_[p] > kHoldMs) {
                held_[p] = 0;
                return p;
            }
        }
        return std::nullopt;
    }
    int HeldBy(int player) const { return player > 0 && player < kPlayers ? held_[player] : 0; }

private:
    int held_[kPlayers] = {};
    std::uint64_t since_[kPlayers] = {};
};

}  // namespace OcarinaPlay
