// SPDX-License-Identifier: GPL-3.0-or-later
// --- Another player's Ocarina notes at their puppet (ocarina_play.hpp): included by upstream.cpp ---
// Each note is emitted on the game's global `Ocarina` sound user, whose root matrix (module + kRootMatrix) is kept at
// the friend's puppet while a note sounds: the game refreshes that matrix itself only while our own Link performs.
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
    enum class Step { idle, allocating, user, searching, stopping, emitting } step = Step::idle;
    std::uint64_t block = 0;
    std::uint64_t user = 0;      // the game's `Ocarina` sound user
    std::uint64_t accessor = 0;  // its resource's accessor
    std::uint64_t module = 0;    // the game's Ocarina sound module
    std::uint64_t user_next_ms = 0;
    bool asset_known[OcarinaPlay::kNotes + 1] = {};
    bool asset_missing[OcarinaPlay::kNotes + 1] = {};
    OcarinaPlay::Held held;
    bool busy = false;  // an action is under way
    int player = 0;
    bool stop = false;
    int play = 0;
    int placed = 0;  // the player whose puppet the user's root matrix follows now, 0 none
    std::string said;
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
    auto word = [&guest](std::uint64_t at) {
        std::uint64_t w = 0;
        return at >= 0x10000 && guest.Read(at, &w, 8) ? w : 0;
    };
    auto say = [&v = g_oplay](const std::string& line) {
        if (line != v.said) {
            v.said = line;
            LOG_INFO(Core, "coop: ocarina-play: {}", line);
        }
    };
    auto puppet = [](int player) {
        std::uint64_t actor = 0;
        for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
            if (slot.number == player && slot.phase == CoopPuppet::Phase::live) {
                actor = slot.actor;
            }
        }
        return actor;
    };
    // The user's root matrix at a player's puppet (its rotation, its place raised to the head). False: no puppet.
    auto place = [&](int player) {
        const std::uint64_t actor = puppet(player);
        std::array<float, 9> rot{};
        std::array<float, 3> pos{};
        if (actor == 0 || v.module == 0 || !guest.Read(actor + CoopPuppet::addr::kActorRotation, rot.data(), 36) ||
            !guest.Read(actor + CoopPuppet::addr::kActorPosition, pos.data(), 12)) {
            return false;
        }
        const std::array<float, 12> m{rot[0], rot[1], rot[2], pos[0], rot[3], rot[4], rot[5], pos[1] + O::kHeadUp,
                                      rot[6], rot[7], rot[8], pos[2]};
        return guest.Write(v.module + O::kRootMatrix, m.data(), sizeof(m));
    };
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
        case OcarinaPlayBox::Step::user: {
            const std::string name = CoopPuppetSound::UserName(guest, result);
            const std::uint64_t resource = result != 0 ? word(result + CoopPuppetSound::addr::kUserResource) : 0;
            const std::uint64_t accessor =
                resource != 0 ? word(resource + CoopPuppetSound::addr::kResourceAccessor) : 0;
            if (name == "Ocarina" && accessor != 0) {
                v.user = result;
                v.accessor = accessor;
                LOG_INFO(Core, "coop: ocarina-play: the game's Ocarina sound user is {:#x} (accessor {:#x})", result,
                         accessor);
            } else {
                say(fmt::format("the Ocarina module's user call gave {:#x} named '{}': not the Ocarina user", result,
                                name));
                v.user_next_ms = now_ms + 10000;
            }
            break;
        }
        case OcarinaPlayBox::Step::searching:
            if ((result & 1) != 0) {
                v.asset_known[v.play] = true;
            } else {
                v.asset_missing[v.play] = true;
                say(fmt::format("no asset '{}' in the Ocarina user", CoopOcarinaSound::AssetName(v.play)));
                v.busy = false;
            }
            break;
        case OcarinaPlayBox::Step::stopping:
            v.stop = false;
            g_ocarina_stopped.fetch_add(1, std::memory_order_relaxed);
            if (v.play == 0) {
                v.busy = false;
            }
            break;
        case OcarinaPlayBox::Step::emitting:
            v.busy = false;
            g_ocarina_played.fetch_add(1, std::memory_order_relaxed);
            if (g_voice_debug || g_ocarina_played.load() <= 20) {
                LOG_INFO(Core, "coop: ocarina-play: P{}'s note '{}' at its puppet (emit gave {:#x}); the Ocarina user "
                         "now plays: {}", v.player, CoopOcarinaSound::AssetName(v.play), result,
                         CoopPuppetSound::Playing(guest, v.user));
            }
            break;
        case OcarinaPlayBox::Step::idle:
            break;
        }
        return std::nullopt;
    }
    // While a friend's note sounds and our Link does not perform, the user's root matrix follows that friend's puppet.
    if (v.placed != 0) {
        if (v.held.HeldBy(v.placed) == 0 || g_ocarina_playing.load(std::memory_order_relaxed) || !place(v.placed)) {
            v.placed = 0;
        }
    }
    if (!v.busy) {
        std::lock_guard lock(v.mutex);
        while (!v.wanted.empty() && now_ms - v.wanted.front().at_ms > 1000) {  // a load: not played late
            v.wanted.erase(v.wanted.begin());
        }
        if (!v.wanted.empty()) {
            const auto w = v.wanted.front();
            v.wanted.erase(v.wanted.begin());
            const O::Action a = v.held.Note(w.player, w.note, now_ms);
            v.player = w.player;
            v.stop = a.stop;
            v.play = a.play;
            v.busy = a.stop || a.play != 0;
        } else if (const auto p = v.held.Stale(now_ms)) {
            v.player = *p;
            v.stop = true;
            v.play = 0;
            v.busy = true;
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
    const std::uint64_t handle = v.block + O::HandleSlot(v.player);
    if (v.stop) {
        c.function = main + O::kKill;
        c.x[0] = handle;
        v.step = OcarinaPlayBox::Step::stopping;
        return c;
    }
    if (v.user == 0) {
        if (now_ms < v.user_next_ms) {
            v.busy = false;
            return std::nullopt;
        }
        const std::uint64_t module = CoopPuppetSound::FindOcarinaModule(
            guest, main, CoopPuppetSound::SoundRoot(guest, main), nullptr);
        v.module = module;
        const std::uint64_t wrapper = module != 0 ? word(module + O::kModuleWrapper) : 0;
        const std::uint64_t vtable = wrapper != 0 ? word(wrapper) : 0;
        const std::uint64_t get = vtable != 0 ? word(vtable + O::kWrapperGetUser) : 0;
        if (get == 0) {
            say(fmt::format("the Ocarina sound module was not found (module {:#x}, wrapper {:#x})", module, wrapper));
            v.user_next_ms = now_ms + 10000;
            v.busy = false;
            return std::nullopt;
        }
        c.function = get;
        c.x[0] = wrapper;
        v.step = OcarinaPlayBox::Step::user;
        return c;
    }
    if (v.play < 1 || v.play > O::kNotes || v.asset_missing[v.play]) {
        v.busy = false;
        return std::nullopt;
    }
    if (!v.asset_known[v.play]) {
        std::array<char, 0x40> text{};
        const std::string name = CoopOcarinaSound::AssetName(static_cast<std::uint8_t>(v.play));
        std::memcpy(text.data(), name.data(), std::min<std::size_t>(name.size(), text.size() - 1));
        const std::uint64_t zero = 0;
        if (!guest.Write(v.block + O::kText, text.data(), text.size()) ||
            !guest.Write(v.block + O::AssetSlot(v.play), &zero, 8)) {
            v.busy = false;
            return std::nullopt;
        }
        c.function = main + O::kSearchAsset;
        c.x[0] = v.accessor;
        c.x[1] = v.block + O::AssetSlot(v.play);
        c.x[2] = v.block + O::kText;
        v.step = OcarinaPlayBox::Step::searching;
        return c;
    }
    if (g_ocarina_playing.load(std::memory_order_relaxed)) {
        say("our own Link plays the Ocarina: another player's note waits");
        v.busy = false;
        return std::nullopt;
    }
    const float d = distance(v.player);
    if (d < 0.0f || d >= O::kHearM) {
        say(d < 0.0f ? fmt::format("P{} has no live puppet here: its note is not played", v.player)
                     : fmt::format("P{}'s puppet is {:.0f} m away, beyond hearing: its note is not played", v.player, d));
        v.busy = false;
        return std::nullopt;
    }
    if (!place(v.player)) {
        v.busy = false;
        return std::nullopt;
    }
    v.placed = v.player;
    const std::array<std::uint8_t, 8> fresh{0xff, 0, 0xff, 0xff, 0, 0, 0, 0};
    if (!guest.Write(handle, fresh.data(), fresh.size())) {
        v.busy = false;
        return std::nullopt;
    }
    c.function = main + O::kEmit;
    c.x[0] = v.user;
    c.x[1] = v.block + O::AssetSlot(v.play);
    c.x[2] = handle;
    c.x[3] = 0;
    v.step = OcarinaPlayBox::Step::emitting;
    return c;
}
