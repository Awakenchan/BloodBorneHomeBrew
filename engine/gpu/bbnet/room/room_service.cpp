// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_service.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
inline void CloseSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
inline void CloseSocket(SocketHandle s) { close(s); }
#endif

#include <httplib.h>
#include <openssl/evp.h>

#include "room_link.h"

namespace BbRoom {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

enum : std::uint8_t { kRequest = 1, kReply = 2, kEvent = 3, kDatagram = 4, kBatch = 5, kVitals = 6, kTravel = 7 };
constexpr std::size_t kVitalsFixed = 1 + 1 + 4 * 4 + 2 + 3 * 4;
constexpr std::size_t kBatchBytes = 16000;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("Seamless: %s\n", text);
    std::fflush(stdout);
}

long long IntOf(const Json& o, const char* key, long long dflt = 0) {
    if (!o.is_object()) return dflt;
    const auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number()) return static_cast<long long>(it->get<double>());
    if (it->is_string()) return std::strtoll(it->get<std::string>().c_str(), nullptr, 10);
    return dflt;
}

std::string StrOf(const Json& o, const char* key) {
    if (!o.is_object()) return "";
    const auto it = o.find(key);
    if (it == o.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}

struct Member {
    int member_id = 0;
    int slot = 0;
};

struct MatchRoom {
    std::string session_id;
    int max_members = 4;
    int next_member = 2;
    std::vector<Member> members;
};

struct Sign {
    std::string data;
    long long type = 0;
    long long chara = 0;
};

struct State {
    Link link;
    std::atomic<int> game_port{0};
    int http_port = 0;
    bool host = false;
    std::atomic<int> own_slot{0};
    std::string player;
    std::atomic<bool> started{false};

    std::mutex mu;
    std::condition_variable cv;
    std::uint32_t next_call = 1;
    std::map<std::uint32_t, Json> replies;
    std::deque<Json> events;
    long long next_event = 1;

    std::mutex host_mu;
    std::map<int, std::string> names;
    std::map<std::uint64_t, MatchRoom> rooms;
    std::uint64_t next_room = 1;
    std::map<int, Sign> signs;

    std::mutex bridge_mu;
    std::map<int, SocketHandle> bridges;
    std::map<int, int> bridge_ports;

    std::mutex vitals_mu;
    std::map<int, std::pair<Vitals, Clock::time_point>> vitals;
    std::uint32_t travel = 0;
    Clock::time_point travel_at;
};

State& S() {
    static State s;
    return s;
}

std::vector<std::uint8_t> Pack(std::uint8_t kind, const std::string& text, std::uint32_t id = 0, bool with_id = false) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() + 5);
    out.push_back(kind);
    if (with_id) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    }
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

void SendJson(int slot, std::uint8_t kind, const Json& value, std::uint32_t id = 0, bool with_id = false) {
    const auto frame = Pack(kind, value.dump(), id, with_id);
    S().link.Send(slot, frame.data(), frame.size());
}

int BridgePort(int slot) {
    std::lock_guard lock(S().bridge_mu);
    const auto it = S().bridge_ports.find(slot);
    return it == S().bridge_ports.end() ? 0 : it->second;
}

void Localize(Json& value) {
    if (value.is_array()) {
        for (auto& v : value) Localize(v);
        return;
    }
    if (!value.is_object()) return;
    const int own = S().own_slot.load();
    if (value.contains("Slot")) {
        const int slot = static_cast<int>(IntOf(value, "Slot"));
        const int port = slot == own ? S().game_port.load() : BridgePort(slot);
        value["Addr"] = "127.0.0.1";
        value["Port"] = port;
        value["LocalAddr"] = "127.0.0.1";
        value["LocalPort"] = port;
        value["MappedAddr"] = "";
        value["MappedPort"] = 0;
    }
    if (value.contains("HostSlot")) {
        const int slot = static_cast<int>(IntOf(value, "HostSlot"));
        const int port = slot == own ? S().game_port.load() : BridgePort(slot);
        value["HostAddr"] = "127.0.0.1";
        value["HostPort"] = port;
        value["HostLocalAddr"] = "127.0.0.1";
        value["HostLocalPort"] = port;
        value["HostMappedAddr"] = "";
        value["HostMappedPort"] = 0;
    }
    if (value.contains("Members")) Localize(value["Members"]);
}

