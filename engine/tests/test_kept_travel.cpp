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

    Check(CanKeepMap(0x18010000u) && CanKeepMap(0x1c000000u), "a world map keeps the party");
    Check(!CanKeepMap(0x15000000u) && !CanKeepMap(0x1d000000u) && !CanKeepMap(0), "the Dream, dungeons and no map don't");
    for (const std::int32_t type : {1, 2, 12, 13}) Check(PhantomType(type), "a phantom type");
    for (const std::int32_t type : {-1, 0, 3, 8, 11, 14}) Check(!PhantomType(type), "not a phantom type");
    Check(WrongTypeForOwnWorld(4, true, 1, -1), "a saved phantom type loading its own world");
    Check(WrongTypeForOwnWorld(0, true, 0, 1), "a saved pending phantom type at the first load");
    Check(!WrongTypeForOwnWorld(0, true, 1, 0), "the game's own pending type already puts it right");
    Check(!WrongTypeForOwnWorld(3, true, 1, -1), "a guest loading the host's world stays a phantom");
    Check(!WrongTypeForOwnWorld(4, false, 1, -1), "a guest placed in the host's world stays a phantom");
    Check(!WrongTypeForOwnWorld(0, true, 0, -1), "a normal hunter is left alone");
    Check(!WrongTypeForOwnWorld(0, true, 8, -1), "a grey hunter is left alone");
    Check(OwnWorldType(1) == 0 && OwnWorldType(-1) == 0 && OwnWorldType(0) == 0, "a bad home type becomes a normal hunter");
    Check(OwnWorldType(8) == 8, "a grey home type is kept");

    Check(HoldsHit(0x1000, 2, 0x138, 0x1000), "the first hit of a block");
    Check(HoldsHit(0x1000, 2, 0x138, 0x1138), "the last hit of a block");
    Check(!HoldsHit(0x1000, 2, 0x138, 0x1270), "one past the block's hits");
    Check(!HoldsHit(0x1000, 2, 0x138, 0xec8), "before the block's hits");
    Check(!HoldsHit(0x1000, 2, 0x138, 0x1001), "inside a hit, not at its start");
    Check(!HoldsHit(0x1000, 0, 0x138, 0x1000), "a block with no hits");

    Check(LampText(2402950) == 70002400 && LampText(2412951) == 70002411 && LampText(3302951) == 70003301, "a lamp's text is its map's, plus its number");
    Check(LampText(2102959) == 70002109 && LampMapText(2102959) == 70002100, "a point with no text of its own falls back to its map");
    Check(LampText(2100950) == LampMapText(2100950), "a low point is its map");
    Check(StoryWarp(12405263) && StoryWarp(13200310) && StoryWarp(kSnatcherEvent), "scripted story warps are the host's own");
    Check(!StoryWarp(7200) && !StoryWarp(12107000) && !StoryWarp(0), "lamp events stay behind the host's own choice");
    Check(HoldForHost(true, false, false), "a guest waits for a host still loading");
    Check(!HoldForHost(true, true, false), "the host is here");
    Check(!HoldForHost(true, false, true) && !HoldForHost(false, false, false), "the wait ends at its limit or with the link");

    if (failures) return 1;
    std::printf("kept-travel-test: ok\n");
    return 0;
}
