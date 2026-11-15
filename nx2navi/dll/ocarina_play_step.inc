// SPDX-License-Identifier: GPL-3.0-or-later
// --- Another player's Ocarina notes at their puppet (ocarina_play.hpp): included by upstream.cpp ---
// Each note is played by the game's own 2D note player (main+0xa44760, as for our Link's notes), its volume set
// from the distance between the friend's puppet and our Link every frame step while it sounds.
std::atomic<bool> g_ocarina_play_on{true};
std::atomic<std::uint64_t> g_ocarina_played{0}, g_ocarina_stopped{0};
struct OcarinaPlayBox {
    std::mutex mutex;
    struct Want {
        int player = 0;
        int note = 0;
        std::uint64_t at_ms = 0;
    };
    std::vector<Want> wanted;  // any thread
    // the calc thread
    enum class Step { idle, allocating, starting, stopping } step = Step::idle;
    std::uint64_t block = 0;
    OcarinaPlay::Held held;
    bool busy = false;  // an action is under way
    int player = 0;
    bool stop = false;
    int play = 0;
    float gain = 1.0f;  // of the note being started
    std::string said;
    struct Sounding {
        std::uint64_t object = 0;  // the game's pooled sound object, 0 none
        std::uint32_t id = 0;      // valid while [object + kNoteId] is this
        float base = 1.0f;         // its volume at gain 1
        float last_gain = -1.0f;
    };
    std::array<Sounding, OcarinaPlay::kPlayers> sounding{};
};
OcarinaPlayBox g_oplay;
constexpr int kOcarinaPlayOrder = 57;  // before the puppet voices (58)

