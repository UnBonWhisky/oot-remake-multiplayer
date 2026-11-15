// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the NX2navi DLL's parts that need no game: its settings, the build
// id search, and the memory access on this process's own memory.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "coop_game_build.h"
#include "guest_memory.hpp"
#include "menu_model.hpp"
#include "mp_saves.hpp"
#include "overlay_model.hpp"
#include "puppet_fx.hpp"
#include "coop/protocol/codec.hpp"
#include "coop/host/notify.hpp"
#include "save_sync.hpp"
#include "settings.hpp"
#include "step_switch.hpp"
#include "presence.hpp"
#include "epona.hpp"
#include "ocarina_play.hpp"
#include "story_start.hpp"
#include "talk_lock.hpp"
#include "ocarina_song.hpp"
#include "item_get.hpp"
#include "teleport_auto.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using CoopNx2navi::ParseSettings;

TEST_CASE("nx2navi settings: the four keys and share_progress") {
  std::vector<std::string> errors;
  const auto s = ParseSettings(
      "# comment\r\nserver = 192.168.1.20\r\nport = 4000\r\nname = Saria\r\n"
      "password = letmein\r\nshare_progress = 1\r\n",
      errors);
  CHECK(errors.empty());
  CHECK(s.enabled());
  CHECK(s.coop.server == "192.168.1.20");
  CHECK(s.coop.port == 4000);
  CHECK(s.coop.name == "Saria");
  CHECK(s.coop.password == "letmein");
  CHECK(s.share_progress);
}

TEST_CASE("nx2navi settings: shared progress is on unless the file says 0") {
  std::vector<std::string> errors;
  CHECK(ParseSettings("server = 1.2.3.4\nname = A\n", errors).share_progress);  // on unless 0
  CHECK_FALSE(ParseSettings("server = 1.2.3.4\nshare_progress = 0\n", errors).share_progress);
  CHECK(ParseSettings("server = 1.2.3.4\n# share_progress = 0\n", errors).share_progress);
  CHECK(errors.empty());
}

TEST_CASE("nx2navi settings: a mistake turns co-op off and names the line") {
  std::vector<std::string> errors;
  auto s = ParseSettings("server = 1.2.3.4\nshare_progress = yes\n", errors);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("line 2") != std::string::npos);
  CHECK_FALSE(s.enabled());

  errors.clear();
  s = ParseSettings("server = 1.2.3.4\nshare_progress = 1\nfrobnicate = 1\n", errors);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("line 3") != std::string::npos);  // numbered as in the file
  CHECK_FALSE(s.enabled());
}

TEST_CASE("nx2navi settings: the launcher's file with no server is off without an error") {
  std::vector<std::string> errors;
  const auto s = ParseSettings(
      "\xef\xbb\xbf# server =\r\nport = 27510\r\nname = Mido\r\n# password =\r\nshare_progress = 0\r\n",
      errors);
  CHECK(errors.empty());
  CHECK_FALSE(s.enabled());
}

TEST_CASE("nx2navi settings: overlay is on unless the file says 0") {
  std::vector<std::string> errors;
  CHECK(ParseSettings("server = 1.2.3.4\n", errors).overlay);
  CHECK(ParseSettings("server = 1.2.3.4\noverlay = 1\n", errors).overlay);
  const auto off = ParseSettings("server = 1.2.3.4\r\noverlay = 0\r\nshare_progress = 1\r\n", errors);
  CHECK_FALSE(off.overlay);
  CHECK(off.share_progress);
  CHECK(off.enabled());
  CHECK(errors.empty());
  const auto bad = ParseSettings("server = 1.2.3.4\noverlay = on\n", errors);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0] == "coop.ini line 2: overlay must be 0 or 1");
  CHECK_FALSE(bad.enabled());
}

TEST_CASE("nx2navi settings: puppet is on unless the file says 0") {
  std::vector<std::string> errors;
  CHECK(ParseSettings("server = 1.2.3.4\n", errors).puppet);
  const auto off = ParseSettings("server = 1.2.3.4\r\npuppet = 0\r\n", errors);
  CHECK_FALSE(off.puppet);
  CHECK(off.overlay);
  CHECK(off.enabled());
  CHECK(errors.empty());
  ParseSettings("server = 1.2.3.4\npuppet = yes\n", errors);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0] == "coop.ini line 2: puppet must be 0 or 1");
}

namespace Ui = CoopNx2navi::Ui;

TEST_CASE("nx2navi overlay: the compass uses the world axes, north being -Z") {
  CHECK(Ui::Compass(0, -10) == "north");
  CHECK(Ui::Compass(10, 0) == "east");
  CHECK(Ui::Compass(0, 10) == "south");
  CHECK(Ui::Compass(-10, 0) == "west");
  CHECK(Ui::Compass(7, -7) == "north-east");
  CHECK(Ui::Compass(-7, 7) == "south-west");
  CHECK(Ui::Compass(10, -3) == "east");        // within 22.5 degrees of east
  CHECK(Ui::Compass(0.2, 0.2) == "");          // too close to say
  CHECK(Ui::Compass(std::nan(""), 1) == "");
}

TEST_CASE("nx2navi overlay: a player's line") {
  Ui::Local local;
  local.game_found = true;
  local.in_game = true;
  local.scene = "spot00";
  local.position = {1, 0, 1};
  Ui::Peer peer;
  peer.number = 2;
  peer.name = "Mido";
  CHECK(Ui::PeerLine(peer, local) == "P2 Mido: joining");
  peer.known = true;
  CHECK(Ui::PeerLine(peer, local) == "P2 Mido: not in game");
  peer.fresh = true;
  CHECK(Ui::PeerLine(peer, local) == "P2 Mido: in game, scene unknown");
  peer.scene = "spot01";
  peer.position = Ui::Vec3{4, 0, -3};
  CHECK(Ui::PeerLine(peer, local) == "P2 Mido: spot01");   // another scene: no distance
  peer.scene = "spot00";
  CHECK(Ui::PeerLine(peer, local) == "P2 Mido: spot00, 5.0 units, north-east");
  peer.position = Ui::Vec3{1, 3, 1};
  CHECK(Ui::PeerLine(peer, local) == "P2 Mido: spot00, 3.0 units, right here");
  local.in_game = false;
  CHECK(Ui::PeerLine(peer, local) == "P2 Mido: spot00");
  CHECK(Ui::LocalLine(local) == "You: not in game (title screen, load or menu)");
  local.game_found = false;
  CHECK(Ui::LocalLine(local) == "You: game not found yet");
}

TEST_CASE("nx2navi overlay: the panel, notices that fade, and the chat box") {
  Ui::Snapshot s;
  s.server = "127.0.0.1:27510";
  s.self_name = "Saria";
  CHECK(Ui::LinkLine(s) == "Co-op: connecting to 127.0.0.1:27510...");
  s.link = Ui::Link::lost;
  CHECK(Ui::LinkLine(s) == "Co-op: server lost, retrying 127.0.0.1:27510");
  s.link = Ui::Link::refused;
  s.refused = "wrong join password";
  CHECK(Ui::LinkLine(s) == "Co-op: refused by the server (wrong join password)");
  s.link = Ui::Link::connected;
  s.self = 1;
  CHECK(Ui::LinkLine(s) == "Co-op: connected as P1 \"Saria\"");

  std::deque<Ui::Notice> notices;
  Ui::AddNotice(notices, "Mido joined", 1000);
  CHECK(Ui::NoticeAlpha(notices[0], 1000) == 1.0f);
  CHECK(Ui::NoticeAlpha(notices[0], 1000 + Ui::kNoticeMs - Ui::kNoticeFadeMs) == 1.0f);
  CHECK(Ui::NoticeAlpha(notices[0], 1000 + Ui::kNoticeMs - Ui::kNoticeFadeMs / 2) ==
        doctest::Approx(0.5));
  CHECK(Ui::NoticeAlpha(notices[0], 1000 + Ui::kNoticeMs) == 0.0f);
  for (int i = 0; i < 10; ++i) {
    Ui::AddNotice(notices, "line " + std::to_string(i), 2000);
  }
  CHECK(notices.size() == Ui::kNoticesKept);
  CHECK(notices.back().text == "line 9");
  Ui::PruneNotices(notices, 2000 + Ui::kNoticeMs);
  CHECK(notices.empty());

  Ui::AddNotice(notices, "Mido joined", 5000);
  Ui::ChatView chat;
  auto lines = Ui::Panel(s, notices, chat, 5000);
  REQUIRE(lines.size() == 5);
  CHECK(lines[0].text == "Co-op: connected as P1 \"Saria\"");
  CHECK(lines[1].text == "You: game not found yet");
  CHECK(lines[2].text == "Nobody else is here yet");
  CHECK(lines[3].text == "Mido joined");
  CHECK(lines[3].role == Ui::Role::notice);
  CHECK(lines[4].text == "Enter: chat   F9: hide");

  Ui::Peer peer;
  peer.number = 2;
  peer.name = "Mido";
  s.peers.push_back(peer);
  chat.open = true;
  chat.input = "hi";
  for (int i = 0; i < 12; ++i) {
    Ui::AddHistory(chat.history, "Mido: " + std::to_string(i));
  }
  CHECK(chat.history.size() == Ui::kHistoryKept);
  lines = Ui::Panel(s, notices, chat, 5000);
  CHECK(lines[2].text == "P2 Mido: joining");
  CHECK(lines[3].text == "Mido: 4");   // the oldest kept
  CHECK(lines[lines.size() - 2].text == "> hi_");
  CHECK(lines.back().text == "Enter sends, Esc cancels");
  for (const auto& line : lines) {
    CHECK(line.text != "Mido joined");   // the box hides the notices while open
  }
}

TEST_CASE("nx2navi overlay: chat text in UTF-8, counted in code points") {
  std::string text;
  CHECK(Ui::Utf8Append(text, 'a', 200));
  CHECK(Ui::Utf8Append(text, 0xe9, 200));      // e acute
  CHECK(Ui::Utf8Append(text, 0x20ac, 200));    // euro sign
  CHECK(Ui::Utf8Append(text, 0x1f600, 200));   // outside the BMP
  CHECK(text == "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
  CHECK(Ui::Utf8Count(text) == 4);
  CHECK_FALSE(Ui::Utf8Append(text, '\r', 200));   // control characters stay out
  CHECK_FALSE(Ui::Utf8Append(text, 0x7f, 200));
  CHECK_FALSE(Ui::Utf8Append(text, 0xd800, 200));
  CHECK_FALSE(Ui::Utf8Append(text, 0x110000, 200));
  CHECK_FALSE(Ui::Utf8Append(text, 'b', 4));      // full
  Ui::Utf8PopBack(text);
  CHECK(text == "a\xc3\xa9\xe2\x82\xac");
  Ui::Utf8PopBack(text);
  Ui::Utf8PopBack(text);
  CHECK(text == "a");
  Ui::Utf8PopBack(text);
  Ui::Utf8PopBack(text);
  CHECK(text.empty());
  std::string full;
  for (int i = 0; i < 250; ++i) {
    Ui::Utf8Append(full, 0xe9, 200);
  }
  CHECK(Ui::Utf8Count(full) == 200);   // the protocol's kMaxChatChars
}

namespace {

// A made-up module image: the header word, "MOD0" and a build id note.
std::vector<std::uint8_t> FakeModule(std::size_t size, const std::array<std::uint8_t, 20>& id,
                                     std::size_t note_at) {
  std::vector<std::uint8_t> image(size, 0);
  const std::uint32_t header = 0x100;
  std::memcpy(image.data() + 4, &header, 4);
  std::memcpy(image.data() + header, "MOD0", 4);
  const std::uint8_t note[16] = {4, 0, 0, 0, 20, 0, 0, 0, 3, 0, 0, 0, 'G', 'N', 'U', 0};
  std::memcpy(image.data() + note_at, note, sizeof(note));
  std::memcpy(image.data() + note_at + sizeof(note), id.data(), id.size());
  return image;
}

std::array<std::uint8_t, 20> IdBytes(std::string_view hex) {
  std::array<std::uint8_t, 20> id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(2 * i, 2)), nullptr, 16));
  }
  return id;
}

}  // namespace

TEST_CASE("nx2navi build id: found in a block of bytes, as upper-case hex") {
  const std::string want(CoopGameBuild::kPrimary.main_id);
  const auto image = FakeModule(0x3000, IdBytes(want), 0x1234);
  const auto found = CoopNx2navi::FindBuildIds(image);
  REQUIRE(found.size() == 1);
  CHECK(found[0] == want);
  CHECK(CoopNx2navi::FindBuildIds(std::vector<std::uint8_t>(0x1000, 0)).empty());
  // A note cut off by the end of the block is not an id.
  CHECK(CoopNx2navi::FindBuildIds(std::span(image).first(0x1234 + 16 + 10)).empty());
}

#ifdef _WIN32

namespace {

struct Mapping {
  explicit Mapping(std::size_t bytes)
      : size(bytes),
        at(static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {}
  ~Mapping() {
    if (at != nullptr) VirtualFree(at, 0, MEM_RELEASE);
  }
  std::uint64_t address() const { return reinterpret_cast<std::uint64_t>(at); }
  std::size_t size;
  std::uint8_t* at;
};

}  // namespace

TEST_CASE("nx2navi memory: reads this process, refuses what is not mapped") {
  Mapping map(3 * 4096);
  REQUIRE(map.at != nullptr);
  for (std::size_t i = 0; i < map.size; ++i) map.at[i] = static_cast<std::uint8_t>(i * 7);
  CoopNx2navi::Memory memory;
  std::array<std::uint8_t, 8> out{};
  CHECK(memory.read(map.address() + 100, out));
  CHECK(std::memcmp(out.data(), map.at + 100, out.size()) == 0);
  // Across a page boundary.
  CHECK(memory.read(map.address() + 4096 - 3, out));
  CHECK(std::memcmp(out.data(), map.at + 4096 - 3, out.size()) == 0);
  // The cached page is the frame's view; Fresh() takes a new one.
  map.at[100] = 0xee;
  CHECK(memory.read(map.address() + 100, out));
  CHECK(out[0] != 0xee);
  memory.Fresh();
  CHECK(memory.read(map.address() + 100, out));
  CHECK(out[0] == 0xee);

  // Not mapped: null, a low address, the kernel's half, a wrap, a closed page.
  CHECK_FALSE(memory.read(0, out));
  CHECK_FALSE(memory.read(0x1000, out));
  CHECK_FALSE(memory.read(0xffff800000000000ull, out));
  CHECK_FALSE(memory.read(0xfffffffffffffffcull, out));
  DWORD old = 0;
  REQUIRE(VirtualProtect(map.at + 2 * 4096, 4096, PAGE_NOACCESS, &old));
  memory.Fresh();
  CHECK_FALSE(memory.read(map.address() + 2 * 4096 + 8, out));
  CHECK_FALSE(memory.read(map.address() + 2 * 4096 - 4, out));  // half in it
  CHECK(memory.failed_reads() == 6);

  // A guard page is refused and stays a guard page (a stack needs its own).
  REQUIRE(VirtualProtect(map.at + 4096, 4096, PAGE_READWRITE | PAGE_GUARD, &old));
  memory.Fresh();
  CHECK_FALSE(memory.read(map.address() + 4096 + 8, out));
  MEMORY_BASIC_INFORMATION info{};
  REQUIRE(VirtualQuery(map.at + 4096, &info, sizeof(info)) == sizeof(info));
  CHECK((info.Protect & PAGE_GUARD) != 0);
}

TEST_CASE("nx2navi memory: writes only read-write memory, and compare-exchange") {
  Mapping map(2 * 4096);
  REQUIRE(map.at != nullptr);
  CoopNx2navi::Memory memory;
  const std::array<std::uint8_t, 4> data{1, 2, 3, 4};
  CHECK(memory.write(map.address() + 16, data));
  CHECK(std::memcmp(map.at + 16, data.data(), 4) == 0);
  std::array<std::uint8_t, 4> out{};
  CHECK(memory.read(map.address() + 16, out));
  CHECK(out == data);
  // A write drops the cached pages: the next read sees it.
  const std::array<std::uint8_t, 4> more{5, 6, 7, 8};
  CHECK(memory.write(map.address() + 16, more));
  CHECK(memory.read(map.address() + 16, out));
  CHECK(out == more);

  std::uint32_t word = 5;
  std::memcpy(map.at + 64, &word, 4);
  CHECK(memory.compare_exchange_u32(map.address() + 64, 5, 9) == 5u);
  std::memcpy(&word, map.at + 64, 4);
  CHECK(word == 9);
  CHECK(memory.compare_exchange_u32(map.address() + 64, 5, 1) == 9u);  // not equal: kept
  std::memcpy(&word, map.at + 64, 4);
  CHECK(word == 9);
  CHECK_FALSE(memory.compare_exchange_u32(map.address() + 66, 0, 1).has_value());  // not aligned

  // Read-only memory (the game's code and constants) is never written.
  DWORD old = 0;
  REQUIRE(VirtualProtect(map.at + 4096, 4096, PAGE_READONLY, &old));
  CHECK_FALSE(memory.write(map.address() + 4096, data));
  CHECK_FALSE(memory.write(map.address() + 4096 - 2, data));  // half in it
  CHECK_FALSE(memory.compare_exchange_u32(map.address() + 4096, 0, 1).has_value());
  CHECK(map.at[4096] == 0);
  CHECK(map.at[4096 - 2] == 0);
  CHECK_FALSE(memory.write(0x1000, data));
  CHECK_FALSE(memory.compare_exchange_u32(0, 0, 1).has_value());
}

TEST_CASE("nx2navi main check: the build is known by its id note") {
  const std::string want(CoopGameBuild::kPrimary.main_id);
  using State = CoopNx2navi::MainCheck::State;
  const std::size_t size = (3u << 20) + 0x5000;  // more than two search pieces

  SUBCASE("the right build, with the note across two pieces") {
    Mapping map(size);
    REQUIRE(map.at != nullptr);
    const auto image = FakeModule(size, IdBytes(want), (1u << 20) - 8);
    std::memcpy(map.at, image.data(), size);
    const auto check = CoopNx2navi::CheckMain(map.address(), want);
    CHECK(check.state == State::ok);
    CHECK(check.build_id == want);
    CHECK(check.size >= size);
  }
  SUBCASE("another build") {
    Mapping map(size);
    REQUIRE(map.at != nullptr);
    auto id = IdBytes(want);
    id[0] ^= 0xff;
    const auto image = FakeModule(size, id, 0x2000);
    std::memcpy(map.at, image.data(), size);
    const auto check = CoopNx2navi::CheckMain(map.address(), want);
    CHECK(check.state == State::other_build);
    CHECK(check.build_id != want);
    CHECK(check.build_id.size() == 40);
  }
  SUBCASE("memory that is no module") {
    Mapping map(size);
    REQUIRE(map.at != nullptr);
    CHECK(CoopNx2navi::CheckMain(map.address(), want).state == State::no_header);
    const std::uint32_t header = 0x100;
    std::memcpy(map.at + 4, &header, 4);
    std::memcpy(map.at + header, "MOD0", 4);
    CHECK(CoopNx2navi::CheckMain(map.address(), want).state == State::no_build_id);
  }
  SUBCASE("nothing mapped") {
    CHECK(CoopNx2navi::CheckMain(0x10000, want).state == State::not_mapped);
    CHECK(CoopNx2navi::CheckMain(0, want).state == State::not_mapped);
  }
}

#endif  // _WIN32

// --- Going to another player by itself (teleport_auto.hpp) ---

TEST_CASE("nx2navi settings: follow and join_at_host") {
  std::vector<std::string> errors;
  auto s = ParseSettings("server = 1.2.3.4\n", errors);
  CHECK(s.follow == CoopNx2navi::Settings::Follow::anyone);  // the default since 2026-11-13
  CHECK(s.join_at_host);
  s = ParseSettings("server = 1.2.3.4\nfollow = host\njoin_at_host = 0\n", errors);
  CHECK(s.follow == CoopNx2navi::Settings::Follow::host);
  CHECK_FALSE(s.join_at_host);
  CHECK(ParseSettings("server = 1.2.3.4\nfollow = off\n", errors).follow ==
        CoopNx2navi::Settings::Follow::off);
  CHECK(errors.empty());
  s = ParseSettings("server = 1.2.3.4\nfollow = everybody\n", errors);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("line 2") != std::string::npos);
  CHECK_FALSE(s.enabled());
}