void Deliver(int slot, Json event) {
    if (slot == S().own_slot.load()) {
        Localize(event);
        {
            std::lock_guard lock(S().mu);
            event["EventId"] = S().next_event++;
            S().events.push_back(std::move(event));
        }
        S().cv.notify_all();
        return;
    }
    SendJson(slot, kEvent, event);
}

Json Record(const MatchRoom& room, const Member& m, std::uint64_t room_id) {
    Json r = {{"MemberId", m.member_id}, {"Slot", m.slot}, {"RoomId", static_cast<long long>(room_id)}};
    const auto it = S().names.find(m.slot);
    r["OnlineId"] = it == S().names.end() ? "" : it->second;
    (void)room;
    return r;
}

void LeaveLocked(std::uint64_t room_id, int slot, const char* reason) {
    const auto it = S().rooms.find(room_id);
    if (it == S().rooms.end()) return;
    MatchRoom& room = it->second;
    const auto m = std::find_if(room.members.begin(), room.members.end(), [&](const Member& x) { return x.slot == slot; });
    if (m == room.members.end()) return;
    const Member gone = *m;
    room.members.erase(m);
    if (gone.member_id == 1) {
        for (const Member& other : room.members) {
            Deliver(other.slot, {{"Name", "room_closed"}, {"RoomId", static_cast<long long>(room_id)}, {"Reason", "host_left"}});
        }
        S().rooms.erase(it);
        return;
    }
    Json ev = Record(room, gone, room_id);
    ev["Name"] = "room_member_left";
    ev["Reason"] = reason;
    for (const Member& other : room.members) Deliver(other.slot, ev);
}

Json HandleSession(int slot, const std::string& path, const Json& body) {
    std::lock_guard lock(S().host_mu);
    if (path == "/mp/matching2/context_start") {
        const std::string name = StrOf(body, "OnlineId");
        if (!name.empty() && S().names[slot] != name) {
            S().names[slot] = name;
            Log("slot %d is %s", slot, name.c_str());
        }
        return {{"ResKind", 0}};
    }
    if (path == "/np/signaling/resolve") {
        const std::string id = StrOf(body, "OnlineId");
        for (const auto& [s, name] : S().names) {
            if (name == id) return {{"ResKind", 0}, {"Slot", s}};
        }
        return {{"ResKind", 1}};
    }
    if (path == "/mp/matching2/create_room") {
        const std::string name = StrOf(body, "OnlineId");
        if (!name.empty()) S().names[slot] = name;
        for (auto it = S().rooms.begin(); it != S().rooms.end();) {
            const bool owned = std::any_of(it->second.members.begin(), it->second.members.end(),
                                           [&](const Member& m) { return m.slot == slot && m.member_id == 1; });
            if (owned) {
                const std::uint64_t id = it->first;
                ++it;
                LeaveLocked(id, slot, "leave_room");
            } else {
                ++it;
            }
        }
        const std::uint64_t id = S().next_room++;
        MatchRoom room;
        room.session_id = "room-" + std::to_string(id);
        room.max_members = static_cast<int>(std::clamp<long long>(IntOf(body, "MaxMembers", 5), 2, 5));
        room.members.push_back({1, slot});
        S().rooms[id] = room;
        return {{"ResKind", 0}, {"RoomId", static_cast<long long>(id)}, {"SessionId", room.session_id},
                {"MemberId", 1}, {"MaxMembers", room.max_members}};
    }
    if (path == "/mp/matching2/join_room") {
        const std::string name = StrOf(body, "OnlineId");
        if (!name.empty()) S().names[slot] = name;
        const auto id = static_cast<std::uint64_t>(IntOf(body, "RoomId"));
        const auto it = S().rooms.find(id);
        if (it == S().rooms.end()) return {{"ResKind", 1}, {"Message", "no such room"}};
        MatchRoom& room = it->second;
        for (const Member& m : room.members) {
            if (m.slot == slot) {
                Json members = Json::array();
                for (const Member& x : room.members) members.push_back(Record(room, x, id));
                return {{"ResKind", 0}, {"SessionId", room.session_id}, {"MemberId", m.member_id},
                        {"OwnerMemberId", 1}, {"MaxMembers", room.max_members}, {"Members", members}};
            }
        }
        if (static_cast<int>(room.members.size()) >= room.max_members) return {{"ResKind", 1}, {"Message", "room full"}};
        const Member joined{room.next_member++, slot};
        room.members.push_back(joined);
        Json members = Json::array();
        for (const Member& x : room.members) members.push_back(Record(room, x, id));
        Json ev = Record(room, joined, id);
        ev["Name"] = "room_member_joined";
        for (const Member& other : room.members) {
            if (other.slot != slot) Deliver(other.slot, ev);
        }
        return {{"ResKind", 0}, {"SessionId", room.session_id}, {"MemberId", joined.member_id},
                {"OwnerMemberId", 1}, {"MaxMembers", room.max_members}, {"Members", members}};
    }
    if (path == "/mp/matching2/leave_room") {
        for (auto it = S().rooms.begin(); it != S().rooms.end();) {
            const std::uint64_t id = it->first;
            const bool match = it->second.session_id == StrOf(body, "SessionId");
            ++it;
            if (match) LeaveLocked(id, slot, "leave_room");
        }
        return {{"ResKind", 0}};
    }
    if (path == "/mp/matching2/heartbeat") {
        for (const auto& [id, room] : S().rooms) {
            if (room.session_id != StrOf(body, "SessionId")) continue;
            for (const Member& m : room.members) {
                if (m.slot == slot) return {{"ResKind", 0}, {"InRoom", 1}};
            }
        }
        return {{"ResKind", 0}, {"InRoom", 0}};
    }
    if (path == "/mp/matching2/kick_member") {
        const int target = static_cast<int>(IntOf(body, "MemberId"));
        for (auto& [id, room] : S().rooms) {
            if (room.session_id != StrOf(body, "SessionId")) continue;
            const bool owner = std::any_of(room.members.begin(), room.members.end(),
                                           [&](const Member& m) { return m.slot == slot && m.member_id == 1; });
            if (!owner) return {{"ResKind", 1}};
            for (const Member& m : room.members) {
                if (m.member_id != target) continue;
                Deliver(m.slot, {{"Name", "room_member_kicked"}, {"RoomId", static_cast<long long>(id)},
                                 {"MemberId", target}, {"OptData", StrOf(body, "OptData")}});
                const int kicked = m.slot;
                LeaveLocked(id, kicked, "kicked");
                return {{"ResKind", 0}};
            }
        }
        return {{"ResKind", 1}};
    }
    return {{"ResKind", 0}};
}

