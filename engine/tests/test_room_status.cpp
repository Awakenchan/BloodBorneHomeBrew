// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>

#include "gpu/bbnet/room/room_status.h"

using namespace BbRoom;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool HasLine(PartyStatus& status, Clock::time_point now, PartyLineKind kind, const char* name = "") {
    for (const PartyLine& line : status.Lines(now))
        if (line.kind == kind && line.name == name) return true;
    return false;
}

void TestLampAsk() {
    PartyStatus status;
    auto now = Clock::now();
    status.Named(2, "Hunter", now);
    status.Named(3, "Other", now);
    int slot = 0;
    std::string who;
    std::uint32_t lamp = 0;
    Check(!status.LampAskNow(now, slot, who, lamp), "no lamp ask to begin with");
    Check(status.LampAsk(2, 2412950, now), "a guest's lamp ask is taken");
    Check(HasLine(status, now, PartyLineKind::LampAsked, "Hunter"), "the host is told who asks");
    Check(status.LampAskNow(now, slot, who, lamp) && slot == 2 && who == "Hunter" && lamp == 2412950, "the ask waits for the host");
    Check(!status.LampAsk(3, 2402950, now), "a second guest can't push the first one's ask out");
    Check(status.LampAskNow(now, slot, who, lamp) && slot == 2 && lamp == 2412950, "the first ask stays");
    Check(!status.LampAskNow(now + kLampAskLasts, slot, who, lamp), "an unanswered ask runs out, nobody moves");
    Check(status.LampAsk(3, 2402950, now + kLampAskLasts), "after that another guest can ask");
    status.Left(3, now + kLampAskLasts);
    Check(!status.LampAskNow(now + kLampAskLasts, slot, who, lamp), "the ask goes with the guest who leaves");
    Check(status.LampAsk(2, 2412950, now), "ask again");
    status.LampAskDone();
    Check(!status.LampAskNow(now, slot, who, lamp), "an answered ask is gone");

    PartyStatus guest;
    guest.GuestLampAsked(now);
    Check(guest.GuestLampWaiting(now), "the guest waits");
    guest.Tick(now + kLampAnswerWithin);
    Check(!guest.GuestLampWaiting(now + kLampAnswerWithin) && HasLine(guest, now + kLampAnswerWithin, PartyLineKind::LampNoAnswer),
          "an older host never answers: the guest stays and is told");
    guest.GuestLampAsked(now);
    guest.GuestLampAnswered(kLampGot, now);
    guest.Tick(now + kLampAnswerWithin);
    Check(guest.GuestLampWaiting(now + kLampAnswerWithin), "a host that has the ask gets its time");
    guest.GuestLampAnswered(kLampRefused, now + seconds(6));
    Check(!guest.GuestLampWaiting(now + seconds(6)) && HasLine(guest, now + seconds(6), PartyLineKind::LampRefused), "refused: the guest stays");
    PartyStatus quiet;
    quiet.GuestLampAsked(now);
    quiet.GuestLampAnswered(kLampGot, now);
    quiet.Tick(now + kLampAskLasts);
    Check(HasLine(quiet, now + kLampAskLasts, PartyLineKind::LampNoAnswer), "the host sat on it: the guest stays and is told");
    PartyStatus yes;
    yes.GuestLampAsked(now);
    yes.GuestLampAnswered(kLampAgreed, now);
    Check(!yes.GuestLampWaiting(now) && HasLine(yes, now, PartyLineKind::LampAgreed), "accepted");
}

void TestPing() {
    PartyStatus status;
    auto now = Clock::now();
    status.OwnConnected(now);
    status.Named(2, "Hunter", now);
    status.Heard(2, now);
    Check(status.List(now)[0].state == LinkState::Unknown, "no ping yet means unknown");
    status.Pinged(2);
    status.Rtt(2, 40);
    Check(status.List(now)[0].state == LinkState::Good && status.List(now)[0].ping_ms == 40, "a quick ping is good");
    for (int i = 0; i < 12; ++i) status.Rtt(2, 600);
    Check(status.List(now)[0].state == LinkState::Slow, "a long ping is slow");
    for (int i = 0; i < 24; ++i) status.Rtt(2, 40);
    Check(status.List(now)[0].state == LinkState::Good, "it recovers");
    status.Rtt(2, -5);
    status.Rtt(2, 99999);
    Check(status.List(now)[0].ping_ms < 100, "impossible round trips are ignored");
}