namespace {
using CoopNx2navi::Teleport::Auto;
using CoopNx2navi::Teleport::FollowMode;
using CoopNx2navi::Teleport::Peer;
constexpr std::uint64_t kSecond = 1'000'000;

Peer MakePeer(std::uint8_t number, bool host, const char* scene, bool fresh = true) {
  Peer p;
  p.number = number;
  p.name = host ? "Host" : "P" + std::to_string(number);
  p.host = host;
  if (scene != nullptr) {
    p.scene = scene;
  }
  p.fresh = fresh;
  return p;
}
std::optional<std::string> In(const char* scene) { return std::string(scene); }
}  // namespace

TEST_CASE("nx2navi teleport: a joiner goes to the host once, after its game settled") {
  Auto a;
  std::vector<Peer> peers = {MakePeer(1, true, "Field")};
  // Not in the game yet: nothing.
  CHECK_FALSE(a.Tick(std::nullopt, false, peers, 1 * kSecond));
  // In: not before the save's own start has run.
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 10 * kSecond));
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 11 * kSecond));
  const auto go = a.Tick(In("Kokiri"), false, peers, 12 * kSecond + 1);
  REQUIRE(go);
  CHECK(go->player == 1);
  CHECK(go->why == Auto::Why::join);
  CHECK(go->first);
  CHECK(go->name == "Host");
  // Tried again (the host was busy, our cutscene ran) until it is done.
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 13 * kSecond));
  const auto again = a.Tick(In("Kokiri"), false, peers, 12 * kSecond + 1 + Auto::kRetryUs);
  REQUIRE(again);
  CHECK_FALSE(again->first);
  a.Done(1);
  CHECK_FALSE(a.pending());
  // Once: a load later and the host elsewhere is no second join.
  CHECK_FALSE(a.Tick(std::nullopt, false, peers, 30 * kSecond));
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 31 * kSecond));
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 40 * kSecond));
}

TEST_CASE("nx2navi teleport: no join for the host, with the setting off, or with no host in the game") {
  std::vector<Peer> peers = {MakePeer(2, false, "Field")};
  Auto host;
  CHECK_FALSE(host.Tick(In("Field"), true, peers, 1 * kSecond));
  CHECK_FALSE(host.Tick(In("Field"), true, peers, 9 * kSecond));

  Auto off(Auto::Options{FollowMode::host, false});
  std::vector<Peer> with_host = {MakePeer(1, true, "Field")};
  CHECK_FALSE(off.Tick(In("Kokiri"), false, with_host, 1 * kSecond));
  CHECK_FALSE(off.Tick(In("Kokiri"), false, with_host, 9 * kSecond));

  // The host is at its title screen (no samples): waited for, but not for ever.
  Auto late;
  std::vector<Peer> away = {MakePeer(1, true, nullptr, false)};
  CHECK_FALSE(late.Tick(In("Kokiri"), false, away, 1 * kSecond));
  CHECK_FALSE(late.Tick(In("Kokiri"), false, away, 20 * kSecond));
  CHECK_FALSE(late.Tick(In("Kokiri"), false, away, 1 * kSecond + Auto::kJoinWindowUs + 1));
  CHECK_FALSE(late.Tick(In("Kokiri"), false, with_host, 2 * kSecond + Auto::kJoinWindowUs + 1));
  CHECK_FALSE(late.pending());
}

TEST_CASE("nx2navi teleport: the host's exit is followed, once, and only from a shared scene") {
  Auto a(Auto::Options{FollowMode::host, false});
  std::vector<Peer> peers = {MakePeer(1, true, "Field"), MakePeer(3, false, "Field")};
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 1 * kSecond));
  // Another player leaves: not the host, not followed.
  peers[1] = MakePeer(3, false, "Market");
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 2 * kSecond));
  // The host loads (no samples, the old scene still named): nothing yet.
  peers[0] = MakePeer(1, true, "Field", false);
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 3 * kSecond));
  // The host is in the next scene.
  peers[0] = MakePeer(1, true, "Kakariko");
  const auto go = a.Tick(In("Field"), false, peers, 4 * kSecond);
  REQUIRE(go);
  CHECK(go->player == 1);
  CHECK(go->why == Auto::Why::follow);
  CHECK(go->first);
  // Our own load, then we are there: the want ends by itself.
  CHECK_FALSE(a.Tick(std::nullopt, false, peers, 5 * kSecond));
  CHECK(a.pending());
  CHECK_FALSE(a.Tick(In("Kakariko"), false, peers, 8 * kSecond));
  CHECK_FALSE(a.pending());
  // The host was never with us in this scene: no follow.
  Auto b(Auto::Options{FollowMode::host, false});
  std::vector<Peer> apart = {MakePeer(1, true, "Market")};
  CHECK_FALSE(b.Tick(In("Field"), false, apart, 1 * kSecond));
  apart[0] = MakePeer(1, true, "Kakariko");
  CHECK_FALSE(b.Tick(In("Field"), false, apart, 2 * kSecond));
}

TEST_CASE("nx2navi teleport: a follow waits through our cutscene and is given up after a minute") {
  Auto a(Auto::Options{FollowMode::host, false});
  std::vector<Peer> peers = {MakePeer(1, true, "Field")};
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 1 * kSecond));
  peers[0] = MakePeer(1, true, "Kakariko");
  REQUIRE(a.Tick(In("Field"), false, peers, 2 * kSecond));
  int tries = 0;
  for (std::uint64_t t = 2 * kSecond + 100'000; t < 2 * kSecond + Auto::kKeepUs; t += 100'000) {
    tries += a.Tick(In("Field"), false, peers, t) ? 1 : 0;
  }
  CHECK(tries >= 20);
  CHECK(tries <= 24);
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 3 * kSecond + Auto::kKeepUs));
  CHECK_FALSE(a.pending());
}

TEST_CASE("nx2navi teleport: the host follows nobody, 'anyone' follows everybody, 'off' nobody") {
  std::vector<Peer> peers = {MakePeer(2, false, "Field")};
  std::vector<Peer> gone = {MakePeer(2, false, "Market")};
  Auto host(Auto::Options{FollowMode::host, false});
  CHECK_FALSE(host.Tick(In("Field"), true, peers, 1 * kSecond));
  CHECK_FALSE(host.Tick(In("Field"), true, gone, 2 * kSecond));
  Auto anyone(Auto::Options{FollowMode::anyone, false});
  CHECK_FALSE(anyone.Tick(In("Field"), true, peers, 1 * kSecond));
  const auto go = anyone.Tick(In("Field"), true, gone, 2 * kSecond);
  REQUIRE(go);
  CHECK(go->player == 2);
  Auto off(Auto::Options{FollowMode::off, false});
  CHECK_FALSE(off.Tick(In("Field"), false, peers, 1 * kSecond));
  CHECK_FALSE(off.Tick(In("Field"), false, gone, 2 * kSecond));
}

TEST_CASE("nx2navi teleport: two players who take different exits at once do not swap") {
  // A (us) and B share the field; both walk into an exit. While we load, B's
  // new scene shows; when we are in ours, B is simply somewhere else.
  Auto a(Auto::Options{FollowMode::anyone, false});
  std::vector<Peer> peers = {MakePeer(2, false, "Field")};
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 1 * kSecond));
  CHECK_FALSE(a.Tick(std::nullopt, false, peers, 2 * kSecond));  // our load
  peers[0] = MakePeer(2, false, "Market");
  CHECK_FALSE(a.Tick(std::nullopt, false, peers, 3 * kSecond));
  CHECK_FALSE(a.Tick(In("Kakariko"), false, peers, 4 * kSecond));
  CHECK_FALSE(a.Tick(In("Kakariko"), false, peers, 9 * kSecond));
  CHECK_FALSE(a.pending());
  // And the one who followed is not followed back: it arrives in our scene.
  Auto leader(Auto::Options{FollowMode::anyone, false});
  std::vector<Peer> behind = {MakePeer(2, false, "Field")};
  CHECK_FALSE(leader.Tick(In("Field"), false, behind, 1 * kSecond));
  CHECK_FALSE(leader.Tick(std::nullopt, false, behind, 2 * kSecond));
  CHECK_FALSE(leader.Tick(In("Market"), false, behind, 3 * kSecond));  // B still in the field
  behind[0] = MakePeer(2, false, "Market");                            // B followed
  CHECK_FALSE(leader.Tick(In("Market"), false, behind, 6 * kSecond));
  CHECK_FALSE(leader.pending());
}

TEST_CASE("nx2navi teleport: a player who leaves the session is not waited for; a lost connection starts over") {
  Auto a(Auto::Options{FollowMode::host, true});
  std::vector<Peer> peers = {MakePeer(1, true, "Field")};
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 1 * kSecond));
  REQUIRE(a.Tick(In("Field"), false, peers, 4 * kSecond));  // the join
  CHECK(a.pending());
  CHECK_FALSE(a.Tick(In("Field"), false, {}, 5 * kSecond));
  CHECK_FALSE(a.pending());
  a.Reset();
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 10 * kSecond));
  CHECK(a.Tick(In("Field"), false, peers, 13 * kSecond));  // joins again in the new session
}

TEST_CASE("nx2navi teleport: after a Game Over, back beside a living friend") {
  using CoopNx2navi::Teleport::RegroupChoice;
  auto dead = [](Peer p) { p.alive = false; return p; };
  auto alive = [](Peer p) { p.alive = true; return p; };
  // The host is dead, one of two guests is alive: that guest.
  std::vector<Peer> peers = {dead(MakePeer(1, true, "Field")), dead(MakePeer(3, false, "Field")),
                             alive(MakePeer(4, false, "Market"))};
  REQUIRE(RegroupChoice(peers, In("Field")) != nullptr);
  CHECK(RegroupChoice(peers, In("Field"))->number == 4);
  // Nobody alive: nothing (the game's own respawn).
  peers[2] = dead(MakePeer(4, false, "Market"));
  CHECK(RegroupChoice(peers, In("Field")) == nullptr);
  // In the scene we fell in before the host elsewhere; the host first within a scene.
  std::vector<Peer> two = {alive(MakePeer(1, true, "Market")), alive(MakePeer(3, false, "Field")),
                           MakePeer(2, false, "Field")};  // P2's life is not known: counts as alive
  CHECK(RegroupChoice(two, In("Field"))->number == 2);
  two[2] = dead(MakePeer(2, false, "Field"));
  CHECK(RegroupChoice(two, In("Field"))->number == 3);
  CHECK(RegroupChoice(two, In("Lake"))->number == 1);
  // Not in the game (no samples) or busy: not chosen.
  std::vector<Peer> away = {alive(MakePeer(1, true, "Field", false)), alive(MakePeer(2, false, "Field"))};
  away[1].busy = true;
  CHECK(RegroupChoice(away, In("Field")) == nullptr);

  // The whole rule: the host died (follow mode host) and goes to the living joiner after its reload.
  Auto host(Auto::Options{FollowMode::host, true});
  std::vector<Peer> joiner = {alive(MakePeer(2, false, "Field"))};
  CHECK_FALSE(host.Tick(In("Field"), true, joiner, 1 * kSecond));
  host.Died(2 * kSecond, std::string("Field"));
  CHECK_FALSE(host.Tick(std::nullopt, true, joiner, 3 * kSecond));  // the reload
  CHECK_FALSE(host.Tick(In("Field"), true, joiner, 10 * kSecond));
  const auto go = host.Tick(In("Field"), true, joiner, 13 * kSecond);
  REQUIRE(go);
  CHECK(go->player == 2);
  CHECK(go->why == Auto::Why::regroup);
  // A dead joiner is not gone to; with nobody alive for the window, the game's respawn stands.
  Auto lone(Auto::Options{FollowMode::host, false});
  std::vector<Peer> gone = {dead(MakePeer(1, true, "Field"))};
  CHECK_FALSE(lone.Tick(In("Field"), false, gone, 1 * kSecond));
  lone.Died(2 * kSecond, std::string("Field"));
  CHECK_FALSE(lone.Tick(std::nullopt, false, gone, 3 * kSecond));
  for (std::uint64_t t = 4; t < 40; ++t) {
    CHECK_FALSE(lone.Tick(In("Field"), false, gone, t * kSecond));
  }
  CHECK_FALSE(lone.died());
}

TEST_CASE("nx2navi settings: shared enemies are on unless the file says 0") {
  std::vector<std::string> errors;
  auto s = ParseSettings("server = 1.2.3.4\n", errors);
  CHECK(s.objects);
  s = ParseSettings("server = 1.2.3.4\nobjects = 0\n", errors);
  CHECK(errors.empty());
  CHECK_FALSE(s.objects);
  s = ParseSettings("server = 1.2.3.4\n", errors);
  CHECK(s.objects_boss);
  CHECK(s.objects_world);
  CHECK(s.objects_ai_hold == 3);  // a copy's AI update is left out (objects/brain.hpp)
  CHECK_FALSE(s.objects_friendly);  // townspeople and animals: each game its own
  CHECK(s.objects_rolls_apart == 30);  // a player 30 m from the host rolls its own Stalchildren
  s = ParseSettings("server = 1.2.3.4\nobjects_rolls_apart = 0\n", errors);
  CHECK(errors.empty());
  CHECK(s.objects_rolls_apart == 0);
  s = ParseSettings("server = 1.2.3.4\nobjects_rolls_apart = far\n", errors);
  REQUIRE(errors.size() == 1);
  errors.clear();
  s = ParseSettings("server = 1.2.3.4\nobjects_friendly = 1\n", errors);
  CHECK(errors.empty());
  CHECK(s.objects_friendly);
  s = ParseSettings("server = 1.2.3.4\nobjects = 1\nobjects_boss = 0\nobjects_world = 0\nobjects_ai_hold = 2\n",
                    errors);
  CHECK(errors.empty());
  CHECK(s.objects);
  CHECK_FALSE(s.objects_boss);
  CHECK_FALSE(s.objects_world);
  CHECK(s.objects_ai_hold == 2);
  s = ParseSettings("server = 1.2.3.4\nobjects_ai_hold = 0\n", errors);
  CHECK(errors.empty());
  CHECK(s.objects_ai_hold == 0);
  s = ParseSettings("server = 1.2.3.4\nobjects_ai_hold = 4\n", errors);
  REQUIRE(errors.size() == 1);
  CHECK_FALSE(s.enabled());
}

TEST_CASE("nx2navi overlay: the other players are green, blue and yellow, in number order, on every screen") {
  using CoopNx2navi::Ui::PeerColour;
  // P1's screen: P2 green, P3 blue, P4 yellow.
  CHECK(PeerColour(1, 2) == 0);
  CHECK(PeerColour(1, 3) == 1);
  CHECK(PeerColour(1, 4) == 2);
  // P3's screen: P1 green, P2 blue, P4 yellow.
  CHECK(PeerColour(3, 1) == 0);
  CHECK(PeerColour(3, 2) == 1);
  CHECK(PeerColour(3, 4) == 2);
  // P4's screen.
  CHECK(PeerColour(4, 1) == 0);
  CHECK(PeerColour(4, 2) == 1);
  CHECK(PeerColour(4, 3) == 2);
  // Ourselves and numbers that are no player have none.
  CHECK(PeerColour(2, 2) == -1);
  CHECK(PeerColour(1, 0) == -1);
  CHECK(PeerColour(1, 5) == -1);
  // Before the welcome (self 0) the three colours still go round.
  CHECK(PeerColour(0, 1) == 0);
  CHECK(PeerColour(0, 4) == 2);
}

TEST_CASE("nx2navi overlay: a player's panel line, chat lines and notices carry that player's colour") {
  namespace Ui = CoopNx2navi::Ui;
  Ui::Snapshot s;
  s.link = Ui::Link::connected;
  s.self = 2;
  s.self_name = "Mido";
  Ui::Peer a;
  a.number = 1;
  a.name = "Saria";
  Ui::Peer b;
  b.number = 4;
  b.name = "Ruto";
  s.peers = {a, b};
  std::deque<Ui::Notice> notices;
  Ui::AddNotice(notices, "Saria: hello", 1000);
  Ui::AddNotice(notices, "Ruto joined", 1000);
  Ui::AddNotice(notices, "Mido: mine", 1000);
  Ui::AddNotice(notices, "Sariah: not Saria", 1000);
  Ui::ChatView chat;
  const auto lines = Ui::Panel(s, notices, chat, 1500);
  int saria = 0, ruto = 0, plain = 0;
  for (const auto& line : lines) {
    if (line.text.rfind("P1 Saria", 0) == 0 || line.text == "Saria: hello") {
      CHECK(line.colour == 0);
      ++saria;
    } else if (line.text.rfind("P4 Ruto", 0) == 0 || line.text == "Ruto joined") {
      CHECK(line.colour == 2);  // P1 green, P3 blue, P4 yellow on P2's screen
      ++ruto;
    } else {
      CHECK(line.colour == -1);
      ++plain;
    }
  }
  CHECK(saria == 2);
  CHECK(ruto == 2);
  CHECK(plain >= 4);
}

