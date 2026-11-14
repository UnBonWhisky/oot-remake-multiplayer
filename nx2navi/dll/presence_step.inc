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
struct PresenceBox {
    std::uint64_t next_ms = 0;
    std::vector<std::uint64_t> due;  // entities whose "became absent" call is to be made
    std::unordered_set<std::uint32_t> gates;
    std::size_t gated_seen = ~std::size_t{0};
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
    if (!v.due.empty()) {
        const std::uint64_t entity = v.due.back();
        v.due.pop_back();
        CoopPuppet::Call c;
        c.function = main + Presence::kRemoveCall;
        c.x[0] = entity;
        return c;
    }
    if (now_ms < v.next_ms || !g_presence_in_game.load(std::memory_order_relaxed)) {
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
    auto u32at = [&guest](std::uint64_t at, std::uint32_t& out) { return at >= 0x10000 && guest.Read(at, &out, 4); };
    // The actor list, as lichii/hook/coop_objects.cpp walks it.
    const std::uint64_t manager = word(main + 0x4e91888);
    const std::uint64_t lists = manager != 0 ? word(manager + 0x118) : 0;
    if (lists == 0) {
        return std::nullopt;
    }
    const std::uint64_t head = lists + 0x670;
    GuestFlags memory(guest, main);
    coop::host::GameFlags flags(memory);
    std::size_t gated = 0, with_flag = 0;
    std::string gated_names;
    std::uint64_t node = word(head);
    for (int walked = 0; node != 0 && node != head && walked < 4096; ++walked, node = word(node)) {
        const std::uint64_t actor = word(node + 0x10);
        if (actor == 0 || actor + 0x538 != node) {
            continue;
        }
        const std::uint64_t entity = word(actor + Presence::kActorEntity);
        std::uint32_t flag = 0, bits = 0, absent = 0;
        if (entity != 0 && u32at(entity + Presence::kEntityFlag, flag) && flag != 0) {
            ++with_flag;
        }
        if (entity == 0 || !u32at(entity + Presence::kEntityFlag, flag) || flag == 0 || !v.gates.contains(flag) ||
            !u32at(entity + Presence::kEntityBits, bits) || !u32at(entity + Presence::kEntityAbsent, absent)) {
            continue;
        }
        const auto value = flags.get_bool(flag);
        ++gated;
        if (gated_names.size() < 400) {
            const auto* g = CoopStory::Find(flag);
            gated_names += " " + ActorNameOf(guest, actor) + "(" + (g != nullptr ? g->name : "?") + "=" +
                           (value ? std::to_string(*value ? 1 : 0) : std::string("?")) + ")";
        }
        if (!value || !Presence::ShouldLeave(bits, absent, *value)) {
            continue;
        }
        void* at = api->map_range(entity + Presence::kEntityAbsent, 4, 1);
        if (at == nullptr) {
            continue;
        }
        const std::uint32_t old =
            __atomic_fetch_or(static_cast<std::uint32_t*>(at), Presence::kAbsentPresence, __ATOMIC_SEQ_CST);
        const auto* story = CoopStory::Find(flag);
        LOG_INFO(Core, "coop: presence: {} ({:#x}) left at once: its gate {} is {} now (another player opened it){}",
                 ActorNameOf(guest, actor), word(entity + Presence::kEntityHash),
                 story != nullptr ? story->name : "?", *value ? 1 : 0,
                 Presence::CallDue(old) ? "" : " (already out for another reason)");
        if (Presence::CallDue(old)) {
            v.due.push_back(entity);
            g_presence_left.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (gated != v.gated_seen) {
        v.gated_seen = gated;
        LOG_INFO(Core, "coop: presence: {} loaded actors have a presence flag, {} of them a shared story gate:{}",
                 with_flag, gated, gated_names);
    }
    return std::nullopt;
}

