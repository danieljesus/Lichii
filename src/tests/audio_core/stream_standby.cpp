// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/sink/stream_standby.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

using namespace std::chrono_literals;
using AudioCore::Sink::SinkBuffer;
using AudioCore::Sink::StreamType;
using Clock = std::chrono::steady_clock;

/// A stream whose backend can be lost and reopened, as a cubeb stream's can. Its backends have no
/// callback, so nothing consumes the stream unless the standby does. Too large for the stack.
class LosableStream final : public AudioCore::Sink::StandbySinkStream {
public:
    struct Backend {
        bool running{};
    };

    LosableStream(Core::System& emu_system, StreamType stream_type, int failed_opens = 0)
        : StandbySinkStream{emu_system, stream_type, {50ms, 200ms}} {
        device_channels = 2;
        SetSystemChannels(2);
        SetRingSize(4);
        failures_left = failed_opens;
        OpenInitialBackend();
        constructed = true;
    }
    ~LosableStream() override {
        Finalize();
    }

    /// What a backend's callback reports when it gives up on its device.
    void ReportLost() {
        BackendLost();
    }
    bool Running() {
        std::scoped_lock lock{mutex};
        return std::ranges::any_of(live, [](const Backend* entry) { return entry->running; });
    }
    std::size_t Live() {
        std::scoped_lock lock{mutex};
        return live.size();
    }
    std::vector<Clock::time_point> AttemptTimes() {
        std::scoped_lock lock{mutex};
        return attempt_times;
    }
    std::vector<u32> AttemptNumbers() {
        std::scoped_lock lock{mutex};
        return attempt_numbers;
    }

    std::atomic<int> failures_left{};
    std::atomic<u32> destroyed{};
    /// Runs in StartBackend, with the stream's backend lock held.
    std::function<void()> on_start;

private:
    void* OpenBackend(u32 attempt) override {
        std::scoped_lock lock{mutex};
        if (constructed) {
            attempt_times.push_back(Clock::now());
            attempt_numbers.push_back(attempt);
        }
        if (failures_left.fetch_sub(1) > 0) {
            return nullptr;
        }
        return *live.insert(new Backend{}).first;
    }
    bool StartBackend(void* backend) override {
        {
            std::scoped_lock lock{mutex};
            static_cast<Backend*>(backend)->running = true;
        }
        if (on_start) {
            on_start();
        }
        return true;
    }
    bool StopBackend(void* backend) override {
        std::scoped_lock lock{mutex};
        static_cast<Backend*>(backend)->running = false;
        return true;
    }
    void DestroyBackend(void* backend) override {
        std::scoped_lock lock{mutex};
        live.erase(static_cast<Backend*>(backend));
        delete static_cast<Backend*>(backend);
        ++destroyed;
    }

    std::mutex mutex;
    std::set<Backend*> live;
    std::vector<Clock::time_point> attempt_times;
    std::vector<u32> attempt_numbers;
    bool constructed{};
};

std::unique_ptr<LosableStream> MakeStream(SysmixTests::Context& context, StreamType type,
                                          int failed_opens = 0) {
    return std::make_unique<LosableStream>(context.system, type, failed_opens);
}

/// Long enough for slow and sanitized runners: every wait ends as soon as its condition holds.
constexpr auto Deadline = 10000ms;