TEST_CASE("nx2navi minimap: a world position on the window, and nothing from a view that is not known") {
  namespace Ui = CoopNx2navi::Ui;
  Ui::MapView view;
  view.centre_x = -23.0f;
  view.centre_z = 197.0f;
  view.world_span = 736.0f;
  view.pane_x = 1490.0f;  // the layout's canvas: 3840 x 2160, origin at the centre, +y up
  view.pane_y = -644.0f;
  view.pane_w = 960.0f;
  view.pane_h = 960.0f;
  // The picture's centre: canvas (1490, -644) is window (1705, 862) at 1920 x 1080.
  auto p = Ui::MapPoint(view, -23.0f, 197.0f, 1920.0f, 1080.0f);
  REQUIRE(p);
  CHECK((*p)[0] == doctest::Approx(1705.0f));
  CHECK((*p)[1] == doctest::Approx(862.0f));
  // East is right and south is down; a quarter of the span is a quarter of the picture.
  p = Ui::MapPoint(view, -23.0f + 184.0f, 197.0f + 184.0f, 1920.0f, 1080.0f);
  REQUIRE(p);
  CHECK((*p)[0] == doctest::Approx(1705.0f + 120.0f));
  CHECK((*p)[1] == doctest::Approx(862.0f + 120.0f));
  // The same share of a smaller window.
  p = Ui::MapPoint(view, -23.0f, 197.0f, 960.0f, 540.0f);
  REQUIRE(p);
  CHECK((*p)[0] == doctest::Approx(852.5f));
  CHECK((*p)[1] == doctest::Approx(431.0f));
  // Outside the picture: no marker (never pinned to the edge).
  CHECK_FALSE(Ui::MapPoint(view, -23.0f + 400.0f, 197.0f, 1920.0f, 1080.0f));
  CHECK_FALSE(Ui::MapPoint(view, -23.0f, 197.0f - 369.0f, 1920.0f, 1080.0f));
  // A view that is not known: no span, no size, or a value that is not a number.
  Ui::MapView none = view;
  none.world_span = 0.0f;
  CHECK_FALSE(Ui::MapPoint(none, -23.0f, 197.0f, 1920.0f, 1080.0f));
  none = view;
  none.pane_w = 0.0f;
  CHECK_FALSE(Ui::MapPoint(none, -23.0f, 197.0f, 1920.0f, 1080.0f));
  none = view;
  none.centre_x = std::nanf("");
  CHECK_FALSE(Ui::MapPoint(none, -23.0f, 197.0f, 1920.0f, 1080.0f));
  CHECK_FALSE(Ui::MapPoint(Ui::MapView{}, 0.0f, 0.0f, 1920.0f, 1080.0f));
}

// --- The title screen's Multiplayer menu (menu_model.hpp) ---

namespace Menu = CoopNx2navi::Menu;
namespace MpSaves = CoopNx2navi::MpSaves;

TEST_CASE("nx2navi menu: the game's language decides, French for a French game, English otherwise") {
  CHECK(Menu::LangFromCode("fr") == Menu::Lang::fr);
  CHECK(Menu::LangFromCode("fr-CA") == Menu::Lang::fr);
  CHECK(Menu::LangFromCode("FR_be") == Menu::Lang::fr);
  CHECK(Menu::LangFromCode("en-US") == Menu::Lang::en);
  CHECK(Menu::LangFromCode("de") == Menu::Lang::en);
  CHECK(Menu::LangFromCode("fra") == Menu::Lang::en);
  CHECK(Menu::LangFromCode("") == Menu::Lang::en);
  // As the runtime decides the game's language: NX2NAVI_LANG, then nx2navi.ini, then the locale.
  const char* ini = "[display]\r\nfullscreen = 1\r\n; lang = de\r\n[language]\r\nLang = \"fr\"\r\n";
  CHECK(Menu::IniValue(ini, "lang") == "fr");
  CHECK(Menu::IniValue("NX2NAVI_LANG = ja\n", "lang") == "ja");
  CHECK(Menu::IniValue(ini, "locale").empty());
  CHECK(Menu::DetectLang("", ini, "", "en-US") == Menu::Lang::fr);
  CHECK(Menu::DetectLang("en-US", ini, "fr-FR", "fr-FR") == Menu::Lang::en);
  CHECK(Menu::DetectLang("", "", "fr-FR", "en-US") == Menu::Lang::fr);
  CHECK(Menu::DetectLang("", "locale = pt-BR\n", "", "fr-FR") == Menu::Lang::en);
  CHECK(Menu::DetectLang("", "", "", "fr-FR") == Menu::Lang::fr);
  CHECK(Menu::DetectLang("", "", "", "") == Menu::Lang::en);
}

TEST_CASE("nx2navi menu: the launcher add-on's rules for a name, a port, a password and an address") {
  CHECK(Menu::ValidName("Saria"));
  CHECK(Menu::ValidName("H\xc3\xa9l\xc3\xa8ne et Zelda!"));  // 16 code points
  CHECK_FALSE(Menu::ValidName(""));
  CHECK_FALSE(Menu::ValidName("   "));
  CHECK_FALSE(Menu::ValidName("12345678901234567"));
  CHECK_FALSE(Menu::ValidName("a\tb"));
  CHECK(Menu::ValidPort("27510"));
  CHECK(Menu::ValidPort("1"));
  CHECK_FALSE(Menu::ValidPort("0"));
  CHECK_FALSE(Menu::ValidPort("65536"));
  CHECK_FALSE(Menu::ValidPort("27a"));
  CHECK_FALSE(Menu::ValidPort(""));
  CHECK(Menu::ValidPassword("three-random-words"));
  CHECK_FALSE(Menu::ValidPassword("two words"));
  CHECK_FALSE(Menu::ValidPassword(""));
  CHECK_FALSE(Menu::ValidPassword(std::string(129, 'x')));
  CHECK(Menu::AddressKind("192.168.1.20") == "lan");
  CHECK(Menu::AddressKind("10.0.0.1") == "lan");
  CHECK(Menu::AddressKind("172.20.1.1") == "lan");
  CHECK(Menu::AddressKind("26.1.2.3") == "radmin");
  CHECK(Menu::AddressKind("25.1.2.3") == "hamachi");
  CHECK(Menu::AddressKind("100.100.1.1") == "tailscale");
  CHECK(Menu::AddressKind("127.0.0.1") == "loopback");
  CHECK(Menu::AddressKind("169.254.3.3") == "link-local");
  CHECK(Menu::AddressKind("8.8.8.8") == "public");
  CHECK(Menu::AddressKind("26.1.2").empty());
  CHECK(Menu::AddressKind("300.1.1.1").empty());
  CHECK(Menu::AddressKind("host").empty());
  using A = Menu::JoinAddress;
  CHECK(Menu::ParseJoinAddress(" 192.168.1.20 ") == A{"192.168.1.20", ""});
  CHECK(Menu::ParseJoinAddress("udp://26.1.2.3:27511/") == A{"26.1.2.3", "27511"});
  CHECK(Menu::ParseJoinAddress("\"LocalHost\"") == A{"127.0.0.1", ""});
  CHECK(Menu::ParseJoinAddress("my-pc.lan.") == A{"my-pc.lan", ""});
  CHECK_FALSE(Menu::ParseJoinAddress("26.1.2"));
  CHECK_FALSE(Menu::ParseJoinAddress("1.2.3.4:70000"));
  CHECK_FALSE(Menu::ParseJoinAddress("a b@c"));
  CHECK_FALSE(Menu::ParseJoinAddress(""));
  // What a host gives, best first: the home network (a card with a default
  // gateway), then a VPN, then a card without a gateway, then virtual cards
  // (Hyper-V, WSL: no friend is behind them), then this PC; nothing else.
  using L = Menu::LocalAddress;
  using G = Menu::GiveAddress;
  CHECK(Menu::VirtualCard("vEthernet (WSL (Hyper-V firewall))", "Hyper-V Virtual Ethernet Adapter"));
  CHECK(Menu::VirtualCard("VMware Network Adapter VMnet8", ""));
  CHECK(Menu::VirtualCard("Ethernet 3", "VirtualBox Host-Only Ethernet Adapter"));
  CHECK_FALSE(Menu::VirtualCard("Ethernet", "Realtek PCIe 2.5GbE Family Controller"));
  CHECK_FALSE(Menu::VirtualCard("Wi-Fi", "Intel(R) Wi-Fi 6E AX211 160MHz"));
  CHECK(Menu::JoinAddresses({L{"172.27.240.1", false, true, "vEthernet (WSL)"},
                             L{"169.254.1.1", false, false, "Ethernet 2"},
                             L{"26.4.5.6", false, true, "Radmin VPN"},
                             L{"10.0.0.7", false, false, "Ethernet 2"},
                             L{"192.168.1.20", true, false, "Ethernet"},
                             L{"192.168.1.20", true, false, "the route outside"},
                             L{"8.8.8.8", true, false, "odd"}}) ==
        std::vector<G>{{"192.168.1.20", G::Kind::lan},
                       {"26.4.5.6", G::Kind::vpn},
                       {"10.0.0.7", G::Kind::other},
                       {"172.27.240.1", G::Kind::virtual_card},
                       {"127.0.0.1", G::Kind::same_pc}});
  CHECK(Menu::JoinAddresses({}) == std::vector<G>{{"127.0.0.1", G::Kind::same_pc}});
}

TEST_CASE("nx2navi menu: the settings file keeps the launcher's keys it does not know") {
  Menu::FlatJson json;
  REQUIRE(json.Parse("{\n  \"name\": \"Sa\\u00efd \\\"x\\\"\",\n  \"port\": 27511,\n  \"overlay\": true,\n"
                     "  \"nested\": {\"a\": [1, \"}\"]},\n  \"follow\": \"host\"\n}"));
  CHECK(json.String("name") == std::string("Sa\xc3\xaf" "d \"x\""));
  CHECK(json.Number("port") == 27511);
  CHECK(json.Bool("overlay") == true);
  CHECK_FALSE(json.String("port"));
  CHECK_FALSE(json.Number("name"));
  CHECK_FALSE(json.Bool("missing"));
  json.SetString("name", "Zelda\n");
  json.SetNumber("port", 4000);
  json.SetString("host_address", "26.1.2.3");
  json.Erase("follow");
  json.SetBool("overlay", false);
  json.SetBool("overlay", true);
  Menu::FlatJson again;
  REQUIRE(again.Parse(json.Dump()));
  CHECK(again.String("name") == std::string("Zelda\n"));
  CHECK(again.Number("port") == 4000);
  CHECK(again.String("host_address") == std::string("26.1.2.3"));
  CHECK(again.Bool("overlay") == true);
  CHECK_FALSE(again.String("follow"));
  CHECK(json.Dump().find("\"nested\": {\"a\": [1, \"}\"]}") != std::string::npos);
  // Not an object: nothing is taken, and an empty object is written.
  Menu::FlatJson bad;
  CHECK_FALSE(bad.Parse("[1, 2]"));
  CHECK_FALSE(bad.Parse("{\"a\": }"));
  CHECK(bad.Dump() == "{}");
  CHECK(bad.Parse("{}"));
}

TEST_CASE("nx2navi menu: the pages on one skeleton, in French and in English") {
  Menu::State s;
  s.lang = Menu::Lang::fr;
  s.name = "Link";
  s.addresses = {{"192.168.1.20", Menu::GiveAddress::Kind::lan},
                 {"172.27.240.1", Menu::GiveAddress::Kind::virtual_card},
                 {"127.0.0.1", Menu::GiveAddress::Kind::same_pc}};
  // 'a' a button, 't' a text row (a button to the game), '-' the ornament line.
  const auto kinds = [](const std::vector<Menu::Row>& rows) {
    std::string out;
    for (const auto& r : rows) {
      out += r.kind == Menu::RowKind::action ? 'a' : r.kind == Menu::RowKind::separator ? '-' : 't';
    }
    return out;
  };
  auto rows = Menu::Rows(s);
  CHECK(Menu::Title(s) == "Multijoueur");
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(kinds(rows) == "aaaaa-aa");  // only the rows the page needs: no language row, no filler
  CHECK(rows[0].label == "H\xc3\xa9" "berger sur Steam");
  CHECK(rows[0].value == "Indisponible");
  CHECK(rows[0].description.find("Pas encore disponible") == 0);
  CHECK(rows[1].label == "H\xc3\xa9" "berger en r\xc3\xa9seau local");
  CHECK(rows[2].label == "Rejoindre via Steam");
  CHECK(rows[2].value == "Indisponible");
  CHECK(rows[3].label == "Rejoindre en r\xc3\xa9seau local");
  CHECK(rows[4].label == "Sauvegardes multijoueur");
  CHECK(rows[4].value == "Indisponible");  // this game cannot be started on a save folder
  CHECK(rows[6].label == "\xc3\x89tat");
  CHECK(rows[6].value == "Hors ligne");
  CHECK(rows[1].description ==
        "Lance le serveur co-op sur ce PC et y joue.\nVos amis rejoignent avec l'adresse de ce PC.");
  // Captions: at most two lines of at most 46 characters (the game's caption
  // box does not wrap; a longer line is cut at its right edge), on every page,
  // for every address kind, typing or not, and for every sentence of the owner.
  const auto fits = [](const std::string& text) {
    std::size_t lines = 1, start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == '\n') {
        if (CoopNx2navi::Ui::Utf8Count(std::string_view(text).substr(start, i - start)) > 46) {
          return false;
        }
        start = i + 1;
        lines += i < text.size() ? 1 : 0;
      }
    }
    return lines <= 2;
  };
  for (const Menu::Lang lang : {Menu::Lang::fr, Menu::Lang::en}) {
    for (const Menu::Page page : {Menu::Page::main, Menu::Page::host, Menu::Page::join}) {
      for (std::size_t shown = 0; shown < 5; ++shown) {
        for (const bool copied : {false, true}) {
          Menu::State t = s;
          t.lang = lang;
          t.page = page;
          t.saves_on = shown % 2 == 1;  // with and without multiplayer saves
          t.addresses = {{"192.168.1.20", Menu::GiveAddress::Kind::lan},
                         {"26.1.2.3", Menu::GiveAddress::Kind::vpn},
                         {"10.0.0.7", Menu::GiveAddress::Kind::other},
                         {"172.27.240.1", Menu::GiveAddress::Kind::virtual_card},
                         {"127.0.0.1", Menu::GiveAddress::Kind::same_pc}};
          t.address_shown = shown;
          t.address_copied = copied;
          for (const auto& r : Menu::Rows(t)) {
            CHECK_MESSAGE(fits(r.description), r.description);
          }
          if (page != Menu::Page::main && shown == 0 && !copied) {
            Menu::Activate(t, 0);  // typing: the help
            CHECK_MESSAGE(fits(Menu::Rows(t)[0].description), Menu::Rows(t)[0].description);
          }
        }
      }
    }
    for (int text = 0; text <= static_cast<int>(Menu::Text::last); ++text) {
      const std::string sentence = Menu::T(lang, static_cast<Menu::Text>(text));
      CHECK_MESSAGE(fits(sentence), sentence);
    }
  }
  // The Steam entries do nothing; shown as disabled rows only when asked for.
  CHECK_FALSE(rows[0].disabled);
  CHECK(Menu::Activate(s, 0) == Menu::Action::none);
  CHECK(Menu::Activate(s, 2) == Menu::Action::none);
  CHECK(s.page == Menu::Page::main);
  s.dim_steam = true;
  CHECK(Menu::Rows(s)[0].disabled);
  CHECK(Menu::Rows(s)[2].disabled);
  CHECK_FALSE(Menu::Rows(s)[1].disabled);
  s.dim_steam = false;

  // Host over Local Network: name, port, password, the address to give, Start.
  CHECK(Menu::Activate(s, 1) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::host);
  rows = Menu::Rows(s);
  CHECK(Menu::Title(s) == "H\xc3\xa9" "berger en r\xc3\xa9seau local");
  CHECK(kinds(rows) == "tttaa-aa");
  CHECK(rows[0].label == "Nom");
  CHECK(rows[0].value == "Link");
  CHECK(rows[1].label == "Port");
  CHECK(rows[1].value == "27510");
  CHECK(rows[2].label == "Mot de passe");
  CHECK(rows[2].value == "aucun");
  CHECK(rows[3].label == "Adresse \xc3\xa0 donner");
  CHECK(rows[3].value == "192.168.1.20");
  CHECK(rows[4].label == "Sauvegarde");
  CHECK(rows[4].value == "La v\xc3\xb4tre");  // no multiplayer saves in this game: the player's own
  CHECK(Menu::Activate(s, 4) == Menu::Action::none);
  CHECK(rows[6].label == "D\xc3\xa9marrer");
  CHECK(rows[6].value == "D\xc3\xa9marrer");
  CHECK(rows[7].label == "Retour");
  // The address row says what the address is; chosen: copied, then the next one of this PC.
  CHECK(rows[3].description == "L'adresse de ce PC sur votre r\xc3\xa9seau local.\n"
                               "Valider : copie, puis l'adresse suivante.");
  CHECK(Menu::Activate(s, 3) == Menu::Action::copy_address);
  CHECK(Menu::Rows(s)[3].value == "192.168.1.20");
  CHECK(Menu::Rows(s)[3].description.find("\nCopi\xc3\xa9" "e") != std::string::npos);
  CHECK(Menu::Activate(s, 3) == Menu::Action::copy_address);
  CHECK(Menu::Rows(s)[3].value == "172.27.240.1");
  CHECK(Menu::Rows(s)[3].description.find("Carte virtuelle (Hyper-V, WSL)") == 0);
  CHECK(Menu::Activate(s, 3) == Menu::Action::copy_address);
  CHECK(Menu::Rows(s)[3].value == "127.0.0.1");
  CHECK(Menu::Rows(s)[3].description.find("Ce PC seulement") == 0);
  s.port = "27511";
  CHECK(Menu::Rows(s)[3].value == "127.0.0.1:27511");
  s.port = "27510";
  CHECK(Menu::Activate(s, 6) == Menu::Action::start_host);
  CHECK(Menu::Activate(s, 7) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::main);

  // Join over Local Network: name, address, port, password, Join.
  CHECK(Menu::Activate(s, 3) == Menu::Action::redraw);
  rows = Menu::Rows(s);
  CHECK(Menu::Title(s) == "Rejoindre en r\xc3\xa9seau local");
  CHECK(kinds(rows) == "tttta-aa");
  CHECK(rows[0].label == "Nom");
  CHECK(rows[1].label == "Adresse");
  CHECK(rows[1].value == "-");
  CHECK(rows[2].label == "Port");
  CHECK(rows[3].label == "Mot de passe");
  CHECK(rows[4].label == "Sauvegarde");
  CHECK(rows[4].value == "Celle de l'h\xc3\xb4te");
  CHECK(rows[6].label == "Rejoindre");
  // No address yet: said, nothing started.
  CHECK(Menu::Activate(s, 6) == Menu::Action::redraw);
  CHECK(s.phase == Menu::Phase::failed);
  CHECK(Menu::Rows(s)[6].value == "\xc3\x89" "chec");
  CHECK(Menu::Rows(s)[6].description == "Tapez d'abord l'adresse de l'h\xc3\xb4te.");
  s.address = "192.168.1.20";
  CHECK(Menu::Activate(s, 6) == Menu::Action::start_join);

  // English; the page left with "back" starts at the first page again.
  s.lang = Menu::Lang::en;
  rows = Menu::Rows(s);
  CHECK(Menu::Title(s) == "Join over Local Network");
  CHECK(rows[0].label == "Name");
  CHECK(rows[1].label == "Address");
  CHECK(rows[3].label == "Password");
  CHECK(rows[6].label == "Join");
  Menu::Closed(s);
  rows = Menu::Rows(s);
  CHECK(Menu::Title(s) == "Multiplayer");
  CHECK(rows[0].label == "Host over Steam");
  CHECK(rows[0].value == "Unavailable");
  CHECK(rows[1].label == "Host over Local Network");
  CHECK(rows[2].label == "Join over Steam");
  CHECK(rows[3].label == "Join over Local Network");
  CHECK(Menu::Activate(s, 7) == Menu::Action::close);

  // A running session: said on the first page, and Start does not start another.
  s.phase = Menu::Phase::hosting;
  s.player = 1;
  CHECK(Menu::Rows(s)[6].value == "Hosting (P1)");
  Menu::Activate(s, 1);
  CHECK(Menu::Activate(s, 6) == Menu::Action::redraw);
  CHECK(s.message.find("already runs") != std::string::npos);
  CHECK(Menu::Activate(s, 0) == Menu::Action::none);  // no typing either
}