void ForgetSlotLocked(int slot) {
    std::vector<std::uint64_t> ids;
    for (const auto& [id, room] : S().rooms) ids.push_back(id);
    for (const std::uint64_t id : ids) LeaveLocked(id, slot, "disconnected");
    S().signs.erase(slot);
    const auto name = S().names.find(slot);
    if (name != S().names.end()) {
        for (const auto& [other, n] : S().names) {
            if (other != slot) Deliver(other, {{"Name", "peer_deactivated"}, {"OnlineId", name->second}});
        }
        S().names.erase(name);
    }
}

bool B64Size(const std::string& text, std::size_t want) {
    std::size_t pad = 0;
    for (std::size_t i = text.size(); i > 0 && text[i - 1] == '='; --i) ++pad;
    if (text.empty() || text.size() % 4 != 0 || pad > 2) return false;
    for (std::size_t i = 0; i < text.size() - pad; ++i) {
        const char c = text[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/')) return false;
    }
    return text.size() / 4 * 3 - pad == want;
}

Json HandleSummon(int slot, const std::string& api, const Json& body) {
    std::lock_guard lock(S().host_mu);
    if (api == "create") {
        const std::string data = StrOf(body, "SummonData");
        if (!B64Size(data, 0xE0)) return {{"ResKind", 0}};
        S().signs[slot] = {data, IntOf(body, "SummonType"), IntOf(body, "CharaId", slot)};
        return {{"ResKind", 0}};
    }
    if (api == "get") {
        std::set<long long> types;
        if (body.contains("SummonTypeList") && body["SummonTypeList"].is_array()) {
            for (const auto& t : body["SummonTypeList"]) types.insert(IntOf(t, "SummonType", -1));
        }
        Json list = Json::array();
        for (const auto& [s, sign] : S().signs) {
            if (s == slot) continue;
            if (!types.empty() && !types.count(sign.type)) continue;
            list.push_back({{"SummonDataVersion", 3}, {"SummonData", sign.data}, {"SummonType", sign.type},
                            {"CharaId", sign.chara}, {"UserId", 1000 + s}});
        }
        return {{"ResKind", 0}, {"SummonDataList", list}};
    }
    if (api == "request") {
        const int target = static_cast<int>(IntOf(body, "TargetUserId") - 1000);
        if (target >= 1 && target <= 5 && target != slot) {
            Json ev = {{"Name", "guest_invite"}, {"HostSlot", slot}};
            const auto name = S().names.find(slot);
            ev["HostOnlineId"] = name == S().names.end() ? "" : name->second;
            for (const auto& [id, room] : S().rooms) {
                for (const Member& m : room.members) {
                    if (m.slot == slot && m.member_id == 1) ev["RoomId"] = static_cast<long long>(id);
                }
            }
            Deliver(target, ev);
        }
        return {{"ResKind", 0}};
    }
    if (api == "delete") {
        S().signs.erase(slot);
        return {{"ResKind", 0}};
    }
    return {{"ResKind", 0}};
}

Json HandleHost(int slot, const Json& request) {
    const std::string path = StrOf(request, "p");
    const Json& body = request.contains("b") ? request["b"] : Json::object();
    if (path.rfind("/summon_messenger/", 0) == 0) return HandleSummon(slot, path.substr(18), body);
    return HandleSession(slot, path, body);
}

int HostSlotWait(int timeout_ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const int host = S().link.HostSlot();
        if (host && S().link.Connected()) return host;
        if (Clock::now() >= end) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool Rpc(const Json& request, Json& reply, std::string& error, int timeout_ms) {
    if (S().host) {
        const auto end = Clock::now() + std::chrono::milliseconds(std::min(timeout_ms, 10000));
        while (!S().own_slot.load() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int own = S().own_slot.load();
        if (!own) {
            error = "not in the room yet";
            return false;
        }
        reply = HandleHost(own, request);
        Localize(reply);
        return true;
    }
    const int host = HostSlotWait(std::min(timeout_ms, 10000));
    if (!host) {
        error = "no host in the room";
        return false;
    }
    std::uint32_t id;
    {
        std::lock_guard lock(S().mu);
        id = S().next_call++;
    }
    SendJson(host, kRequest, request, id, true);
    std::unique_lock lock(S().mu);
    if (!S().cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return S().replies.count(id) > 0; })) {
        error = "the host did not answer";
        return false;
    }
    reply = std::move(S().replies[id]);
    S().replies.erase(id);
    lock.unlock();
    Localize(reply);
    return true;
}

bool PollEvent(const Json& body, Json& reply) {
    const long long wait = std::clamp<long long>(IntOf(body, "WaitMs", 0), 0, 25000);
    std::unique_lock lock(S().mu);
    S().cv.wait_for(lock, std::chrono::milliseconds(wait), [] { return !S().events.empty(); });
    const long long cursor = IntOf(body, "Cursor", 0);
    if (S().events.empty()) {
        reply = {{"HasEvent", 0}, {"NextCursor", cursor}, {"WaitMaxMs", 25000}};
        return true;
    }
    reply = std::move(S().events.front());
    S().events.pop_front();
    reply["HasEvent"] = 1;
    reply["NextCursor"] = cursor + 1;
    reply["WaitMaxMs"] = 25000;
    return true;
}

SocketHandle OpenLoopbackUdp(int* port) {
    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return kNoSocket;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        CloseSocket(s);
        return kNoSocket;
    }
    *port = ntohs(addr.sin_port);
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return s;
}

