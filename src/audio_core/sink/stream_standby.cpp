// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/sink_stream.h"
#include "audio_core/sink/stream_standby.h"
#include "common/logging.h"
#include "common/thread.h"

namespace AudioCore::Sink {

StreamStandby::StreamStandby(SinkStream& stream_, std::function<void()> release_,
                             std::function<bool()> reopen_, Timing timing_)
    : stream{stream_}, release{std::move(release_)}, reopen{std::move(reopen_)}, timing{timing_} {
    worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
}

StreamStandby::~StreamStandby() {
    Shutdown();
}

void StreamStandby::Lost() {
    {
        std::scoped_lock lock{mutex};
        ++losses;
        active = true;
    }
    wake.notify_all();
}

void StreamStandby::Shutdown() {
    if (worker.joinable()) {
        worker.request_stop();
        wake.notify_all();
        worker.join();
    }
}

void StreamStandby::Run(std::stop_token stop_token) {
    Common::SetCurrentThreadName("AudioStandby");
    while (!stop_token.stop_requested()) {
        {
            std::unique_lock lock{mutex};
            if (!wake.wait(lock, stop_token, [this] { return active.load(); })) {
                return;
            }
        }
        // Drop the lost backend first: its callbacks have stopped once it is destroyed, so from
        // here on only this thread consumes the stream.
        release();
        u64 seen = losses.load();
        auto delay = timing.first_retry;
        while (true) {
            if (!StandIn(stop_token, std::chrono::steady_clock::now() + delay)) {
                return;
            }
            ++attempts;
            if (!reopen()) {
                // A loss reported by the attempt itself is this one: the backend is gone again.
                seen = losses.load();
                delay = (std::min)(delay * 2, timing.max_retry);
                continue;
            }
            active = false;
            if (losses.load() == seen) {
                ++reopens;
                break;
            }
            // Lost again while it reopened: stand in once more.
            active = true;
            release();
            seen = losses.load();
            delay = timing.first_retry;
        }
    }
}

bool StreamStandby::StandIn(std::stop_token stop_token,
                            std::chrono::steady_clock::time_point until) {
    constexpr std::chrono::microseconds period{TargetSampleCount * 1'000'000ULL / TargetSampleRate};
    std::vector<s16> block(TargetSampleCount * stream.GetDeviceChannels());
    auto next = std::chrono::steady_clock::now() + period;
    while (true) {
        {
            std::unique_lock lock{mutex};
            wake.wait_until(lock, stop_token, (std::min)(next, until), [] { return false; });
        }
        if (stop_token.stop_requested()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= until) {
            return true;
        }
        if (now < next) {
            continue;
        }
        if (!stream.IsPaused()) {
            if (stream.GetType() == StreamType::In) {
                std::ranges::fill(block, s16{0});
                stream.ProcessAudioIn(block, TargetSampleCount);
            } else {
                stream.ProcessAudioOutAndRender(block, TargetSampleCount);
            }
        }
        // Make up for late wake-ups, but after a host stall skip all but 20 blocks of it.
        next = (std::max)(next + period, now - 20 * period);
    }
}

} // namespace AudioCore::Sink