TEST_CASE("nx2navi menu: typing with a keyboard, and with a gamepad") {
  Menu::State s;
  s.name = "Link";
  s.page = Menu::Page::join;
  // The name: the old text is there to change.
  CHECK(Menu::Activate(s, 0) == Menu::Action::begin_edit);
  CHECK(s.editing == Menu::Field::name);
  CHECK(Menu::Rows(s)[0].value == "Link_");
  CHECK(Menu::Rows(s)[0].description.find("Enter confirms") != std::string::npos);
  CHECK(Menu::Rows(s)[0].description.find("Pad:") != std::string::npos);
  CHECK(Menu::Activate(s, 6) == Menu::Action::none);  // nothing else while typing
  for (int i = 0; i < 4; ++i) Menu::EditBackspace(s);
  for (const char c : std::string("Zelda")) Menu::EditText(s, static_cast<unsigned char>(c));
  Menu::EditText(s, 0xe9);  // e acute
  Menu::EditText(s, '\n');  // dropped
  CHECK(Menu::CommitEdit(s));
  CHECK(s.name == "Zelda\xc3\xa9");
  CHECK(s.editing == Menu::Field::none);
  // A name that breaks the rule is not taken; the rule is said.
  Menu::Activate(s, 0);
  for (int i = 0; i < 20; ++i) Menu::EditBackspace(s);
  CHECK_FALSE(Menu::CommitEdit(s));
  CHECK(s.name == "Zelda\xc3\xa9");
  CHECK(Menu::Rows(s)[0].description == "The name needs 1 to 16 characters.");
  // The port takes digits only; cancelled typing changes nothing.
  Menu::Activate(s, 2);
  CHECK(s.editing == Menu::Field::port);
  for (int i = 0; i < 5; ++i) Menu::EditBackspace(s);
  for (const char c : std::string("2x7 511999")) Menu::EditText(s, static_cast<unsigned char>(c));
  CHECK(s.edit == "27511");
  Menu::CancelEdit(s);
  CHECK(s.port == "27510");
  // An address with a port sets both.
  Menu::Activate(s, 1);
  for (const char c : std::string("26.1.2.3:27511")) Menu::EditText(s, static_cast<unsigned char>(c));
  CHECK(Menu::CommitEdit(s));
  CHECK(s.address == "26.1.2.3");
  CHECK(s.port == "27511");
  Menu::Activate(s, 1);
  for (int i = 0; i < 30; ++i) Menu::EditBackspace(s);
  for (const char c : std::string("26.1.2")) Menu::EditText(s, static_cast<unsigned char>(c));
  CHECK_FALSE(Menu::CommitEdit(s));
  CHECK(s.address == "26.1.2.3");
  // A password is typed anew and never shown afterwards.
  Menu::Activate(s, 3);
  CHECK(s.editing == Menu::Field::join_password);
  CHECK(s.edit.empty());
  for (const char c : std::string("se cret")) Menu::EditText(s, static_cast<unsigned char>(c));
  CHECK(Menu::Rows(s)[3].value == "secret_");
  CHECK(Menu::CommitEdit(s));
  CHECK(s.join_password == "secret");
  CHECK(Menu::Rows(s)[3].value == "set");
  Menu::Activate(s, 3);
  CHECK(Menu::CommitEdit(s));  // empty: no password
  CHECK(s.join_password.empty());
  CHECK(Menu::Rows(s)[3].value == "none");

  // A keyboard in a game that takes a text row's typing itself (interface
  // version 4): the name, address and port rows are the game's text rows, the
  // password row stays ours (it shows a word, not the text).
  {
    Menu::State k = s;
    k.game_typing = true;
    k.address.clear();
    auto rows = Menu::Rows(k);
    CHECK(rows[0].text_max == 64);
    CHECK(rows[1].text_max == Menu::kMaxAddress);
    CHECK(rows[1].value.empty());  // the typing starts from the row's own text: no "-"
    CHECK(rows[2].text_max == 5);
    CHECK(rows[3].text_max == 0);
    CHECK(rows[6].text_max == 0);
    // What the game hands back is taken by the field's rule.
    CHECK(Menu::TakeText(k, 1, " 26.1.2.3:27512 "));
    CHECK(k.address == "26.1.2.3");
    CHECK(k.port == "27512");
    CHECK_FALSE(Menu::TakeText(k, 2, "70000"));
    CHECK(k.port == "27512");
    CHECK(Menu::Rows(k)[2].description == "The port is a number from 1 to 65535.");
    CHECK(Menu::TakeText(k, 0, "Zelda"));
    CHECK(k.name == "Zelda");
    CHECK_FALSE(Menu::TakeText(k, 6, "x"));  // not a field
    // Our own typing (a gamepad's press was seen): plain buttons again.
    k.game_typing = false;
    CHECK(Menu::Rows(k)[0].text_max == 0);
    // While we type ourselves, or a session is on its way, the game takes nothing.
    k.game_typing = true;
    Menu::Activate(k, 3);
    CHECK(Menu::Rows(k)[0].text_max == 0);
  }

  // A gamepad: up and down change the last character, right adds one, left removes one.
  s.address.clear();
  Menu::Activate(s, 1);
  Menu::EditPad(s, Menu::PadKey::up);  // "0"
  CHECK(s.edit == "0");
  Menu::EditPad(s, Menu::PadKey::up);     // "1"
  Menu::EditPad(s, Menu::PadKey::right);  // "11"
  Menu::EditPad(s, Menu::PadKey::down);   // "10"
  Menu::EditPad(s, Menu::PadKey::down);   // wraps to the alphabet's last, '-'
  CHECK(s.edit == "1-");
  Menu::EditPad(s, Menu::PadKey::left);
  CHECK(s.edit == "1");
  Menu::CancelEdit(s);
  // 127.0.0.1 with the pad alone.
  Menu::Activate(s, 1);
  const auto put = [&s](char c) {
    Menu::EditPad(s, s.edit.empty() ? Menu::PadKey::up : Menu::PadKey::right);
    for (int guard = 0; guard < 64 && s.edit.back() != c; ++guard) {
      Menu::EditPad(s, Menu::PadKey::up);
    }
  };
  for (const char c : std::string("127.0.0.1")) put(c);
  CHECK(s.edit == "127.0.0.1");
  CHECK(Menu::CommitEdit(s));
  CHECK(s.address == "127.0.0.1");
  // Leaving the page ends the typing.
  Menu::Activate(s, 0);
  Menu::Closed(s);
  CHECK(s.editing == Menu::Field::none);
  CHECK(s.page == Menu::Page::main);
}

TEST_CASE("nx2navi settings: host_save, and the file the game writes for its own restart") {
  std::vector<std::string> errors;
  CHECK_FALSE(ParseSettings("server = 127.0.0.1\n", errors).host_save);
  auto s = ParseSettings("server = 26.1.2.3\nport = 27511\nname = Mido\npassword = secret\nhost_save = 1\n"
                         "follow = anyone\npuppet = 0\nobjects_ai_hold = 2\n",
                         errors);
  CHECK(errors.empty());
  CHECK(s.host_save);
  const std::string text = CoopNx2navi::SettingsIni(s, false);
  CHECK(text.find("password") == std::string::npos);  // never written by the game
  const auto back = ParseSettings(text, errors);
  CHECK(errors.empty());
  CHECK(back.coop.server == "26.1.2.3");
  CHECK(back.coop.port == 27511);
  CHECK(back.coop.name == "Mido");
  CHECK(back.coop.password.empty());
  CHECK(back.host_save);
  CHECK(back.follow == CoopNx2navi::Settings::Follow::anyone);
  CHECK_FALSE(back.puppet);
  CHECK(back.objects_ai_hold == 2);
  CHECK(ParseSettings(CoopNx2navi::SettingsIni(s, true), errors).coop.password == "secret");
}

TEST_CASE("nx2navi save: the host's slot goes into a co-op profile, never into the player's own") {
  namespace fs = std::filesystem;
  namespace save = coop::host::save;
  const fs::path root = fs::temp_directory_path() / "coop-nx2navi-save-test";
  fs::remove_all(root);
  const fs::path own = root / "profile";
  const fs::path own_save = own / "save" / "0123456789abcdef";
  const auto put = [](const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  };
  const auto get = [](const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  put(own / "account.uid", "uid");
  put(own_save / "account.sav", "my account");
  put(own_save / "option.sav", "my options");
  put(own_save / "slot_00" / "progress.sav", "my progress");
  put(own_save / "slot_03" / "progress.sav", "my other progress");
  save::SaveSlot slot;
  slot.files.push_back({"progress.sav", {'h', 'o', 's', 't'}});
  slot.files.push_back({"caption.sav", {'c', 'a', 'p'}});
  const fs::path coop = root / "mod" / "coop-profile";
  std::string error;
  REQUIRE(CoopNx2navi::SaveSync::WriteCoopProfile(own, own_save, coop, slot, error));
  const fs::path target = coop / "save" / "0123456789abcdef";
  CHECK(get(target / "slot_00" / "progress.sav") == "host");
  CHECK(get(target / "slot_00" / "caption.sav") == "cap");
  CHECK(get(coop / "account.uid") == "uid");
  CHECK(get(target / "account.sav") == "my account");
  CHECK(get(target / "option.sav") == "my options");
  CHECK_FALSE(fs::exists(target / "slot_03"));
  // The player's own saves are as they were.
  CHECK(get(own_save / "slot_00" / "progress.sav") == "my progress");
  CHECK(get(own_save / "slot_03" / "progress.sav") == "my other progress");
  CHECK_FALSE(fs::exists(own_save / "slot_00" / "caption.sav"));

  // A second session: the earlier co-op save (with what the game added to it) is kept once.
  put(target / "slot_01" / "progress.sav", "autosave of the first session");
  put(coop / "save-tx" / "ready-0000000001" / "slot_00" / "progress.sav", "a commit that waited");
  slot.files[0].bytes = {'n', 'e', 'w'};
  REQUIRE(CoopNx2navi::SaveSync::WriteCoopProfile(own, own_save, coop, slot, error));
  CHECK(get(target / "slot_00" / "progress.sav") == "new");
  CHECK_FALSE(fs::exists(target / "slot_01"));
  CHECK_FALSE(fs::exists(coop / "save-tx"));
  CHECK(get(coop / "save-previous" / "slot_00" / "progress.sav") == "host");
  CHECK(get(coop / "save-previous" / "slot_01" / "progress.sav") == "autosave of the first session");

  // Refused: the player's own profile, a folder inside its saves, no progress, nothing known.
  CHECK_FALSE(CoopNx2navi::SaveSync::WriteCoopProfile(own, own_save, own, slot, error));
  CHECK(error.find("own") != std::string::npos);
  CHECK_FALSE(CoopNx2navi::SaveSync::WriteCoopProfile(own, own_save, own_save / "coop", slot, error));
  CHECK_FALSE(CoopNx2navi::SaveSync::WriteCoopProfile(own, own_save, root, slot, error));
  CHECK_FALSE(CoopNx2navi::SaveSync::WriteCoopProfile(own, fs::path(), coop, slot, error));
  save::SaveSlot empty;
  CHECK_FALSE(CoopNx2navi::SaveSync::WriteCoopProfile(own, own_save, coop, empty, error));
  CHECK(get(own_save / "slot_00" / "progress.sav") == "my progress");
  fs::remove_all(root);
}

namespace {

// Two lines of at most 46 characters: what the game's caption box shows whole.
bool CaptionFits(const std::string& text) {
  std::size_t lines = 1, start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      if (CoopNx2navi::Ui::Utf8Count(std::string_view(text).substr(start, i - start)) > 46) {
        return false;
      }
      start = i + 1;
      lines += i < text.size() ? 1 : 0;
    }
  }
  return lines <= 2;
}

Menu::SaveInfo Save(std::string folder, std::string played = "2026-11-13 05:47", std::string place = "HyruleField_Adult") {
  Menu::SaveInfo info;
  const auto parts = MpSaves::PartsOf(folder);
  info.folder = std::move(folder);
  info.name = parts.name;
  info.host = parts.host;
  info.received = parts.received;
  info.empty = played.empty();
  info.played = std::move(played);
  info.place = info.empty ? std::string() : std::move(place);
  return info;
}

// A game that can be started on a save folder, with some multiplayer saves.
Menu::State WithSaves(Menu::Lang lang, std::vector<Menu::SaveInfo> saves) {
  Menu::State s;
  s.lang = lang;
  s.name = "Link";
  s.saves_on = true;
  s.dim_steam = true;
  s.solo_save = true;
  Menu::SetSaves(s, std::move(saves));
  return s;
}

}  // namespace

TEST_CASE("nx2navi menu: hosting needs a multiplayer save, chosen on the Host page's Save row") {
  Menu::State s = WithSaves(Menu::Lang::fr, {});
  auto rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(rows[4].label == "Sauvegardes multijoueur");
  CHECK(rows[4].value == "Aucune");
  CHECK_FALSE(rows[4].disabled);
  // Host: no save yet, so Start says what is missing and starts nothing.
  CHECK(Menu::Activate(s, 1) == Menu::Action::redraw);
  rows = Menu::Rows(s);
  CHECK(rows[4].label == "Sauvegarde");
  CHECK(rows[4].value == "Choisir");
  CHECK(rows[4].description ==
        "La sauvegarde multijoueur de cette partie.\nVos sauvegardes solo ne sont jamais modifi\xc3\xa9" "es.");
  CHECK(rows[6].description.find("Le jeu red\xc3\xa9marre sur la sauvegarde choisie") == 0);
  CHECK(Menu::Activate(s, 6) == Menu::Action::redraw);
  CHECK(s.phase == Menu::Phase::failed);
  CHECK(Menu::Rows(s)[6].description ==
        "Choisissez d'abord une sauvegarde multijoueur\n(ligne Sauvegarde ci-dessus).");
  std::string why;
  CHECK_FALSE(Menu::ReadyToHost(s, why));

  // The Save row opens the list: nothing there, the cursor on "New save".
  CHECK(Menu::Activate(s, 4) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::saves);
  CHECK(s.focus == 1 + 4);
  CHECK(Menu::Title(s) == "Sauvegardes multijoueur");
  rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(rows[i].label == "-");
    CHECK(rows[i].disabled);
    CHECK(Menu::Activate(s, i) == Menu::Action::none);
  }
  CHECK(rows[3].label == "Autres sauvegardes");
  CHECK(rows[3].disabled);
  CHECK(rows[4].label == "Nouvelle sauvegarde");
  CHECK(rows[4].kind == Menu::RowKind::text);
  CHECK(rows[4].value == "Coop");  // a name to start from
  CHECK(rows[5].kind == Menu::RowKind::separator);
  CHECK(rows[6].label == "Copier ma sauvegarde solo");
  CHECK(rows[6].value == "Copier");
  CHECK(rows[7].label == "Retour");

  // A new save: its name is typed; the owner is told to make it.
  CHECK(Menu::Activate(s, 4) == Menu::Action::begin_edit);
  CHECK(s.editing == Menu::Field::save_new);
  CHECK(s.edit == "Coop");
  for (int i = 0; i < 4; ++i) Menu::EditBackspace(s);
  for (const char c : std::string("Zelda")) Menu::EditText(s, static_cast<unsigned char>(c));
  Menu::EditText(s, 0xe9);  // any character a name can have
  CHECK(Menu::Rows(s)[4].value == "Zelda\xc3\xa9_");
  CHECK(Menu::CommitEdit(s));
  Menu::PendingOp op = Menu::TakePending(s);
  CHECK(op.op == Menu::SaveOp::create);
  CHECK(op.name == "Zelda\xc3\xa9");
  CHECK(Menu::TakePending(s).op == Menu::SaveOp::none);  // once
  // Made: it is the save to play on, and the Host page is back with it on its row.
  Menu::OpDone(s, op, true, "Zelda\xc3\xa9", {Save("Zelda\xc3\xa9", "")});
  CHECK(s.page == Menu::Page::host);
  CHECK(s.focus == 1 + 4);
  CHECK(s.save == "Zelda\xc3\xa9");
  CHECK(Menu::Rows(s)[4].value == "Zelda\xc3\xa9");
  CHECK(Menu::ReadyToHost(s, why));
  s.phase = Menu::Phase::idle;
  s.message.clear();
  CHECK(Menu::Activate(s, 6) == Menu::Action::start_host);

  // Names that cannot be: said under the row, nothing asked of the owner.
  Menu::Activate(s, 4);
  REQUIRE(s.page == Menu::Page::saves);
  CHECK(s.focus == 1);  // the chosen save under the cursor
  for (const char* bad : {"", "a/b", "x@y", "CON", "zelda\xc3\xa9", "12345678901234567", " ", "_deleted"}) {
    CAPTURE(bad);
    CHECK_FALSE(Menu::TakeText(s, 4, bad));
    CHECK(Menu::TakePending(s).op == Menu::SaveOp::none);
    CHECK_FALSE(s.save_note.empty());
    CHECK(Menu::Rows(s)[4].description == s.save_note);
    CHECK(CaptionFits(s.save_note));
  }
  CHECK(s.save_note == "Nom d'une sauvegarde : 1 \xc3\xa0 16 caract\xc3\xa8res,\nsans \\ / : * ? \" < > | @");
  CHECK_FALSE(Menu::TakeText(s, 4, "ZELDA\xc3\xa9"));  // one folder to Windows
  CHECK(s.save_note == "Une sauvegarde porte d\xc3\xa9j\xc3\xa0 ce nom.");
  // The game's own typing (a keyboard): the name comes whole.
  s.game_typing = true;
  CHECK(Menu::Rows(s)[4].text_max == 64);
  CHECK(Menu::Rows(s)[4].value == "Coop");
  CHECK(Menu::TakeText(s, 4, " Navi "));
  op = Menu::TakePending(s);
  CHECK(op == Menu::PendingOp{Menu::SaveOp::create, "", "Navi"});
  // It could not be made: said, the page stays.
  Menu::OpDone(s, op, false, "", {Save("Zelda\xc3\xa9", "")});
  CHECK(s.page == Menu::Page::saves);
  CHECK(s.save_note == "Impossible pour cette sauvegarde\n(coop-nx2navi.log dit pourquoi).");
  CHECK(s.save == "Zelda\xc3\xa9");
  // Back: the Host page, the cursor on the Save row.
  CHECK(Menu::Activate(s, 7) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::host);
  CHECK(s.focus == 1 + 4);
}