void BridgeOpen(int slot) {
    std::lock_guard lock(S().bridge_mu);
    if (S().bridges.count(slot)) return;
    int port = 0;
    const SocketHandle s = OpenLoopbackUdp(&port);
    if (s == kNoSocket) return;
    S().bridges[slot] = s;
    S().bridge_ports[slot] = port;
}

void BridgeToGame(int slot, const std::uint8_t* data, std::size_t size) {
    SocketHandle s;
    {
        std::lock_guard lock(S().bridge_mu);
        const auto it = S().bridges.find(slot);
        if (it == S().bridges.end()) return;
        s = it->second;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(static_cast<std::uint16_t>(S().game_port));
    ::sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&to),
             sizeof(to));
}

void BridgeLoop() {
    std::vector<std::uint8_t> buffer(65536);
    for (;;) {
        std::map<int, SocketHandle> bridges;
        {
            std::lock_guard lock(S().bridge_mu);
            bridges = S().bridges;
        }
        if (bridges.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        fd_set set;
        FD_ZERO(&set);
        SocketHandle top = 0;
        for (const auto& [slot, s] : bridges) {
            FD_SET(s, &set);
            top = std::max(top, s);
        }
        timeval tv{0, 50000};
        if (::select(static_cast<int>(top + 1), &set, nullptr, nullptr, &tv) <= 0) continue;
        for (const auto& [slot, s] : bridges) {
            if (!FD_ISSET(s, &set)) continue;
            std::vector<std::uint8_t> batch{kBatch};
            int count = 0;
            for (;;) {
                sockaddr_in from{};
                socklen_t len = sizeof(from);
                const int n = ::recvfrom(s, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                         reinterpret_cast<sockaddr*>(&from), &len);
                if (n <= 0) break;
                if (batch.size() + 2 + static_cast<std::size_t>(n) > kBatchBytes && count) {
                    S().link.Send(slot, batch.data(), batch.size());
                    batch.assign(1, kBatch);
                    count = 0;
                }
                batch.push_back(static_cast<std::uint8_t>(n));
                batch.push_back(static_cast<std::uint8_t>(n >> 8));
                batch.insert(batch.end(), buffer.begin(), buffer.begin() + n);
                ++count;
            }
            if (count) S().link.Send(slot, batch.data(), batch.size());
        }
    }
}

void OnFrame(int sender, const std::vector<std::uint8_t>& data) {
    if (data.empty()) return;
    const std::uint8_t kind = data[0];
    if (kind == kDatagram) {
        BridgeToGame(sender, data.data() + 1, data.size() - 1);
        return;
    }
    if (kind == kBatch) {
        std::size_t at = 1;
        while (data.size() - at >= 2) {
            const std::size_t n = data[at] | (std::size_t(data[at + 1]) << 8);
            at += 2;
            if (n > data.size() - at) break;
            BridgeToGame(sender, data.data() + at, n);
            at += n;
        }
        return;
    }
    if (kind == kTravel) {
        if (data.size() != 5 || sender != S().link.HostSlot()) return;
        std::uint32_t lamp = 0;
        std::memcpy(&lamp, data.data() + 1, 4);
        Log("the host travels to lamp %u: following", lamp);
        std::lock_guard lock(S().vitals_mu);
        S().travel = lamp;
        S().travel_at = Clock::now();
        return;
    }
    if (kind == kVitals) {
        if (data.size() < kVitalsFixed) return;
        const std::size_t name_size = data[1];
        if (data.size() != kVitalsFixed + name_size) return;
        Vitals v;
        v.slot = sender;
        std::size_t at = 2;
        v.name.assign(reinterpret_cast<const char*>(data.data() + at), name_size);
        at += name_size;
        for (std::int32_t* field : {&v.hp, &v.hp_max, &v.stamina, &v.stamina_max}) {
            std::memcpy(field, data.data() + at, 4);
            at += 4;
        }
        std::memcpy(&v.revives, data.data() + at, 2);
        at += 2;
        std::memcpy(v.pos, data.data() + at, sizeof v.pos);
        std::lock_guard lock(S().vitals_mu);
        S().vitals[sender] = {std::move(v), Clock::now()};
        return;
    }
    if (kind == kRequest || kind == kReply) {
        if (data.size() < 5) return;
        std::uint32_t id = 0;
        for (int i = 0; i < 4; ++i) id |= static_cast<std::uint32_t>(data[1 + i]) << (8 * i);
        Json value = Json::parse(data.begin() + 5, data.end(), nullptr, false);
        if (value.is_discarded()) return;
        if (kind == kRequest) {
            if (!S().host) return;
            SendJson(sender, kReply, HandleHost(sender, value), id, true);
            return;
        }
        if (!id) return;
        {
            std::lock_guard lock(S().mu);
            S().replies[id] = std::move(value);
        }
        S().cv.notify_all();
        return;
    }
    if (kind == kEvent) {
        if (sender != S().link.HostSlot()) return;
        Json value = Json::parse(data.begin() + 1, data.end(), nullptr, false);
        if (!value.is_object()) return;
        Localize(value);
        {
            std::lock_guard lock(S().mu);
            value["EventId"] = S().next_event++;
            S().events.push_back(std::move(value));
        }
        S().cv.notify_all();
    }
}

void LinkLoop() {
    int last_host = 0;
    for (;;) {
        S().link.WaitEvent(500);
        for (auto& e : S().link.Drain()) {
            switch (e.kind) {
            case Event::Kind::Connected:
                if (S().own_slot.load() && S().own_slot.load() != e.slot)
                    Log("came back as slot %d (was %d)", e.slot, S().own_slot.load());
                S().own_slot = e.slot;
                Log("in the room as slot %d%s", e.slot, S().host ? " (host)" : "");
                break;
            case Event::Kind::Joined:
                BridgeOpen(e.slot);
                Log("slot %d joined the room", e.slot);
                break;
            case Event::Kind::Left:
                Log("slot %d left the room", e.slot);
                {
                    std::lock_guard lock(S().vitals_mu);
                    S().vitals.erase(e.slot);
                }
                if (S().host) {
                    std::lock_guard lock(S().host_mu);
                    ForgetSlotLocked(e.slot);
                }
                break;
            case Event::Kind::Host:
                if (e.slot != last_host) Log("the room's host is slot %d", e.slot);
                if (last_host && e.slot != last_host && !S().host) {
                    std::lock_guard lock(S().mu);
                    S().events.push_back({{"Name", "room_closed"}, {"RoomId", 0}, {"Reason", "host_left"},
                                          {"EventId", S().next_event++}});
                    S().cv.notify_all();
                }
                last_host = e.slot;
                if (e.slot && !S().host && !S().player.empty())
                    SendJson(e.slot, kRequest,
                             Json{{"p", "/mp/matching2/context_start"}, {"b", {{"OnlineId", S().player}}}}, 0, true);
                break;
            case Event::Kind::Data:
                OnFrame(e.slot, e.data);
                break;
            case Event::Kind::Disconnected:
                Log("lost the room connection; reconnecting");
                break;
            case Event::Kind::Refused:
                Log("could not join the room (%s): it may be full (5 players), or your connection already has another "
                    "room open (one room per player); retrying",
                    S().link.LastError().empty() ? "refused" : S().link.LastError().c_str());
                break;
            }
        }
    }
}

std::string ApiList(const std::string& path) {
    static const std::map<std::string, std::string> lists = {
        {"/blood_messenger/create", "BloodMessIdList"},
        {"/blood_messenger/message_area", "BloodMessList"},
        {"/blood_messenger/evaluation", "BloodMessEvaluationList"},
        {"/blood_messenger/exist_messages", "BloodMessEvaluationList"},
        {"/chair_messenger/get", "ChairMessList"},
        {"/tomb_messenger/message_area", "TombMessList"},
        {"/wandering_ghost/get", "WanderingGhostList"},
    };
    const auto it = lists.find(path);
    return it == lists.end() ? "" : it->second;
}

std::string GameApiAnswer(const std::string& path, const Json& body) {
    const int slot = S().own_slot.load();
    if (path == "/basic_utils/login") {
        return Json{{"ResKind", 0}, {"SessionId", "s" + std::to_string(slot)}, {"UserId", 1000 + slot},
                    {"UserStatus", 0}, {"LanguageId", 1}, {"ServerVersion", 109}}
            .dump();
    }
    if (path == "/basic_utils/sync_chara_id") {
        return Json{{"ResKind", 0}, {"PublishCharacterIdList", Json::array({{{"PublishCharaId", slot ? slot : 1}}})}}.dump();
    }
    if (path == "/basic_utils/get_normal_notice") return R"({"ResKind":0,"NoticeList":[]})";
    if (path == "/basic_utils/get_emergency_notice") {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &tm);
        return Json{{"ResKind", 0}, {"NoticeList", Json::array()}, {"CheckTime", text}}.dump();
    }
    if (path.rfind("/summon_messenger/", 0) == 0) {
        Json reply;
        std::string error;
        if (!Rpc({{"p", path}, {"b", body}}, reply, error, 8000)) return "";
        return reply.dump();
    }
    Json reply = {{"ResKind", 0}};
    if (const std::string list = ApiList(path); !list.empty()) reply[list] = Json::array();
    if (path == "/blood_messenger/exist_messages") reply["LostBloodMessIdList"] = Json::array();
    if (path == "/chair_messenger/get") reply["ChairMessCount"] = 0;
    if (path.rfind("/channel/", 0) == 0) {
        reply["ChannelList"] = Json::array();
        reply["ChannelInfoList"] = Json::array();
    }
    return reply.dump();
}

