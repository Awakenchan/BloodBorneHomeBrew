// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace BbEngine::SharedProgress {

constexpr std::uint32_t kClear = 0x80000000u;
constexpr std::size_t kLargest = 4096;

inline bool HostStepIsNewer(std::uint32_t host, std::uint32_t guest) {
    for (std::uint32_t bit = 0x80000000u; bit; bit >>= 1) {
        if ((host & bit) != (guest & bit)) return (host & bit) != 0;
        if (host & bit) return false;
    }
    return false;
}

class Ledger {
public:
    void Record(std::uint32_t flag, const std::set<std::string>& present) {
        if (present.empty()) return;
        if (events_.size() >= kLargest) events_.erase(events_.begin());
        events_.push_back({flag, present});
    }

    std::vector<std::uint32_t> For(const std::string& name) const {
        std::map<std::uint32_t, std::size_t> last;
        std::vector<std::uint32_t> order;
        for (const Event& e : events_) {
            if (!e.present.count(name)) continue;
            const std::uint32_t id = e.flag & ~kClear;
            const auto it = last.find(id);
            if (it == last.end()) {
                last.emplace(id, order.size());
                order.push_back(e.flag);
            } else {
                order[it->second] = e.flag;
            }
        }
        return order;
    }

    std::size_t Size() const { return events_.size(); }
    void Clear() { events_.clear(); }

private:
    struct Event {
        std::uint32_t flag;
        std::set<std::string> present;
    };
    std::vector<Event> events_;
};

template <typename Clock> class Presence {
public:
    using Time = typename Clock::time_point;
    using Span = typename Clock::duration;

    explicit Presence(Span grace) : grace_(grace) {}

    void Seen(const std::string& name, Time now) {
        if (!name.empty()) seen_[name] = now;
    }
    void Gone(const std::string& name) { seen_.erase(name); }
    void Clear() { seen_.clear(); }

    std::set<std::string> Here(Time now) {
        std::set<std::string> out;
        for (auto it = seen_.begin(); it != seen_.end();) {
            if (now - it->second > grace_) {
                it = seen_.erase(it);
                continue;
            }
            out.insert(it->first);
            ++it;
        }
        return out;
    }

private:
    Span grace_;
    std::map<std::string, Time> seen_;
};

} // namespace BbEngine::SharedProgress