TEST_CASE("nx2navi menu: the list of multiplayer saves, a copy of the solo save, and a save's own page") {
  Menu::State s = WithSaves(Menu::Lang::en, {Save("Zelda"), Save("Coop", ""), Save("Quest@Saria", "2026-11-12 21:00", "GerudoValley_Adult"),
                                             Save("Four"), Save("@Mido")});
  CHECK(Menu::Rows(s)[4].value == "5");
  CHECK(Menu::Activate(s, 4) == Menu::Action::redraw);  // from the first page
  CHECK(s.page == Menu::Page::saves);
  CHECK(s.focus == 1);
  auto rows = Menu::Rows(s);
  CHECK(rows[0].label == "Zelda");
  CHECK(rows[0].value == "2026-11-13 05:47");
  CHECK(rows[0].description == "Saved in HyruleField_Adult\nYour multiplayer save.");
  CHECK(rows[1].label == "Coop");
  CHECK(rows[1].value == "New game");
  CHECK(rows[1].description == "Not begun yet: a new game.\nYour multiplayer save.");
  CHECK(rows[2].label == "Quest (Saria)");
  CHECK(rows[2].description == "Saved in GerudoValley_Adult\nReceived from Saria.");
  CHECK(rows[3].label == "More saves");
  CHECK(rows[3].value == "1-3 / 5");
  CHECK_FALSE(rows[3].disabled);
  CHECK(rows[4].value == "Coop 2");  // "Coop" is taken
  // The next ones, and round again.
  CHECK(Menu::Activate(s, 3) == Menu::Action::redraw);
  rows = Menu::Rows(s);
  CHECK(rows[0].label == "Four");
  CHECK(rows[1].label == "(Mido)");  // a host that told no save name
  CHECK(rows[2].label == "-");
  CHECK(rows[2].disabled);
  CHECK(rows[3].value == "4-5 / 5");
  CHECK(Menu::Activate(s, 2) == Menu::Action::none);
  CHECK(Menu::Activate(s, 3) == Menu::Action::redraw);
  CHECK(s.save_first == 0);

  // Copy my solo save: a name by itself, the owner told to copy.
  CHECK(Menu::Activate(s, 6) == Menu::Action::save_op);
  Menu::PendingOp op = Menu::TakePending(s);
  CHECK(op == Menu::PendingOp{Menu::SaveOp::copy_solo, "", "Solo"});
  auto now = s.saves;
  now.insert(now.begin(), Save("Solo"));
  Menu::OpDone(s, op, true, "Solo", now);
  CHECK(s.page == Menu::Page::saves);  // the list was opened from the first page
  CHECK(s.save == "Solo");
  CHECK(s.focus == 1);
  CHECK(Menu::Rows(s)[0].label == "Solo");
  CHECK(Menu::Rows(s)[0].description == "Saved in HyruleField_Adult\nChosen for hosting.");
  CHECK(Menu::Activate(s, 6) == Menu::Action::save_op);
  CHECK(Menu::TakePending(s).name == "Solo 2");
  // No save of the player's own: said, nothing asked.
  s.solo_save = false;
  CHECK(Menu::Rows(s)[6].value == "None");
  CHECK(Menu::Activate(s, 6) == Menu::Action::redraw);
  CHECK(Menu::Rows(s)[6].description == "You have no solo save to copy yet.");
  CHECK(Menu::TakePending(s).op == Menu::SaveOp::none);
  s.solo_save = true;

  // A save's own page.
  CHECK(Menu::Activate(s, 1) == Menu::Action::redraw);  // Zelda
  CHECK(s.page == Menu::Page::save);
  CHECK(Menu::Title(s) == "Zelda");
  rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(rows[0].label == "Play on it");
  CHECK(rows[0].value == "Choose");
  CHECK(rows[1].label == "Name");
  CHECK(rows[1].kind == Menu::RowKind::text);
  CHECK(rows[1].value == "Zelda");
  CHECK(rows[2].label == "Last played");
  CHECK(rows[2].value == "2026-11-13 05:47");
  CHECK(rows[3].label == "Place");
  CHECK(rows[3].value == "HyruleField_Adult");
  CHECK(rows[4].label == "Origin");
  CHECK(rows[4].value == "Yours");
  CHECK(rows[4].description == "Your save: it is the reference. Friends who\njoin get a copy of it.");
  CHECK(rows[5].kind == Menu::RowKind::separator);
  CHECK(rows[6].label == "Delete");
  CHECK(rows[6].value == "Delete");
  CHECK(rows[7].label == "Back");
  for (const std::size_t told : {2u, 3u, 4u}) {
    CHECK(Menu::Activate(s, told) == Menu::Action::none);  // rows that only tell
  }
  // Another name.
  CHECK(Menu::Activate(s, 1) == Menu::Action::begin_edit);
  CHECK(s.edit == "Zelda");
  Menu::EditText(s, '2');
  CHECK(Menu::CommitEdit(s));
  op = Menu::TakePending(s);
  CHECK(op == Menu::PendingOp{Menu::SaveOp::rename, "Zelda", "Zelda2"});
  now = s.saves;
  for (auto& save : now) {
    if (save.folder == "Zelda") save = Save("Zelda2");
  }
  s.save = "Zelda";  // it was the chosen one
  Menu::OpDone(s, op, true, "Zelda2", now);
  CHECK(s.page == Menu::Page::save);
  CHECK(Menu::Title(s) == "Zelda2");
  CHECK(s.save == "Zelda2");  // still the chosen one, under its new name
  CHECK(Menu::Rows(s)[0].value == "Chosen");
  CHECK_FALSE(Menu::TakeText(s, 1, "Four"));  // taken
  CHECK(Menu::Rows(s)[1].description == "A save of that name is there already.");
  CHECK(Menu::TakeText(s, 1, "Zelda2"));      // as it was: nothing to do
  CHECK(Menu::TakePending(s).op == Menu::SaveOp::none);
  CHECK(Menu::TakeText(s, 1, "ZELDA2"));      // only the capitals: the same folder, renamed
  CHECK(Menu::TakePending(s).op == Menu::SaveOp::rename);
  // Play on it: chosen, and on to the Host page with the cursor on Start.
  s.save.clear();
  CHECK(Menu::Activate(s, 0) == Menu::Action::save_chosen);
  CHECK(s.save == "Zelda2");
  CHECK(s.page == Menu::Page::host);
  CHECK(s.focus == 1 + 6);
  CHECK(Menu::Rows(s)[4].value == "Zelda2");

  // A save received from a host: named by the host, not renamed here.
  Menu::Activate(s, 4);
  REQUIRE(s.page == Menu::Page::saves);
  for (std::size_t i = 0; i < s.saves.size(); ++i) {
    if (s.saves[i].folder == "Quest@Saria") {
      s.save_first = i - i % 3;
      CHECK(Menu::Activate(s, i % 3) == Menu::Action::redraw);
    }
  }
  CHECK(Menu::Title(s) == "Quest (Saria)");
  rows = Menu::Rows(s);
  CHECK(rows[1].kind == Menu::RowKind::action);
  CHECK(rows[1].value == "Quest");
  CHECK(Menu::Activate(s, 1) == Menu::Action::none);
  CHECK_FALSE(Menu::TakeText(s, 1, "Mine"));
  CHECK(rows[4].value == "Saria");
  CHECK(rows[4].description ==
        "A copy of the host's save, replaced each\ntime you join them (the last one is kept).");
  // Back: the list, the cursor on that save.
  CHECK(Menu::Activate(s, 7) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::saves);
  CHECK(Menu::Rows(s)[s.focus - 1].label == "Quest (Saria)");
  // Leaving the pages forgets what was open, not what was chosen.
  Menu::Closed(s);
  CHECK(s.page == Menu::Page::main);
  CHECK(s.save == "Zelda2");
}

TEST_CASE("nx2navi menu: deleting a multiplayer save is asked twice") {
  Menu::State s = WithSaves(Menu::Lang::fr, {Save("Zelda", "13/11/2026 05:47"), Save("Coop", "")});
  s.save = "Zelda";
  s.running_save = "Coop";  // the game was started on it
  Menu::Activate(s, 4);
  CHECK(Menu::Activate(s, 0) == Menu::Action::redraw);
  REQUIRE(s.page == Menu::Page::save);
  auto rows = Menu::Rows(s);
  CHECK(rows[6].label == "Supprimer");
  CHECK(rows[6].value == "Supprimer");
  CHECK(rows[6].description == "Retire cette sauvegarde de la liste\n(gard\xc3\xa9" "e dans coop-saves\\_deleted).");
  // Once: armed, the row asks; nothing is asked of the owner yet.
  CHECK(Menu::Activate(s, 6) == Menu::Action::redraw);
  CHECK(s.delete_armed);
  rows = Menu::Rows(s);
  CHECK(rows[6].value == "Confirmer");
  CHECK(rows[6].description == "Supprimer \xc2\xab Zelda \xc2\xbb ?\nValidez encore, ou changez de ligne.");
  CHECK(Menu::TakePending(s).op == Menu::SaveOp::none);
  // The cursor on the row itself changes nothing; on another row it lets go.
  CHECK_FALSE(Menu::Focused(s, 6));
  CHECK(s.delete_armed);
  CHECK(Menu::Focused(s, 7));
  CHECK_FALSE(s.delete_armed);
  CHECK(Menu::Rows(s)[6].value == "Supprimer");
  CHECK_FALSE(Menu::Focused(s, 6));
  // Armed again, then another row chosen: let go as well.
  Menu::Activate(s, 6);
  CHECK(Menu::Activate(s, 2) == Menu::Action::redraw);
  CHECK_FALSE(s.delete_armed);
  // Twice: the owner is told; done, the list is back and the chosen save is none.
  Menu::Activate(s, 6);
  CHECK(Menu::Activate(s, 6) == Menu::Action::save_op);
  const Menu::PendingOp op = Menu::TakePending(s);
  CHECK(op == Menu::PendingOp{Menu::SaveOp::remove, "Zelda", ""});
  Menu::OpDone(s, op, true, "Zelda", {Save("Coop", "")});
  CHECK(s.page == Menu::Page::saves);
  CHECK(s.save.empty());
  CHECK(s.saves.size() == 1);
  CHECK(s.focus == 1);
  // Not the save the game runs on.
  CHECK(Menu::Activate(s, 0) == Menu::Action::redraw);
  CHECK(Menu::Title(s) == "Coop");
  CHECK(Menu::Activate(s, 6) == Menu::Action::redraw);
  CHECK_FALSE(s.delete_armed);
  CHECK(Menu::Rows(s)[6].description == "Le jeu tourne sur cette sauvegarde :\npas pendant ce temps.");
  CHECK(Menu::TakePending(s).op == Menu::SaveOp::none);
  // A save that went away under the page: the list.
  Menu::SetSaves(s, {});
  CHECK(s.page == Menu::Page::saves);
  CHECK(s.focus == 1 + 4);
}

TEST_CASE("nx2navi menu: every caption of the saves pages fits, in both languages") {
  const std::string long_name = "WWWWWWWWWWWWWWWW";  // 16 characters
  for (const Menu::Lang lang : {Menu::Lang::fr, Menu::Lang::en}) {
    for (const bool on : {true, false}) {
      Menu::State s = WithSaves(lang, {Save(long_name, "13/11/2026 05:47", std::string(64, 'S')), Save("Coop", ""),
                                       Save(long_name + "@" + long_name), Save("@" + long_name),
                                       Save("Five", "2026-11-13 05:47", "")});
      s.saves_on = on;
      s.save = long_name;
      const auto check = [&](const char* what) {
        CAPTURE(what);
        const auto rows = Menu::Rows(s);
        REQUIRE(rows.size() == Menu::kRows);
        std::string kinds;
        for (const auto& r : rows) {
          kinds += r.kind == Menu::RowKind::separator ? '-' : 'b';  // a text row is a button to the game
          CHECK_MESSAGE(CaptionFits(r.description), r.description);
        }
        CHECK(kinds == "bbbbb-bb");  // one skeleton: a page change is an update of the rows
      };
      for (const Menu::Page page : {Menu::Page::main, Menu::Page::host, Menu::Page::join}) {
        s.page = page;
        check("a first page");
      }
      s.page = Menu::Page::saves;
      for (s.save_first = 0; s.save_first < s.saves.size(); s.save_first += 3) {
        check("the list");
      }
      s.save_first = 0;
      s.solo_save = false;
      check("the list, no solo save");
      Menu::Activate(s, 6);
      check("the list, no solo save said");
      s.solo_save = true;
      Menu::Activate(s, 4);
      check("the list, typing a name");
      Menu::CancelEdit(s);
      Menu::TakeText(s, 4, "a/b");
      check("the list, a refused name");
      Menu::TakeText(s, 4, "coop");
      check("the list, a taken name");
      for (const auto& save : s.saves) {
        s.page = Menu::Page::save;
        s.save_shown = save.folder;
        s.save_note.clear();
        check("a save");
        Menu::Activate(s, 6);
        check("a save, delete armed");
        Menu::Focused(s, 0);
        s.running_save = save.folder;
        Menu::Activate(s, 6);
        check("a save, in use");
        s.running_save.clear();
        if (!save.received) {
          Menu::Activate(s, 1);
          check("a save, typing its name");
          Menu::CancelEdit(s);
        }
      }
      Menu::OpDone(s, Menu::PendingOp{Menu::SaveOp::remove, "x", ""}, false, "", s.saves);
      check("a failed change");
    }
  }
}

