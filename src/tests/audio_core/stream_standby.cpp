// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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

/// A stream whose backend can be lost and reopened, as a cubeb stream's can. Its open backend has
/// no callback, so nothing consumes it unless the standby does. Too large for the stack.
class LosableStream final : public AudioCore::Sink::SinkStream {
public:
    LosableStream(Core::System& emu_system, StreamType stream_type)
        : SinkStream{emu_system, stream_type} {
        device_channels = 2;
        SetSystemChannels(2);
        SetRingSize(4);
    }
    ~LosableStream() override {
        standby.Shutdown();
    }
    bool IsReady() const override {
        return !failed && (backend.load() || standby.IsActive());
    }
    void Start(bool = false) override {
        if (IsReady()) {
            paused = false;
        }
    }
    void Stop() override {
        SignalPause();
    }
    std::vector<Clock::time_point> Attempts() {
        std::scoped_lock lock{mutex};
        return attempts;
    }

    std::atomic<bool> backend{true};
    std::atomic<int> failures_left{};
    std::atomic<u32> releases{};
    std::atomic<bool> playing_on_reopen{};
    std::mutex mutex;
    std::vector<Clock::time_point> attempts;
    AudioCore::Sink::StreamStandby standby{*this,
                                           [this] {
                                               ++releases;
                                               backend = false;
                                           },
                                           [this] {
                                               {
                                                   std::scoped_lock lock{mutex};
                                                   attempts.push_back(Clock::now());
                                               }
                                               if (failures_left.fetch_sub(1) > 0) {
                                                   return false;
                                               }
                                               backend = true;
                                               playing_on_reopen = !IsPaused();
                                               return true;
                                           },
                                           {50ms, 200ms}};
};

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
    auto owned = std::make_unique<LosableStream>(context.system, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 2;
    const auto lost = Clock::now();
    stream.standby.Lost();
    REQUIRE(stream.IsReady());

    // The renderer keeps its pace and is never left waiting for queue space no device frees.
    REQUIRE(Render(stream, 40, 3000ms));

    REQUIRE(WaitFor([&] { return stream.standby.Reopens() == 1; }, 5000ms));
    CHECK(stream.releases == 1);
    CHECK(stream.standby.ReopenAttempts() == 3);
    CHECK(stream.playing_on_reopen);
    CHECK_FALSE(stream.standby.IsActive());
    CHECK(stream.IsReady());
    const auto attempts = stream.Attempts();
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
    auto owned = std::make_unique<LosableStream>(context.system, StreamType::Render);
    auto& stream = *owned;
    stream.failures_left = 1000;
    stream.standby.Lost();

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
    REQUIRE(WaitFor([&] { return stream.GetQueueSize() == 0; }, 1000ms));

    // Paused again before the device returns, it reopens paused.
    stream.Stop();
    stream.failures_left = 0;
    REQUIRE(WaitFor([&] { return stream.standby.Reopens() == 1; }, 2000ms));
    CHECK_FALSE(stream.playing_on_reopen);
    CHECK(stream.IsPaused());
}

TEST_CASE("A renderer waiting on a lost stream is released at shutdown",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = std::make_unique<LosableStream>(context.system, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 1000;
    stream.standby.Lost();
    context.system.SetShuttingDown(true);
    const bool released = Render(stream, 20, 2000ms);
    context.system.SetShuttingDown(false);
    REQUIRE(released);
}

TEST_CASE("A stream its owner faulted stays faulted when its device is lost",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = std::make_unique<LosableStream>(context.system, StreamType::Render);
    auto& stream = *owned;
    stream.MarkFaulted();
    stream.standby.Lost();
    CHECK_FALSE(stream.IsReady());
    stream.Start();
    CHECK(stream.IsPaused());
}

TEST_CASE("A lost input stream captures silence until it reopens", "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = std::make_unique<LosableStream>(context.system, StreamType::In);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 1000;
    stream.standby.Lost();
    REQUIRE(WaitFor([&] { return stream.DrainInputSamples(240).size() != 0; }, 1000ms));
    const auto captured = stream.DrainInputSamples(4800);
    CHECK(std::ranges::all_of(captured, [](s16 sample) { return sample == 0; }));
}
