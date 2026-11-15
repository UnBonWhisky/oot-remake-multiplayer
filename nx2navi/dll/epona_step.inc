// SPDX-License-Identifier: GPL-3.0-or-later
// --- Epona herself in a game whose Get_Epona came from another player (epona.hpp): included by upstream.cpp ---
std::atomic<bool> g_epona_on{true};
struct EponaBox {
    std::mutex mutex;
    float place[3] = {0, 0, 0};
    bool place_known = false;
    std::uint64_t next_ms = 0;
    int name_first = -1;  // -1 unread yet this session, 1 empty, 0 set
    int asked = 0;
    std::uint64_t asked_ms = 0;
    std::string seen;
};
EponaBox g_epona;
constexpr int kEponaOrder = 61;

std::optional<CoopPuppet::Call> EponaStep(CoopPuppet::Guest& guest, bool after_call, std::uint64_t,
                                          std::uint64_t now_ms) {
    EponaBox& v = g_epona;
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    if (after_call || main == 0 || !g_epona_on.load(std::memory_order_relaxed) ||
        !g_presence_in_game.load(std::memory_order_relaxed) || now_ms < v.next_ms) {
        return std::nullopt;
    }
    v.next_ms = now_ms + 1000;
    auto word = [&guest](std::uint64_t at) {
        std::uint64_t w = 0;
        return at >= 0x10000 && guest.Read(at, &w, 8) ? w : 0;
    };
    GuestFlags memory(guest, main);
    coop::host::GameFlags flags(memory);
    const auto name = flags.get_string64_array(CoopQuests::epona::kOwnedActor, 0);
    const auto child = flags.get_bool(CoopQuests::epona::kChildhood);
    if (!name || !child) {
        return std::nullopt;
    }
    if (v.name_first < 0) {
        v.name_first = name->empty() ? 1 : 0;
    }
    const std::uint64_t x = word(main + Epona::kGlobal);
    const std::uint64_t list = x != 0 ? word(x + Epona::kList) : 0;
    std::uint32_t count = 0;
    if (list == 0 || !guest.Read(list + Epona::kListCount, &count, 4) || count < 0x12) {
        return std::nullopt;
    }
    const std::uint64_t array = word(list + Epona::kListArray);
    const std::uint64_t mgr = array != 0 ? word(array + Epona::kSlot) : 0;
    Epona::Mgr m;
    if (mgr == 0 || !guest.Read(mgr + Epona::kStructHash, &m.struct_hash, 4) ||
        !guest.Read(mgr + Epona::kMemberHash, &m.member_hash, 4) || !guest.Read(mgr + Epona::kLinkedId, &m.linked, 4) ||
        !guest.Read(mgr + Epona::kFlags, &m.flags, 4) || !guest.Read(mgr + Epona::kReady, &m.ready, 1) ||
        !guest.Read(mgr + Epona::kMakeAt, &m.make_at, 4)) {
        return std::nullopt;
    }
    const std::string seen = fmt::format("name '{}' (empty at start: {}), adult {}, manager {}: linked horse {:#x}, "
                                         "flags {:#x}, ready {}",
                                         *name, v.name_first, *child ? 0 : 1, Epona::IsManager(m) ? "found" : "NOT found",
                                         m.linked, m.flags, m.ready);
    if (seen != v.seen) {
        v.seen = seen;
        LOG_INFO(Core, "coop: epona: {}", seen);
    }
    float link[3];
    {
        std::lock_guard lock(v.mutex);
        if (!v.place_known) {
            return std::nullopt;
        }
        std::memcpy(link, v.place, sizeof link);
    }
    // At most three asks a session, 10 s apart (the update may not run in a place where riding is not allowed).
    if (v.asked >= 3 || (v.asked > 0 && now_ms < v.asked_ms + 10000) ||
        !Epona::ShouldMake(m, v.name_first == 1, !name->empty(), !*child)) {
        return std::nullopt;
    }
    float at[3];
    Epona::PlaceBeside(link, at);
    if (!guest.Write(mgr + Epona::kMakeAt + 4, &at[1], 4) || !guest.Write(mgr + Epona::kMakeAt + 8, &at[2], 4) ||
        !guest.Write(mgr + Epona::kMakeAt, &at[0], 4)) {
        return std::nullopt;
    }
    ++v.asked;
    v.asked_ms = now_ms;
    LOG_INFO(Core, "coop: epona: asked the owned-horse manager for '{}' at {:.1f} {:.1f} {:.1f} (ask {} of 3)", *name,
             at[0], at[1], at[2], v.asked);
    return std::nullopt;
}