TEST_CASE("nx2navi saves: names, the folder of a save received from a host, dates and places") {
  CHECK(MpSaves::ValidName("Zelda"));
  CHECK(MpSaves::ValidName("Ma partie 2"));
  CHECK(MpSaves::ValidName("\xc3\x89t\xc3\xa9"));
  CHECK(MpSaves::ValidName("1234567890123456"));
  for (const char* bad : {"", " ", " x", "x ", "x.", ".x", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b",
                          "a|b", "a@b", "a\nb", "12345678901234567", "CON", "nul", "com1", "LPT9", "con.txt",
                          "_deleted", ".."}) {
    CAPTURE(bad);
    CHECK_FALSE(MpSaves::ValidName(bad));
  }
  // A save received from a host: the host's save name, '@', the host's player name.
  CHECK(MpSaves::ReceivedFolder("Zelda", "Saria") == "Zelda@Saria");
  CHECK(MpSaves::ReceivedFolder("", "Saria") == "@Saria");          // a host that told no name
  CHECK(MpSaves::ReceivedFolder("Zelda", "") == "Zelda@host");
  CHECK(MpSaves::ReceivedFolder("a/b", "S:a\\r*i?a.") == "a_b@S_a_r_i_a_");  // nothing a folder cannot hold
  CHECK(MpSaves::ReceivedFolder("x@y", "a@b") == "x_y@a_b");
  CHECK_FALSE(MpSaves::ValidName(MpSaves::ReceivedFolder("Zelda", "Saria")));  // never a typed name
  auto parts = MpSaves::PartsOf("Zelda@Saria");
  CHECK(parts.received);
  CHECK(parts.name == "Zelda");
  CHECK(parts.host == "Saria");
  parts = MpSaves::PartsOf("Zelda");
  CHECK_FALSE(parts.received);
  CHECK(parts.name == "Zelda");
  // Which multiplayer save a game's save folder is.
  namespace fs = std::filesystem;
  const fs::path mod = fs::path("C:/game/mods/OoT Co-op");
  CHECK(MpSaves::Root(mod) == mod / "coop-saves");
  CHECK(MpSaves::NameOfSaveDir(MpSaves::Root(mod) / "Zelda" / "save" / "0123abcd") == "Zelda");
  CHECK(MpSaves::FolderOfSaveDir(MpSaves::Root(mod) / "Zelda@Saria" / "save" / "0123abcd") == "Zelda@Saria");
  CHECK(MpSaves::NameOfSaveDir(MpSaves::Root(mod) / "Zelda@Saria" / "save" / "0123abcd") == "Zelda");
  CHECK(MpSaves::NameOfSaveDir(MpSaves::Root(mod) / "@Saria" / "save" / "0123abcd").empty());
  CHECK(MpSaves::NameOfSaveDir(fs::path("C:/game/profile/save/0123abcd")).empty());  // the player's own
  CHECK(MpSaves::NameOfSaveDir(fs::path("C:/game/coop-saves/save/0123abcd")).empty());
  CHECK(MpSaves::NameOfSaveDir({}).empty());
  // The place a caption.sav names, and the date as the page shows it.
  const std::string caption = std::string("\x04\x03\x02\x01junk/x\0more", 16) + "CHyruleField" +
                              std::string(1, '\0') + "HyruleField/HyruleField_Adult" +
                              std::string("\0\x7f/\x01", 4);
  CHECK(MpSaves::PlaceOf(caption) == "HyruleField_Adult");
  // A byte of the file's own right before the text (a length that happens to be a letter).
  CHECK(MpSaves::PlaceOf(std::string(1, '\0') + "CGerudoValley/GerudoValley_Adult" + std::string(1, '\0')) ==
        "GerudoValley_Adult");
  CHECK(MpSaves::PlaceOf("nothing here / at all").empty());
  CHECK(MpSaves::PlaceOf("").empty());
  std::tm t{};
  t.tm_year = 126;
  t.tm_mon = 10;
  t.tm_mday = 3;
  t.tm_hour = 5;
  t.tm_min = 7;
  CHECK(MpSaves::DateText(t, false) == "2026-11-03 05:07");
  CHECK(MpSaves::DateText(t, true) == "03/11/2026 05:07");
  CHECK(MpSaves::DateText(std::int64_t{0}, true).empty());
  // The save's name as a host sends it after the save.
  namespace sync = CoopNx2navi::SaveSync;
  const auto packet = sync::NamePacket("Zelda");
  CHECK(sync::NameOf(packet) == std::optional<std::string>("Zelda"));
  CHECK(sync::NameOf(sync::NamePacket("../x")) == std::optional<std::string>(""));  // not a name: none
  const std::vector<std::uint8_t> slot = {'O', 'O', 'T', 'S', 'A', 'V', 'E', '1', 0};
  CHECK_FALSE(sync::NameOf(slot).has_value());  // a save slot is not a name
  CHECK_FALSE(sync::NameOf(std::vector<std::uint8_t>{}).has_value());
  // And what a host that is beginning a new game on an empty save answers with.
  CHECK(sync::NewGameOf(sync::NewGamePacket("Coop")) == std::optional<std::string>("Coop"));
  CHECK_FALSE(sync::NewGameOf(packet).has_value());                    // a name is not "a new game"
  CHECK_FALSE(sync::NameOf(sync::NewGamePacket("Coop")).has_value());  // nor the other way round
  CHECK_FALSE(sync::NewGameOf(slot).has_value());
  CHECK(sync::NewGameOf(sync::NewGamePacket("a\\b")) == std::optional<std::string>(""));
}

TEST_CASE("nx2navi saves: made, copied from the solo save, listed, renamed and deleted beside the player's own") {
  namespace fs = std::filesystem;
  const fs::path base = fs::temp_directory_path() / "coop-nx2navi-mp-saves-test";
  fs::remove_all(base);
  const std::string account = "0123456789abcdef0123456789abcdef";
  const fs::path own = base / "profile" / "save" / account;
  const fs::path root = MpSaves::Root(base / "mod");
  const auto put = [](const fs::path& path, const std::string& text, int age_s = 0) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
    if (age_s != 0) {
      fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::seconds(age_s));
    }
  };
  const auto get = [](const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  // Every file under a folder with its bytes: the player's own saves must stay as they are.
  const auto snapshot = [&get](const fs::path& dir) {
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file()) {
        files.emplace_back(entry.path().lexically_relative(dir).generic_string(), get(entry.path()));
      }
    }
    std::sort(files.begin(), files.end());
    return files;
  };
  put(own / "account.sav", "my account");
  put(own / "option.sav", "my options");
  put(own / "slot_00" / "progress.sav", "old solo progress", 5000);
  put(own / "slot_00" / "caption.sav", "xxOldPlace/OldPlace_Childxx", 5000);
  put(own / "slot_03" / "progress.sav", "NEWEST SOLO PROGRESS", 100);
  put(own / "slot_03" / "caption.sav", std::string("\x01\x02", 2) + "HyruleField/HyruleField_Adult" + std::string(1, '\0'), 100);
  const auto solo_before = snapshot(base / "profile");
  std::string error;

  CHECK(MpSaves::List(root).empty());  // no folder yet
  // A new save: a folder the game can start on, with the player's settings and no slot.
  REQUIRE(MpSaves::Create(root, "Zelda", account, own, error));
  const fs::path zelda = root / "Zelda" / "save" / account;
  CHECK(get(zelda / "option.sav") == "my options");
  CHECK(get(zelda / "account.sav") == "my account");
  CHECK_FALSE(fs::exists(zelda / "slot_00"));
  auto list = MpSaves::List(root);
  REQUIRE(list.size() == 1);
  CHECK(list[0].folder == "Zelda");
  CHECK(list[0].name == "Zelda");
  CHECK(list[0].empty);
  CHECK(list[0].played == 0);
  CHECK_FALSE(list[0].received);
  CHECK_FALSE(MpSaves::Create(root, "zelda", account, own, error));  // the same folder to Windows
  CHECK(error.find("already") != std::string::npos);
  CHECK_FALSE(MpSaves::Create(root, "a/b", account, own, error));
  CHECK_FALSE(MpSaves::Create(root, "NoAccount", "", own, error));
  CHECK_FALSE(MpSaves::Create(own / "inside", "Bad", account, own, error));  // never in the player's own save
  CHECK(MpSaves::FreeName(list, "Zelda") == "Zelda 2");
  CHECK(MpSaves::FreeName(list, "Coop") == "Coop");

  // A copy of the newest solo slot, under the slot's own name; the solo save untouched.
  REQUIRE(MpSaves::CopySolo(root, "Solo", own, error));
  const fs::path solo = root / "Solo" / "save" / account;
  CHECK(get(solo / "slot_03" / "progress.sav") == "NEWEST SOLO PROGRESS");
  CHECK(get(solo / "slot_03" / "caption.sav").find("HyruleField_Adult") != std::string::npos);
  CHECK_FALSE(fs::exists(solo / "slot_00"));
  CHECK(get(solo / "option.sav") == "my options");
  CHECK(fs::hard_link_count(solo / "slot_03" / "progress.sav") == 1);  // a copy of its own, never a link
  CHECK_FALSE(MpSaves::CopySolo(root, "Solo", own, error));
  CHECK_FALSE(MpSaves::CopySolo(root, "FromMp", solo, error));  // a multiplayer save is not a solo save
  CHECK(error.find("multiplayer") != std::string::npos);
  CHECK_FALSE(MpSaves::CopySolo(root, "Empty", base / "profile2" / "save" / account, error));
  CHECK(error.find("no save") != std::string::npos);
  CHECK_FALSE(fs::exists(root / "Empty"));
  CHECK_FALSE(fs::exists(root / "FromMp"));

  // What the game and a host leave there: a played save, and one received from Saria's game.
  put(zelda / "slot_00" / "progress.sav", "co-op progress");  // written now: the last played
  put(zelda / "slot_00" / "caption.sav", "..GerudoValley/GerudoValley_Adult..");
  put(root / "Quest@Saria" / "save" / account / "slot_00" / "progress.sav", "the host's", 50000);
  put(root / "Quest@Saria" / "save-previous" / "slot_00" / "progress.sav", "the join before");
  put(root / "stray.txt", "not a save");
  fs::create_directories(root / "NotASave" / "other");
  list = MpSaves::List(root);
  REQUIRE(list.size() == 3);
  CHECK(list[0].folder == "Zelda");  // the last played first
  CHECK_FALSE(list[0].empty);
  CHECK(list[0].place == "GerudoValley_Adult");
  CHECK(list[0].slots == 1);
  CHECK(list[0].played > 1700000000);
  CHECK(list[1].folder == "Solo");
  CHECK(list[1].place == "HyruleField_Adult");
  CHECK(list[1].played < list[0].played);
  CHECK(list[2].folder == "Quest@Saria");
  CHECK(list[2].name == "Quest");
  CHECK(list[2].host == "Saria");
  CHECK(list[2].received);
  CHECK(MpSaves::Find(root, "Solo").has_value());
  CHECK_FALSE(MpSaves::Find(root, "Nope").has_value());
  CHECK(MpSaves::FreeName(list, "Solo") == "Solo 2");

  // Another name; not one that is taken, not a received save's, not the save the game runs on.
  CHECK(MpSaves::Rename(root, "Solo", "Ma copie", {}, error));
  CHECK(get(root / "Ma copie" / "save" / account / "slot_03" / "progress.sav") == "NEWEST SOLO PROGRESS");
  CHECK_FALSE(fs::exists(root / "Solo"));
  CHECK_FALSE(MpSaves::Rename(root, "Ma copie", "ZELDA", {}, error));
  CHECK_FALSE(MpSaves::Rename(root, "Quest@Saria", "Mine", {}, error));
  CHECK_FALSE(MpSaves::Rename(root, "Zelda", "Zelda2", zelda, error));
  CHECK(error.find("runs on") != std::string::npos);
  CHECK(MpSaves::Rename(root, "Zelda", "ZELDA", {}, error));  // only the capitals
  CHECK(MpSaves::Find(root, "ZELDA").has_value());
  CHECK(MpSaves::Rename(root, "ZELDA", "Zelda", {}, error));

  // Deleted: out of the list, kept once in _deleted; never the save in use, never anything else.
  CHECK_FALSE(MpSaves::Remove(root, "Zelda", zelda, error));
  CHECK(error.find("runs on") != std::string::npos);
  CHECK_FALSE(MpSaves::Remove(root, "..", {}, error));
  CHECK_FALSE(MpSaves::Remove(root, "", {}, error));
  CHECK_FALSE(MpSaves::Remove(root, "Zelda/save", {}, error));
  CHECK_FALSE(MpSaves::Remove(root, "_deleted", {}, error));
  CHECK_FALSE(MpSaves::Remove(root, "NotASave", {}, error));  // no save folder in it
  CHECK_FALSE(MpSaves::Remove(root, "Nope", {}, error));
  CHECK(fs::exists(root / "NotASave" / "other"));
  REQUIRE(MpSaves::Remove(root, "Zelda", {}, error));
  CHECK_FALSE(fs::exists(root / "Zelda"));
  CHECK(get(root / "_deleted" / "Zelda" / "save" / account / "slot_00" / "progress.sav") == "co-op progress");
  list = MpSaves::List(root);
  REQUIRE(list.size() == 2);  // _deleted is not a save
  CHECK(list[0].folder == "Ma copie");
  // The same name again, deleted again: the earlier one in _deleted is replaced.
  REQUIRE(MpSaves::Create(root, "Zelda", account, own, error));
  REQUIRE(MpSaves::Remove(root, "Zelda", {}, error));
  CHECK_FALSE(fs::exists(root / "_deleted" / "Zelda" / "save" / account / "slot_00"));
  CHECK(MpSaves::Remove(root, "Quest@Saria", {}, error));

  // The player's own saves: every file as it was.
  CHECK(snapshot(base / "profile") == solo_before);
  CHECK(fs::hard_link_count(own / "slot_03" / "progress.sav") == 1);
  fs::remove_all(base);
}

TEST_CASE("nx2navi saves: a joiner keeps the host's save among its multiplayer saves, under the host's save name") {
  namespace fs = std::filesystem;
  namespace save = coop::host::save;
  const fs::path base = fs::temp_directory_path() / "coop-nx2navi-mp-join-test";
  fs::remove_all(base);
  const std::string account = "0123456789abcdef0123456789abcdef";
  const fs::path profile = base / "profile";
  const fs::path own = profile / "save" / account;
  const auto put = [](const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  };
  const auto get = [](const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  put(profile / "account.uid", "uid");
  put(own / "option.sav", "my options");
  put(own / "slot_00" / "progress.sav", "my solo progress");
  const fs::path root = MpSaves::Root(base / "mod");
  // The host plays on its save "Zelda" and is called Saria: the folder is Zelda@Saria.
  const fs::path folder = root / MpSaves::ReceivedFolder("Zelda", "Saria");
  save::SaveSlot slot;
  slot.files.push_back({"progress.sav", {'h', 'o', 's', 't'}});
  std::string error;
  REQUIRE(CoopNx2navi::SaveSync::WriteCoopProfile(profile, own, folder, slot, error));
  CHECK(get(folder / "save" / account / "slot_00" / "progress.sav") == "host");
  CHECK(get(folder / "save" / account / "option.sav") == "my options");
  auto list = MpSaves::List(root);
  REQUIRE(list.size() == 1);
  CHECK(list[0].folder == "Zelda@Saria");
  CHECK(list[0].name == "Zelda");
  CHECK(list[0].host == "Saria");
  CHECK(list[0].received);
  CHECK_FALSE(list[0].empty);
  // The joiner's own save "Zelda" is another folder: neither replaces the other.
  REQUIRE(MpSaves::Create(root, "Zelda", account, own, error));
  CHECK(MpSaves::List(root).size() == 2);
  // The next join of the same host's save: the host's is the reference; what the joiner had is kept once.
  put(folder / "save" / account / "slot_01" / "progress.sav", "what the joiner's game saved in the session");
  slot.files[0].bytes = {'n', 'e', 'x', 't'};
  REQUIRE(CoopNx2navi::SaveSync::WriteCoopProfile(profile, own, folder, slot, error));
  CHECK(get(folder / "save" / account / "slot_00" / "progress.sav") == "next");
  CHECK_FALSE(fs::exists(folder / "save" / account / "slot_01"));
  CHECK(get(folder / "save-previous" / "slot_01" / "progress.sav") == "what the joiner's game saved in the session");
  CHECK(MpSaves::List(root).size() == 2);
  // Another save of that host, and the same save name from another host, are folders of their own.
  REQUIRE(CoopNx2navi::SaveSync::WriteCoopProfile(profile, own, root / MpSaves::ReceivedFolder("Quest", "Saria"), slot, error));
  REQUIRE(CoopNx2navi::SaveSync::WriteCoopProfile(profile, own, root / MpSaves::ReceivedFolder("Zelda", "Mido"), slot, error));
  REQUIRE(CoopNx2navi::SaveSync::WriteCoopProfile(profile, own, root / MpSaves::ReceivedFolder("", "Mido"), slot, error));
  CHECK(MpSaves::List(root).size() == 5);
  // The host is beginning a new game on its empty save "Fresh": the joiner's folder for it has
  // the player's settings and no slot (the game started on it begins a new game too).
  const fs::path fresh = root / MpSaves::ReceivedFolder("Fresh", "Saria");
  REQUIRE(CoopNx2navi::SaveSync::WriteEmptyCoopProfile(profile, own, fresh, error));
  CHECK(get(fresh / "save" / account / "option.sav") == "my options");
  CHECK(get(fresh / "account.uid") == "uid");
  CHECK_FALSE(fs::exists(fresh / "save" / account / "slot_00"));
  auto made = MpSaves::Find(root, "Fresh@Saria");
  REQUIRE(made.has_value());
  CHECK(made->empty);
  CHECK(made->received);
  CHECK(made->host == "Saria");
  // Begun again later by that host: what the joiner had played there is kept once.
  put(fresh / "save" / account / "slot_00" / "progress.sav", "the joiner's own new game");
  REQUIRE(CoopNx2navi::SaveSync::WriteEmptyCoopProfile(profile, own, fresh, error));
  CHECK_FALSE(fs::exists(fresh / "save" / account / "slot_00"));
  CHECK(get(fresh / "save-previous" / "slot_00" / "progress.sav") == "the joiner's own new game");
  CHECK_FALSE(CoopNx2navi::SaveSync::WriteEmptyCoopProfile(profile, own, own / "x", error));  // never in the player's own
  CHECK(MpSaves::Remove(root, "Fresh@Saria", {}, error));
  // A joiner whose game runs on a multiplayer save of its own (it hosted before): still beside it.
  const fs::path running = root / "Zelda" / "save" / account;
  CHECK_FALSE(CoopNx2navi::SaveSync::WriteCoopProfile(root / "Zelda", running, root / "Zelda", slot, error));
  CHECK(get(own / "slot_00" / "progress.sav") == "my solo progress");
  fs::remove_all(base);
}

TEST_CASE("nx2navi teleport: after a Game Over the follower goes back to the host") {
  Auto a;  // follow = host
  std::vector<Peer> peers = {MakePeer(1, true, "Field")};
  // Joined and standing with the host.
  a.Tick(In("Field"), false, peers, 10 * kSecond);
  if (const auto go = a.Tick(In("Field"), false, peers, 13 * kSecond)) {
    a.Done(go->player);
  }
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 20 * kSecond));
  // No life left: nothing while the Game Over shows (our Link is still read).
  a.Died(21 * kSecond);
  CHECK(a.died());
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 25 * kSecond));
  // The reload: our Link is gone, then stands in the save's scene.
  CHECK_FALSE(a.Tick(std::nullopt, false, peers, 30 * kSecond));
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 35 * kSecond));
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 36 * kSecond));
  const auto go = a.Tick(In("Kokiri"), false, peers, 37 * kSecond + 1);
  REQUIRE(go);
  CHECK(go->player == 1);
  CHECK(go->why == Auto::Why::regroup);
  CHECK(go->first);
  CHECK_FALSE(a.died());
  a.Done(1);
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 60 * kSecond));

  // The same scene as the host after the reload: still to the host (the save's start is elsewhere).
  a.Died(70 * kSecond);
  a.Tick(std::nullopt, false, peers, 75 * kSecond);
  a.Tick(In("Field"), false, peers, 80 * kSecond);
  const auto same = a.Tick(In("Field"), false, peers, 82 * kSecond + 1);
  REQUIRE(same);
  CHECK(same->why == Auto::Why::regroup);
  a.Done(1);
}