/// Queue blocks as the audio renderer does, waiting for free space after each. Returns whether it
/// finished within the deadline; a renderer left waiting is released before returning.
bool Render(LosableStream& stream, u32 blocks, std::chrono::milliseconds deadline) {
    std::atomic<bool> done{};
    std::jthread renderer([&](std::stop_token stop_token) {
        SinkBuffer block{240, 0, 0, false};
        std::array<s16, 480> samples{};
        for (u32 i = 0; i < blocks && !stop_token.stop_requested(); ++i) {
            stream.AppendBuffer(block, samples);
            stream.WaitFreeSpace(stop_token);
        }
        done = !stop_token.stop_requested();
    });
    const auto until = Clock::now() + deadline;
    while (!done && Clock::now() < until) {
        std::this_thread::sleep_for(1ms);
    }
    renderer.request_stop();
    renderer.join();
    return done;
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds deadline) {
    const auto until = Clock::now() + deadline;
    while (!predicate()) {
        if (Clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

} // Anonymous namespace

TEST_CASE("A stream that lost its device plays into nothing and reopens with backoff",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    REQUIRE(stream.Running());
    stream.failures_left = 2;
    const auto lost = Clock::now();
    stream.ReportLost();
    REQUIRE(stream.IsReady());

    // The renderer keeps its pace and is never left waiting for queue space no device frees.
    REQUIRE(Render(stream, 40, Deadline));

    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }, Deadline));
    CHECK(stream.destroyed == 1);
    CHECK(stream.Live() == 1);
    CHECK(stream.Running());
    CHECK_FALSE(stream.IsStandingIn());
    CHECK(stream.IsReady());
    CHECK(stream.AttemptNumbers() == std::vector<u32>{1, 2, 3});
    const auto attempts = stream.AttemptTimes();
    REQUIRE(attempts.size() == 3);
    CHECK(attempts[0] - lost >= 50ms);
    CHECK(attempts[1] - attempts[0] >= 100ms);
    CHECK(attempts[2] - attempts[1] >= 200ms);

    // Once reopened, only the device consumes the stream.
    const u32 queued = stream.GetQueueSize();
    SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    stream.AppendBuffer(block, samples);
    std::this_thread::sleep_for(30ms);
    CHECK(stream.GetQueueSize() == queued + 1);
}

TEST_CASE("A lost stream keeps its owner's pause and resume through the reopen",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.failures_left = 1000;
    stream.ReportLost();

    // Paused, it is not played, as a stopped device would not be.
    SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    stream.AppendBuffer(block, samples);
    stream.AppendBuffer(block, samples);
    std::this_thread::sleep_for(30ms);
    CHECK(stream.GetQueueSize() == 2);

    // A resume while the device is lost is kept, and it plays into nothing.
    stream.Start();
    CHECK_FALSE(stream.IsPaused());
    REQUIRE(WaitFor([&] { return stream.GetQueueSize() == 0; }, Deadline));

    // Paused again before the device returns, it reopens paused, and starts on the next resume.
    stream.Stop();
    stream.failures_left = 0;
    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }, Deadline));
    CHECK_FALSE(stream.Running());
    CHECK(stream.IsPaused());
    stream.Start();
    CHECK(stream.Running());
}

TEST_CASE("A stop right after a reopen stops the new backend", "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    for (u64 cycle = 1; cycle <= 5; ++cycle) {
        INFO("cycle " << cycle);
        stream.Start();
        // The emulation stops the stream while the reopen starts the new backend: the stop waits
        // on the backend lock for the reopen to publish it, and then has to stop it.
        std::atomic<bool> armed{true};
        std::atomic<bool> starting{};
        stream.on_start = [&] {
            if (armed.exchange(false)) {
                starting = true;
                std::this_thread::sleep_for(20ms);
            }
        };
        stream.ReportLost();
        REQUIRE(WaitFor([&] { return starting.load(); }, Deadline));
        stream.Stop();
        stream.on_start = nullptr;
        CHECK(stream.Reopens() == cycle);
        CHECK_FALSE(stream.Running());
        CHECK(stream.IsPaused());
    }
}

TEST_CASE("A renderer waiting on a lost stream is released at shutdown",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 1000;
    stream.ReportLost();
    context.system.SetShuttingDown(true);
    const bool released = Render(stream, 20, Deadline);
    context.system.SetShuttingDown(false);
    REQUIRE(released);
}

