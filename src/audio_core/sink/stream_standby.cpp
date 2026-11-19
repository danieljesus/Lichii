// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/stream_standby.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace AudioCore::Sink {

StandbySinkStream::StandbySinkStream(Core::System& system_, StreamType type_, Timing timing_)
    : SinkStream{system_, type_}, timing{timing_} {}

StandbySinkStream::~StandbySinkStream() {
    StopStandby();
}

void StandbySinkStream::OpenInitialBackend() {
    if (void* const opened = OpenBackend(0)) {
        published = opened;
        return;
    }
    if (type == StreamType::In) {
        // Owners of input streams fall back to silence themselves when there is no microphone.
        return;
    }
    LOG_WARNING(Audio_Sink, "Stream {} has no device yet: playing it into nothing until one opens",
                name);
    BackendLost();
}

bool StandbySinkStream::IsReady() const {
    return !failed && (published.load() != nullptr || active.load());
}

void StandbySinkStream::Start(bool) {
    std::scoped_lock lock{backend_mutex};
    if (failed || !paused) {
        return;
    }
    void* const current = published.load();
    if (!current && !active) {
        return;
    }
    if (!current || backend_broken) {
        // Standing in, or lost and not released yet: play into nothing, and on the device once a
        // backend reopens.
        paused = false;
        return;
    }
    if (!StartBackend(current)) {
        LOG_ERROR(Audio_Sink, "Error starting stream {}: reopening it", name);
        paused = false;
        BackendLost();
        return;
    }
    paused = false;
}

void StandbySinkStream::Stop() {
    std::scoped_lock lock{backend_mutex};
    if (paused) {
        return;
    }
    SignalPause();
    void* const current = published.load();
    if (current && !backend_broken && !StopBackend(current)) {
        LOG_ERROR(Audio_Sink, "Error stopping stream {}: reopening it", name);
        BackendLost();
    }
}

void StandbySinkStream::Finalize() {
    // The worker releases and reopens the backend: join it before destroying that.
    StopStandby();
    std::scoped_lock lock{backend_mutex};
    const bool playing = !paused;
    // Always: a renderer waiting for queue space that no device or standby frees any more.
    SignalPause();
    if (void* const current = published.exchange(nullptr)) {
        if (playing && !backend_broken) {
            StopBackend(current);
        }
        DestroyBackend(current);
    }
}

void StandbySinkStream::BackendLost() {
    backend_broken = true;
    {
        std::scoped_lock lock{standby_mutex};
        if (stopped) {
            return;
        }
        active = true;
        if (!worker.joinable()) {
            worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
        }
    }
    wake.notify_all();
}

void StandbySinkStream::StopStandby() {
    std::jthread joining;
    {
        std::scoped_lock lock{standby_mutex};
        stopped = true;
        joining = std::move(worker);
    }
    if (joining.joinable()) {
        joining.request_stop();
        wake.notify_all();
        joining.join();
    }
    active = false;
}

void StandbySinkStream::Run(std::stop_token stop_token) {
    Common::SetCurrentThreadName("AudioStandby");
#ifdef _WIN32
    // For the backend calls made here, once for the thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    bool running = true;
    while (running) {
        {
            std::unique_lock lock{standby_mutex};
            if (!wake.wait(lock, stop_token, [this] { return active.load(); })) {
                break;
            }
        }
        // Destroying the lost backend stops its callbacks: from here on only this thread
        // consumes the stream.
        ReleaseBackend();
        auto delay = timing.first_retry;
        for (u32 attempt = 0;; ++attempt) {
            if (failed) {
                // The owner faulted the stream: that stays final, so neither stand in nor reopen.
                active = false;
                break;
            }
            if (!StandIn(stop_token, std::chrono::steady_clock::now() + delay)) {
                running = false;
                break;
            }
            const Reopen result = TryReopen(attempt);
            if (result != Reopen::Failed) {
                break;
            }
            delay = (std::min)(delay * 2, timing.max_retry);
        }
    }
#ifdef _WIN32
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
#endif
}

void StandbySinkStream::ReleaseBackend() {
    std::scoped_lock lock{backend_mutex};
    if (void* const current = published.exchange(nullptr)) {
        DestroyBackend(current);
    }
}

StandbySinkStream::Reopen StandbySinkStream::TryReopen(u32 attempt) {
    if (failed) {
        active = false;
        return Reopen::Abandoned;
    }
    if (system.IsShuttingDown()) {
        return Reopen::Failed;
    }
    ++attempts;
    // Without the lock: opening may block while the device comes and goes, and the emulation's
    // Start and Stop must not wait for it.
    void* const opened = OpenBackend(attempt);
    if (!opened) {
        return Reopen::Failed;
    }
    std::scoped_lock lock{backend_mutex};
    if (failed) {
        DestroyBackend(opened);
        active = false;
        return Reopen::Abandoned;
    }
    // Published in one step with the end of the standby, so Start and Stop act on this backend
    // from now on. It reports no loss before it is started.
    backend_broken = false;
    published = opened;
    active = false;
    if (!paused && !StartBackend(opened)) {
        published = nullptr;
        active = true;
        DestroyBackend(opened);
        return Reopen::Failed;
    }
    ++reopens;
    LOG_INFO(Audio_Sink, "Stream {} reopened after {} attempts{}", name, attempt + 1,
             paused ? ", paused" : "");
    return Reopen::Opened;
}

bool StandbySinkStream::StandIn(std::stop_token stop_token,
                                std::chrono::steady_clock::time_point until) {
    constexpr std::chrono::microseconds period{TargetSampleCount * 1'000'000ULL / TargetSampleRate};
    std::vector<s16> block(TargetSampleCount * GetDeviceChannels());
    auto next = std::chrono::steady_clock::now() + period;
    while (true) {
        {
            std::unique_lock lock{standby_mutex};
            wake.wait_until(lock, stop_token, (std::min)(next, until), [] { return false; });
        }
        if (stop_token.stop_requested()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= until || failed) {
            return true;
        }
        if (now < next) {
            continue;
        }
        if (!IsPaused()) {
            if (type == StreamType::In) {
                std::ranges::fill(block, s16{0});
                ProcessAudioIn(block, TargetSampleCount);
            } else {
                ProcessAudioOutAndRender(block, TargetSampleCount);
            }
        }
        // Make up for late wake-ups, but after a host stall skip all but 20 blocks of it.
        next = (std::max)(next + period, now - 20 * period);
    }
}

} // namespace AudioCore::Sink
