// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "gpu/bbnet/room/room_announce.h"
#include "gpu/bbnet/room/room_service.h"

using namespace BbRoom;
using Clock = std::chrono::steady_clock;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void TestClean() {
    Check(CleanAnnouncement("  a\n\tb\x01" "c  ") == "a b c", "control characters become single spaces");
    Check(CleanAnnouncement("\xE2\x80\xAE" "evil") == "evil", "text direction overrides are removed");
    Check(CleanAnnouncement("\xFF\xFEok\xC0") == "ok", "broken UTF-8 is removed");
    Check(CleanAnnouncement("Привет") == "Привет", "Cyrillic stays");
    Check(CleanAnnouncement(std::string(500, 'x')).size() == kLongestAnnouncement, "long text is cut");
    std::string cyrillic;
    for (int i = 0; i < 300; ++i) cyrillic += "я";
    Check(CleanAnnouncement(cyrillic).size() == kLongestAnnouncement * 2, "the limit counts characters, not bytes");
    Check(CleanAnnouncement("%s%n%d") == "%s%n%d", "format text stays plain text");
    Check(CleanAnnouncement(" \n ").empty(), "blank text is empty");
}

void TestServers() {
    Check(AnnouncementServer("https://185-244-28-175.sslip.io"), "the relay is trusted");
    Check(AnnouncementServer("https://185-244-28-175.sslip.io/"), "with a slash too");
    Check(AnnouncementServer("wss://185-244-28-175.sslip.io"), "and as wss");
    Check(AnnouncementServer("185-244-28-175.sslip.io"), "and bare, which connects with TLS");
    Check(AnnouncementServer("http://127.0.0.1:8787"), "a local test relay is trusted");
    Check(!AnnouncementServer("ws://127.0.0.1:38765"), "a direct room's own host is not");
    Check(!AnnouncementServer("ws://203.0.113.5:38765"), "a direct room is not");
    Check(!AnnouncementServer("https://evil.example/185-244-28-175.sslip.io"), "a look-alike path is not");
    Check(!AnnouncementServer("https://185-244-28-175.sslip.io.evil.example"), "a look-alike host is not");
    Check(!AnnouncementServer("http://185-244-28-175.sslip.io"), "the relay without TLS is not");
    Check(!AnnouncementServer("http://127.0.0.1.evil.example:80"), "a look-alike local address is not");
}

void TestQueue() {
    AnnouncementQueue queue;
    auto now = Clock::now();
    std::string text;
    float alpha = 0;
    Check(!queue.Current(now, text, alpha), "nothing to show at first");
    Check(queue.Offer(1, "first", now), "a new id is taken");
    Check(!queue.Offer(1, "first", now), "the same id is not shown twice");
    Check(queue.Current(now, text, alpha) && text == "first" && alpha == 0.0f, "it starts faded out");
    now += std::chrono::seconds(1);
    Check(queue.Current(now, text, alpha) && alpha == 1.0f, "then fades in");
    Check(queue.Offer(2, "second", now), "a second one waits");
    now += std::chrono::seconds(8);
    Check(queue.Current(now, text, alpha) && text == "second", "the next one follows");
    Check(!queue.Offer(3, " \n", now), "a blank one is refused");
    int taken = 0;
    for (std::uint64_t id = 10; id < 30; ++id) taken += queue.Offer(id, "flood", now);
    Check(taken <= 3, "a flood is held back");
    now += std::chrono::minutes(5);
    while (queue.Current(now, text, alpha)) now += std::chrono::seconds(20);
    Check(queue.Offer(100, "after", now), "it works again later");
}

int Live(const std::string& expected, int seconds) {
    Start(47900);
    std::string text;
    float alpha = 0;
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        if (CurrentAnnouncement(text, alpha)) {
            std::printf("shown: %s\n", text.c_str());
            std::fflush(stdout);
            return !expected.empty() && text == expected ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("nothing shown\n");
    std::fflush(stdout);
    return expected.empty() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "live") {
        const int result = Live(argc >= 3 ? argv[2] : "", argc >= 4 ? std::atoi(argv[3]) : 20);
        std::fflush(stdout);
        std::_Exit(result);
    }
    TestClean();
    TestServers();
    TestQueue();
    if (failures) return 1;
    std::printf("room-announce-test: ok\n");
    return 0;
}