TEST_CASE("Closing a stream that stands in releases a renderer waiting on it",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 1000;
    stream.ReportLost();
    // Renders until told how many blocks are left, then that many: after the close, enough to
    // fill the queue and wait were nothing to release it, far fewer than the queue holds.
    std::atomic<int> left{-1};
    std::atomic<bool> finished{};
    std::jthread renderer([&](std::stop_token stop_token) {
        SinkBuffer block{240, 0, 0, false};
        std::array<s16, 480> samples{};
        while (left != 0 && !stop_token.stop_requested()) {
            stream.AppendBuffer(block, samples);
            stream.WaitFreeSpace(stop_token);
            if (left > 0) {
                --left;
            }
        }
        finished = !stop_token.stop_requested();
    });
    std::this_thread::sleep_for(50ms);
    // With the standby stopped nothing frees queue space: only the pause lets the renderer on.
    stream.Finalize();
    left = 32;
    const bool released = WaitFor([&] { return finished.load(); }, Deadline);
    renderer.request_stop();
    renderer.join();
    CHECK(released);
    CHECK_FALSE(stream.IsReady());
}

TEST_CASE("A loss reported after the stream is closed is ignored", "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.Finalize();
    // As a backend can when it is destroyed.
    stream.ReportLost();
    CHECK_FALSE(stream.IsStandingIn());
    CHECK_FALSE(stream.IsReady());
    std::this_thread::sleep_for(100ms);
    CHECK(stream.ReopenAttempts() == 0);
}

TEST_CASE("A stream its owner faulted stays faulted when its device is lost",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    SECTION("faulted before the loss") {
        auto owned = MakeStream(context, StreamType::Render);
        auto& stream = *owned;
        stream.MarkFaulted();
        stream.ReportLost();
        CHECK_FALSE(stream.IsReady());
        stream.Start();
        CHECK(stream.IsPaused());
        std::this_thread::sleep_for(150ms);
        CHECK(stream.ReopenAttempts() == 0);
        CHECK(stream.Live() == 0);
    }
    SECTION("faulted while standing in") {
        auto owned = MakeStream(context, StreamType::Render);
        auto& stream = *owned;
        stream.Start();
        stream.failures_left = 1000;
        stream.ReportLost();
        REQUIRE(WaitFor([&] { return stream.ReopenAttempts() >= 1; }, Deadline));
        stream.MarkFaulted();
        stream.failures_left = 0;
        // The worker stands down at its next check; then nothing reopens.
        REQUIRE(WaitFor([&] { return !stream.IsStandingIn(); }, Deadline));
        const u64 attempts = stream.ReopenAttempts();
        std::this_thread::sleep_for(300ms);
        CHECK(stream.ReopenAttempts() == attempts);
        CHECK(stream.Reopens() == 0);
        CHECK(stream.Live() == 0);
        CHECK_FALSE(stream.IsReady());
    }
}

TEST_CASE("Only renderer and audio out streams stand in when their first open fails",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    for (const StreamType type : {StreamType::Render, StreamType::Out}) {
        auto owned = MakeStream(context, type, 2);
        auto& stream = *owned;
        INFO("type " << static_cast<int>(type));
        CHECK(stream.IsReady());
        CHECK(stream.IsStandingIn());
        // Two failures: the initial open and the first reopen.
        REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }, Deadline));
        CHECK(stream.AttemptNumbers() == std::vector<u32>{1, 2});
    }
    // Their owners handle it: sysmix fails the guest's open, input streams capture silence.
    for (const StreamType type : {StreamType::Sysmix, StreamType::In}) {
        auto owned = MakeStream(context, type, 1);
        auto& stream = *owned;
        INFO("type " << static_cast<int>(type));
        CHECK_FALSE(stream.IsReady());
        CHECK_FALSE(stream.IsStandingIn());
        std::this_thread::sleep_for(100ms);
        CHECK(stream.ReopenAttempts() == 0);
    }
}

TEST_CASE("A lost input stream captures silence until it reopens", "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::In);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 1000;
    stream.ReportLost();
    std::vector<s16> captured;
    REQUIRE(WaitFor(
        [&] {
            captured = stream.DrainInputSamples(240);
            return !captured.empty();
        },
        Deadline));
    CHECK(std::ranges::all_of(captured, [](s16 sample) { return sample == 0; }));
}