TEST_CASE("nx2navi teleport: the host regroups too; none with follow off, or when our Link was brought back") {
  std::vector<Peer> guest = {MakePeer(2, false, "Field")};
  Auto host;  // we are the host: since the living-friend rule, a dead host comes back beside a guest
  host.Tick(In("Field"), true, guest, 10 * kSecond);
  host.Died(11 * kSecond);
  host.Tick(std::nullopt, true, guest, 15 * kSecond);
  CHECK_FALSE(host.Tick(In("Kokiri"), true, guest, 20 * kSecond));  // settling
  const auto back = host.Tick(In("Kokiri"), true, guest, 23 * kSecond);
  REQUIRE(back);
  CHECK(back->player == 2);
  CHECK_FALSE(host.died());

  Auto::Options off;
  off.follow = CoopNx2navi::Teleport::FollowMode::off;
  Auto none(off);
  std::vector<Peer> peers = {MakePeer(1, true, "Field")};
  none.Died(5 * kSecond);
  CHECK_FALSE(none.died());

  // A fairy: no life for a moment, no reload after it. Forgotten, so that a load
  // minutes later (a door) is not taken for the Game Over's.
  Auto a;
  a.Tick(In("Field"), false, peers, 10 * kSecond);
  if (const auto go = a.Tick(In("Field"), false, peers, 13 * kSecond)) {
    a.Done(go->player);
  }
  a.Died(20 * kSecond);
  CHECK_FALSE(a.Tick(In("Field"), false, peers, 20 * kSecond + Auto::kDiedForgetUs + 1));
  CHECK_FALSE(a.died());
  CHECK_FALSE(a.Tick(std::nullopt, false, peers, 200 * kSecond));
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 205 * kSecond));
  CHECK_FALSE(a.Tick(In("Kokiri"), false, peers, 210 * kSecond));

  // 'anyone' with no host in the game: the first player who is.
  Auto::Options anyone;
  anyone.follow = CoopNx2navi::Teleport::FollowMode::anyone;
  Auto b(anyone);
  std::vector<Peer> others = {MakePeer(3, false, "Field"), MakePeer(4, false, "Lake")};
  b.Tick(In("Field"), false, others, 10 * kSecond);
  b.Died(11 * kSecond);
  b.Tick(std::nullopt, false, others, 15 * kSecond);
  b.Tick(In("Kokiri"), false, others, 20 * kSecond);
  const auto go = b.Tick(In("Kokiri"), false, others, 22 * kSecond + 1);
  REQUIRE(go);
  CHECK(go->player == 3);
}

TEST_CASE("the other players' hearts: quarters per heart, and the red moment after a hit") {
    namespace Ui = CoopNx2navi::Ui;
    // 6 hearts with 4 and a quarter left: the first full, the second a quarter, the rest empty.
    CHECK(Ui::HeartQuarters(5, 0) == 4);
    CHECK(Ui::HeartQuarters(5, 1) == 1);
    CHECK(Ui::HeartQuarters(5, 2) == 0);
    CHECK(Ui::HeartQuarters(0, 0) == 0);
    CHECK(Ui::HeartQuarters(24, 5) == 4);
    // The name tag is red at the loss and plain again 0.7 s later; a player who never lost life has none.
    CHECK(Ui::HurtFlash(0) == doctest::Approx(1.0f));
    CHECK(Ui::HurtFlash(350) == doctest::Approx(0.5f));
    CHECK(Ui::HurtFlash(Ui::kHurtFlashMs) == 0.0f);
    CHECK(Ui::HurtFlash(Ui::Peer{}.hurt_ago_ms) == 0.0f);
}

