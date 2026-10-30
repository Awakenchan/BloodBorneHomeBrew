// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace BbRoom {

constexpr std::size_t kLongestAnnouncement = 200;

std::string CleanAnnouncement(const std::string& text);
bool AnnouncementServer(const std::string& server);

class AnnouncementQueue {
public:
    using Time = std::chrono::steady_clock::time_point;

    bool Offer(std::uint64_t id, const std::string& text, Time now);
    bool Current(Time now, std::string& text, float& alpha);

private:
    struct Item {
        std::string text;
        std::chrono::milliseconds lasts{};
    };
    std::deque<std::uint64_t> seen_;
    std::deque<Time> offered_;
    std::deque<Item> waiting_;
    std::string showing_;
    std::chrono::milliseconds lasts_{};
    Time since_{};
};

} // namespace BbRoom
