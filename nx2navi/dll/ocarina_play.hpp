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
// - Seen in run ep21: emitted on the puppet's own user, the note was taken (an event came back) but that user
//   played nothing. So the note is emitted on U itself, with U's performer (module+0x48, set by main+0x335b3f0,
//   which also looks up the instrument in the actor's equipment) made the puppet for that note and our Link again
//   right after: the notes come from the puppet's head.
// So: the note's asset is searched once in U's resource (main+0x43db00), emitted on U with a handle of that
// player's, and stopped by that handle when the player lets go (note 0), plays the next note, or
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
inline constexpr std::uint64_t kSetPerformer = 0x335b3f0;  // (module, actor): whose head the notes come from
inline constexpr int kNotes = 19;
inline constexpr int kPlayers = 17;
// The scratch block: the asset name, the 19 assets once found, one handle per player.
inline constexpr std::uint64_t kText = 0x00;
inline constexpr std::uint64_t kAssets = 0x40;
inline constexpr std::uint64_t kHandles = 0x100;
inline constexpr std::uint64_t kBlock = 0x200;
inline constexpr std::uint64_t kHoldMs = 4000;  // a note held longer than any song's with no word is let go

// Since 2026-11-15 (runs sp-oc1 to sp-oc4 and two research sub-agents reading the lifted code): the emit on the
// `Ocarina` user made no audio voice because that user's root matrix (module + kRootMatrix) was zero: the module's
// update writes it (main+0x335aee0, the performer's head) only while our own Link performs. Writing the friend's
// puppet there before the emit, and every frame step while the note sounds, gives a voice placed at the puppet: in
// sp-oc4, at 3 m in front 0.61 / 0.62 to the two front speakers (our Link's own note 0.71 / 0.71, through the
// game's 2D note player), 25 m 0.12, 4 m to the right 0.28 / 0.93, to the left 0.95 / 0.31, 60 m 0.02. A note of a
// puppet kHearM or more away is not played. The game's 2D note player (main+0xa44760, kStartNote, ...) started no
// voice when called for another player (sp-oc3); its addresses are kept for the record.
inline constexpr std::uint64_t kRootMatrix = 0xb0;  // 3 rows of 4 floats, the place in the 4th column
inline constexpr float kHeadUp = 1.3f;              // metres from a puppet's feet to its head, about
inline constexpr std::uint64_t kStartNote = 0xa44760;
inline constexpr std::uint64_t kStopNote = 0x3a49ea0;
inline constexpr std::uint64_t kNoteId = 0x10;
inline constexpr std::uint64_t kNoteVolume = 0x64;
inline constexpr std::uint64_t kArgs = 0x1a0;  // in the scratch block, 0x28 bytes
inline constexpr float kNearM = 4.0f;
inline constexpr float kHearM = 40.0f;
// The note's volume factor for a puppet `metres` from our Link: 1 within kNearM, falling to 0 at kHearM.
inline float Gain(float metres) {
    if (!(metres > kNearM)) return 1.0f;
    if (metres >= kHearM) return 0.0f;
    const float f = 1.0f - (metres - kNearM) / (kHearM - kNearM);
    return f * f;
}

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
