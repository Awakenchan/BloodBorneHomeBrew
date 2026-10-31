// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "gpu/shadps4/video_core/renderer_vulkan/occlusion_model.h"

using namespace OcclusionModel;

namespace {
constexpr std::uint32_t Native = 1u << 16;

struct Pipe {
    std::uint64_t budget = 64;
    std::uint64_t issued = 0;
    std::uint64_t next_query = 0;
    std::uint64_t frame = 0;
    std::vector<std::uint64_t> samples;
    OpenBlocks open;

    void Draw(std::uint64_t count, bool dropped = false) {
        ++issued;
        if (dropped || !open.WantsQuery(next_query, budget)) {
            return;
        }
        samples.push_back(count);
        ++next_query;
    }
    void Begin(std::uint64_t block) {
        open.Begin(block, issued, next_query, frame);
    }
    std::uint64_t End(std::uint64_t block) {
        const auto closed = open.End(block, issued, next_query);
        if (!closed || !closed->complete) {
            return PlainlyVisible;
        }
        std::uint64_t sum = 0;
        for (std::uint64_t q = closed->first_query; q < closed->first_query + closed->queries;
             ++q) {
            sum += samples[q];
        }
        return sum;
    }
};

std::uint64_t Passed(const std::uint64_t* words, std::uint32_t dbs) {
    std::uint64_t sum = 0;
    for (std::uint32_t db = 0; db < dbs; ++db) {
        sum += (words[2 * db + 1] & ~ValidBit) - (words[2 * db] & ~ValidBit);
    }
    return sum;
}

void BlockWords() {
    for (const std::uint32_t dbs : {1u, 8u, 16u}) {
        for (std::uint64_t count = 0; count < 300; ++count) {
            std::uint64_t sum = 0;
            for (std::uint32_t db = 0; db < dbs; ++db) {
                sum += DbPart(count, dbs, db);
                assert(DbPart(count + 1, dbs, db) >= DbPart(count, dbs, db));
            }
            assert(sum == count);
        }
        for (const std::uint64_t base : {0ull, 5ull, 987654321ull}) {
            for (const std::uint64_t samples : std::initializer_list<std::uint64_t>{0, 1, 7, 5000, PlainlyVisible}) {
                std::uint64_t words[2 * MaxDbs] = {};
                assert(ReadAnswer(words, dbs) == Answer::Pending);
                FillBlock(words, dbs, base, samples);
                assert(Passed(words, dbs) == samples);
                assert(ReadAnswer(words, dbs) == (samples ? Answer::Seen : Answer::Hidden));
                std::uint64_t half[2 * MaxDbs] = {};
                for (std::uint32_t db = 0; db < dbs; ++db) {
                    half[2 * db] = words[2 * db];
                }
                assert(ReadAnswer(half, dbs) == Answer::Pending);
                half[2 * (dbs - 1) + 1] = 0;
                for (std::uint32_t db = 0; db + 1 < dbs; ++db) {
                    half[2 * db + 1] = words[2 * db + 1];
                }
                assert(ReadAnswer(half, dbs) == Answer::Pending);
            }
        }
    }
}

void QueryCounts() {
    assert(QueryCount(0, false, true, Native) == PlainlyVisible);
    assert(QueryCount(0, false, false, Native) == PlainlyVisible);
    assert(QueryCount(1, true, false, Native) == PlainlyVisible);
    assert(QueryCount(0, true, false, Native) == 0);
    assert(QueryCount(0, true, true, 4 * Native) == 0);
    assert(QueryCount(10, true, true, Native) == 10);
    assert(QueryCount(1, true, true, Native + 1) == 2);

    assert(ScaleToNative(1920, 1080) == Native);
    assert(ScaleToNative(2560, 1440) == Native);
    assert(ScaleToNative(960, 540) == 4 * Native);
    assert(ScaleToNative(0, 0) == Native);
    assert(QueryCount(250, true, true, ScaleToNative(960, 540)) == 1000);
}

void Predication() {
    assert(DrawPredicated(false, true, true));
    assert(DrawPredicated(true, true, false));
    assert(DrawPredicated(true, false, true));
    assert(!DrawPredicated(false, false, true));
    assert(DrawPredicated(false, false, false));
    assert(!DrawPredicated(true, false, false));
}

void ConservativeVisibility() {
    assert(QueryFresh(100, 100, 6));
    assert(QueryFresh(100, 106, 6));
    assert(!QueryFresh(100, 107, 6));
    assert(!QueryFresh(100, 0, 6));
    assert(VisibilityCount(0, 1, true, true) == 0);
    assert(VisibilityCount(0, 64, true, true) == 0);
    for (const std::uint64_t samples : std::initializer_list<std::uint64_t>{1, 2, 10, 1000, PlainlyVisible}) {
        assert(VisibilityCount(samples, 1, true, true) == PlainlyVisible);
    }
    assert(VisibilityCount(0, 0, true, true) == PlainlyVisible);
    assert(VisibilityCount(0, 1, false, true) == PlainlyVisible);
    assert(VisibilityCount(0, 1, true, false) == PlainlyVisible);
    SteadyCount history;
    for (unsigned i = 0; i < SteadyCount::Window; ++i) {
        history.Add(0);
    }
    assert(history.Add(VisibilityCount(1, 1, true, true)) == PlainlyVisible);
    for (unsigned i = 0; i + 1 < SteadyCount::Window; ++i) {
        assert(history.Add(0) == PlainlyVisible);
    }
    assert(history.Add(0) == 0);
}

void LateAnswers() {
    assert(LateAnswerWritable(100, 106, 6, false));
    assert(!LateAnswerWritable(100, 107, 6, false));
    assert(LateAnswerWritable(100, 112, 6, true));
    assert(!LateAnswerWritable(100, 113, 6, true));
    assert(!LateAnswerWritable(100, 0, 6, true));
}

void Steadying() {
    {
        SteadyCount fresh;
        for (std::uint32_t i = 0; i + 1 < SteadyCount::Window; ++i) {
            assert(fresh.Add(0) == SteadyCount::Unseen);
        }
        assert(fresh.Add(0) == 0);
        assert(fresh.Add(9) == 9);
        for (std::uint32_t i = 0; i + 1 < SteadyCount::Window; ++i) {
            assert(fresh.Add(0) == 9);
        }
        assert(fresh.Add(0) == 0);
    }
    {
        SteadyCount lamp;
        for (const std::uint64_t m : {40ull, 0ull, 35ull, 0ull, 0ull, 41ull, 0ull, 38ull}) {
            assert(lamp.Add(m) >= 35);
        }
        SteadyCount fading;
        for (std::uint32_t i = 0; i < SteadyCount::Window; ++i) {
            fading.Add(100);
        }
        for (std::uint32_t i = 0; i + 1 < SteadyCount::Window; ++i) {
            assert(fading.Add(10) == 100);
        }
        assert(fading.Add(10) == 10);
        SteadyCount unknown;
        unknown.Add(0);
        assert(unknown.Add(PlainlyVisible) == PlainlyVisible);
        assert(unknown.Add(0) == PlainlyVisible);
    }
}

void Measuring() {
    const std::uint64_t a = 0x1000, b = 0x2000;
    {
        Pipe pipe;
        pipe.Draw(500);
        assert(pipe.next_query == 0);
        pipe.Begin(a);
        pipe.Draw(0);
        assert(pipe.End(a) == 0);
        pipe.Draw(500);
        pipe.Begin(a);
        pipe.Draw(12);
        assert(pipe.End(a) == 12);
        assert(pipe.next_query == 2);
        assert(pipe.open.Empty());
    }
    {
        Pipe pipe;
        pipe.Begin(a);
        pipe.Draw(0);
        pipe.Begin(b);
        pipe.Draw(0);
        assert(pipe.End(b) == 0);
        pipe.Draw(0);
        pipe.Draw(0);
        assert(pipe.End(a) == 0);
        pipe.Begin(a);
        pipe.Begin(b);
        pipe.Draw(3);
        assert(pipe.End(b) == 3);
        pipe.Draw(0);
        assert(pipe.End(a) == 3);
    }
    {
        Pipe pipe;
        pipe.Begin(a);
        pipe.Draw(1);
        pipe.Begin(b);
        pipe.Draw(0);
        assert(pipe.End(a) == 1);
        pipe.Draw(4);
        assert(pipe.End(b) == 4);
    }
    {
        Pipe pipe;
        pipe.budget = 3;
        pipe.Begin(a);
        for (int i = 0; i < 3; ++i) {
            pipe.Draw(0);
        }
        assert(pipe.End(a) == 0);
        pipe.Begin(a);
        for (int i = 0; i < 4; ++i) {
            pipe.Draw(0);
        }
        assert(pipe.End(a) == PlainlyVisible);
        pipe.Begin(a);
        for (int i = 0; i < 3; ++i) {
            pipe.Draw(0);
        }
        assert(!pipe.open.WantsQuery(pipe.next_query, pipe.budget));
        pipe.Begin(b);
        assert(pipe.open.WantsQuery(pipe.next_query, pipe.budget));
        pipe.Draw(0);
        assert(pipe.End(b) == 0);
        assert(pipe.End(a) == 0);
    }
    {
        Pipe pipe;
        pipe.Begin(a);
        pipe.Draw(0);
        pipe.Draw(0, true);
        assert(pipe.End(a) == PlainlyVisible);
        assert(pipe.End(b) == PlainlyVisible);
        pipe.Begin(a);
        pipe.Draw(9);
        pipe.Begin(a);
        pipe.Draw(0);
        assert(pipe.End(a) == 0);
        assert(pipe.open.Empty());
    }
    {
        Pipe pipe;
        assert(pipe.open.FirstNeeded(pipe.next_query) == 0);
        pipe.Begin(a);
        pipe.Draw(1);
        pipe.Begin(b);
        pipe.Draw(1);
        assert(pipe.open.FirstNeeded(pipe.next_query) == 0);
        pipe.End(a);
        assert(pipe.open.FirstNeeded(pipe.next_query) == 1);
        pipe.End(b);
        assert(pipe.open.FirstNeeded(pipe.next_query) == 2);
        pipe.Begin(a);
        pipe.Draw(1);
        pipe.frame = 10;
        pipe.open.Expire(pipe.frame, 8);
        assert(pipe.open.Empty());
        assert(pipe.open.FirstNeeded(pipe.next_query) == 3);
        assert(pipe.End(a) == PlainlyVisible);
        pipe.Begin(a);
        pipe.frame = 18;
        pipe.open.Expire(pipe.frame, 8);
        assert(pipe.open.Size() == 1);
    }
    {
        Pipe pipe;
        for (std::uint64_t i = 0; i <= OpenBlocks::Capacity; ++i) {
            pipe.Begin(0x10000 + 16 * i);
        }
        assert(pipe.open.Size() == OpenBlocks::Capacity);
        pipe.Draw(0);
        assert(pipe.End(0x10000) == PlainlyVisible);
        assert(pipe.End(0x10000 + 16 * OpenBlocks::Capacity) == 0);
    }
}

void Answers() {
    for (const std::uint32_t dbs : {8u, 16u}) {
        SteadyCount steady;
        std::uint64_t counter = 0;
        const std::uint64_t measured[] = {12, 0, 0, 0, 0, 0, 5, PlainlyVisible, 0};
        const Answer expected[] = {Answer::Seen, Answer::Seen,   Answer::Seen,
                                   Answer::Seen, Answer::Seen,   Answer::Hidden,
                                   Answer::Seen, Answer::Seen,   Answer::Seen};
        for (std::size_t i = 0; i < sizeof(measured) / sizeof(measured[0]); ++i) {
            const std::uint64_t report = steady.Add(measured[i]);
            std::uint64_t words[2 * MaxDbs] = {};
            FillBlock(words, dbs, counter, report);
            counter += report;
            assert(Passed(words, dbs) == report);
            assert(ReadAnswer(words, dbs) == expected[i]);
        }
    }
}

}

int main() {
    BlockWords();
    QueryCounts();
    Predication();
    ConservativeVisibility();
    LateAnswers();
    Steadying();
    Measuring();
    Answers();
    std::printf("occlusion model: ok\n");
    return 0;
}
