// Tests for the shared NPC talk flags (lichii/hook/coop_talks.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "coop_talks.h"
#include "coop/host/game_flags.hpp"

TEST_CASE("talks: 402 HaveTalked_Special flags, sorted, each keyed by the hash of its name") {
    const auto talks = CoopTalks::Talks();
    CHECK(talks.size() == 402);
    CHECK(std::is_sorted(talks.begin(), talks.end(), [](const auto& a, const auto& b) { return a.hash < b.hash; }));
    for (const auto& t : talks) {
        CHECK(std::string(t.name).rfind("HaveTalked_Special.", 0) == 0);
        CHECK(coop::host::flag_hash(t.name) == t.hash);
    }
    // The owl's, already a shared story flag, is one of them.
    CHECK(coop::host::flag_hash("HaveTalked_Special.Npc_Kaepora_DeathMountainTrail_Talk_Event2") == 0x992fb436u);
}

TEST_CASE("talks: merged into the shared flags only when on") {
    std::optional<coop::host::SharedFlags> shared;
    CHECK(CoopTalks::Merge(shared, false) == 0);
    CHECK_FALSE(shared);
    CHECK(CoopTalks::Merge(shared, true) == 402);
    REQUIRE(shared);
    CHECK(shared->contains(coop::host::FlagKey(0x992fb436u, std::uint16_t{0})));
    CHECK(CoopTalks::Merge(shared, true) == 0);  // no repeats
}
