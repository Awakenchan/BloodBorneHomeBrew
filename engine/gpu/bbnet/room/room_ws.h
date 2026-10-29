// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace BbRoom {

constexpr int kRoomProtocol = 9;

class WebSocket {
public:
    using Handler = std::function<void(std::string&& message, bool binary)>;

    WebSocket();
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    bool Connect(const std::string& url, int timeout_s, std::string& error);
    void Run(const Handler& handler);
    bool Send(const void* data, std::size_t size, bool binary);
    void Close();
    bool Open() const { return open_.load(); }

private:
    struct Impl;
    Impl* impl_;
    std::atomic<bool> open_{false};
    std::atomic<bool> closing_{false};
    std::mutex queue_mutex_;
    std::deque<std::vector<std::uint8_t>> queue_;
    void Wake();
};

} // namespace BbRoom
