// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbEngine::KeptTravel {

constexpr std::uint32_t kDreamArea = 21, kDungeonArea = 29;
constexpr std::uint32_t kFlagsMagic = 0x45464e49u;
constexpr std::size_t kFlagsLargest = 4096;
constexpr std::uint32_t kFlagsBlocksMost = 64;

constexpr bool CanKeep(std::uint32_t lamp) {
    const std::uint32_t area = lamp / 100000;
    return lamp && area != kDreamArea && area != kDungeonArea;
}

constexpr std::uint32_t SceneSeenFlag(std::uint32_t event) {
    switch (event) {
    case 12405263: return 12401000;
    case 12200130: return 12204020;
    case 12800431: return 12800434;
    default: return event;
    }
}

inline std::uint32_t ReadWord(const std::uint8_t* data, std::size_t word) {
    std::uint32_t value = 0;
    std::memcpy(&value, data + word * 4, 4);
    return value;
}

inline bool ValidFlags(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 20 || size > kFlagsLargest) return false;
    std::uint16_t version = 0;
    std::memcpy(&version, data + 4, 2);
    if (ReadWord(data, 0) != kFlagsMagic || version != 1) return false;
    const std::uint32_t blocks = ReadWord(data, 2), table = ReadWord(data, 3), body = ReadWord(data, 4);
    if (!blocks || blocks > kFlagsBlocksMost || table % 4 || table > size || body > size) return false;
    if (std::uint64_t(table) + std::uint64_t(blocks) * 12 > size) return false;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        const std::size_t word = table / 4 + std::size_t(i) * 3;
        const std::uint64_t end = std::uint64_t(body) + ReadWord(data, word + 1) + ReadWord(data, word + 2);
        if (end > size) return false;
    }
    return true;
}

enum class Step { Wait, Rebuild, Done, GiveUp };

template <typename Clock> class Load {
public:
    using Time = typename Clock::time_point;
    using Seconds = std::chrono::seconds;
    static constexpr Seconds kLoadStart{30}, kLoadLimit{300}, kSettle{2}, kBuildAnyway{20}, kBuildEvery{3}, kBackLimit{120};

    void Begin(Time now) {
        state_ = State::Asked;
        begun_ = now;
        placed_ = rebuilt_ = Time{};
    }
    bool Active() const { return state_ != State::Off; }
    bool LoadOver() const { return state_ == State::Placed; }

    Step Tick(Time now, bool loading, bool placed, bool session, bool others_here, int missing) {
        if (state_ == State::Off) return Step::Wait;
        if (!session) return Finish(Step::GiveUp);
        if (loading) {
            state_ = State::Loading;
            if (now - begun_ > kLoadLimit) return Finish(Step::GiveUp);
            return Step::Wait;
        }
        if (state_ == State::Asked) return now - begun_ > kLoadStart ? Finish(Step::GiveUp) : Step::Wait;
        if (state_ == State::Loading) {
            if (!placed) return now - begun_ > kLoadLimit ? Finish(Step::GiveUp) : Step::Wait;
            state_ = State::Placed;
            placed_ = now;
            return Step::Wait;
        }
        const auto since = now - placed_;
        if (since < kSettle) return Step::Wait;
        if (missing == 0 && others_here) return Finish(Step::Done);
        if (since > kBackLimit) return Finish(Step::GiveUp);
        if (missing > 0 && (others_here || since >= kBuildAnyway) && (rebuilt_ == Time{} || now - rebuilt_ >= kBuildEvery)) {
            rebuilt_ = now;
            return Step::Rebuild;
        }
        return Step::Wait;
    }

private:
    enum class State { Off, Asked, Loading, Placed };
    Step Finish(Step step) {
        state_ = State::Off;
        return step;
    }
    State state_ = State::Off;
    Time begun_{}, placed_{}, rebuilt_{};
};

}