void TestSilence() {
    PartyStatus status;
    auto now = Clock::now();
    status.OwnConnected(now);
    now += seconds(20);
    status.Named(2, "Hunter", now);
    Check(HasLine(status, now, PartyLineKind::EnteredRoom, "Hunter"), "a new name enters the room");
    status.Heard(2, now);
    status.Pinged(2);
    ReconnectNotice notice;
    now += kSilentAfter - seconds(1);
    status.Tick(now);
    Check(!status.Reconnecting(now, notice), "a short gap is not a dropout");
    now += seconds(2);
    status.Tick(now);
    Check(status.Reconnecting(now, notice) && !notice.own && notice.names.size() == 1 && notice.names[0] == "Hunter", "silence shows the name");
    Check(status.List(now)[0].state == LinkState::Reconnecting, "the row says reconnecting");
    status.Heard(2, now);
    Check(!status.Reconnecting(now, notice), "any frame ends it");
    Check(HasLine(status, now, PartyLineKind::Reconnected, "Hunter"), "and says so");
}

void TestOlderClient() {
    PartyStatus status;
    auto now = Clock::now();
    status.OwnConnected(now);
    status.Named(3, "Old", now);
    status.Heard(3, now);
    now += seconds(300);
    status.Tick(now);
    ReconnectNotice notice;
    Check(!status.Reconnecting(now, notice), "a client without pings is never called reconnecting");
    Check(status.List(now)[0].state == LinkState::Unknown && status.List(now)[0].ping_ms == -1, "it stays unknown in the list");
}

void TestOwnLink() {
    PartyStatus status;
    auto now = Clock::now();
    ReconnectNotice notice;
    Check(!status.Reconnecting(now, notice), "nothing before the first connect");
    status.OwnConnected(now);
    status.Named(2, "Hunter", now);
    Check(!HasLine(status, now, PartyLineKind::EnteredRoom, "Hunter"), "people already in the room are not announced");
    status.Heard(2, now);
    status.Pinged(2);
    now += seconds(1);
    status.OwnLost(now);
    now += seconds(30);
    status.Tick(now);
    Check(status.Reconnecting(now, notice) && notice.own && notice.seconds == 30, "own dropout counts seconds");
    Check(status.List(now)[0].state == LinkState::Reconnecting, "nobody can be reached meanwhile");
    status.OwnConnected(now);
    Check(!status.Reconnecting(now, notice), "own notice ends");
    Check(HasLine(status, now, PartyLineKind::BackOnline), "back online line");
    Check(!HasLine(status, now, PartyLineKind::Reconnected, "Hunter"), "others are not blamed for own dropout");
    now += seconds(1);
    status.Tick(now);
    Check(status.List(now)[0].state != LinkState::Reconnecting, "others get a fresh start");
    status.OwnLost(now);
    status.OwnGaveUp(now);
    Check(status.Reconnecting(now, notice) && notice.gave_up, "giving up is shown");
    Check(!status.Reconnecting(now + kGaveUpShown, notice), "for a while");
}

void TestDeath() {
    PartyStatus status;
    auto now = Clock::now();
    status.OwnConnected(now);
    status.Named(2, "Hunter", now);
    status.Vitals(2, 0, 800, 7, now);
    Check(!HasLine(status, now, PartyLineKind::Died, "Hunter"), "first vitals at zero are not a death");
    status.Vitals(2, 500, 800, 7, now);
    status.Vitals(2, 0, 0, 7, now);
    Check(!HasLine(status, now, PartyLineKind::Died, "Hunter"), "a body not built yet is not a death");
    status.Vitals(2, 500, 800, 7, now);
    status.Vitals(2, 0, 800, 8, now);
    Check(!HasLine(status, now, PartyLineKind::Died, "Hunter"), "a map change is not a death");
    status.Vitals(2, 500, 800, 8, now);
    status.Vitals(2, 0, 800, 8, now);
    Check(HasLine(status, now, PartyLineKind::Died, "Hunter"), "hp reaching zero on the same map is");
}

