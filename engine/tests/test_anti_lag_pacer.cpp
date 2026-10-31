// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "shim/lantern_anti_lag_pacer.h"

using namespace LanternAntiLag;
using namespace std::chrono_literals;

namespace {

struct Call {
    Stage stage;
    std::uint64_t frame;
};

struct Driver {
    std::mutex mutex;
    std::vector<Call> calls;
    std::chrono::milliseconds input_sleep{0};

    Pacer::Send Hook() {
        return [this](Stage stage, std::uint64_t frame) {
            std::chrono::milliseconds sleep{0};
            {
                std::scoped_lock lock{mutex};
                calls.push_back({stage, frame});
                sleep = input_sleep;
            }
            if (stage == Stage::Input) {
                std::this_thread::sleep_for(sleep);
            }
        };
    }

    int Count(Stage stage) {
        std::scoped_lock lock{mutex};
        int count = 0;
        for (const Call& call : calls) {
            count += call.stage == stage;
        }
        return count;
    }
};

std::chrono::milliseconds Timed(Pacer& pacer, bool wanted = true) {
    const auto start = Pacer::Clock::now();
    pacer.BeforeInput(wanted);
    return std::chrono::duration_cast<std::chrono::milliseconds>(Pacer::Clock::now() - start);
}

void TestPairsInputWithPresent() {
    Driver driver;
    {
        Pacer pacer{driver.Hook()};
        pacer.BeforePresent();
        for (int i = 0; i < 5; ++i) {
            pacer.BeforeInput(true);
            pacer.BeforeInput(true);
            pacer.BeforeInput(true);
            pacer.BeforePresent();
        }
        std::vector<std::uint64_t> inputs, presents;
        std::scoped_lock lock{driver.mutex};
        for (const Call& call : driver.calls) {
            if (call.stage == Stage::Input) inputs.push_back(call.frame);
            if (call.stage == Stage::Present) presents.push_back(call.frame);
        }
        assert(inputs.size() <= presents.size() + 2);
        assert(presents.size() == 5);
        for (std::size_t i = 0; i < presents.size(); ++i) {
            assert(presents[i] == inputs[i]);
        }
    }
    assert(driver.Count(Stage::Off) == 1);
}

void TestNothingBeforeFirstPresent() {
    Driver driver;
    {
        Pacer pacer{driver.Hook()};
        pacer.BeforeInput(true);
        assert(driver.Count(Stage::Input) == 0);
    }
    assert(driver.Count(Stage::Off) == 0);
}

void TestLongDriverWaitIsCutAndEndsAntiLag() {
    Driver driver;
    driver.input_sleep = 190ms;
    Limits limits;
    limits.wait = 20ms;
    limits.hitch = 2s;
    {
        Pacer pacer{driver.Hook(), limits};
        pacer.BeforePresent();
        for (int stall = 0; stall < 3; ++stall) {
            assert(Timed(pacer) < 100ms);
            std::this_thread::sleep_for(200ms);
            pacer.BeforePresent();
        }
        assert(pacer.Disabled());
        std::this_thread::sleep_for(200ms);
        assert(driver.Count(Stage::Off) == 1);
        const int inputs = driver.Count(Stage::Input);
        assert(Timed(pacer) < 5ms);
        pacer.BeforePresent();
        assert(driver.Count(Stage::Input) == inputs);
        assert(driver.Count(Stage::Present) == 0);
    }
    assert(driver.Count(Stage::Off) == 1);
}

void TestHeldDriverSkipsFramesWithoutWaiting() {
    Driver driver;
    driver.input_sleep = 150ms;
    Limits limits;
    limits.wait = 10ms;
    limits.hitch = 2s;
    limits.strikes = 100;
    Pacer pacer{driver.Hook(), limits};
    pacer.BeforePresent();
    assert(Timed(pacer) < 100ms);
    assert(Timed(pacer) < 5ms);
    assert(driver.Count(Stage::Input) == 1);
    pacer.BeforePresent();
    assert(driver.Count(Stage::Present) == 0);
    std::this_thread::sleep_for(200ms);
}

void TestHitchSendsNoMarker() {
    Driver driver;
    Limits limits;
    limits.hitch = 20ms;
    Pacer pacer{driver.Hook(), limits};
    pacer.BeforePresent();
    std::this_thread::sleep_for(40ms);
    pacer.BeforeInput(true);
    assert(driver.Count(Stage::Input) == 0);
    pacer.BeforePresent();
    pacer.BeforeInput(true);
    assert(driver.Count(Stage::Input) == 1);
}

void TestSettingOffSendsOffOnce() {
    Driver driver;
    Pacer pacer{driver.Hook()};
    pacer.BeforePresent();
    pacer.BeforeInput(true);
    pacer.BeforePresent();
    pacer.BeforeInput(false);
    std::this_thread::sleep_for(20ms);
    pacer.BeforeInput(false);
    std::this_thread::sleep_for(20ms);
    assert(driver.Count(Stage::Off) == 1);
    pacer.BeforePresent();
    pacer.BeforeInput(false);
    assert(driver.Count(Stage::Input) == 1);
}

void TestPresentProgressesDuringInputWait() {
    Driver driver;
    driver.input_sleep = 60ms;
    Limits limits;
    limits.wait = 500ms;
    limits.hitch = 2s;
    Pacer pacer{driver.Hook(), limits};
    pacer.BeforePresent();
    std::thread game{[&] { pacer.BeforeInput(true); }};
    std::this_thread::sleep_for(10ms);
    const auto start = Pacer::Clock::now();
    pacer.BeforePresent();
    assert(Pacer::Clock::now() - start < 30ms);
    game.join();
}

} // namespace

int main() {
    TestPairsInputWithPresent();
    TestNothingBeforeFirstPresent();
    TestLongDriverWaitIsCutAndEndsAntiLag();
    TestHeldDriverSkipsFramesWithoutWaiting();
    TestHitchSendsNoMarker();
    TestSettingOffSendsOffOnce();
    TestPresentProgressesDuringInputWait();
    std::puts("anti-lag pacer: all tests passed");
    return 0;
}
