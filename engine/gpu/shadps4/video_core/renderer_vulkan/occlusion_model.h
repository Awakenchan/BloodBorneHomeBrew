// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace OcclusionModel {
constexpr std::uint64_t ValidBit = 1ull << 63;
constexpr std::uint32_t MaxDbs = 16;
constexpr std::uint64_t PlainlyVisible = 0x2FFFFFFull;

constexpr std::uint64_t DbPart(std::uint64_t count, std::uint32_t dbs, std::uint32_t db) {
    const std::uint64_t each = count / dbs;
    return db < count % dbs ? each + 1 : each;
}

inline void FillBlock(std::uint64_t* words, std::uint32_t dbs, std::uint64_t base,
                      std::uint64_t samples) {
    for (std::uint32_t db = 0; db < dbs; ++db) {
        words[2 * db] = DbPart(base, dbs, db) | ValidBit;
        words[2 * db + 1] = DbPart(base + samples, dbs, db) | ValidBit;
    }
}

enum class Answer : std::uint8_t { Hidden, Seen, Pending };

inline Answer ReadAnswer(const std::uint64_t* words, std::uint32_t dbs) {
    std::uint64_t passed = 0;
    for (std::uint32_t i = 0; i < 2 * dbs; i += 2) {
        if ((words[i] & words[i + 1] & ValidBit) == 0) {
            return Answer::Pending;
        }
        passed += (words[i + 1] & ~ValidBit) - (words[i] & ~ValidBit);
    }
    return passed == 0 ? Answer::Hidden : Answer::Seen;
}

constexpr std::uint32_t ScaleToNative(std::uint32_t width, std::uint32_t height) {
    constexpr std::uint64_t native = 1920ull * 1080ull;
    const std::uint64_t area = std::uint64_t(width) * height;
    return area == 0 || area >= native ? 1u << 16
                                       : static_cast<std::uint32_t>((native << 16) / area);
}

constexpr std::uint64_t QueryCount(std::uint64_t raw, bool available, bool precise,
                                   std::uint32_t scale) {
    if (!available || (!precise && raw != 0)) {
        return PlainlyVisible;
    }
    const std::uint64_t scaled = (raw * scale + 0xFFFFu) >> 16;
    return scaled < raw ? raw : scaled;
}

constexpr bool QueryFresh(std::uint64_t frame, std::uint64_t now, std::uint64_t max_lag) {
    return now >= frame && now - frame <= max_lag;
}

constexpr std::uint64_t VisibilityCount(std::uint64_t samples, std::uint64_t queries, bool complete,
                                        bool fresh) {
    return complete && fresh && queries != 0 && samples == 0 ? 0 : PlainlyVisible;
}

constexpr bool LateAnswerWritable(std::uint64_t frame, std::uint64_t now, std::uint64_t max_lag,
                                  bool answered_before) {
    return QueryFresh(frame, now, max_lag) ||
           (answered_before && QueryFresh(frame, now, 2 * max_lag));
}

constexpr bool DrawPredicated(bool any_seen, bool any_unknown, bool draw_when_seen) {
    return any_unknown || any_seen == draw_when_seen;
}

class SteadyCount {
public:
    static constexpr std::uint32_t Window = 5;
    static constexpr std::uint64_t Unseen = 1;

    constexpr std::uint64_t Add(std::uint64_t count) {
        recent[slot] = count;
        slot = slot + 1 == Window ? 0 : slot + 1;
        std::uint64_t largest = 0;
        for (const std::uint64_t c : recent) {
            largest = std::max(largest, c);
        }
        return largest;
    }

private:
    std::array<std::uint64_t, Window> recent{Unseen, Unseen, Unseen, Unseen, Unseen};
    std::uint32_t slot = 0;
};

struct OpenBlock {
    std::uint64_t block;
    std::uint64_t issued;
    std::uint64_t first_query;
    std::uint64_t frame;
};

struct ClosedBlock {
    std::uint64_t first_query;
    std::uint64_t queries;
    std::uint64_t frame;
    bool complete;
};

class OpenBlocks {
public:
    static constexpr std::uint32_t Capacity = 64;

    [[nodiscard]] bool Empty() const noexcept {
        return size == 0;
    }
    [[nodiscard]] std::uint32_t Size() const noexcept {
        return size;
    }

    [[nodiscard]] bool WantsQuery(std::uint64_t next_query, std::uint64_t budget) const noexcept {
        for (std::uint32_t i = 0; i < size; ++i) {
            if (next_query - open[i].first_query < budget) {
                return true;
            }
        }
        return false;
    }

    void Begin(std::uint64_t block, std::uint64_t issued, std::uint64_t next_query,
               std::uint64_t frame) noexcept {
        Remove(block);
        if (size == Capacity) {
            RemoveAt(0);
        }
        open[size++] = {block, issued, next_query, frame};
    }

    std::optional<ClosedBlock> End(std::uint64_t block, std::uint64_t issued,
                                   std::uint64_t next_query) noexcept {
        for (std::uint32_t i = 0; i < size; ++i) {
            if (open[i].block != block) {
                continue;
            }
            const OpenBlock b = open[i];
            RemoveAt(i);
            const std::uint64_t queries = next_query - b.first_query;
            return ClosedBlock{b.first_query, queries, b.frame, issued - b.issued == queries};
        }
        return std::nullopt;
    }

    void Expire(std::uint64_t now, std::uint64_t frames) noexcept {
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < size; ++i) {
            if (now - open[i].frame <= frames) {
                open[kept++] = open[i];
            }
        }
        size = kept;
    }

    [[nodiscard]] std::uint64_t FirstNeeded(std::uint64_t next_query) const noexcept {
        std::uint64_t first = next_query;
        for (std::uint32_t i = 0; i < size; ++i) {
            first = std::min(first, open[i].first_query);
        }
        return first;
    }

private:
    void Remove(std::uint64_t block) noexcept {
        for (std::uint32_t i = 0; i < size; ++i) {
            if (open[i].block == block) {
                RemoveAt(i);
                return;
            }
        }
    }
    void RemoveAt(std::uint32_t i) noexcept {
        std::copy(open.begin() + i + 1, open.begin() + size, open.begin() + i);
        --size;
    }

    std::array<OpenBlock, Capacity> open{};
    std::uint32_t size = 0;
};

}