std::string SsInfo() {
    static const char* const apis[] = {
        "Login", "ServerTimeGet", "SyncCharaId", "NoticeNormalGet", "NoticeEmergencyGet", "UserAgreementGet",
        "BloodMessCreate", "BloodMessGetList", "BloodMessEvaluate", "BloodMessGetEvaluate", "BloodMessRemove",
        "BloodMessSearchAdd", "ChannelUpload", "ChannelShare", "ChannelSearch", "ChannelWordSearch",
        "ChannelGetDetailsInfo", "ChannelGetInfo", "ChannelRandomJoin", "ChannelAddMaterial",
        "ChannelAddMaterialCompleteNotify", "MessengerShellUpload", "MultiPlayNetError", "UserPropertiesMoveCount",
        "UserPropertiesMoveCountCheck", "SummonDataCreate", "SummonDataGetList", "SummonDataRemove",
        "SummonDataSummon", "ChairMessGetList", "ChairMessRespawnPointNotice", "TombMessCreate", "TombMessGetList",
        "DeathVisionGet", "TombMessRemove", "WanderingGhostCreate", "WanderingGhostGet"};
    static const std::pair<const char*, const char*> numbers[] = {
        {"ReloadServerStatusInfoInterval", "600"},
        {"SummonDataCreateInterval", "1"},
        {"SummonDataGetListInterval", "1"},
        {"SummonDataGetListGuardTimer", "0"},
        {"SummonDataGetListNomalCoopWaitCount", "0"},
        {"SummonDataGetListCombinationInvasionWaitCount", "0"},
        {"SummonDataGetListNaturalEnemyWaitCountFixed", "0"},
        {"SummonDataGetListAllAreaMethodWaitCount", "0"},
        {"SummonDataGetListGetCountPerSummonType", "8"},
        {"SummonDataGetListGetMaxCount", "8"},
        {"SummonDataGetListCoopGuardTimerClampTime", "0"},
        {"SummonDataGetListInvationGuardTimerClampTime", "0"},
        {"SummonDataGetListCoopGuardTimerLimitationTimeRate", "0"},
        {"SummonDataGetListCoopGuardTimerObserveTimeRate", "0"},
        {"SummonDataGetListInvationGuardTimerLimitationTimeRate", "0"},
        {"SummonDataGetListInvationGuardTimerObserveTimeRate", "0"},
        {"TombMessGetListInterval", "600"},
        {"BloodMessGetListInterval", "600"},
        {"WanderingGhostCreateInterval", "600"},
        {"WanderingGhostGetInterval", "600"},
        {"ChairMessGetListInterval", "60"},
        {"NoticeEmergencyGetInterval", "3600"},
        {"PlayLog", "0"},
        {"SummonDataCoopMatchingLevelLowerAbs", "-1000"},
        {"SummonDataCoopMatchingLevelLowerRel", "0"},
        {"SummonDataCoopMatchingLevelUpperAbs", "1000"},
        {"SummonDataCoopMatchingLevelUpperRel", "0"},
        {"SummonDataCreateNaturalEnemyPercentFixed", "0"},
        {"SummonDataBloodMadHunterPercent", "0"},
    };
    const std::string base = "http://127.0.0.1:" + std::to_string(S().http_port);
    std::string text = "<ssinfo><ss>0</ss>";
    for (int n = 0; n < 10; ++n) {
        const std::string k = std::to_string(n);
        text += "<gameurl" + k + ">";
        for (const char* api : apis) text += std::string("<api_") + api + ">" + base + "</api_" + api + ">";
        text += "</gameurl" + k + ">";
        for (const auto& [tag, value] : numbers) text += std::string("<") + tag + k + ">" + value + "</" + tag + k + ">";
    }
    text += "</ssinfo>";
    std::string encoded(4 * ((text.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                  reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    encoded.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return encoded;
}

void HttpLoop() {
    httplib::Server server;
    server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 60) Log("game server: %s %s -> %d", req.method.c_str(), req.path.c_str(), res.status);
    });
    server.Get(".*", [](const httplib::Request& req, httplib::Response& res) {
        if (req.path.size() >= 8 && req.path.compare(req.path.size() - 8, 8, "/ss.info") == 0) {
            res.set_content(SsInfo(), "text/plain");
            return;
        }
        res.set_content(GameApiAnswer(req.path, Json::object()), "application/json");
    });
    server.Post(".*", [](const httplib::Request& req, httplib::Response& res) {
        const Json body = Json::parse(req.body, nullptr, false);
        const std::string answer = GameApiAnswer(req.path, body.is_object() ? body : Json::object());
        if (answer.empty()) {
            res.status = 503;
            return;
        }
        res.set_content(answer, "application/json");
    });
    server.Put(".*", [](const httplib::Request&, httplib::Response& res) { res.status = 200; });
    if (!server.listen("127.0.0.1", S().http_port)) Log("the game server could not listen on port %d", S().http_port);
}

} // namespace

