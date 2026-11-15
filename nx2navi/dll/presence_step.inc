// SPDX-License-Identifier: GPL-3.0-or-later
// --- A story gate another player opened leaves our game at once (presence.hpp): included by upstream.cpp ---
struct GuestFlags final : coop::host::GameMemory {
    explicit GuestFlags(CoopPuppet::Guest& g, std::uint64_t main) : guest(g), main_(main) {}
    std::uint64_t main_base() const override { return main_; }
    bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
        return address >= 0x10000 && guest.Read(address, out.data(), out.size());
    }
    CoopPuppet::Guest& guest;
    std::uint64_t main_;
};

std::string ActorNameOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    std::uint64_t p = 0;
    char buf[64] = {};
    if (!guest.Read(actor + 0x260, &p, 8) || p < 0x10000 || !guest.Read(p, buf, sizeof buf - 1)) {
        return "?";
    }
    return std::string(buf, strnlen(buf, sizeof buf - 1));
}

std::atomic<bool> g_presence_on{true};
std::atomic<bool> g_presence_in_game{false};  // our Link is read: never on the title screen or in a load
std::atomic<std::uint64_t> g_presence_left{0};
std::atomic<std::uint64_t> g_presence_back{0};
struct PresenceToggle {
    std::uint64_t entity;
    std::uint32_t bit;
    bool set;
};
struct PresenceBox {
    std::uint64_t next_ms = 0;
    std::uint64_t in_game_since_ms = 0;
    std::vector<PresenceToggle> due;                              // game calls still to make, one a frame step
    std::vector<std::pair<std::uint64_t, std::uint64_t>> remake;  // (entity, when): blocker NPCs to make again
    std::unordered_set<std::uint32_t> gates;
    std::size_t gated_seen = ~std::size_t{0};
    std::uint64_t table_seen = 0;
};
PresenceBox g_presence;
constexpr int kPresenceOrder = 60;

