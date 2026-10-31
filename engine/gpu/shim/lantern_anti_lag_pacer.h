// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace LanternAntiLag {

enum class Stage { Off, Input, Present };

struct Limits {
    std::chrono::milliseconds wait{50};
    std::chrono::milliseconds hitch{100};
    int strikes = 3;
    std::chrono::seconds strike_window{30};
    std::chrono::seconds report{60};
    std::size_t frames_ahead = 2;
};

class Pacer {
public:
    using Send = std::function<void(Stage, std::uint64_t)>;
    using Clock = std::chrono::steady_clock;

    explicit Pacer(Send send_, Limits limits_ = {})
        : send{std::move(send_)}, limits{limits_}, report_from{Clock::now()},
          worker{[this] { Run(); }} {}

    ~Pacer() {
        {
            std::scoped_lock lock{mutex};
            stop = true;
        }
        work_ready.notify_all();
        worker.join();
        if (on) {
            send(Stage::Off, 0);
        }
    }

    Pacer(const Pacer&) = delete;
    Pacer& operator=(const Pacer&) = delete;

    void BeforeInput(bool wanted) {
        if (disabled.load(std::memory_order_relaxed)) {
            return;
        }
        std::unique_lock lock{mutex};
        if (!wanted) {
            frames.clear();
            if (on && !busy) {
                on = false;
                Post(Stage::Off, 0);
            }
            return;
        }
        if (busy) {
            return;
        }
        const auto now = Clock::now();
        if (!presented || now - last_present > limits.hitch) {
            frames.clear();
            return;
        }
        if (frames.size() >= limits.frames_ahead) {
            return;
        }
        const std::uint64_t frame = ++next_frame;
        frames.push_back(frame);
        on = true;
        Post(Stage::Input, frame);
        const bool returned =
            work_done.wait_for(lock, limits.wait, [this] { return !busy; });
        if (!returned) {
            std::erase(frames, frame);
            Strike(now);
        }
        MaybeReport(now);
    }

    void BeforePresent() {
        std::unique_lock lock{mutex};
        last_present = Clock::now();
        presented = true;
        if (disabled.load(std::memory_order_relaxed) || !on || frames.empty()) {
            return;
        }
        const std::uint64_t frame = frames.front();
        frames.pop_front();
        lock.unlock();
        const auto start = Clock::now();
        send(Stage::Present, frame);
        const auto now = Clock::now();
        if (now - start > limits.wait) {
            lock.lock();
            Strike(now);
        }
    }

    bool Disabled() const {
        return disabled.load(std::memory_order_relaxed);
    }

private:
    void Post(Stage stage, std::uint64_t frame) {
        job_stage = stage;
        job_frame = frame;
        busy = true;
        job_ready = true;
        work_ready.notify_one();
    }

    void Run() {
        std::unique_lock lock{mutex};
        while (true) {
            work_ready.wait(lock, [this] { return stop || job_ready; });
            if (stop && !job_ready) {
                return;
            }
            job_ready = false;
            const Stage stage = job_stage;
            const std::uint64_t frame = job_frame;
            lock.unlock();
            const auto start = Clock::now();
            send(stage, frame);
            const auto elapsed = Clock::now() - start;
            lock.lock();
            busy = false;
            if (stage == Stage::Input) {
                const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
                ++report_marked;
                report_total_ms += ms;
                report_max_ms = ms > report_max_ms ? ms : report_max_ms;
            }
            if (disabled.load(std::memory_order_relaxed) && on) {
                on = false;
                Post(Stage::Off, 0);
            }
            work_done.notify_all();
        }
    }

    void Strike(Clock::time_point now) {
        if (now - strike_from > limits.strike_window) {
            strike_from = now;
            strike_count = 0;
        }
        ++strike_count;
        ++report_over;
        std::printf("AMD Anti-Lag 2: the driver wait passed %lld ms, the game goes on without it "
                    "(%d of %d within %lld s; BB_ANTI_LAG_WAIT_MS)\n",
                    static_cast<long long>(limits.wait.count()), strike_count, limits.strikes,
                    static_cast<long long>(limits.strike_window.count()));
        if (strike_count < limits.strikes) {
            return;
        }
        disabled.store(true, std::memory_order_relaxed);
        frames.clear();
        std::printf("AMD Anti-Lag 2: off until the next start, the driver held the game too often "
                    "(BB_ANTI_LAG=0: never on)\n");
        if (on && !busy) {
            on = false;
            Post(Stage::Off, 0);
        }
    }

    void MaybeReport(Clock::time_point now) {
        if (now - report_from < limits.report) {
            return;
        }
        std::printf("AMD Anti-Lag 2: %llu frames marked in %lld s, driver wait average %.1f ms, "
                    "longest %.1f ms, %d past the limit\n",
                    static_cast<unsigned long long>(report_marked),
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::seconds>(now - report_from).count()),
                    report_marked ? report_total_ms / double(report_marked) : 0.0, report_max_ms,
                    report_over);
        report_from = now;
        report_marked = 0;
        report_total_ms = 0.0;
        report_max_ms = 0.0;
        report_over = 0;
    }

    Send send;
    Limits limits;
    std::mutex mutex;
    std::condition_variable work_ready;
    std::condition_variable work_done;
    std::deque<std::uint64_t> frames;
    std::atomic<bool> disabled{false};
    std::uint64_t next_frame = 0;
    std::uint64_t job_frame = 0;
    Stage job_stage = Stage::Off;
    bool job_ready = false;
    bool busy = false;
    bool stop = false;
    bool on = false;
    bool presented = false;
    Clock::time_point last_present{};
    Clock::time_point strike_from{};
    int strike_count = 0;
    Clock::time_point report_from;
    std::uint64_t report_marked = 0;
    double report_total_ms = 0.0;
    double report_max_ms = 0.0;
    int report_over = 0;
    std::thread worker;
};

} // namespace LanternAntiLag
