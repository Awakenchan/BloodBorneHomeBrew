// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <httplib.h>

#include "gpu/bbnet/room/room_service.h"

using Json = nlohmann::json;

namespace {

int Fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    std::fflush(stdout);
    return 1;
}

bool Poll(Json& ev, const char* name, int seconds = 20) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        Json reply;
        std::string error;
        BbRoom::Call("/np/events/poll", {{"WaitMs", 1000}, {"Cursor", 0}}, reply, error, 3000);
        if (reply.value("HasEvent", 0) && reply.value("Name", "") == name) {
            ev = reply;
            return true;
        }
    }
    return false;
}

Json Game(int http_port, const std::string& path, const Json& body) {
    httplib::Client cli("127.0.0.1", http_port);
    cli.set_read_timeout(std::chrono::seconds(15));
    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res || res->status != 200) return Json();
    return Json::parse(res->body, nullptr, false);
}

int OpenUdp(int port) {
    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    DWORD timeout = 15000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{15, 0};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return static_cast<int>(s);
}

std::string Sign() {
    return std::string(299, 'A') + "=";
}

int Host(int game_port, int http_port) {
    const int udp = OpenUdp(game_port);
    BbRoom::Start(game_port);
    Json reply;
    std::string error;
    if (!BbRoom::Call("/mp/matching2/context_start", {{"OnlineId", "HostHunter"}}, reply, error, 8000)) return Fail("host context_start");
    if (!BbRoom::Call("/mp/matching2/create_room", {{"OnlineId", "HostHunter"}, {"MaxMembers", 4}}, reply, error, 8000) ||
        reply.value("ResKind", -1) != 0)
        return Fail("create_room");
    const long long room = reply.value("RoomId", 0LL);
    std::printf("host: room %lld\n", room);
    Json list;
    for (int i = 0; i < 60; ++i) {
        list = Game(http_port, "/summon_messenger/get", {{"SummonTypeList", Json::array({{{"SummonType", 7}}})}});
        if (list.is_object() && list["SummonDataList"].size() == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!list.is_object() || list["SummonDataList"].size() != 1) return Fail("the guest's sign never showed up");
    const long long user = list["SummonDataList"][0].value("UserId", 0LL);
    std::printf("host: sign from user %lld\n", user);
    Game(http_port, "/summon_messenger/request", {{"TargetUserId", user}});
    Json ev;
    if (!Poll(ev, "room_member_joined")) return Fail("host saw no member join");
    std::printf("host: member %lld joined at %s:%lld\n", ev.value("MemberId", 0LL), ev.value("Addr", "").c_str(),
                ev.value("Port", 0LL));
    char buffer[64];
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const int n = ::recvfrom(udp, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n != 5 || std::string(buffer, 5) != "hello") return Fail("no P2P datagram from the guest");
    if (ntohs(from.sin_port) != ev.value("Port", 0LL)) return Fail("datagram came from another port than the member's");
    ::sendto(udp, "back!", 5, 0, reinterpret_cast<sockaddr*>(&from), len);
    std::printf("host: P2P ok\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::printf("HOST OK\n");
    return 0;
}

int Guest(int game_port, int http_port) {
    const int udp = OpenUdp(game_port);
    BbRoom::Start(game_port);
    Json reply;
    std::string error;
    if (!BbRoom::Call("/mp/matching2/context_start", {{"OnlineId", "GuestHunter"}}, reply, error, 15000)) return Fail("guest context_start");
    const Json login = Game(http_port, "/basic_utils/login", Json::object());
    if (!login.is_object() || login.value("ResKind", -1) != 0) return Fail("login");
    const Json created = Game(http_port, "/summon_messenger/create", {{"SummonData", Sign()}, {"SummonType", 7}, {"CharaId", 2}});
    if (!created.is_object() || created.value("ResKind", -1) != 0) return Fail("sign create");
    Json ev;
    if (!Poll(ev, "guest_invite")) return Fail("no invite");
    const long long room = ev.value("RoomId", 0LL);
    std::printf("guest: invited to room %lld by %s at %s:%lld\n", room, ev.value("HostOnlineId", "").c_str(),
                ev.value("HostAddr", "").c_str(), ev.value("HostPort", 0LL));
    if (!BbRoom::Call("/mp/matching2/join_room", {{"OnlineId", "GuestHunter"}, {"RoomId", room}}, reply, error, 8000) ||
        reply.value("ResKind", -1) != 0)
        return Fail("join_room");
    std::printf("guest: member %lld, %zu members\n", reply.value("MemberId", 0LL), reply["Members"].size());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(static_cast<std::uint16_t>(ev.value("HostPort", 0LL)));
    char buffer[64];
    for (int i = 0; i < 5; ++i) {
        ::sendto(udp, "hello", 5, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const int n = ::recvfrom(udp, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n == 5 && std::string(buffer, 5) == "back!") {
            std::printf("GUEST OK\n");
            return 0;
        }
    }
    return Fail("no P2P answer from the host");
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) return Fail("usage: room-service-test host|guest");
    const bool host = std::string(argv[1]) == "host";
    const int game_port = host ? 47658 : 48658;
    const int http_port = game_port + 1;
    const int result = host ? Host(game_port, http_port) : Guest(game_port, http_port);
    std::fflush(stdout);
    std::_Exit(result);
}
