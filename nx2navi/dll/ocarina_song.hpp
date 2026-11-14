// SPDX-License-Identifier: GPL-3.0-or-later
// Which song another player just played, from the Ocarina notes their game sends (OCARINA_NOTE, the note's index
// in the game's note table plus one: lichii/hook/coop_ocarina_sound.h; the five notes are D 4, F 7, A 11, B 13 and
// the high D 16). A song is the last notes played matching one of the game's songs (Ocarina of Time's, as the
// game's own prompt "L R Y X A" gives the five notes from low to high). Note 0 (let go) is not a note.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopNx2navi::OcarinaSong {

inline constexpr std::uint8_t D = 4, F = 7, A = 11, B = 13, D2 = 16;

struct Song {
    const char* en;
    const char* fr;
    std::vector<std::uint8_t> notes;
};

inline const std::vector<Song>& Songs() {
    static const std::vector<Song> k = {
        {"Zelda's Lullaby", "la Berceuse de Zelda", {B, D2, A, B, D2, A}},
        {"Epona's Song", "le Chant d'Epona", {D2, B, A, D2, B, A}},
        {"Saria's Song", "le Chant de Saria", {F, A, B, F, A, B}},
        {"the Sun's Song", "le Chant du Soleil", {A, F, D2, A, F, D2}},
        {"the Song of Time", "le Chant du Temps", {A, D, F, A, D, F}},
        {"the Song of Storms", "le Chant des Temp\xc3\xaates", {D, F, D2, D, F, D2}},
        {"the Minuet of Forest", "le Menuet des Bois", {D, D2, B, A, B, A}},
        {"the Bolero of Fire", "le Bol\xc3\xa9ro du Feu", {F, D, F, D, A, F, A, F}},
        {"the Serenade of Water", "la S\xc3\xa9r\xc3\xa9nade de l'Eau", {D, F, A, A, B}},
        {"the Nocturne of Shadow", "le Nocturne de l'Ombre", {B, A, A, D, B, A, F}},
        {"the Requiem of Spirit", "le Requiem des Esprits", {D, F, D, A, F, D}},
        {"the Prelude of Light", "le Pr\xc3\xa9lude de la Lumi\xc3\xa8re", {D2, A, D2, A, B, D2}},
    };
    return k;
}

// The notice line for a song.
inline std::string Notice(const std::string& player, const Song& song, bool french) {
    return french ? player + " a jou\xc3\xa9 " + song.fr : player + " played " + song.en;
}

// One player's notes.
class Listener {
public:
    // A note came (0 = let go, ignored). Returns the song completed by it, if any; the notes are then forgotten.
    const Song* Note(std::uint8_t note, std::uint64_t now_us) {
        if (note == 0) {
            return nullptr;
        }
        if (!notes_.empty() && now_us - last_us_ > 6'000'000) {
            notes_.clear();  // a pause of 6 s starts a new tune
        }
        last_us_ = now_us;
        notes_.push_back(note);
        if (notes_.size() > 8) {
            notes_.pop_front();
        }
        for (const Song& s : Songs()) {
            if (notes_.size() < s.notes.size()) {
                continue;
            }
            bool match = true;
            const std::size_t off = notes_.size() - s.notes.size();
            for (std::size_t i = 0; i < s.notes.size() && match; ++i) {
                match = notes_[off + i] == s.notes[i];
            }
            if (match) {
                notes_.clear();
                return &s;
            }
        }
        return nullptr;
    }

private:
    std::deque<std::uint8_t> notes_;
    std::uint64_t last_us_ = 0;
};

}  // namespace CoopNx2navi::OcarinaSong
