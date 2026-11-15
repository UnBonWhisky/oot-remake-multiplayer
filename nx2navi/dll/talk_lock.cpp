// SPDX-License-Identifier: GPL-3.0-or-later
#include "talk_lock.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace CoopNx2navi::TalkLock {

std::vector<std::uint8_t> Encode(const Note& n) {
    std::vector<std::uint8_t> out;
    out.push_back(n.kind == kNote ? (n.talking ? 1 : 0) : n.kind);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(n.hash >> (8 * i)));
    }
    if (n.kind != kNote) {
        out.push_back(n.target);
        out.push_back(n.holder);
    }
    const std::size_t len = n.scene.size() > 80 ? 80 : n.scene.size();
    out.insert(out.end(), n.scene.begin(), n.scene.begin() + static_cast<std::ptrdiff_t>(len));
    return out;
}

std::optional<Note> Decode(const std::vector<std::uint8_t>& data) {
    if (data.size() < 9 || data[0] > kDeny) {
        return std::nullopt;
    }
    Note n;
    std::size_t at = 9;
    if (data[0] <= 1) {
        n.talking = data[0] == 1;
    } else {
        if (data.size() < 11) {
            return std::nullopt;
        }
        n.kind = data[0];
        n.target = data[9];
        n.holder = data[10];
        at = 11;
    }
    for (int i = 0; i < 8; ++i) {
        n.hash |= static_cast<std::uint64_t>(data[1 + i]) << (8 * i);
    }
    n.scene.assign(data.begin() + static_cast<std::ptrdiff_t>(at), data.end());
    if (n.hash == 0) {
        return std::nullopt;
    }
    return n;
}

std::string Refusal(const std::string& name, bool french) {
    return french ? name + " parle d\xc3\xa9j\xc3\xa0 \xc3\xa0 ce personnage" : name + " is already talking to this NPC";
}

void Lock::SetScene(const std::string& scene) {
    std::lock_guard lock(mutex_);
    if (scene == scene_) {
        return;
    }
    scene_ = scene;
    remote_.clear();
    if (ours_ != 0 && said_) {
        ended_ = ours_;
    }
    ours_ = 0;
}

void Lock::SetRoles(std::uint8_t self, std::uint8_t authority) {
    std::lock_guard lock(mutex_);
    self_ = self;
    authority_ = authority;
}

void Lock::SetSender(std::function<void(const Note&)> send) {
    std::lock_guard lock(mutex_);
    send_ = std::move(send);
}

std::uint8_t Lock::HolderLocked(const std::string& scene, std::uint64_t hash, std::uint8_t except,
                                std::uint64_t now_us) const {
    if (ours_ == hash && scene == scene_ && self_ != 0 && self_ != except) {
        return self_;
    }
    if (const auto it = granted_.find({scene, hash});
        it != granted_.end() && it->second.first != except && it->second.second > now_us) {
        return it->second.first;
    }
    return 0;
}

std::uint8_t Lock::Request(std::uint64_t hash, std::uint64_t now_us, std::uint64_t wait_us) {
    std::unique_lock lock(mutex_);
    if (hash == 0) {
        return 0;
    }
    auto refuse = [&](std::uint8_t player) {
        if (now_us - refused_us_ >= 1'000'000) {
            refused_us_ = now_us;
            refusals_.push_back(player);
            char text[160];
            std::snprintf(text, sizeof text, "talk: P%u talks to NPC %#llx: our talk to it is refused", player,
                          static_cast<unsigned long long>(hash));
            lines_.emplace_back(text);
        }
        return player;
    };
    if (const auto it = remote_.find(hash); it != remote_.end()) {
        for (const auto& [player, until] : it->second) {
            if (until > now_us) {
                return refuse(player);
            }
        }
    }
    auto take = [&] {
        if (ours_ != hash) {
            ours_ = hash;
            said_ = false;
            event_seen_ = false;
            asked_us_ = now_us;
            char text[120];
            std::snprintf(text, sizeof text, "talk: we talk to NPC %#llx", static_cast<unsigned long long>(hash));
            lines_.emplace_back(text);
        }
        return std::uint8_t{0};
    };
    if (ours_ == hash) {
        return 0;  // our talk goes on
    }
    if (self_ != 0 && authority_ == self_) {
        // The host's game: decided here, against what it granted.
        if (const std::uint8_t holder = HolderLocked(scene_, hash, self_, now_us); holder != 0) {
            return refuse(holder);
        }
        return take();  // ours_ now: HolderLocked refuses asks for it while it lasts
    }
    if (self_ == 0 || authority_ == 0 || !send_ || wait_us == 0) {
        return take();  // nobody to ask: as before
    }
    // Ask the host's game and hold the talk until it answers.
    asking_ = hash;
    answer_ = 0;
    answer_holder_ = 0;
    const Note ask{scene_, hash, false, kAsk, self_, 0};
    auto send = send_;
    lock.unlock();
    send(ask);
    lock.lock();
    const bool got = answered_.wait_for(lock, std::chrono::microseconds(wait_us),
                                        [&] { return answer_ != 0 || asking_ != hash; });
    const std::uint8_t answer = got && asking_ == hash ? answer_ : 0;
    const std::uint8_t holder = answer_holder_;
    asking_ = 0;
    if (answer == kDeny && holder != 0) {
        remote_[hash][holder] = now_us + kHoldUs;
        return refuse(holder);
    }
    if (answer != kGrant) {
        char text[160];
        std::snprintf(text, sizeof text, "talk: no answer from the host's game (P%u) for NPC %#llx in %llu ms: "
                      "talking anyway", authority_, static_cast<unsigned long long>(hash),
                      static_cast<unsigned long long>(wait_us / 1000));
        lines_.emplace_back(text);
    }
    return take();
}

