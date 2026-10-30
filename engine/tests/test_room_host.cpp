// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "gpu/bbnet/room/room_crypto.h"
#include "gpu/bbnet/room/room_host.h"
#include "gpu/bbnet/room/room_link.h"
#include "gpu/bbnet/room/room_ws.h"

using namespace BbRoom;

namespace {

int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool Await(Link& link, Event::Kind kind, Event* out = nullptr, int timeout_ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(50);
        for (auto& e : link.Drain()) {
            if (e.kind == kind) {
                if (out) *out = std::move(e);
                return true;
            }
        }
    }
    return false;
}

bool AwaitData(Link& link, const std::vector<std::uint8_t>& wanted, int from) {
    Event e;
    return Await(link, Event::Kind::Data, &e) && e.data == wanted && e.slot == from;
}

} // namespace

int main() {
    const std::string name = "direct-room", password = "secret";
    const int port = 38765;
    const std::string server = "ws://127.0.0.1:" + std::to_string(port);
    LocalRoom room;
    std::string error;
    Check(room.Start(port, Derive(name, password).room_id, error), "the room opens its port");
    LocalRoom second;
    Check(!second.Start(port, Derive(name, password).room_id, error) && !error.empty(), "a port in use is refused with a reason");

    Link host, guest, other;
    host.SetHost(true);
    host.Start(server, name, password);
    Check(Await(host, Event::Kind::Connected), "the host is welcomed");
    Check(host.Slot() == 1, "the host gets slot 1");
    guest.Start(server, name, password);
    Check(Await(guest, Event::Kind::Connected), "a guest is welcomed");
    Check(guest.Slot() == 2, "the guest gets slot 2");
    Check(Await(host, Event::Kind::Joined), "the host sees the guest join");
    Check(guest.HostSlot() == 1, "the guest learns the host's slot");
    other.Start(server, name, password);
    Check(Await(other, Event::Kind::Connected) && other.Slot() == 3, "a third hunter is welcomed");
    Await(host, Event::Kind::Joined, nullptr, 500);
    Await(guest, Event::Kind::Joined, nullptr, 500);

    const std::vector<std::uint8_t> small{1, 2, 3};
    Check(guest.Send(host.Slot(), small.data(), small.size()), "a guest can send");
    Check(AwaitData(host, small, guest.Slot()), "a frame reaches the slot it names, from the sender's slot");
    Check(!Await(other, Event::Kind::Data, nullptr, 300), "a targeted frame reaches nobody else");
    Check(host.Send(0, small.data(), small.size()), "the host can send to all");
    Check(AwaitData(guest, small, host.Slot()) && AwaitData(other, small, host.Slot()), "a frame for all reaches everyone");
    Check(!Await(host, Event::Kind::Data, nullptr, 300), "a sender does not get its own frame");
    const std::vector<std::uint8_t> big(60000, 7);
    Check(guest.Send(host.Slot(), big.data(), big.size()), "a large frame can be sent");
    Check(AwaitData(host, big, guest.Slot()), "a large frame arrives whole");

    Check(guest.SendGame(host.Slot(), small.data(), small.size()), "game packets can be sent");
    Check(AwaitData(host, small, guest.Slot()), "a droppable frame still reaches the slot it names");
    Check(!Await(other, Event::Kind::Data, nullptr, 300), "and nobody else");

    const std::string member = std::string(32, 'a');
    const std::string own_url = RoomUrl(server, Derive(name, password).room_id) + "/" + member;
    std::atomic<int> first_slot{0}, next_slot{0};
    WebSocket original, replacement;
    Check(original.Connect(own_url, 5, error), "a member connects with its token");
    std::thread original_run([&] {
        original.Run([&](std::string&& message, bool binary) {
            if (!binary && message.find("welcome") != std::string::npos) first_slot = nlohmann::json::parse(message).value("slot", 0);
        });
    });
    Await(host, Event::Kind::Joined, nullptr, 1000);
    Await(guest, Event::Kind::Joined, nullptr, 200);
    Await(other, Event::Kind::Joined, nullptr, 200);
    Check(replacement.Connect(own_url, 5, error), "the same member connects again before the room saw it drop");
    std::thread replacement_run([&] {
        replacement.Run([&](std::string&& message, bool binary) {
            if (!binary && message.find("welcome") != std::string::npos) next_slot = nlohmann::json::parse(message).value("slot", 0);
        });
    });
    for (int i = 0; i < 100 && !next_slot.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(first_slot.load() && first_slot.load() == next_slot.load(), "it keeps its slot");
    Check(!Await(host, Event::Kind::Joined, nullptr, 300), "and the others see no new member");
    original_run.join();
    const std::string leave = R"({"t":"leave"})";
    replacement.Send(leave.data(), leave.size(), false);
    replacement.Close();
    replacement_run.join();
    Check(Await(host, Event::Kind::Left, nullptr, 2000), "a clean leave after the takeover frees the slot");
    Await(guest, Event::Kind::Left, nullptr, 500);
    Await(other, Event::Kind::Left, nullptr, 500);

    Link wrong;
    wrong.Start(server, name, "not the password");
    Check(!Await(wrong, Event::Kind::Connected, nullptr, 1500) && !wrong.Connected(), "a wrong password is never let in");
    wrong.Stop();

    Link voice_a, voice_b;
    voice_a.Start(server, name, password, "voice");
    voice_b.Start(server, name, password, "voice");
    Check(Await(voice_a, Event::Kind::Connected) && Await(voice_b, Event::Kind::Connected), "voice members are welcomed");
    Await(voice_a, Event::Kind::Joined, nullptr, 200);
    Check(voice_a.SendLossy(voice_b.Slot(), small.data(), small.size(), 2048), "voice can send");
    Check(AwaitData(voice_b, small, voice_a.Slot()), "voice reaches a voice member");
    Check(!Await(host, Event::Kind::Data, nullptr, 200), "voice stays out of the game room");
    voice_a.Stop();
    voice_b.Stop();

    Link fourth, fifth, sixth;
    fourth.Start(server, name, password);
    fifth.Start(server, name, password);
    Check(Await(fourth, Event::Kind::Connected) && Await(fifth, Event::Kind::Connected), "the room holds five");
    sixth.Start(server, name, password);
    Check(Await(sixth, Event::Kind::Refused), "the sixth hunter is refused");
    sixth.Stop();
    fourth.Stop();
    fifth.Stop();

    other.Stop();
    Check(Await(guest, Event::Kind::Left), "a clean leave reaches the others");
    guest.Stop();
    Check(Await(host, Event::Kind::Left), "a clean leave reaches the host");
    host.Stop();
    room.Stop();
    LocalRoom again;
    Check(again.Start(port, Derive(name, password).room_id, error), "the port opens again after the room stops");
    again.Stop();

    if (failures) return 1;
    std::printf("room-host-test: ok\n");
    return 0;
}