void QueuePuppetNote(std::uint8_t player, std::uint8_t note) {
    if (!g_ocarina_play_on.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock(g_oplay.mutex);
    if (g_oplay.wanted.size() < 64) {
        g_oplay.wanted.push_back({player, note, SteadyMs()});
    }
}

std::optional<CoopPuppet::Call> OcarinaPlayStep(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                                std::uint64_t now_ms) {
    namespace O = OcarinaPlay;
    OcarinaPlayBox& v = g_oplay;
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    if (main == 0) {
        return std::nullopt;
    }
    auto u32at = [&guest](std::uint64_t at) {
        std::uint32_t w = 0;
        return at >= 0x10000 && guest.Read(at, &w, 4) ? w : 0xffffffffu;
    };
    auto say = [&v = g_oplay](const std::string& line) {
        if (line != v.said) {
            v.said = line;
            LOG_INFO(Core, "coop: ocarina-play: {}", line);
        }
    };
    // The live puppet of a player, 0 none.
    auto puppet = [](int player) {
        std::uint64_t actor = 0;
        for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
            if (slot.number == player && slot.phase == CoopPuppet::Phase::live) {
                actor = slot.actor;
            }
        }
        return actor;
    };
    // How far a player's puppet is from our Link (metres), or a negative number when unknown.
    auto distance = [&](int player) -> float {
        const std::uint64_t actor = puppet(player);
        const std::uint64_t link = CoopPose::FindPlayer(guest, main);
        std::array<float, 3> a{}, b{};
        if (actor == 0 || link == 0 || !guest.Read(actor + CoopPuppet::addr::kActorPosition, a.data(), 12) ||
            !guest.Read(link + CoopPuppet::addr::kActorPosition, b.data(), 12)) {
            return -1.0f;
        }
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    if (after_call) {
        const auto done = v.step;
        v.step = OcarinaPlayBox::Step::idle;
        switch (done) {
        case OcarinaPlayBox::Step::allocating:
            v.block = result;
            break;
        case OcarinaPlayBox::Step::stopping:
            v.sounding[v.player] = {};
            v.stop = false;
            g_ocarina_stopped.fetch_add(1, std::memory_order_relaxed);
            if (v.play == 0) {
                v.busy = false;
            }
            break;
        case OcarinaPlayBox::Step::starting: {
            v.busy = false;
            if (result == 0) {
                say(fmt::format("the game's note player refused '{}' for P{}", CoopOcarinaSound::AssetName(v.play),
                                v.player));
                break;
            }
            OcarinaPlayBox::Sounding& n = v.sounding[v.player];
            n.object = result;
            n.id = u32at(result + O::kNoteId);
            float volume = 0.0f;
            guest.Read(result + O::kNoteVolume, &volume, 4);
            n.base = v.gain > 0.0f ? volume / v.gain : volume;
            n.last_gain = v.gain;
            g_ocarina_played.fetch_add(1, std::memory_order_relaxed);
            if (g_voice_debug || g_ocarina_played.load() <= 20) {
                LOG_INFO(Core, "coop: ocarina-play: P{}'s note '{}' plays: volume {:.3f} (gain {:.2f} of {:.3f})",
                         v.player, CoopOcarinaSound::AssetName(v.play), volume, v.gain, n.base);
            }
            break;
        }
        case OcarinaPlayBox::Step::idle:
            break;
        }
        return std::nullopt;
    }
    if (!v.busy) {
        // The notes that sound: the volume follows the puppet's distance; one that ended by itself is forgotten.
        for (int p = 1; p < O::kPlayers; ++p) {
            OcarinaPlayBox::Sounding& n = v.sounding[p];
            if (n.object == 0) {
                continue;
            }
            if (u32at(n.object + O::kNoteId) != n.id) {
                n = {};
                continue;
            }
            const float d = distance(p);
            const float g = d < 0.0f ? 0.0f : O::Gain(d);
            if (std::fabs(g - n.last_gain) > 0.01f) {
                const float volume = n.base * g;
                guest.Write(n.object + O::kNoteVolume, &volume, 4);
                n.last_gain = g;
            }
        }
        std::lock_guard lock(v.mutex);
        while (!v.wanted.empty() && now_ms - v.wanted.front().at_ms > 1000) {  // a load: not played late
            v.wanted.erase(v.wanted.begin());
        }
        if (!v.wanted.empty()) {
            const auto w = v.wanted.front();
            v.wanted.erase(v.wanted.begin());
            const O::Action a = v.held.Note(w.player, w.note, now_ms);
            v.player = w.player;
            v.stop = a.stop && v.sounding[w.player].object != 0;
            v.play = a.play;
            v.busy = v.stop || a.play != 0;
        } else if (const auto p = v.held.Stale(now_ms)) {
            v.player = *p;
            v.stop = v.sounding[*p].object != 0;
            v.play = 0;
            v.busy = v.stop;
        }
    }
    if (!v.busy) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    if (v.block == 0) {
        c.function = main + CoopPuppet::addr::kMalloc;
        c.x[0] = O::kBlock;
        v.step = OcarinaPlayBox::Step::allocating;
        return c;
    }
    if (v.stop) {
        const OcarinaPlayBox::Sounding& n = v.sounding[v.player];
        if (n.object == 0 || u32at(n.object + O::kNoteId) != n.id) {  // ended already
            v.sounding[v.player] = {};
            v.stop = false;
            if (v.play == 0) {
                v.busy = false;
                return std::nullopt;
            }
        } else {
            c.function = main + O::kStopNote;
            c.x[0] = n.object;
            v.step = OcarinaPlayBox::Step::stopping;
            return c;
        }
    }
    if (v.play < 1 || v.play > O::kNotes) {
        v.busy = false;
        return std::nullopt;
    }
    const float d = distance(v.player);
    if (d < 0.0f) {
        say(fmt::format("P{} has no live puppet here: its note is not played", v.player));
        v.busy = false;
        return std::nullopt;
    }
    v.gain = O::Gain(d);
    if (v.gain <= 0.0f) {
        say(fmt::format("P{}'s puppet is {:.0f} m away, beyond hearing ({:.0f} m): its note is not played", v.player,
                        d, O::kHearM));
        v.busy = false;
        return std::nullopt;
    }
    std::array<char, 0x40> text{};
    const std::string name = CoopOcarinaSound::AssetName(static_cast<std::uint8_t>(v.play));
    std::memcpy(text.data(), name.data(), std::min<std::size_t>(name.size(), text.size() - 1));
    std::array<std::uint8_t, 0x28> args{};
    const std::uint64_t name_at = v.block + O::kText;
    const float fade = 0.0f;
    const std::uint32_t minus1 = 0xffffffffu;
    std::memcpy(args.data() + 0x08, &name_at, 8);
    std::memcpy(args.data() + 0x18, &fade, 4);
    std::memcpy(args.data() + 0x1c, &v.gain, 4);
    std::memcpy(args.data() + 0x24, &minus1, 4);
    if (!guest.Write(v.block + O::kText, text.data(), text.size()) ||
        !guest.Write(v.block + O::kArgs, args.data(), args.size())) {
        v.busy = false;
        return std::nullopt;
    }
    c.function = main + O::kStartNote;
    c.x[0] = v.block + O::kArgs;
    v.step = OcarinaPlayBox::Step::starting;
    return c;
}
