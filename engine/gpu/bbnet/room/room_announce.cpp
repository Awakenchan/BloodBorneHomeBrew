// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_announce.h"

#include <algorithm>

namespace BbRoom {
namespace {

constexpr std::size_t kMostSeen = 32;
constexpr std::size_t kMostWaiting = 3;
constexpr std::size_t kMostPerMinute = 6;
constexpr auto kFade = std::chrono::milliseconds(400);
constexpr auto kShortest = std::chrono::seconds(8);
constexpr auto kLongest = std::chrono::seconds(15);
constexpr auto kPerCharacter = std::chrono::milliseconds(60);

std::size_t SequenceLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;
}

std::uint32_t CodePoint(const std::string& text, std::size_t at, std::size_t length) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (length == 1) return lead;
    std::uint32_t point = lead & (0xFFu >> (length + 1));
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0) != 0x80) return 0xFFFFFFFFu;
        point = (point << 6) | (next & 0x3F);
    }
    return point;
}

bool Hidden(std::uint32_t point) {
    return point < 0x20 || (point >= 0x7F && point < 0xA0) || point == 0xAD || (point >= 0x200B && point <= 0x200F) ||
           (point >= 0x2028 && point <= 0x202E) || (point >= 0x2060 && point <= 0x206F) || (point >= 0xD800 && point <= 0xF8FF) ||
           point == 0xFEFF || point > 0x10FFFF;
}

} // namespace

std::string CleanAnnouncement(const std::string& text) {
    std::string out;
    std::size_t count = 0;
    bool space = false;
    for (std::size_t at = 0; at < text.size() && count < kLongestAnnouncement;) {
        const std::size_t length = SequenceLength(static_cast<unsigned char>(text[at]));
        if (!length || at + length > text.size()) {
            ++at;
            continue;
        }
        const std::uint32_t point = CodePoint(text, at, length);
        if (point == ' ' || Hidden(point)) {
            space = true;
        } else {
            if (space && !out.empty()) {
                out.push_back(' ');
                ++count;
            }
            space = false;
            if (count < kLongestAnnouncement) {
                out.append(text, at, length);
                ++count;
            }
        }
        at += length;
    }
    return out;
}

bool AnnouncementServer(const std::string& server) {
    std::string host = server;
    while (!host.empty() && host.back() == '/') host.pop_back();
    if (host.rfind("http://127.0.0.1:", 0) == 0) return true;
    for (const char* scheme : {"https://", "wss://"}) {
        const std::string prefix = scheme;
        if (host.rfind(prefix, 0) == 0) return host.substr(prefix.size()) == "185-244-28-175.sslip.io";
    }
    return host == "185-244-28-175.sslip.io";
}

bool AnnouncementQueue::Offer(std::uint64_t id, const std::string& text, Time now) {
    if (std::find(seen_.begin(), seen_.end(), id) != seen_.end()) return false;
    seen_.push_back(id);
    if (seen_.size() > kMostSeen) seen_.pop_front();
    while (!offered_.empty() && now - offered_.front() >= std::chrono::minutes(1)) offered_.pop_front();
    if (offered_.size() >= kMostPerMinute || waiting_.size() >= kMostWaiting) return false;
    std::string clean = CleanAnnouncement(text);
    if (clean.empty()) return false;
    offered_.push_back(now);
    const auto reading = kPerCharacter * static_cast<long long>(clean.size());
    const auto lasts = std::clamp<std::chrono::milliseconds>(reading, kShortest, kLongest);
    waiting_.push_back({std::move(clean), lasts});
    return true;
}

bool AnnouncementQueue::Current(Time now, std::string& text, float& alpha) {
    if (!showing_.empty() && now - since_ >= lasts_) showing_.clear();
    if (showing_.empty()) {
        if (waiting_.empty()) return false;
        showing_ = std::move(waiting_.front().text);
        lasts_ = waiting_.front().lasts;
        waiting_.pop_front();
        since_ = now;
    }
    const auto age = now - since_;
    const auto left = lasts_ - age;
    const auto edge = std::min<std::chrono::steady_clock::duration>(age, left);
    alpha = std::clamp(std::chrono::duration<float>(edge).count() / std::chrono::duration<float>(kFade).count(), 0.0f, 1.0f);
    text = showing_;
    return true;
}

} // namespace BbRoom
