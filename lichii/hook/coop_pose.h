// SPDX-License-Identifier: GPL-3.0-or-later
// Full-body pose copy for puppets (docs/puppet.md, "Pose").
//
// Each game frame, on the game's calc thread (the puppet cave's SVC), the
// local Link's skeleton is read: every bone's local transform as the game's
// animation left it. The network thread sends the bones that move as
// PLAYER_POSE at most kSendHz times a second. A received pose is kept per
// player; each frame the newest two are blended and written onto that
// player's puppet skeleton.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <map>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopPuppet {
class Guest;
}

namespace CoopPose {

using Quat = std::array<float, 4>;  // x, y, z, w
using Vec3 = std::array<float, 3>;
using Rows = std::array<float, 9>;  // 3x3 rotation as the game stores it, rows

// Read from the running game with the emulator's debug server (2026-11-09,
// Kakariko, child): see docs/puppet.md, Pose.
namespace addr {
// Actor -> skeleton object: read a pointer at each offset in turn. The model
// object sits inline at +0x120 of the fourth, its skeleton pointer at +0x20.
inline constexpr std::array<std::uint64_t, 5> kSkeletonPath{0x270, 0x8, 0x18, 0x18, 0x140};
inline constexpr std::uint64_t kModelResource = 0x120;  // of the fourth: "FMDL"
inline constexpr std::uint64_t kModelName = 0x8;  // FMDL: pointer to u16 length + chars
inline constexpr std::uint64_t kBoneCount = 0x48;       // u16 in the skeleton object
inline constexpr std::uint64_t kLocalArray = 0x18;      // pointer to the local transforms
inline constexpr std::uint64_t kLocalStride = 0x60;
// One local transform: u32 flags (bits 23 to 30 change with the pose),
// scale (3 floats at +0x10), rotation (3 rows of 4 floats at +0x20, the 4th
// of each 0) and translation (3 floats at +0x50).
inline constexpr std::uint32_t kFlagShift = 23;
inline constexpr std::uint32_t kFlagMask = 0xffu << kFlagShift;
}  // namespace addr

inline constexpr std::size_t kMaxBones = 512;
// Link's pose goes 15 times a second (30 before the 16-player work); the
// receiver's Track blends the last two, so it still plays smooth.
// A build may raise both for a small group (LICHII_COOP_POSE_SEND_HZ, LICHII_COOP_POSE_SEND_BONES; the NX2navi
// build: 30 and 128, after a player report that the other Links moved choppily since the 16-player work).
#ifdef LICHII_COOP_POSE_SEND_HZ
inline constexpr int kSendHz = LICHII_COOP_POSE_SEND_HZ;
#else
inline constexpr int kSendHz = 15;
#endif
// At most this many of Link's bones go in one pose (about 300 move). First up
// to kRefreshBones of those unsent for kBoneRefreshMs or longer, oldest first
// (a lost or thinned pose, a late joiner: every bone goes again within about
// kBoneRefreshMs + 300 / kRefreshBones sends, ~3 s), then the ones changed most
// since they were last sent. The receiver keeps the rest from its newest pose
// (Track::Fill).
// About 48 x 18 bytes x 15/s = 13 KB/s per player, where every moving bone at
// 30/s was about 160 KB/s.
#ifdef LICHII_COOP_POSE_SEND_BONES
inline constexpr std::size_t kMaxSendBones = LICHII_COOP_POSE_SEND_BONES;
#else
inline constexpr std::size_t kMaxSendBones = 48;
#endif
// Playout (a build that defines LICHII_COOP_POSE_PLAYOUT_MS; the NX2navi build: 50). Each pose is stamped
// with the server time its bones were read at, and the receiver shows the pose of that many milliseconds ago
// by the sender's clock, blended between the two poses around it. Without it (0, every other build) the
// receiver blends the last two poses from the moment the newest arrived, so an early or late arrival makes
// the puppet jump: the network thread's send and the game's frames beat against each other at 30 a second.
#ifdef LICHII_COOP_POSE_PLAYOUT_MS
inline constexpr std::uint64_t kPlayoutMs = LICHII_COOP_POSE_PLAYOUT_MS;
#else
inline constexpr std::uint64_t kPlayoutMs = 0;
#endif
// The playout in use: kPlayoutMs, or 0 when the environment says LICHII_COOP_POSE_PLAYOUT=0 (for comparing).
std::uint64_t PlayoutMs();
// The bones a pose carries at most: kMaxSendBones, or LICHII_COOP_POSE_SEND_BONES from the environment (a test
// switch for comparing; at most kMaxBones).
std::size_t SendBones();
// Bone keys (with playout only; LICHII_COOP_POSE_BONE_KEYS=0 turns them off, for comparing). The sender sent a
// bone only once it moved more than kBoneStep since it was last sent, so a standing Link's slow bones (about
// 0.014 degrees a frame, under the step of about 0.03) went every second or third pose. Shown pose by pose, each
// such bone held still, then stepped (runs eq4 to eq7). With bone keys:
// - the receiver shows each bone between the two poses that carried it, at their own times; past its newest
//   carried value it goes on at the speed of its last two carried values (KeyAhead), for at most kKeyAheadMs;
// - the sender works out that same guess for each bone and sends the bone once the guess is more than
//   kBoneStep off (dead reckoning), not once the bone moved kBoneStep. A bone turning at an even speed then goes
//   seldom and still plays smooth; a bone that turns back or stops goes at once.
// The guess runs on the poses' stamps (the server time the bones were read at), the same numbers on both sides.
bool BoneKeys();
inline constexpr std::uint64_t kKeyAheadMs = 500;
inline constexpr std::int64_t kKeyEaseMs = 100;
// Keys are for slow bones only: a bone whose last two carried values are further apart than this (packed units,
// about 0.17 degrees) is shown and sent as without keys. A running Link's bones go nearly every pose anyway, and
// with the bone budget full a guess ahead for them did worse than the blend (unit test "running").
inline constexpr int kKeySlowUnits = 48;
// The guess for a bone at `t`: its carried value `a` (at ta) going on at the speed from `p` (at tp) to `a`, for at
// most kKeyAheadMs; `a` itself when tp >= ta. Packed units (the sender) and unpacked values (the receiver) alike.
inline float KeyAhead(std::uint64_t tp, std::uint64_t ta, std::uint64_t t) {
    if (ta <= tp || t <= ta) return 0.0f;
    const std::uint64_t ahead = std::min<std::uint64_t>(t - ta, kKeyAheadMs);
    return static_cast<float>(ahead) / static_cast<float>(ta - tp);
}
// Playout: a pose may go this much before 1000 / kSendHz, so a 30 Hz game's frames are not skipped in turn.
inline constexpr std::uint64_t kSendEarlyMs = 8;
inline constexpr std::size_t kPlayoutKeep = 32;   // poses kept per track
inline constexpr std::size_t kPlayoutLags = 64;   // arrival lags remembered (the least is the clock's offset)
inline constexpr std::uint64_t kBoneRefreshMs = 500;
inline constexpr std::size_t kRefreshBones = 8;
// A bone is changed once a packed part moved more than this (in the packed
// units: 1/32767 of a quaternion part, 1/kPoseUnit of a translation or scale).
inline constexpr int kBoneStep = 8;
// A part counts as moving once it differs from the first read by more than this.
inline constexpr float kMoved = 1e-4f;

// Held pieces (issue 158). The pieces in a Link's hands have skeletons of
// their own that the game animates apart from Link's: the slingshot's band
// pulled back with a seed in it, then let go when it shoots; the bow's string.
// The puppet's copy of the piece is not animated by anyone, so it stayed
// pulled back after the shot. Each piece out in a hand slot below (the
// EquipmentUser's hand slots: 1 SubWeapon, 2 ThirdWeapon, 3 Bow, 5 Tool,
// 6 SubTool; not the sword or shield) is sent as a PLAYER_POSE of its own
// skeleton, every bone, and written onto the piece of that skeleton in the
// puppet's hands. No new message: the skeleton id tells the poses apart.
inline constexpr std::array<std::uint32_t, 5> kPieceSlots{1, 2, 3, 5, 6};
inline constexpr std::uint32_t kMaxPieceBones = 64;      // bigger: not sent
inline constexpr std::uint64_t kPieceRefreshMs = 250;    // an unchanged piece, again
inline constexpr std::uint64_t kTrackForgetMs = 5000;    // a skeleton not heard from
inline constexpr std::size_t kMaxTracks = 8;             // skeletons kept per player

// One bone of a pose: the parts that move.
struct BonePose {
    std::uint16_t bone = 0;
    std::uint8_t flags = 0;
    std::optional<Quat> rotation;
    std::optional<Vec3> translation;
    std::optional<Vec3> scale;
};

struct Pose {
    std::uint32_t skeleton = 0;
    std::vector<BonePose> bones;  // increasing bone order
};

Quat RowsToQuat(const Rows& m);  // normalised, w >= 0
Rows QuatToRows(const Quat& q);  // q normalised first; identity if it is 0
std::array<std::int16_t, 4> Pack(const Quat& q);
Quat Unpack(const std::array<std::int16_t, 4>& q);
std::array<std::int16_t, 3> PackUnit(const Vec3& v);  // scaled by protocol::kPoseUnit
Vec3 UnpackUnit(const std::array<std::int16_t, 3>& v);
// Shortest-path normalised lerp from a to b; t in [0, 1].
Quat Nlerp(const Quat& a, const Quat& b, float t);

// A skeleton's id, sent with every pose: the bone count in the low 9 bits and
// a hash of the model's name ("Player_Child", "Player_Adult") above it, so a
// puppet applies only a pose of its own model.
std::uint32_t SkeletonId(std::uint32_t bone_count, std::string_view model_name);
std::uint32_t BoneCountOf(std::uint32_t skeleton);
// The name of the actor's model ("Player_Child", "Player_Adult") when it has
// a skeleton and an FMDL model resource; nothing when not (yet).
std::optional<std::string> ModelNameOf(CoopPuppet::Guest& guest, std::uint64_t actor);

// The newest poses of one remote player, and the blend shown now.
class Track {
public:
    void Push(Pose pose, std::uint64_t now_ms);
    // The pose to show at now_ms: shown one interval behind, the last two
    // blended, so a 30 Hz stream plays smooth at 60 frames a second.
    std::optional<Pose> At(std::uint64_t now_ms) const;
    std::uint64_t last_ms() const { return newest_ms_; }
    // Playout (kPlayoutMs): keep `pose`, read at `sent_ms` by the sender's (server) clock, arrived at now_ms.
    // `carried` (may be null: every bone of `pose`): the bones the message itself carried, the rest of `pose`
    // having come from Fill. With bone keys (BoneKeys()) each bone is then shown between its own carried
    // values, not stepped to a new value at the one pose that carries it.
    void PushSent(Pose pose, std::uint64_t sent_ms, std::uint64_t now_ms,
                  const std::vector<std::uint16_t>* carried = nullptr);
    // Playout: the pose of `delay_ms` ago by the sender's clock, blended between the two kept poses around it.
    std::optional<Pose> PlayAt(std::uint64_t now_ms, std::uint64_t delay_ms) const;
    std::size_t Kept() const { return kept_.size(); }
    // `pose` (some bones of this track's skeleton: the sender sends only those
    // that changed most) with the bones it lacks taken from the newest pose.
    Pose Fill(Pose pose) const;

private:
    std::optional<Pose> older_;
    std::optional<Pose> newest_;
    std::uint64_t older_ms_ = 0;
    std::uint64_t newest_ms_ = 0;
    struct Stamped {
        Pose pose;
        std::uint64_t sent_ms = 0;
    };
    // Bone keys: each bone's last carried values, in sent order (at most kKeysKept).
    struct Key {
        std::uint64_t sent_ms = 0;
        BonePose value;
    };
    static constexpr std::size_t kKeysKept = 6;
    std::vector<std::vector<Key>> keys_;  // by bone
    // A new carried value changes a bone's course; the jump from the old course to the new one, at the shown time
    // it came, is eased out over kKeyEaseMs instead of shown at once.
    struct Ease {
        std::int64_t from = 0;  // the shown time (sender's clock) it starts at
        std::array<float, 4> rotation{};
        std::array<float, 3> translation{}, scale{};
        bool on = false;
    };
    std::vector<Ease> ease_;  // by bone
    mutable std::uint64_t delay_ms_ = kPlayoutMs;  // the last PlayAt's delay, for the shown time in PushSent
    // The bone's place at `target` by its keys alone (no ease); false when it has none at or before target.
    bool KeyValue(BonePose& out, std::int64_t target) const;
    // Its ease's part at `target`.
    void AddEase(BonePose& out, std::int64_t target) const;
    // Bone keys: `base` (the blend of the two poses around `target`) with each bone moved to its place between
    // its own carried values around `target`, or on from its newest at its last speed.
    void KeyBones(Pose& base, std::int64_t target) const;
    std::vector<Stamped> kept_;            // playout: sent order
    std::vector<std::int64_t> lags_;    // playout: arrival minus sent, the newest last
};

// Playout's frame clock (LICHII_COOP_POSE_FRAME_CLOCK=0 turns it off, for comparing). The game moves Link's bones
// by one fixed step a game frame, but the wall clock between two game frames varies. A pose stamped with the wall
// time it was read at, and a puppet shown at the wall time of each frame, map even steps onto uneven times: the
// puppet's motion a frame then follows the frame times' jitter (standing, its jerk stayed 2.7 against the real
// Link's 0.4 with bone keys or without, runs hq1 and hq2). This clock advances by the mean frame period at each
// frame, and is pulled slowly towards the wall clock; both the stamps and the shown time use it.
class FrameClock {
public:
    std::uint64_t Tick(std::uint64_t now_ms);
private:
    bool on_ = false;
    std::uint64_t last_ = 0;
    double period_ = 33.3;
    double smooth_ = 0.0;
};
bool FrameClockOn();

// Two poses of one skeleton blended bone by bone (t 0: a, 1: b); a bone in one of them only is taken from it.
Pose Blend(const Pose& a, const Pose& b, float t);

// How far a pose moved from the one before: the sum over the bones both have of the rotation's angle, in
// degrees. For the smoothness figures of LICHII_COOP_DEBUG_POSE.
double Turned(const Pose& before, const Pose& now);

// Smoothness of a stream of poses, one a game frame: the mean motion a frame, the mean change of that motion
// from one frame to the next (the jerk), and the frames with no motion at all.
struct Motion {
    std::optional<Pose> last;
    double last_v = -1.0;
    double v_sum = 0.0, jerk_sum = 0.0;
    unsigned frames = 0, still = 0;
    void Add(const Pose& pose);
    std::string Take();  // "v 1.23 jerk 0.45 still 2/30" and zeroed
};

// The sender's side of kMaxSendBones: which of a full pose's bones to send now.
class BonePicker {
public:
    // `full`: every moving bone, increasing order. Returns at most `budget`
    // of them, increasing order, and remembers them as sent at now_ms.
    // `key_ms` (bone keys; 0: off): the pose's stamp, the time the receiver shows it at. A bone then counts as
    // changed by how far it is from the receiver's guess (KeyAhead from its last two sends), not from its last send.
    std::vector<coop::protocol::PoseBone> Pick(std::uint32_t skeleton,
                                               const std::vector<coop::protocol::PoseBone>& full,
                                               std::uint64_t now_ms,
                                               std::size_t budget = kMaxSendBones,
                                               std::uint64_t key_ms = 0);
    // How far `now` is from `then`, in packed units; a part come or gone is far.
    static int Change(const coop::protocol::PoseBone& then, const coop::protocol::PoseBone& now);

private:
    struct Sent {
        coop::protocol::PoseBone bone;
        std::uint64_t ms = 0;
        std::uint64_t key_ms = 0;                        // its pose's stamp (bone keys)
        std::optional<coop::protocol::PoseBone> before;  // the send before it, for the guess
        std::uint64_t before_key_ms = 0;
    };
    std::uint32_t skeleton_ = 0;
    std::map<std::uint16_t, Sent> sent_;
};

// The local player's actor, as coop::host::PlayerReader finds it; 0 none.
std::uint64_t FindPlayer(CoopPuppet::Guest& guest, std::uint64_t main_base);

// Calc thread: copy every bone's local transform of `from`'s skeleton onto
// `to`'s, when both skeletons are found and have the same bone count. For
// the fairies: the local Navi's wing beat onto each puppet's Navi, whose own
// actor plays no animation. `note` (may be null) gets both models and bone
// counts, or why nothing was copied.
bool CopySkeleton(CoopPuppet::Guest& guest, std::uint64_t from, std::uint64_t to,
                  std::string* note);

// Calc thread, once a frame: read the local player's skeleton and write the
// shown pose onto each live puppet. puppets: (player number, actor); the
// debug puppet (number 0) copies the local player's own pose.
void OnCalcFrame(CoopPuppet::Guest& guest, std::uint64_t main_base,
                 const std::vector<std::pair<std::uint8_t, std::uint64_t>>& puppets,
                 std::uint64_t now_ms);
// Network thread: send the newest local pose when it is time.
void Send(coop::net::Client& client, std::uint64_t now_ms);
// What Send sends now: Link's pose, then each held piece that changed (or
// every kPieceRefreshMs while it holds still). Empty when it is not time.
std::vector<coop::protocol::PlayerPose> TakeOutgoing(std::uint64_t server_now_us,
                                                     std::uint64_t now_ms);
// Network thread: keep a received PLAYER_POSE.
void OnEvent(const coop::net::Event& event, std::uint64_t now_ms);
void Receive(const coop::protocol::PlayerPose& pose, std::uint64_t now_ms);
// LICHII_COOP_DEBUG_POSE=1: a line a second with bone counts and rates.
// The pose copy wins over the AS commands: a puppet whose poses arrive plays
// no AS command of its own (see Drives). LICHII_COOP_POSE_AS=1 (as_too) plays
// them as well, as before the pose copy.
void SetLog(std::function<void(const std::string&)> log, bool debug, bool as_too = false);
// Calc thread: whether the pose copy drives this player's puppet now (poses
// arrived in the last kFreshMs), so the director plays no AS command on it.
bool Drives(std::uint8_t number, std::uint64_t now_ms);
// Our own pose stops being read for the others while frozen: they keep the last one (an item-get scene the
// NX2navi port plays here for another player's item: the others would see our puppet hold up an empty hand).
void FreezeOwnPose(bool frozen);
inline constexpr std::uint64_t kFreshMs = 1000;

}  // namespace CoopPose