TEST_CASE("a struck puppet is red as the game's own Link: the damage ratio over time") {
    namespace Fx = CoopNx2navi::PuppetFx;
    // The game's own values at 30 frames a second (two ticks a frame): 1, 1, 0.9957, 0.7059, 0.2423, then 0.
    CHECK(Fx::DamageRatio(0) == doctest::Approx(0.5f));
    CHECK(Fx::DamageRatio(33) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(Fx::DamageRatio(67) == doctest::Approx(1.0f));
    CHECK(Fx::DamageRatio(100) == doctest::Approx(0.99568f).epsilon(0.01));
    CHECK(Fx::DamageRatio(133) == doctest::Approx(0.70593f).epsilon(0.02));
    CHECK(Fx::DamageRatio(167) == doctest::Approx(0.24233f).epsilon(0.03));
    CHECK(Fx::DamageRatio(200) == 0.0f);
    CHECK(Fx::DamageRatio(Fx::kFlashMs) == 0.0f);
    for (std::uint64_t ms = 0; ms < 300; ++ms) {
        CHECK(Fx::DamageRatio(ms) >= 0.0f);
        CHECK(Fx::DamageRatio(ms) <= 1.0f);
    }
    // The optional blink: hidden first, shown again every other 66 ms, always shown once it is over.
    CHECK_FALSE(Fx::BlinkShown(0));
    CHECK(Fx::BlinkShown(70));
    CHECK_FALSE(Fx::BlinkShown(140));
    CHECK(Fx::BlinkShown(Fx::kBlinkMs));
}

TEST_CASE("our Link's sounds and magic for the other games: names, the budget, the magic bytes") {
    namespace Fx = CoopNx2navi::PuppetFx;
    CHECK(Fx::SoundName("Damage_S_01"));
    CHECK(Fx::SoundName("FootStep"));
    CHECK_FALSE(Fx::SoundName(""));
    CHECK_FALSE(Fx::SoundName("a b"));
    CHECK_FALSE(Fx::SoundName("../x"));
    CHECK_FALSE(Fx::SoundName(std::string(64, 'a')));
    CHECK(Fx::SoundName(std::string(63, 'a')));
    Fx::SoundBudget budget;
    int taken = 0;
    for (int i = 0; i < 100; ++i) {
        taken += budget.Take(5000 + i) ? 1 : 0;
    }
    CHECK(taken == Fx::kSoundsPerSecond);
    CHECK(budget.Take(6000));  // the next second
    const auto bytes = Fx::EncodeMagic(30, 48);
    REQUIRE(bytes.size() == 4);
    const auto magic = Fx::DecodeMagic(bytes);
    REQUIRE(magic);
    CHECK(magic->now == 30);
    CHECK(magic->most == 48);
    CHECK(Fx::DecodeMagic(Fx::EncodeMagic(99, 48))->now == 48);  // never more than the most
    CHECK_FALSE(Fx::DecodeMagic({1, 2, 3}));
    // PLAYER_EXTRA goes through the codec as it is.
    coop::protocol::PlayerExtra extra{0, Fx::kExtraMagic, bytes};
    std::vector<std::uint8_t> packet;
    REQUIRE(coop::protocol::encode(coop::protocol::Message{extra}, packet) == coop::protocol::Error::none);
    coop::protocol::Message back;
    REQUIRE(coop::protocol::decode(packet, back) == coop::protocol::Error::none);
    REQUIRE(std::holds_alternative<coop::protocol::PlayerExtra>(back));
    CHECK(std::get<coop::protocol::PlayerExtra>(back) == extra);
    extra.data.assign(coop::protocol::kMaxPlayerExtraBytes + 1, 0);
    packet.clear();
    CHECK(coop::protocol::encode(coop::protocol::Message{extra}, packet) != coop::protocol::Error::none);
}

TEST_CASE("the other players' hearts in the game's look: which part of the heart picture shows, and the magic") {
    namespace Ui = CoopNx2navi::Ui;
    Ui::UvRect part[2];
    CHECK(Ui::HeartFill(0, part) == 0);
    REQUIRE(Ui::HeartFill(1, part) == 1);  // the top left quadrant
    CHECK(part[0].u1 == 0.5f);
    CHECK(part[0].v1 == 0.5f);
    REQUIRE(Ui::HeartFill(2, part) == 1);  // the left half
    CHECK(part[0].u1 == 0.5f);
    CHECK(part[0].v1 == 1.0f);
    REQUIRE(Ui::HeartFill(3, part) == 2);  // everything but the top right
    CHECK(part[1].u0 == 0.5f);
    CHECK(part[1].v0 == 0.5f);
    REQUIRE(Ui::HeartFill(4, part) == 1);
    CHECK(part[0].u1 == 1.0f);
    CHECK(part[0].v1 == 1.0f);
    CHECK(Ui::MagicShare(0, 48) == 0.0f);
    CHECK(Ui::MagicShare(24, 48) == doctest::Approx(0.5f));
    CHECK(Ui::MagicShare(60, 48) == 1.0f);
    CHECK(Ui::MagicShare(10, 0) == 0.0f);  // their game told no magic: no bar
}

TEST_CASE("the other players' block starts under the game's own HUD, one heart row lower above ten hearts") {
    namespace Ui = CoopNx2navi::Ui;
    CHECK(Ui::PeerBlockTop(0) == doctest::Approx(0.29f));
    CHECK(Ui::PeerBlockTop(40) == doctest::Approx(0.29f));
    CHECK(Ui::PeerBlockTop(44) > Ui::PeerBlockTop(40));
}

TEST_CASE("the panel's own texts are French in a French game") {
    namespace Ui = CoopNx2navi::Ui;
    Ui::Snapshot s;
    s.link = Ui::Link::connected;
    s.self = 1;
    s.self_name = "Zelda";
    s.local.game_found = true;
    s.local.in_game = true;
    s.local.scene = "HyruleField_Adult";
    Ui::Peer peer;
    peer.number = 2;
    peer.name = "Saria";
    peer.known = true;
    peer.fresh = true;
    peer.scene = "HyruleField_Adult";
    peer.position = Ui::Vec3{10.0f, 0.0f, 0.0f};
    s.peers.push_back(peer);
    CHECK_FALSE(Ui::French());
    CHECK(Ui::LinkLine(s) == "Co-op: connected as P1 \"Zelda\"");
    CHECK(Ui::PeerLine(peer, s.local) == "P2 Saria: HyruleField_Adult, 10.0 units, east");
    Ui::SetFrench(true);
    CHECK(Ui::LinkLine(s) == "Coop : connecté en tant que J1 \"Zelda\"");
    CHECK(Ui::LocalLine(s.local) == "Vous : HyruleField_Adult");
    CHECK(Ui::PeerLine(peer, s.local) == "J2 Saria : HyruleField_Adult, 10.0 m, est");
    peer.fresh = false;
    CHECK(Ui::PeerLine(peer, s.local) == "J2 Saria : hors du jeu");
    Ui::SetFrench(false);
    CHECK(Ui::LocalLine(s.local) == "You: HyruleField_Adult");
}

// ------------------------------- floor switches pressed by any player (step_switch.hpp)

TEST_CASE("step switch: a note survives its encoding, a bad one is refused") {
    namespace S = CoopNx2navi::StepSwitch;
    const S::Note n{"ForestTemple", 0x0123456789abcdefULL, true};
    const auto back = S::Decode(S::Encode(n));
    REQUIRE(back);
    CHECK(back->scene == "ForestTemple");
    CHECK(back->hash == n.hash);
    CHECK(back->pressed);
    CHECK_FALSE(S::Decode({1, 2, 3}));
    std::vector<std::uint8_t> zero(9, 0);
    zero[0] = 1;
    CHECK_FALSE(S::Decode(zero));  // hash 0
}

TEST_CASE("step switch: pressed here is said at once and again every 2 s; up again is said once") {
    namespace S = CoopNx2navi::StepSwitch;
    S::Switches w;
    w.SetScene("ForestTemple");
    w.Seen(0x77, false, 1'000'000);
    CHECK(w.TakeSends(1'000'000).empty());  // up from the start: nothing to say
    w.Seen(0x77, true, 1'100'000);
    auto s = w.TakeSends(1'100'000);
    REQUIRE(s.size() == 1);
    CHECK(s[0].pressed);
    CHECK(s[0].scene == "ForestTemple");
    CHECK(w.TakeSends(2'000'000).empty());
    CHECK(w.TakeSends(3'200'000).size() == 1);  // again after 2 s
    w.Seen(0x77, false, 3'300'000);
    s = w.TakeSends(3'300'000);
    REQUIRE(s.size() == 1);
    CHECK_FALSE(s[0].pressed);
    CHECK(w.TakeSends(9'000'000).empty());
}

TEST_CASE("step switch: another player's press holds ours down while said, and our forced switch is never echoed") {
    namespace S = CoopNx2navi::StepSwitch;
    S::Switches w;
    w.SetScene("ForestTemple");
    CHECK_FALSE(w.Force(0x77, 1'000'000));
    w.Remote(1, {"ForestTemple", 0x77, true}, 1'000'000);
    CHECK(w.Force(0x77, 1'100'000));
    w.Seen(0x77, true, 1'100'000);       // down because we forced it
    CHECK(w.TakeSends(1'200'000).empty());  // not sent back
    CHECK(w.Force(0x77, 5'900'000));      // within 5 s of the last note
    CHECK_FALSE(w.Force(0x77, 6'100'000));  // not said again: let go
    w.Remote(1, {"ForestTemple", 0x77, true}, 7'000'000);
    CHECK(w.Force(0x77, 7'000'000));
    w.Remote(1, {"ForestTemple", 0x77, false}, 7'500'000);  // they stepped off
    CHECK_FALSE(w.Force(0x77, 7'600'000));
    w.Remote(2, {"WaterTemple", 0x77, true}, 8'000'000);  // another scene: not ours
    CHECK_FALSE(w.Force(0x77, 8'000'000));
    // Two players on theirs: held while either says so.
    w.Remote(1, {"ForestTemple", 0x88, true}, 10'000'000);
    w.Remote(2, {"ForestTemple", 0x88, true}, 11'000'000);
    w.Remote(1, {"ForestTemple", 0x88, false}, 12'000'000);
    CHECK(w.Force(0x88, 12'100'000));
    w.SetScene("DekuTree");  // a new scene forgets them
    CHECK_FALSE(w.Force(0x88, 12'200'000));
}

// ------------------------------- the item-get moment for another player's item (item_get.hpp)

TEST_CASE("item get: a notice line names its item actor and icon") {
    namespace I = CoopNx2navi::ItemGet;
    CHECK(I::ActorFor("Host got the map (Forest Temple)") == "Item_Dungeon_Map");
    CHECK(I::ActorFor("Host got the compass (Water Temple)") == "Item_Dungeon_Compass");
    CHECK(I::ActorFor("Host got the Boss Key (Fire Temple)") == "Item_Dungeon_BossKey");
    CHECK(I::ActorFor("Host learned Zelda's Lullaby").empty());
    CHECK(I::ActorFor("Host got a Heart Container - your max health went up").empty());
    const std::string hookshot = I::ActorFor("Saria got the " + coop::host::ItemName("Item_Hookshot") +
                                             " - you got one too!");
    CHECK(hookshot == "Item_Hookshot");
    CHECK(I::IconPath("Item_Dungeon_Map") == "romfs:UI/Tex/Icon/Item_Dungeon_Map.bntx.zs#Item_Dungeon_Map");
    CHECK(I::IconPath("Item_Medal_Water").empty());
    CHECK(I::FanfareType("Item_Dungeon_Map") == 2);
}

TEST_CASE("item get: the scene only when safe, one at a time; too long a wait or no actor: the fanfare") {
    namespace I = CoopNx2navi::ItemGet;
    I::Queue q;
    I::Look ok;
    ok.in_game = true;
    ok.life = 24;
    ok.height = 10.0f;
    q.Offer("Host got the map (Forest Temple)", 0);
    q.Offer("Host got the compass (Forest Temple)", 0);
    CHECK_FALSE(q.Frame(ok, 1'000'000));  // readable for 3 s first
    I::Look busy = ok;
    busy.event = true;
    CHECK_FALSE(q.Frame(busy, 3'500'000));  // in a cutscene
    auto d = q.Frame(ok, 4'200'000);
    REQUIRE(d);
    CHECK(d->scene);
    CHECK(d->want.actor == "Item_Dungeon_Map");
    CHECK_FALSE(q.Frame(ok, 5'000'000));  // one at a time
    q.Done(true, 9'000'000);
    CHECK_FALSE(q.Frame(ok, 10'000'000));  // 3 s apart
    I::Look hurt = ok;
    hurt.life = 20;
    CHECK_FALSE(q.Frame(hurt, 11'000'000));  // struck: 8 s of quiet
    d = q.Frame(hurt, 12'500'000);           // 12 s after it came: the fanfare instead
    REQUIRE(d);
    CHECK_FALSE(d->scene);
    // A scene that failed gives its fanfare next.
    q.Offer("Host got the Boss Key (Forest Temple)", 13'000'000);
    I::Look still = ok;
    still.life = 20;
    d = q.Frame(still, 30'000'000);
    REQUIRE(d);
    REQUIRE(d->scene);
    q.Done(false, 31'000'000);
    d = q.Frame(still, 31'100'000);
    REQUIRE(d);
    CHECK_FALSE(d->scene);
    CHECK(d->want.actor == "Item_Dungeon_BossKey");
    // A song has no actor: its fanfare at once (out of a cutscene).
    q.Offer("Host learned Saria's Song", 40'000'000);
    d = q.Frame(still, 40'000'000);
    REQUIRE(d);
    CHECK_FALSE(d->scene);
}

// ------------------------------- gear sounds: a footstep with the properties that choose its variation

TEST_CASE("gear sounds: a footstep and its variation values survive the message, the rest stay names") {
    namespace Fx = CoopNx2navi::PuppetFx;
    Fx::SoundProps s{"FootStep", {{3, 1}, {7, 12}, {9, 0x3f800000}}};
    const auto data = Fx::EncodeSoundProps(s);
    CHECK(data.size() <= 96);
    const auto back = Fx::DecodeSoundProps(data);
    REQUIRE(back);
    CHECK(back->name == "FootStep");
    REQUIRE(back->values.size() == 3);
    CHECK(back->values[2].first == 9);
    CHECK(back->values[2].second == 0x3f800000u);
    CHECK_FALSE(Fx::DecodeSoundProps({}));
    CHECK_FALSE(Fx::DecodeSoundProps({3, 'a', 'b'}));
    // Thirteen properties of a long name still fit one message.
    Fx::SoundProps many{"EquipmentNoise_FootStep", {}};
    for (std::uint8_t i = 0; i < 13; ++i) {
        many.values.emplace_back(i, i * 1000u);
    }
    const auto big = Fx::EncodeSoundProps(many);
    CHECK(big.size() <= 96);
    REQUIRE(Fx::DecodeSoundProps(big));
    CHECK(Fx::DecodeSoundProps(big)->values.size() == 13);
    CHECK(Fx::HasVariations("FootStep"));
    CHECK(Fx::HasVariations("EquipmentNoise_FootStep"));
    CHECK_FALSE(Fx::HasVariations("SwordAttack_S_01"));
}

// ------------------------------- a story gate opened elsewhere leaves at once (presence.hpp)

TEST_CASE("presence: a gate whose flag went to 0 leaves, once, and only what the game has not taken out") {
    using namespace Presence;
    // The Kokiri exit wall: present while its blocker flag is set (no negation).
    CHECK(PresentNow(0x0, true));
    CHECK_FALSE(PresentNow(0x0, false));
    CHECK(ShouldLeave(0x0, 0x0, false));            // flag 0, still there: leave
    CHECK_FALSE(ShouldLeave(0x0, 0x0, true));       // flag still 1: stays
    CHECK_FALSE(ShouldLeave(0x0, kAbsentPresence, false));  // the game already marked it absent
    // A negated gate (present while the flag is NOT set).
    CHECK(ShouldLeave(kBitNegation, 0x0, true));
    CHECK_FALSE(ShouldLeave(kBitNegation, 0x0, false));
    // Lazy bit (0x4) does not change the answer.
    CHECK(ShouldLeave(0x4, 0x0, false));
    // The "became absent" call only when no other reason was set before our OR.
    CHECK(CallDue(0x0));
    CHECK_FALSE(CallDue(0x80));
    CHECK_FALSE(CallDue(0x800000));
    CHECK(CallDue(0x4));  // a bit outside the game's mask
}

TEST_CASE("presence: every banc entity, both ways: a gate the group opened leaves, one it closed comes back") {
    using namespace Presence;
    CHECK(RecordLoaded(0x1));
    CHECK(RecordLoaded(0x1 | 0x8));
    CHECK_FALSE(RecordLoaded(0x3));
    CHECK_FALSE(RecordLoaded(0x0));
    CHECK(kAbsentPresence == (1u << kPresenceBit));
    // The Kokiri exit wall (no negation): the gate flag 0 = the way is open, the wall leaves.
    CHECK(GateAction(0x0, 0x0, false) == Gate::leave);
    CHECK(GateAction(0x0, kAbsentPresence, false) == Gate::keep);  // already out
    // The group's flag says closed again (the host's save): the wall comes back.
    CHECK(GateAction(0x0, kAbsentPresence, true) == Gate::back);
    CHECK(GateAction(0x0, 0x0, true) == Gate::keep);
    // Taken out for another reason only: never put back by us.
    CHECK(GateAction(0x0, 0x80, true) == Gate::keep);
    // Negated gates.
    CHECK(GateAction(kBitNegation, 0x0, true) == Gate::leave);
    CHECK(GateAction(kBitNegation, kAbsentPresence, false) == Gate::back);
    // Lazy placements are the game's own.
    CHECK(GateAction(kBitLazy, 0x0, false) == Gate::keep);
    CHECK(GateAction(kBitLazy, kAbsentPresence, true) == Gate::keep);
    // The Kokiri who blocks stands 0.6 m from the wall; one 10 m away is not its blocker.
    const float wall[3] = {-82.0f, 115.1f, -38.0f};
    const float kokiri[3] = {-81.4f, 115.7f, -37.9f};
    const float away[3] = {-72.0f, 115.1f, -38.0f};
    CHECK(IsBlockerNear(wall, kokiri));
    CHECK_FALSE(IsBlockerNear(wall, away));
}

// ------------------------------- Epona herself, not only her name (epona.hpp)

TEST_CASE("epona: the owned-horse manager is asked for the horse once the hook wrote her name") {
    using namespace Epona;
    Mgr m;
    m.struct_hash = kOwnedHorseList;
    m.member_hash = kActorName;
    m.ready = 1;
    CHECK(IsManager(m));
    CHECK(ShouldMake(m, true, true, true));
    CHECK_FALSE(ShouldMake(m, false, true, true));  // the save owned her already: the area load makes her
    CHECK_FALSE(ShouldMake(m, true, false, true));  // no name yet
    CHECK_FALSE(ShouldMake(m, true, true, false));  // a child
    Mgr linked = m;
    linked.linked = 0x1234;
    CHECK_FALSE(ShouldMake(linked, true, true, true));  // a horse is here already
    Mgr pending = m;
    pending.flags = kPending;
    CHECK_FALSE(ShouldMake(pending, true, true, true));
    Mgr not_ready = m;
    not_ready.ready = 0;
    CHECK_FALSE(ShouldMake(not_ready, true, true, true));
    Mgr asked = m;
    asked.make_at = 1.0f;
    CHECK_FALSE(ShouldMake(asked, true, true, true));  // an ask not yet taken
    Mgr wrong = m;
    wrong.struct_hash = 0;
    CHECK_FALSE(ShouldMake(wrong, true, true, true));  // not the manager: never written
    const float link[3] = {-68.6f, 151.8f, 126.0f};
    float at[3];
    PlaceBeside(link, at);
    CHECK(at[0] == doctest::Approx(-65.6f));
    CHECK(at[1] == doctest::Approx(151.8f));
    CHECK(at[2] == doctest::Approx(126.0f));
}

// ------------------------------- another player's Ocarina notes at their puppet (ocarina_play.hpp)

TEST_CASE("ocarina play: a friend's note is full near, softer with distance, and silent beyond hearing") {
    using namespace OcarinaPlay;
    CHECK(Gain(0.0f) == 1.0f);
    CHECK(Gain(kNearM) == 1.0f);
    CHECK(Gain(10.0f) < 1.0f);
    CHECK(Gain(20.0f) < Gain(10.0f));
    CHECK(Gain(kHearM - 1.0f) > 0.0f);
    CHECK(Gain(kHearM) == 0.0f);
    CHECK(Gain(500.0f) == 0.0f);
    CHECK(kArgs + 0x28 <= kBlock);  // the start's arguments fit the scratch block
}

TEST_CASE("ocarina play: each player's held note is stopped by the next, by a let-go, or after a silence") {
    using namespace OcarinaPlay;
    Held h;
    auto a = h.Note(2, 16, 1000);  // P2: high D
    CHECK_FALSE(a.stop);
    CHECK(a.play == 16);
    CHECK(h.HeldBy(2) == 16);
    a = h.Note(3, 11, 1100);  // another player's note does not stop P2's
    CHECK_FALSE(a.stop);
    CHECK(h.HeldBy(2) == 16);
    a = h.Note(2, 13, 1500);  // P2's next note stops the first
    CHECK(a.stop);
    CHECK(a.play == 13);
    a = h.Note(2, 0, 1800);  // let go
    CHECK(a.stop);
    CHECK(a.play == 0);
    CHECK(h.HeldBy(2) == 0);
    a = h.Note(2, 0, 1900);  // a second let-go: nothing
    CHECK_FALSE(a.stop);
    CHECK_FALSE(h.Stale(1100 + kHoldMs - 1));
    CHECK(h.Stale(1100 + kHoldMs + 1) == 3);  // P3 sent nothing more: let go
    CHECK_FALSE(h.Stale(1100 + kHoldMs + 2));
    CHECK_FALSE(h.Note(0, 5, 0).play);    // no player 0
    CHECK_FALSE(h.Note(2, 20, 0).play);   // no note 20
    CHECK(AssetSlot(1) == kAssets);
    CHECK(AssetSlot(kNotes) + 8 <= kHandles);
    CHECK(HandleSlot(kPlayers - 1) + 8 <= kBlock);
}

// ------------------------------- a story cutscene for every player, wherever (story_start.hpp)

TEST_CASE("story start: the scene and the event flow go to every game and come back whole") {
    using namespace CoopNx2navi::StoryStart;
    const Start s{"KokiriForest_Child", "Dm_Kokiri_0030"};
    const auto back = Decode(Encode(s));
    REQUIRE(back);
    CHECK(back->scene == s.scene);
    CHECK(back->name == s.name);
    CHECK_FALSE(Decode({}));
    CHECK_FALSE(Decode({0, 'a', 'b'}));                  // no scene
    CHECK_FALSE(Decode({'a', 'b', 0}));                  // no name
    CHECK_FALSE(Decode({'a', '/', 0, 'b'}));             // not a name
    CHECK(kExtraStory == 6);
}

// ------------------------------- one talker per NPC (talk_lock.hpp)

TEST_CASE("talk lock: a second player's talk to the same NPC is refused while the first talks") {
    using namespace CoopNx2navi::TalkLock;
    const Note n{"KokiriForest_Child", 0x1234567890abcdefull, true};
    const auto back = Decode(Encode(n));
    REQUIRE(back);
    CHECK(back->hash == n.hash);
    CHECK(back->scene == n.scene);
    CHECK(back->talking);
    CHECK_FALSE(Decode({}));
    CHECK(Refusal("Saria", false) == "Saria is already talking to this NPC");
    CHECK(Refusal("Saria", true) == "Saria parle d\xc3\xa9j\xc3\xa0 \xc3\xa0 ce personnage");

    Lock b;  // the second player's game
    b.SetScene("KokiriForest_Child");
    b.Remote(1, n, 1'000'000);
    CHECK(b.Request(n.hash, 1'500'000) == 1);         // refused: P1 talks to it
    CHECK(b.TakeRefusals() == std::vector<std::uint8_t>{1});
    CHECK(b.Request(n.hash, 1'600'000) == 1);          // still refused, no second line within a second
    CHECK(b.TakeRefusals().empty());
    CHECK(b.Request(0x99, 1'700'000) == 0);            // another NPC: free
    b.Event(false, 1'700'000 + kStartUs + 1);          // that talk never started
    CHECK(b.Talking() == 0);
    b.Remote(1, Note{n.scene, n.hash, false}, 2'000'000);  // P1 ended
    CHECK(b.Request(n.hash, 2'100'000) == 0);
    b.Remote(1, n, 3'000'000);
    b.SetScene("HyruleField");                          // another scene forgets it
    b.Remote(2, Note{"KokiriForest_Child", 7, true}, 3'000'000);
    CHECK(b.Request(7, 3'100'000) == 0);
    CHECK(b.Request(n.hash, 3'100'000) == 0);

    Lock a;  // the first player's game: says its talk, again every 2 s, and 0 at its end
    a.SetScene("KokiriForest_Child");
    CHECK(a.Request(n.hash, 10'000'000) == 0);
    auto sends = a.TakeSends(10'000'000);
    REQUIRE(sends.size() == 1);
    CHECK(sends[0].talking);
    a.Event(true, 10'100'000);
    CHECK(a.TakeSends(11'000'000).empty());
    CHECK(a.TakeSends(12'100'000).size() == 1);
    a.Event(false, 13'000'000);
    sends = a.TakeSends(13'000'000);
    REQUIRE(sends.size() == 1);
    CHECK_FALSE(sends[0].talking);
    CHECK(a.Talking() == 0);
    // A held note runs out by itself.
    Lock c;
    c.SetScene("S");
    c.Remote(3, Note{"S", 5, true}, 0);
    CHECK(c.Others(kHoldUs - 1) == std::vector<std::uint64_t>{5});  // for the NPC copies (SetTalkedNpcs)
    CHECK(c.Request(5, kHoldUs - 1) == 3);
    CHECK(c.Request(5, kHoldUs + 1) == 0);
    CHECK(c.Others(kHoldUs + 1).empty());
    c.Remote(3, Note{"S", 6, true}, kHoldUs);
    c.Remote(3, Note{"S", 6, false}, kHoldUs + 10);
    CHECK(c.Others(kHoldUs + 20).empty());  // ended
}

TEST_CASE("talk lock: the host's game decides, so two asks close together never both talk (run tk13)") {
    using namespace CoopNx2navi::TalkLock;
    const std::string scene = "KokiriForest_Child";
    const std::uint64_t npc = 0x3612b617269aa6fbull;
    // An ask, a grant and a refusal go through the wire form and back.
    const Note ask{scene, npc, false, kAsk, 2, 0};
    const auto ask_back = Decode(Encode(ask));
    REQUIRE(ask_back);
    CHECK(ask_back->kind == kAsk);
    CHECK(ask_back->target == 2);
    CHECK(ask_back->scene == scene);
    CHECK(ask_back->hash == npc);
    const auto deny_back = Decode(Encode(Note{scene, npc, false, kDeny, 2, 1}));
    REQUIRE(deny_back);
    CHECK(deny_back->kind == kDeny);
    CHECK(deny_back->holder == 1);
    CHECK_FALSE(Decode({5, 1, 0, 0, 0, 0, 0, 0, 0}));  // no such state
    CHECK_FALSE(Decode({kAsk, 1, 0, 0, 0, 0, 0, 0, 0}));  // an ask without its target and holder

    auto pair = [&](Lock& host, Lock& guest) {
        host.SetScene(scene);
        guest.SetScene(scene);
        host.SetRoles(1, 1);
        guest.SetRoles(2, 1);
        // The guest's ask reaches the host's game, and its answer comes back (here at once).
        guest.SetSender([&](const Note& n) {
            if (const auto reply = host.Remote(2, *Decode(Encode(n)), 1'000'000)) {
                guest.Remote(1, *Decode(Encode(*reply)), 1'000'000);
            }
        });
    };
    {
        // The host pressed A first: the guest's ask, 33 ms later and before the host's note, is refused.
        Lock host, guest;
        pair(host, guest);
        CHECK(host.Request(npc, 1'000'000) == 0);
        CHECK(guest.Request(npc, 1'033'000) == 1);
        CHECK(guest.TakeRefusals() == std::vector<std::uint8_t>{1});
        CHECK(guest.Talking() == 0);
        // The host's talk ends: the guest is granted.
        host.Event(true, 1'100'000);
        host.Event(false, 2'000'000);
        guest.Remote(1, Note{scene, npc, false}, 2'000'000);
        CHECK(guest.Request(npc, 2'100'000) == 0);
        CHECK(guest.Talking() == npc);
    }
    {
        // The guest's ask came first: granted; the host's own press then is refused.
        Lock host, guest;
        pair(host, guest);
        CHECK(guest.Request(npc, 1'000'000) == 0);
        CHECK(guest.Talking() == npc);
        CHECK(host.Request(npc, 1'010'000) == 2);
        CHECK(host.TakeRefusals() == std::vector<std::uint8_t>{2});
        // Another NPC is free for the host.
        CHECK(host.Request(0x77, 1'020'000) == 0);
        // The guest's talk ends (its note 0): the NPC is free for the host again.
        host.Remote(2, Note{scene, npc, false}, 3'000'000);
        host.Event(false, 1'020'000 + kStartUs + 1);
        CHECK(host.Request(npc, 3'100'000) == 0);
    }
    {
        // Two guests: the host's game grants the first and refuses the second.
        Lock host, g2, g3;
        host.SetScene(scene);
        host.SetRoles(1, 1);
        for (auto* g : {&g2, &g3}) g->SetScene(scene);
        g2.SetRoles(2, 1);
        g3.SetRoles(3, 1);
        g2.SetSender([&](const Note& n) {
            if (const auto r = host.Remote(2, n, 1'000'000)) g2.Remote(1, *r, 1'000'000);
        });
        g3.SetSender([&](const Note& n) {
            if (const auto r = host.Remote(3, n, 1'000'000)) {
                g2.Remote(1, *r, 1'000'000);  // not for g2: ignored
                g3.Remote(1, *r, 1'000'000);
            }
        });
        CHECK(g2.Request(npc, 1'000'000) == 0);
        CHECK(g3.Request(npc, 1'000'001) == 2);
        CHECK(g2.Talking() == npc);
    }
    {
        // No answer (an older host build, a slow link): after the hold, the talk goes ahead as before.
        Lock guest;
        guest.SetScene(scene);
        guest.SetRoles(2, 1);
        int asks = 0;
        guest.SetSender([&](const Note&) { ++asks; });
        CHECK(guest.Request(npc, 1'000'000, 2'000) == 0);
        CHECK(asks == 1);
        bool said = false;
        for (const auto& line : guest.TakeLines()) said = said || line.find("no answer") != std::string::npos;
        CHECK(said);
        // The host's game itself asks nobody.
        Lock host;
        host.SetScene(scene);
        host.SetRoles(1, 1);
        host.SetSender([&](const Note&) { ++asks; });
        CHECK(host.Request(npc, 1'000'000) == 0);
        CHECK(asks == 1);
        // An ask that reaches a game that is not the host's gets no answer from it.
        CHECK_FALSE(guest.Remote(3, Note{scene, 9, false, kAsk, 3, 0}, 1'000'000));
    }
}

TEST_CASE("overlay: another player's quest step becomes a notice line") {
    using CoopNx2navi::Ui::QuestNotice;
    CHECK(QuestNotice("quests: Get_Epona ReadyForGambleRace -> Complete (shared Threads of Time; issue 193)", false) ==
          std::optional<std::string>("Quest Get_Epona: Complete"));
    CHECK(QuestNotice("quests: Meet_DekuTree Ready -> Step02 (shared Threads of Time; issue 193)", true) ==
          std::optional<std::string>("Qu\xc3\xaate Meet_DekuTree : Step02"));
    CHECK_FALSE(QuestNotice("quests: sharing 12 quests (Threads of Time, and Epona won)", false));
    CHECK_FALSE(QuestNotice("quests: Get_Epona / childhood / owned horse: 0x0 0 ''", false));
    CHECK_FALSE(QuestNotice("story: quest DoorOfTime 0x1 -> 0x2 (shared story; issue 173)", false));
}

TEST_CASE("ocarina song: another player's notes make a song notice") {
    using namespace CoopNx2navi::OcarinaSong;
    Listener ear;
    const std::uint8_t epona[] = {D2, B, A, D2, B, A};
    const Song* got = nullptr;
    std::uint64_t t = 1'000'000;
    ear.Note(D, t);  // a stray note first
    for (const std::uint8_t n : epona) {
        CHECK(got == nullptr);
        ear.Note(0, t);  // let go: no note
        got = ear.Note(n, t += 400'000);
    }
    REQUIRE(got != nullptr);
    CHECK(std::string(got->en) == "Epona's Song");
    CHECK(Notice("Host", *got, false) == "Host played Epona's Song");
    CHECK(Notice("Host", *got, true) == "Host a jou\xc3\xa9 le Chant d'Epona");
    // A long pause starts a new tune.
    Listener e2;
    for (int i = 0; i < 3; ++i) e2.Note(epona[i], t += 400'000);
    t += 7'000'000;
    const Song* none = nullptr;
    for (int i = 3; i < 6; ++i) none = e2.Note(epona[i], t += 400'000);
    CHECK(none == nullptr);
    // Every song is told apart.
    for (const Song& s : Songs()) {
        Listener e;
        const Song* r = nullptr;
        for (const auto n : s.notes) r = e.Note(n, t += 100'000);
        REQUIRE(r != nullptr);
        CHECK(std::string(r->en) == s.en);
    }
}
