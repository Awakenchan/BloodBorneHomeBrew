// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_link.h"

#include <algorithm>
#include <chrono>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace BbRoom {

std::string RoomUrl(const std::string& server, const std::string& room_id) {
    std::string base = server;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.rfind("https://", 0) == 0) {
        base = "wss://" + base.substr(8);
    } else if (base.rfind("http://", 0) == 0) {
        base = "ws://" + base.substr(7);
    } else if (base.rfind("wss://", 0) != 0 && base.rfind("ws://", 0) != 0) {
        base = "wss://" + base;
    }
    return base + "/room/" + room_id;
}

Link::Link() = default;

Link::~Link() { Stop(); }

bool Link::Start(const std::string& server, const std::string& name, const std::string& password) {
    Stop();
    if (server.empty() || name.empty()) return false;
    secret_ = Derive(name, password);
    url_ = RoomUrl(server, secret_.room_id);
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
    return true;
}

void Link::Stop() {
    stop_ = true;
    {
        std::lock_guard lock(socket_mutex_);
        if (socket_) socket_->close();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    connected_ = false;
    slot_ = 0;
    host_ = 0;
    std::lock_guard lock(mutex_);
    members_.clear();
}

bool Link::Send(int target, const std::uint8_t* data, std::size_t size) {
    const int slot = slot_.load();
    if (!connected_ || !slot || target == slot || target < 0 || target > 255) return false;
    auto sealed = Seal(secret_.key, static_cast<std::uint8_t>(slot), data, size);
    if (sealed.empty()) return false;
    sealed.insert(sealed.begin(), static_cast<std::uint8_t>(target));
    std::shared_ptr<httplib::ws::WebSocketClient> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    return socket && socket->send(reinterpret_cast<const char*>(sealed.data()), sealed.size());
}

void Link::SetHost(bool host) {
    want_host_ = host;
    std::shared_ptr<httplib::ws::WebSocketClient> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    if (socket && connected_) socket->send(std::string(host ? R"({"t":"host"})" : R"({"t":"unhost"})"));
}

std::vector<Event> Link::Drain() {
    std::lock_guard lock(mutex_);
    std::vector<Event> out(std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
    events_.clear();
    return out;
}

bool Link::WaitEvent(int timeout_ms) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !events_.empty() || stop_; }) &&
           !events_.empty();
}

std::vector<int> Link::Members() const {
    std::lock_guard lock(mutex_);
    return members_;
}

void Link::Push(Event event) {
    {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }
    cv_.notify_all();
}

void Link::Run() {
    int backoff_ms = 500;
    while (!stop_) {
        const bool welcomed = Session();
        if (stop_) break;
        backoff_ms = welcomed ? 500 : std::min(backoff_ms * 2, 8000);
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(backoff_ms), [this] { return stop_.load(); });
    }
}

bool Link::Session() {
    auto socket = std::make_shared<httplib::ws::WebSocketClient>(url_);
    if (!socket->is_valid()) return false;
    socket->set_connection_timeout(10);
    socket->set_tcp_nodelay(true);
    socket->set_websocket_ping_interval(15);
    {
        std::lock_guard lock(socket_mutex_);
        if (stop_) return false;
        socket_ = socket;
    }
    bool welcomed = false;
    const bool opened = socket->connect();
    if (opened) {
        std::string message;
        while (!stop_) {
            const auto result = socket->read(message);
            if (result == httplib::ws::ReadResult::Fail) break;
            if (result == httplib::ws::ReadResult::Text) {
                const bool was = connected_.load();
                Handle(message);
                if (!was && connected_) {
                    welcomed = true;
                    if (want_host_) socket->send(std::string(R"({"t":"host"})"));
                }
                continue;
            }
            if (message.size() < 1 + kSealOverhead) continue;
            const auto sender = static_cast<std::uint8_t>(message[0]);
            auto plain = Open(secret_.key, sender, reinterpret_cast<const std::uint8_t*>(message.data()) + 1,
                              message.size() - 1);
            if (!plain) continue;
            Push({Event::Kind::Data, sender, std::move(*plain)});
        }
    }
    {
        std::lock_guard lock(socket_mutex_);
        socket_.reset();
    }
    socket->close();
    if (connected_.exchange(false)) {
        {
            std::lock_guard lock(mutex_);
            members_.clear();
        }
        slot_ = 0;
        host_ = 0;
        Push({Event::Kind::Disconnected, 0, {}});
    } else if (opened && !welcomed && !stop_) {
        Push({Event::Kind::Refused, 0, {}});
    }
    return welcomed;
}

void Link::Handle(const std::string& text) {
    const auto message = nlohmann::json::parse(text, nullptr, false);
    if (!message.is_object()) return;
    const std::string type = message.value("t", "");
    const int slot = message.value("slot", 0);
    if (type == "welcome") {
        std::vector<int> members;
        if (message.contains("members") && message["members"].is_array()) {
            for (const auto& m : message["members"]) {
                if (m.is_number_integer()) members.push_back(m.get<int>());
            }
        }
        {
            std::lock_guard lock(mutex_);
            members_ = members;
        }
        slot_ = slot;
        host_ = message.value("host", 0);
        connected_ = true;
        Push({Event::Kind::Connected, slot, {}});
        for (const int m : members) Push({Event::Kind::Joined, m, {}});
        if (host_) Push({Event::Kind::Host, host_.load(), {}});
    } else if (type == "join") {
        {
            std::lock_guard lock(mutex_);
            if (std::find(members_.begin(), members_.end(), slot) == members_.end()) members_.push_back(slot);
        }
        Push({Event::Kind::Joined, slot, {}});
    } else if (type == "leave") {
        {
            std::lock_guard lock(mutex_);
            members_.erase(std::remove(members_.begin(), members_.end(), slot), members_.end());
        }
        Push({Event::Kind::Left, slot, {}});
    } else if (type == "host") {
        host_ = slot;
        Push({Event::Kind::Host, slot, {}});
    }
}

} // namespace BbRoom
