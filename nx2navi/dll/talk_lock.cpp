// SPDX-License-Identifier: GPL-3.0-or-later
#include "talk_lock.hpp"

#include <cstdio>

namespace CoopNx2navi::TalkLock {

std::vector<std::uint8_t> Encode(const Note& n) {
    std::vector<std::uint8_t> out;
    out.push_back(n.talking ? 1 : 0);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(n.hash >> (8 * i)));
    }
    const std::size_t len = n.scene.size() > 80 ? 80 : n.scene.size();
    out.insert(out.end(), n.scene.begin(), n.scene.begin() + static_cast<std::ptrdiff_t>(len));
    return out;
}

std::optional<Note> Decode(const std::vector<std::uint8_t>& data) {
    if (data.size() < 9 || data[0] > 1) {
        return std::nullopt;
    }
    Note n;
    n.talking = data[0] == 1;
    for (int i = 0; i < 8; ++i) {
        n.hash |= static_cast<std::uint64_t>(data[1 + i]) << (8 * i);
    }
    n.scene.assign(data.begin() + 9, data.end());
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

std::uint8_t Lock::Request(std::uint64_t hash, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (hash == 0) {
        return 0;
    }
    if (const auto it = remote_.find(hash); it != remote_.end()) {
        for (const auto& [player, until] : it->second) {
            if (until > now_us) {
                if (now_us - refused_us_ >= 1'000'000) {
                    refused_us_ = now_us;
                    refusals_.push_back(player);
                    char text[160];
                    std::snprintf(text, sizeof text, "talk: P%u talks to NPC %#llx: our talk to it is refused",
                                  player, static_cast<unsigned long long>(hash));
                    lines_.emplace_back(text);
                }
                return player;
            }
        }
    }
    if (ours_ != hash) {
        ours_ = hash;
        said_ = false;
        event_seen_ = false;
        asked_us_ = now_us;
        char text[120];
        std::snprintf(text, sizeof text, "talk: we talk to NPC %#llx", static_cast<unsigned long long>(hash));
        lines_.emplace_back(text);
    }
    return 0;
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

void Lock::Remote(std::uint8_t player, const Note& n, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (n.scene != scene_) {
        return;
    }
    if (n.talking) {
        remote_[n.hash][player] = now_us + kHoldUs;
    } else if (const auto it = remote_.find(n.hash); it != remote_.end()) {
        it->second.erase(player);
        if (it->second.empty()) {
            remote_.erase(it);
        }
    }
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