void Lock::Event(bool running, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (ours_ == 0) {
        return;
    }
    if (running) {
        event_seen_ = true;
        return;
    }
    if (event_seen_ || now_us - asked_us_ > kStartUs) {
        char text[120];
        std::snprintf(text, sizeof text, "talk: our talk to NPC %#llx %s", static_cast<unsigned long long>(ours_),
                      event_seen_ ? "ended" : "did not start");
        lines_.emplace_back(text);
        if (said_) {
            ended_ = ours_;
        }
        ours_ = 0;
    }
}

std::optional<Note> Lock::Remote(std::uint8_t player, const Note& n, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (n.kind == kAsk) {
        if (self_ == 0 || authority_ != self_ || player == 0) {
            return std::nullopt;  // asks are for the host's game
        }
        Note reply{n.scene, n.hash, false, kGrant, player, 0};
        if (const std::uint8_t holder = HolderLocked(n.scene, n.hash, player, now_us); holder != 0) {
            reply.kind = kDeny;
            reply.holder = holder;
        } else {
            granted_[{n.scene, n.hash}] = {player, now_us + kHoldUs};
        }
        char text[160];
        std::snprintf(text, sizeof text, "talk: P%u asks to talk to NPC %#llx: %s", player,
                      static_cast<unsigned long long>(n.hash),
                      reply.kind == kGrant ? "granted" : ("refused, P" + std::to_string(reply.holder) + " talks to it").c_str());
        lines_.emplace_back(text);
        return reply;
    }
    if (n.kind == kGrant || n.kind == kDeny) {
        if (n.target == self_ && n.hash == asking_ && answer_ == 0) {
            answer_ = n.kind;
            answer_holder_ = n.holder;
            answered_.notify_all();
        }
        return std::nullopt;
    }
    // A talk said or ended: for the host's game's decisions in every scene, and for our own quick refusal here.
    if (n.talking) {
        granted_[{n.scene, n.hash}] = {player, now_us + kHoldUs};
    } else if (const auto it = granted_.find({n.scene, n.hash}); it != granted_.end() && it->second.first == player) {
        granted_.erase(it);
    }
    if (n.scene != scene_) {
        return std::nullopt;
    }
    if (n.talking) {
        remote_[n.hash][player] = now_us + kHoldUs;
    } else if (const auto it = remote_.find(n.hash); it != remote_.end()) {
        it->second.erase(player);
        if (it->second.empty()) {
            remote_.erase(it);
        }
    }
    return std::nullopt;
}

std::vector<std::uint64_t> Lock::Others(std::uint64_t now_us) const {
    std::lock_guard lock(mutex_);
    std::vector<std::uint64_t> out;
    for (const auto& [hash, players] : remote_) {
        for (const auto& [player, until] : players) {
            if (until > now_us) {
                out.push_back(hash);
                break;
            }
        }
    }
    return out;
}

std::vector<Note> Lock::TakeSends(std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    std::vector<Note> out;
    if (ended_ != 0) {
        out.push_back(Note{scene_, ended_, false});
        ended_ = 0;
    }
    if (ours_ != 0 && (!said_ || now_us - sent_us_ >= kAgainUs)) {
        said_ = true;
        sent_us_ = now_us;
        out.push_back(Note{scene_, ours_, true});
    }
    return out;
}

std::vector<std::uint8_t> Lock::TakeRefusals() {
    std::lock_guard lock(mutex_);
    std::vector<std::uint8_t> out;
    out.swap(refusals_);
    return out;
}

std::vector<std::string> Lock::TakeLines() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(lines_);
    return out;
}

std::uint64_t Lock::Talking() const {
    std::lock_guard lock(mutex_);
    return ours_;
}

} // namespace CoopNx2navi::TalkLock
