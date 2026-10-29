// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "room_crypto.h"
#include "room_ws.h"

namespace BbRoom {

struct Event {
    enum class Kind { Connected, Joined, Left, Host, Data, Disconnected, Refused, GaveUp };
    Kind kind = Kind::Data;
    int slot = 0;
    std::vector<std::uint8_t> data;
};

class Link {
public:
    Link();
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool Start(const std::string& server, const std::string& name, const std::string& password);
    void Stop();
    bool Send(int target, const std::uint8_t* data, std::size_t size);
    void SetHost(bool host);
    std::vector<Event> Drain();
    bool WaitEvent(int timeout_ms);

    bool Connected() const { return connected_.load(); }
    int Slot() const { return slot_.load(); }
    int HostSlot() const { return host_.load(); }
    std::vector<int> Members() const;
    const std::string& RoomId() const { return secret_.room_id; }
    const std::string& LastError() const { return last_error_; }

private:
    void Run();
    bool Session();
    void Handle(const std::string& text);
    void Push(Event event);

    std::string url_;
    Secret secret_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> slot_{0};
    std::atomic<int> host_{0};
    std::atomic<bool> want_host_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Event> events_;
    std::vector<int> members_;
    std::mutex socket_mutex_;
    std::shared_ptr<WebSocket> socket_;
    std::string last_error_;
};

std::string RoomUrl(const std::string& server, const std::string& room_id);

} // namespace BbRoom