void TestLines() {
    PartyStatus status;
    auto now = Clock::now();
    status.AddLine(PartyLineKind::Left, "A", now);
    status.AddLine(PartyLineKind::Left, "A", now);
    Check(status.Lines(now).size() == 1, "the same line twice shows once");
    Check(status.Lines(now)[0].alpha == 1.0f, "full strength at first");
    Check(status.Lines(now + kLineLasts - milliseconds(300))[0].alpha < 1.0f, "fades at the end");
    for (const char* name : {"B", "C", "D", "E", "F"}) status.AddLine(PartyLineKind::Died, name, now + kLineLasts);
    Check(status.Lines(now + kLineLasts).size() == kMostLines, "only a few at a time");
    Check(status.Lines(now + kLineLasts * 2).empty(), "they go away");
    status.Named(1, "A", now);
    status.Left(1, now + kLineLasts * 3);
    Check(HasLine(status, now + kLineLasts * 3, PartyLineKind::Left, "A"), "leaving the link names the hunter");
    status.Left(9, now);
    Check(status.Lines(now + kLineLasts * 3).size() == 1, "an unknown slot says nothing");
}

void TestAsks() {
    PartyStatus status;
    auto now = Clock::now();
    status.Named(2, "Guest", now);
    Check(status.Ask(2, now), "the first ask is new");
    Check(HasLine(status, now, PartyLineKind::WarpAsked, "Guest"), "the host sees it");
    Check(status.List(now)[0].asked, "and in the row");
    Check(!status.Ask(2, now + seconds(1)), "asking again is not new");
    status.ClearAsk(2);
    Check(!status.List(now)[0].asked, "bring clears it");
    status.Ask(2, now);
    Check(!status.List(now + kWarpAskLasts)[0].asked, "it runs out");
    status.Ask(2, now + kWarpAskLasts);
    status.ClearAsk(0);
    Check(!status.List(now + kWarpAskLasts)[0].asked, "bring all clears all");
    status.Ask(2, now);
    status.Left(2, now);
    Check(status.List(now).empty(), "gone with the hunter");
}

void TestGuestSide() {
    PartyStatus status;
    auto now = Clock::now();
    Check(!status.GuestWaiting(now), "not waiting at first");
    status.GuestAnswered(kWarpGot, now);
    Check(!HasLine(status, now, PartyLineKind::WarpSeen), "an answer nobody asked for is dropped");
    status.GuestAsked(now);
    Check(status.GuestWaiting(now), "waiting after asking");
    status.Tick(now + kWarpAnswerWithin);
    Check(!status.GuestWaiting(now + kWarpAnswerWithin), "no answer ends the wait");
    Check(HasLine(status, now + kWarpAnswerWithin, PartyLineKind::WarpNoAnswer), "an older host is explained");
    status.GuestAsked(now);
    status.GuestAnswered(kWarpGot, now);
    status.Tick(now + kWarpAnswerWithin);
    Check(status.GuestWaiting(now + kWarpAnswerWithin), "an answered ask keeps waiting for the host");
    Check(!status.GuestWaiting(now + kWarpAskLasts), "but not forever");
    status.GuestAsked(now);
    status.GuestBrought();
    Check(!status.GuestWaiting(now), "being brought ends the wait");
    status.GuestAsked(now);
    status.GuestAnswered(kWarpRefused, now);
    Check(!status.GuestWaiting(now), "a refusal ends the wait");
    Check(HasLine(status, now, PartyLineKind::WarpOff), "and says so");
}

} // namespace

int main() {
    TestLampAsk();
    TestPing();
    TestSilence();
    TestOlderClient();
    TestOwnLink();
    TestDeath();
    TestLines();
    TestAsks();
    TestGuestSide();
    if (failures) return 1;
    std::printf("room status ok\n");
    return 0;
}