std::optional<CoopPuppet::Call> PresenceStep(CoopPuppet::Guest& guest, bool after_call, std::uint64_t,
                                             std::uint64_t now_ms) {
    PresenceBox& v = g_presence;
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (after_call || main == 0 || api == nullptr || !g_presence_on.load(std::memory_order_relaxed)) {
        return std::nullopt;
    }
    if (!g_presence_in_game.load(std::memory_order_relaxed)) {
        v.in_game_since_ms = 0;
        v.due.clear();
        v.remake.clear();
        return std::nullopt;
    }
    if (v.in_game_since_ms == 0) {
        v.in_game_since_ms = now_ms;
    }
    auto call = [main](const PresenceToggle& t) {
        CoopPuppet::Call c;
        c.function = main + Presence::kToggleCall;
        c.x[0] = t.entity;
        c.x[1] = t.bit;
        c.x[2] = t.set ? 1 : 0;
        return c;
    };
    if (!v.due.empty()) {
        const PresenceToggle t = v.due.front();
        v.due.erase(v.due.begin());
        return call(t);
    }
    for (auto it = v.remake.begin(); it != v.remake.end(); ++it) {
        if (now_ms >= it->second) {
            const PresenceToggle t{it->first, Presence::kPresenceBit, false};
            v.remake.erase(it);
            LOG_INFO(Core, "coop: presence: blocker NPC entity {:#x} made again (its schedule reads the gate anew)",
                     t.entity);
            return call(t);
        }
    }
    // Not in the first 3 s in the game (the area's own Presence pass may still run), and once a second.
    if (now_ms < v.next_ms || now_ms < v.in_game_since_ms + 3000) {
        return std::nullopt;
    }
    v.next_ms = now_ms + 1000;
    if (v.gates.empty()) {
        for (const auto& f : CoopStory::Flags()) {
            if (f.kind == CoopStory::Kind::world) {
                v.gates.insert(f.hash);
            }
        }
    }
    auto word = [&guest](std::uint64_t at) {
        std::uint64_t w = 0;
        return at >= 0x10000 && guest.Read(at, &w, 8) ? w : 0;
    };
    auto u32at = [&guest](std::uint64_t at) {
        std::uint32_t w = 0;
        return at >= 0x10000 && guest.Read(at, &w, 4) ? w : 0u;
    };
    const std::uint64_t table = word(main + Presence::kEntityTable);
    if (table == 0) {
        return std::nullopt;
    }
    GuestFlags memory(guest, main);
    coop::host::GameFlags flags(memory);
    struct Seen {
        std::uint64_t entity;
        std::uint64_t actor;
        std::uint32_t flag;
        float place[3];
    };
    std::vector<Seen> npcs;     // live actors without a Presence of their own (blocker NPC candidates)
    std::vector<Seen> changed;  // gated placements toggled now
    std::size_t gated = 0, with_flag = 0, entities = 0;
    std::string gated_names;
    std::vector<std::uint8_t> buf;
    for (const std::uint64_t at : {std::uint64_t{0xc8}, std::uint64_t{0xd8}}) {
        const auto count = static_cast<std::int32_t>(u32at(table + at));
        const std::uint64_t records = word(table + at + 8);
        if (count <= 0 || count > 256 || records == 0) {
            continue;
        }
        for (std::int32_t r = 0; r < count; ++r) {
            const std::uint64_t rec = records + Presence::kRecordSize * static_cast<std::uint64_t>(r);
            if (!Presence::RecordLoaded(u32at(rec + 0x50))) {
                continue;
            }
            const auto n0 = static_cast<std::int32_t>(u32at(rec));
            const std::uint32_t n1 = u32at(rec + 0x14);
            const std::uint64_t ents = word(rec + 8);
            const std::uint32_t n = n0 <= 0 ? 0 : std::min<std::uint32_t>(static_cast<std::uint32_t>(n0), n1);
            if (ents == 0 || n == 0 || n > 8192) {
                continue;
            }
            buf.resize(static_cast<std::size_t>(n) * Presence::kEntitySize);
            if (!guest.Read(ents, buf.data(), buf.size())) {
                continue;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint8_t* e = buf.data() + static_cast<std::size_t>(i) * Presence::kEntitySize;
                const std::uint64_t entity = ents + static_cast<std::uint64_t>(i) * Presence::kEntitySize;
                ++entities;
                std::uint32_t flag = 0, bits = 0, absent = 0;
                std::uint64_t actor = 0;
                Seen s{};
                std::memcpy(&flag, e + Presence::kEntityFlag, 4);
                std::memcpy(&bits, e + Presence::kEntityBits, 4);
                std::memcpy(&absent, e + Presence::kEntityAbsent, 4);
                std::memcpy(&actor, e + Presence::kEntityActor, 8);
                std::memcpy(s.place, e + Presence::kEntityPlace, 12);
                s.entity = entity;
                s.actor = actor & ~std::uint64_t{3};
                s.flag = flag;
                if (flag == 0) {
                    if (s.actor != 0 && (actor & 2) == 0) {
                        npcs.push_back(s);
                    }
                    continue;
                }
                ++with_flag;
                if (!v.gates.contains(flag)) {
                    continue;
                }
                const auto value = flags.get_bool(flag);
                ++gated;
                const auto* story = CoopStory::Find(flag);
                if (gated_names.size() < 600) {
                    gated_names += fmt::format(" {}({}={}{})", s.actor != 0 ? ActorNameOf(guest, s.actor) : "no actor",
                                               story != nullptr ? story->name : "?",
                                               value ? (*value ? "1" : "0") : "?",
                                               (absent & Presence::kAbsentPresence) != 0 ? ", out" : "");
                }
                if (!value) {
                    continue;
                }
                const Presence::Gate g = Presence::GateAction(bits, absent, *value);
                if (g == Presence::Gate::keep) {
                    continue;
                }
                const bool leave = g == Presence::Gate::leave;
                std::uint64_t hash = 0;
                std::memcpy(&hash, e + Presence::kEntityHash, 8);
                LOG_INFO(Core,
                         "coop: presence: {} ({:#x} at {:.1f} {:.1f} {:.1f}) {}: its gate {} is {} now (the group's flag)",
                         s.actor != 0 ? ActorNameOf(guest, s.actor) : std::string("placement without actor"), hash,
                         s.place[0], s.place[1], s.place[2], leave ? "leaves at once" : "comes back at once",
                         story != nullptr ? story->name : "?", *value ? 1 : 0);
                v.due.push_back({entity, Presence::kPresenceBit, leave});
                (leave ? g_presence_left : g_presence_back).fetch_add(1, std::memory_order_relaxed);
                changed.push_back(s);
            }
        }
    }
    // Blocker NPCs on a wall whose gate changed now: made again, so their schedule reads the flag anew.
    for (const Seen& wall : changed) {
        for (const Seen& npc : npcs) {
            if (npc.entity == wall.entity || !Presence::IsBlockerNear(wall.place, npc.place)) {
                continue;
            }
            const std::string name = ActorNameOf(guest, npc.actor);
            if (name.rfind("Npc_", 0) != 0) {
                continue;
            }
            LOG_INFO(Core, "coop: presence: blocker NPC {} beside that wall: made again", name);
            v.due.push_back({npc.entity, Presence::kPresenceBit, true});
            v.remake.emplace_back(npc.entity, now_ms + 1000);
        }
    }
    if (gated != v.gated_seen || table != v.table_seen) {
        v.gated_seen = gated;
        v.table_seen = table;
        LOG_INFO(Core,
                 "coop: presence: {} banc entities loaded, {} with a presence flag, {} of them a shared story gate:{}",
                 entities, with_flag, gated, gated_names);
    }
    return std::nullopt;
}
