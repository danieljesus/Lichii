// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/adsp/apps/audio_renderer/audio_renderer.h"
#include "audio_core/renderer/command/command_list_header.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/// Long enough for slow and sanitized runners: every wait ends as soon as its condition holds.
constexpr auto Deadline = 10000ms;

/// Renderer streams whose backend never asks for audio, as a stream does whose callback stopped
/// without an error. Closing one keeps it, so that a watchdog can still release it.
class StalledSink final : public AudioCore::Sink::Sink {
public:
    AudioCore::Sink::SinkStream* AcquireSinkStream(Core::System& system, u32 channels,
        const std::string&, AudioCore::Sink::StreamType type) override {
        auto& stream = streams.emplace_back(
            std::make_unique<SysmixTests::Stream>(system, type, 2, true));
        stream->SetSystemChannels(channels);
        return stream.get();
    }
    void CloseStream(AudioCore::Sink::SinkStream*) override {}
    void CloseStreams() override {}
    f32 GetDeviceVolume() const override { return 1.0f; }
    void SetDeviceVolume(f32) override {}
    void SetSystemVolume(f32) override {}

    std::vector<std::unique_ptr<SysmixTests::Stream>> streams;
};

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds deadline = Deadline) {
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

TEST_CASE("Stopping the audio renderer releases a render waiting on a stalled stream",
          "[audio_core][renderer_stalled_stream]") {
    SysmixTests::Context context;
    StalledSink sink;
    AudioCore::ADSP::AudioRenderer::AudioRenderer renderer{context.system, sink};
    renderer.Start();
    REQUIRE(!sink.streams.empty());
    auto& stream = *sink.streams[0];

    // A playing stream whose queue is full past where the renderer waits for space, with no
    // backend callback to drain it.
    stream.Start();
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    for (int i = 0; i < 8; ++i) {
        stream.AppendBuffer(block, samples);
    }

    // An empty command list: enough for the renderer to wait for the stream's space.
    AudioCore::Renderer::CommandListHeader header{};
    renderer.SetCommandBuffer(0, reinterpret_cast<AudioCore::CpuAddr>(&header), sizeof(header), 2'000'000, 0,
                              &*context.process, false);
    renderer.Signal();

    std::atomic<bool> stopped{};
    std::jthread stopper([&] {
        renderer.Stop();
        stopped = true;
    });
    const bool prompt = WaitFor([&] { return stopped.load(); });
    if (!prompt) {
        // The watchdog: release the render so that Stop, and the test, end.
        stream.Stop();
    }
    stopper.join();
    CHECK(prompt);
}

TEST_CASE("A wait for a render on a stalled stream ends when it is stopped",
          "[audio_core][renderer_stalled_stream]") {
    SysmixTests::Context context;
    StalledSink sink;
    AudioCore::ADSP::AudioRenderer::AudioRenderer renderer{context.system, sink};
    renderer.Start();
    REQUIRE(!sink.streams.empty());
    auto& stream = *sink.streams[0];
    stream.Start();
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    for (int i = 0; i < 8; ++i) {
        stream.AppendBuffer(block, samples);
    }
    AudioCore::Renderer::CommandListHeader header{};
    renderer.SetCommandBuffer(0, reinterpret_cast<AudioCore::CpuAddr>(&header), sizeof(header),
                              2'000'000, 0, &*context.process, false);
    renderer.Signal();

    // As the system manager's thread waits for each render.
    std::atomic<bool> returned{};
    std::jthread waiter([&](std::stop_token stop_token) {
        renderer.Wait(stop_token);
        returned = true;
    });
    // The render is stuck on the stream, so its wait is too.
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(returned);
    waiter.request_stop();
    const bool prompt = WaitFor([&] { return returned.load(); });
    if (!prompt) {
        // The watchdog: release the render so that the wait, and the test, end.
        stream.Stop();
    }
    waiter.join();
    CHECK(prompt);
    renderer.Stop();
}
