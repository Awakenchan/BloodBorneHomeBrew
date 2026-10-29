// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/shared_progress.h"
#include <chrono>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

struct FakeClock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<FakeClock, duration>;
};
}

int main() {
    using namespace BbEngine::SharedProgress;
    using std::chrono::seconds;
    Ledger ledger;
    Presence<FakeClock> presence(seconds(10));
    FakeClock::time_point t{};

    std::set<std::uint32_t> host_save;
    for (std::uint32_t id = 0; id < 383; ++id) host_save.insert(12100000 + id);
    Check(ledger.For("staytorn").empty(), "the host's 383 flags from before the session reach nobody");

    presence.Seen("j3loo", t);
    ledger.Record(9401, presence.Here(t));
    Check(ledger.For("j3loo") == std::vector<std::uint32_t>{9401}, "a boss killed with j3loo there goes to j3loo");
    Check(ledger.For("staytorn").empty(), "a guest who had not joined yet does not get it");

    t += seconds(30);
    presence.Seen("staytorn", t);
    ledger.Record(9402, presence.Here(t));
    Check(ledger.For("staytorn") == std::vector<std::uint32_t>{9402}, "staytorn gets only what happened after joining");
    Check(ledger.For("j3loo") == std::vector<std::uint32_t>{9401}, "j3loo left more than 10 s ago and misses it");

    for (const std::uint32_t id : ledger.For("staytorn")) Check(!host_save.count(id), "no pre-join flag in staytorn's share");

    t += seconds(5);
    presence.Seen("j3loo", t);
    presence.Seen("staytorn", t);
    ledger.Record(9403, presence.Here(t));
    Check(ledger.For("j3loo") == (std::vector<std::uint32_t>{9401, 9403}), "a rejoin keeps the earlier share and adds the new one");

    presence.Seen("staytorn", t);
    ledger.Record(1000100, presence.Here(t));
    ledger.Record(1000100 | kClear, presence.Here(t));
    Check(ledger.For("staytorn").back() == (1000100 | kClear), "an NPC step's latest value wins");

    t += seconds(11);
    Check(presence.Here(t).empty(), "a hunter not seen for 10 s is no longer present");
    ledger.Record(9404, presence.Here(t));
    Check(ledger.For("staytorn").size() == 3 && ledger.For("j3loo").size() == 3, "an event with nobody there is kept from everyone");

    if (failures) return 1;
    std::printf("shared-progress-test: ok\n");
    return 0;
}