bool Active() {
    static const bool on = Env("BB_NET_BACKEND") == "room";
    return on;
}

bool IsHost() {
    return Env("BB_ROOM_HOST") == "1";
}

void SendVitals(const Vitals& own) {
    if (!S().started.load() || !S().link.Connected()) return;
    const std::string name = own.name.substr(0, 32);
    std::vector<std::uint8_t> frame(kVitalsFixed + name.size());
    frame[0] = kVitals;
    frame[1] = static_cast<std::uint8_t>(name.size());
    std::size_t at = 2;
    std::memcpy(frame.data() + at, name.data(), name.size());
    at += name.size();
    for (const std::int32_t field : {own.hp, own.hp_max, own.stamina, own.stamina_max}) {
        std::memcpy(frame.data() + at, &field, 4);
        at += 4;
    }
    std::memcpy(frame.data() + at, &own.revives, 2);
    at += 2;
    std::memcpy(frame.data() + at, own.pos, sizeof own.pos);
    S().link.Send(0, frame.data(), frame.size());
}

std::vector<Vitals> Party(int max_age_ms) {
    std::vector<Vitals> out;
    const auto now = Clock::now();
    std::lock_guard lock(S().vitals_mu);
    for (const auto& [slot, entry] : S().vitals) {
        Vitals v = entry.first;
        v.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.second).count();
        if (v.age_ms <= max_age_ms) out.push_back(std::move(v));
    }
    return out;
}

