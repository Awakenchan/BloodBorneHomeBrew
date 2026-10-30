// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gpu/bbnet/room/room_link.h"

using namespace BbRoom;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint8_t kGame = 200;
constexpr std::uint8_t kBulk = 201;
constexpr std::size_t kHeader = 1 + 8 + 4;

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::string Arg(int argc, char** argv, const char* name, const char* fallback) {
    const std::string key = std::string("--") + name + "=";
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], key.c_str(), key.size()) == 0) return argv[i] + key.size();
    }
    return fallback;
}

double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    const auto at = static_cast<std::size_t>(p * double(sorted.size() - 1));
    return sorted[at];
}

} // namespace

int main(int argc, char** argv) {
    const std::string server = Arg(argc, argv, "server", "");
    const std::string room = Arg(argc, argv, "room", "bench");
    const std::string label = Arg(argc, argv, "label", "member");
    const int expect = std::atoi(Arg(argc, argv, "expect", "2").c_str());
    const int seconds = std::atoi(Arg(argc, argv, "seconds", "30").c_str());
    const int rate = std::atoi(Arg(argc, argv, "rate", "30").c_str());
    const int size = std::max<int>(kHeader, std::atoi(Arg(argc, argv, "size", "300").c_str()));
    const int peers_each = std::atoi(Arg(argc, argv, "bundle", "0").c_str());
    const int bulk_bytes = std::atoi(Arg(argc, argv, "bulk", "0").c_str());
    const int bulk_every = std::atoi(Arg(argc, argv, "bulk-every", "20").c_str());
    const int settle_ms = std::atoi(Arg(argc, argv, "settle", "500").c_str());

    const auto started = Clock::now();
    Link link;
    if (!link.Start(server, room, "bench")) {
        std::printf("{\"label\":\"%s\",\"error\":\"no start\"}\n", label.c_str());
        return 1;
    }

    std::vector<double> latency;
    std::map<int, std::uint32_t> highest;
    std::map<int, long long> received;
    long long bytes_in = 0, bulk_in = 0, disconnects = 0, refused = 0, reordered = 0;
    double connect_ms = -1;
    std::set<int> slots;
    std::vector<double> gaps;
    std::int64_t last_arrival = 0;
    const auto drain = [&] {
        for (auto& e : link.Drain()) {
            switch (e.kind) {
            case Event::Kind::Connected:
                if (connect_ms < 0) connect_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                slots.insert(e.slot);
                break;
            case Event::Kind::Disconnected:
                ++disconnects;
                break;
            case Event::Kind::Refused:
                ++refused;
                break;
            case Event::Kind::Data: {
                bytes_in += static_cast<long long>(e.data.size());
                if (e.data.empty()) break;
                if (e.data[0] == kBulk) ++bulk_in;
                if (e.data[0] != kGame || e.data.size() < kHeader) break;
                std::int64_t sent = 0;
                std::uint32_t seq = 0;
                std::memcpy(&sent, e.data.data() + 1, 8);
                std::memcpy(&seq, e.data.data() + 9, 4);
                const std::int64_t now = NowNs();
                latency.push_back(double(now - sent) / 1e6);
                if (last_arrival) gaps.push_back(double(now - last_arrival) / 1e6);
                last_arrival = now;
                auto& top = highest[e.slot];
                if (seq < top) ++reordered;
                top = std::max(top, seq);
                ++received[e.slot];
                break;
            }
            default:
                break;
            }
        }
    };

    const auto ready_by = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < ready_by && static_cast<int>(link.Members().size()) + 1 < expect) {
        link.WaitEvent(50);
        drain();
    }
    const auto ready_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto settle_until = Clock::now() + std::chrono::milliseconds(settle_ms);
    while (Clock::now() < settle_until) {
        link.WaitEvent(20);
        drain();
    }
    latency.clear();
    gaps.clear();
    received.clear();
    last_arrival = 0;

    std::uint32_t seq = 0;
    long long sent = 0, dropped = 0;
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(size), 0x5a);
    frame[0] = kGame;
    const auto tick = std::chrono::nanoseconds(1000000000LL / std::max(rate, 1));
    const auto begin = Clock::now();
    auto next = begin;
    auto next_bulk = begin + std::chrono::seconds(2);
    const auto end = begin + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        const auto now = Clock::now();
        if (now >= next) {
            const std::int64_t stamp = NowNs();
            ++seq;
            std::memcpy(frame.data() + 1, &stamp, 8);
            std::memcpy(frame.data() + 9, &seq, 4);
            bool ok = false;
#ifdef ROOM_BENCH_GAME_SEND
            if (peers_each) {
                for (const int member : link.Members()) ok |= link.SendGame(member, frame.data(), frame.size());
            } else {
                ok = link.SendGame(0, frame.data(), frame.size());
            }
#else
            if (peers_each) {
                for (const int member : link.Members()) ok |= link.Send(member, frame.data(), frame.size());
            } else {
                ok = link.Send(0, frame.data(), frame.size());
            }
#endif
            ok ? ++sent : ++dropped;
            next += tick;
        }
        if (bulk_bytes && now >= next_bulk) {
            std::vector<std::uint8_t> bulk(static_cast<std::size_t>(bulk_bytes), 0x11);
            bulk[0] = kBulk;
#ifdef ROOM_BENCH_GAME_SEND
            for (std::size_t at = 0; at < bulk.size(); at += 1204) {
                const std::size_t part = std::min<std::size_t>(1204, bulk.size() - at);
                link.SendPaced(0, bulk.data() + at, part);
            }
#else
            link.Send(0, bulk.data(), bulk.size());
#endif
            next_bulk += std::chrono::seconds(bulk_every);
        }
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto drain_until = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < drain_until) {
        link.WaitEvent(50);
        drain();
    }

    std::sort(latency.begin(), latency.end());
    double largest_gap = 0;
    for (const double g : gaps) largest_gap = std::max(largest_gap, g);
    long long over100 = 0, over250 = 0, over1000 = 0;
    for (const double l : latency) {
        over100 += l > 100;
        over250 += l > 250;
        over1000 += l > 1000;
    }
    long long got = 0;
    for (const auto& [slot, n] : received) got += n;
    const int final_slot = link.Slot();
    const auto members = link.Members().size();
    link.Stop();
    std::printf(
        "{\"label\":\"%s\",\"connect_ms\":%.0f,\"ready_ms\":%.0f,\"sent\":%lld,\"send_refused\":%lld,\"received\":%lld,"
        "\"p50\":%.1f,\"p95\":%.1f,\"p99\":%.1f,\"max\":%.1f,\"over100\":%lld,\"over250\":%lld,\"over1000\":%lld,"
        "\"largest_gap\":%.0f,\"bytes_in\":%lld,\"bulk_in\":%lld,\"disconnects\":%lld,\"refused\":%lld,\"reordered\":%lld,"
        "\"slots_seen\":%zu,\"slot\":%d,\"members\":%zu}\n",
        label.c_str(), connect_ms, ready_ms, sent, dropped, got, Percentile(latency, 0.5), Percentile(latency, 0.95),
        Percentile(latency, 0.99), latency.empty() ? 0.0 : latency.back(), over100, over250, over1000, largest_gap, bytes_in,
        bulk_in, disconnects, refused, reordered, slots.size(), final_slot, members);
    return 0;
}
