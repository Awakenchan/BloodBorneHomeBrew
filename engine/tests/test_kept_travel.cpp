// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/kept_travel.h"
#include <chrono>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

struct FakeClock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<FakeClock, duration>;
};

void Word(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    std::memcpy(out.data() + at, &value, 4);
}

std::vector<std::uint8_t> Snapshot() {
    std::vector<std::uint8_t> out(0xa4 + 12 * 125, 0);
    Word(out, 0, BbEngine::KeptTravel::kFlagsMagic);
    out[4] = 1;
    Word(out, 8, 12);
    Word(out, 12, 0x14);
    Word(out, 16, 0xa4);
    for (std::uint32_t i = 0; i < 12; ++i) {
        Word(out, 0x14 + i * 12, 100 + i);
        Word(out, 0x18 + i * 12, i * 125);
        Word(out, 0x1c + i * 12, 125);
    }
    return out;
}
}

int main() {
    using namespace BbEngine::KeptTravel;
    using std::chrono::seconds;
    Check(SceneSeenFlag(12410000) == 12410000, "a run-once scene is seen when its own event flag is set");
    Check(SceneSeenFlag(12405263) == 12401000 && SceneSeenFlag(12200130) == 12204020 && SceneSeenFlag(12800431) == 12800434, "repeating scene events have a flag of their own");

    Check(CanKeep(2412951), "a lamp in Yharnam keeps the party");
    Check(!CanKeep(2102961), "the Hunter's Dream goes through each hunter's own Dream");
    Check(!CanKeep(2900100), "a chalice dungeon lamp is left to the summon");
    Check(!CanKeep(0), "no lamp, nothing to keep");

    auto good = Snapshot();
    Check(ValidFlags(good.data(), good.size()), "the size the game's serializer writes is accepted");
    Check(!ValidFlags(nullptr, 100), "no data");
    Check(!ValidFlags(good.data(), 16), "too short for its header");
    auto bad = good;
    bad[0] ^= 1;
    Check(!ValidFlags(bad.data(), bad.size()), "wrong magic");
    bad = good;
    bad[4] = 2;
    Check(!ValidFlags(bad.data(), bad.size()), "wrong version");
    bad = good;
    Word(bad, 8, 100000);
    Check(!ValidFlags(bad.data(), bad.size()), "an absurd block count");
    bad = good;
    Word(bad, 0x18 + 11 * 12, 12 * 125);
    Check(!ValidFlags(bad.data(), bad.size()), "a block past the end");
    bad = good;
    Word(bad, 12, 0xfffffff0u);
    Check(!ValidFlags(bad.data(), bad.size()), "a table past the end");
    Check(!ValidFlags(good.data(), good.size() - 1), "a cut frame");

    using KeptLoad = BbEngine::KeptTravel::Load<FakeClock>;
    FakeClock::time_point t{};
    auto Tick = [&](KeptLoad& load, bool loading, bool placed, bool session, bool here, int missing) {
        t += std::chrono::milliseconds(100);
        return load.Tick(t, loading, placed, session, here, missing);
    };

    KeptLoad idle;
    Check(!idle.Active() && Tick(idle, true, true, true, true, 0) == Step::Wait, "nothing happens before Begin");

    KeptLoad smooth;
    smooth.Begin(t);
    Check(Tick(smooth, false, true, true, false, 0) == Step::Wait, "waits for the load to start");
    Check(Tick(smooth, true, false, true, false, 0) == Step::Wait, "loading");
    Check(!smooth.LoadOver(), "not over while loading");
    Check(Tick(smooth, false, false, true, false, 0) == Step::Wait, "no player yet");
    Check(Tick(smooth, false, true, true, false, 1) == Step::Wait, "placed");
    Check(smooth.LoadOver(), "over once placed");
    t += seconds(3);
    Check(Tick(smooth, false, true, true, true, 1) == Step::Rebuild, "bodies are built once the others are here");
    Check(Tick(smooth, false, true, true, true, 1) == Step::Wait, "not again at once");
    t += seconds(3);
    Check(Tick(smooth, false, true, true, true, 1) == Step::Rebuild, "again if it is still missing");
    Check(Tick(smooth, false, true, true, true, 0) == Step::Done, "done when everyone is back");
    Check(!smooth.Active(), "finished");

    KeptLoad alone;
    alone.Begin(t);
    Tick(alone, true, false, true, false, 0);
    Tick(alone, false, true, true, false, 1);
    t += seconds(3);
    Check(Tick(alone, false, true, true, false, 1) == Step::Wait, "waits for the others before building");
    t += seconds(20);
    Check(Tick(alone, false, true, true, false, 1) == Step::Rebuild, "builds after 20 s anyway");

    KeptLoad never;
    never.Begin(t);
    t += seconds(31);
    Check(Tick(never, false, true, true, false, 0) == Step::GiveUp, "the load never started");
    Check(!never.Active(), "off after giving up");

    KeptLoad slow;
    slow.Begin(t);
    t += seconds(200);
    Check(Tick(slow, true, false, true, false, 0) == Step::Wait, "a slow load is waited for");
    t += seconds(101);
    Check(Tick(slow, true, false, true, false, 0) == Step::GiveUp, "a stuck load is not");

    KeptLoad gone;
    gone.Begin(t);
    Tick(gone, true, false, true, false, 0);
    Check(Tick(gone, true, false, false, false, 0) == Step::GiveUp, "the session closed anyway: the old flow takes over");

    KeptLoad late;
    late.Begin(t);
    Tick(late, true, false, true, false, 0);
    Tick(late, false, true, true, false, 0);
    t += seconds(121);
    Check(Tick(late, false, true, true, false, 0) == Step::GiveUp, "the others never came");

    KeptLoad twice;
    twice.Begin(t);
    Tick(twice, true, false, true, false, 0);
    Tick(twice, false, true, true, false, 0);
    Check(twice.LoadOver(), "first load over");
    Tick(twice, true, false, true, false, 0);
    Check(!twice.LoadOver(), "a second load counts as loading");

    if (failures) return 1;
    std::printf("kept-travel-test: ok\n");
    return 0;
}