void SendTravel(std::uint32_t lamp) {
    if (!S().started.load() || !S().link.Connected()) return;
    std::uint8_t frame[5] = {kTravel};
    std::memcpy(frame + 1, &lamp, 4);
    S().link.Send(0, frame, sizeof frame);
    Log("travelling to lamp %u: the party follows", lamp);
}

bool TakeTravel(std::uint32_t& lamp) {
    std::lock_guard lock(S().vitals_mu);
    if (!S().travel) return false;
    if (Clock::now() - S().travel_at > std::chrono::seconds(10)) {
        S().travel = 0;
        return false;
    }
    lamp = S().travel;
    S().travel = 0;
    return true;
}

std::string PlayerName() {
    return Env("BB_ROOM_PLAYER").substr(0, 16);
}

void Start(int game_p2p_port) {
    if (S().started.exchange(true)) return;
    if (!S().game_port) S().game_port = game_p2p_port;
    S().host = IsHost();
    S().player = PlayerName();
    S().http_port = std::atoi(Env("BB_ROOM_HTTP_PORT").c_str());
    if (S().http_port <= 0 || S().http_port > 65535) S().http_port = game_p2p_port + 1;
    S().link.SetHost(S().host);
    if (!S().link.Start(Env("BB_ROOM_SERVER"), Env("BB_ROOM_NAME"), Env("BB_ROOM_PASSWORD"))) {
        Log("no room server or room name: seamless co-op is off");
        return;
    }
    Log("joining room on %s as %s (%s), game server on 127.0.0.1:%d", Env("BB_ROOM_SERVER").c_str(),
        S().player.c_str(), S().host ? "host" : "guest", S().http_port);
    std::thread(LinkLoop).detach();
    std::thread(BridgeLoop).detach();
    std::thread(HttpLoop).detach();
}

void SetGamePort(int port) {
    if (port > 0 && port <= 65535) S().game_port = port;
}

bool Call(const std::string& path, const Json& body, Json& reply, std::string& error, int timeout_ms) {
    if (path == "/np/events/poll") return PollEvent(body, reply);
    if (path == "/np/events/ack" || path == "/mp/matching2/signaling_update") {
        reply = {{"ResKind", 0}};
        return true;
    }
    return Rpc({{"p", path}, {"b", body}}, reply, error, std::max(timeout_ms, 3000));
}

} // namespace BbRoom
