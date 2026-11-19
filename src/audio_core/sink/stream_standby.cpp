// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>

#include "audio_core/common/common.h"
#include "audio_core/sink/stream_standby.h"
#include "common/assert.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace AudioCore::Sink {

namespace {
constexpr std::chrono::microseconds BlockPeriod{TargetSampleCount * 1'000'000ULL /
                                                TargetSampleRate};
} // Anonymous namespace

StandbySinkStream::StandbySinkStream(Core::System& system_, StreamType type_, Timing timing_)
    : SinkStream{system_, type_}, timing{timing_} {}

StandbySinkStream::~StandbySinkStream() {
    ASSERT_MSG(!worker.joinable(), "Stream {}: the derived destructor must call Finalize", name);
    StopWorker();
}

bool StandbySinkStream::IsReady() const {
    if (failed) {
        return false;
    }
    switch (state.load()) {
    case State::Live:
        return true;
    case State::Lost:
    case State::Reopening:
        return StandsIn();
    case State::Stopped:
        return false;
    }
    return false;
}

bool StandbySinkStream::IsStandingIn() const {
    const State current = state.load();
    return StandsIn() && !failed && (current == State::Lost || current == State::Reopening);
}

void StandbySinkStream::OpenInitialBackend() {
    void* const opened = OpenBackend(0);
    {
        std::scoped_lock lock{state_mutex};
        if (opened && losses == 0) {
            live_backend = opened;
            state = State::Live;
            return;
        }
        state = State::Lost;
    }
    // It did not open, or reported a loss while it did.
    if (opened) {
        DestroyBackend(opened);
    }
    if (!StandsIn()) {
        // Not ready, as the owners of input and sysmix streams expect of one that did not open.
        return;
    }
    LOG_WARNING(Audio_Sink, "Stream {} has no device yet: playing it into nothing until one opens",
                name);
    {
        std::scoped_lock lock{state_mutex};
        if (state == State::Lost && !worker.joinable()) {
            worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
        }
    }
    wake.notify_all();
}

void StandbySinkStream::BackendLost() {
    {
        std::scoped_lock lock{state_mutex};
        ++losses;
        if (state != State::Live) {
            // Already lost, finalized, or reported by a backend being reopened, which the reopen
            // then drops.
            return;
        }
        state = State::Lost;
        if (!StandsIn()) {
            LOG_WARNING(Audio_Sink, "Stream {} lost its device", name);
            return;
        }
        LOG_WARNING(Audio_Sink,
                    "Stream {} lost its device: playing it into nothing until it reopens", name);
        if (!worker.joinable()) {
            worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
        }
    }
    wake.notify_all();
}

void StandbySinkStream::Start(bool) {
    std::scoped_lock control{control_mutex};
    if (failed || !paused) {
        return;
    }
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        if (state != State::Live) {
            if (state == State::Stopped || !StandsIn()) {
                return;
            }
            // Standing in: play into nothing, and on the device once a backend reopens.
            paused = false;
            current = nullptr;
        } else {
            current = live_backend;
        }
    }
    if (!current) {
        wake.notify_all();
        return;
    }
    if (!StartBackend(current)) {
        LOG_ERROR(Audio_Sink, "Error starting stream {}", name);
        // A stream that stands in plays into nothing until it reopens; others stay not ready
        // and paused, as their owners expect of one that did not start.
        if (StandsIn()) {
            paused = false;
        }
        BackendLost();
        return;
    }
    paused = false;
}

void StandbySinkStream::Stop() {
    std::scoped_lock control{control_mutex};
    if (paused) {
        return;
    }
    SignalPause();
    State current_state;
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        current_state = state;
        current = live_backend;
    }
    if (current_state == State::Live && current && !StopBackend(current)) {
        LOG_ERROR(Audio_Sink, "Error stopping stream {}", name);
        BackendLost();
    }
}

void StandbySinkStream::Finalize() {
    // The worker releases and reopens the backend: join it before destroying that.
    const State before = StopWorker();
    std::scoped_lock control{control_mutex};
    const bool playing = !paused;
    // Always: a renderer waiting for queue space that no device or stand-in frees any more.
    SignalPause();
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        current = std::exchange(live_backend, nullptr);
    }
    if (current) {
        if (playing && before == State::Live) {
            StopBackend(current);
        }
        DestroyBackend(current);
    }
}

StandbySinkStream::State StandbySinkStream::StopWorker() {
    std::jthread joining;
    State before;
    {
        std::scoped_lock lock{state_mutex};
        before = state;
        state = State::Stopped;
        joining = std::move(worker);
    }
    if (joining.joinable()) {
        joining.request_stop();
        wake.notify_all();
        joining.join();
    }
    return before;
}

void StandbySinkStream::Run(std::stop_token stop_token) {
    Common::SetCurrentThreadName("AudioStandby");
#ifdef _WIN32
    // For the backend calls made here, once for the thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    stand_in_block.resize(TargetSampleCount * GetDeviceChannels());
    bool running = true;
    while (running) {
        {
            std::unique_lock lock{state_mutex};
            if (!wake.wait(lock, stop_token, [this] { return state == State::Lost; })) {
                break;
            }
        }
        if (failed) {
            // The owner's fault is final: neither stand in nor reopen, and pause the stream so
            // that nothing waits for queue space only the stand-in would free.
            std::scoped_lock control{control_mutex};
            SignalPause();
            break;
        }
        // Destroying the lost backend stops its callbacks: from here on only this thread
        // consumes the stream, until a new backend is started.
        ReleaseBackend();
        next_block = std::chrono::steady_clock::now();
        auto delay = timing.first_retry;
        for (u32 attempt = 1;; ++attempt) {
            if (!StandIn(stop_token, std::chrono::steady_clock::now() + delay)) {
                running = false;
                break;
            }
            if (failed) {
                // Faulted while standing in: stand down, and pause the stream so that nothing is
                // left waiting for queue space only the stand-in freed.
                std::scoped_lock control{control_mutex};
                SignalPause();
                running = false;
                break;
            }
            const Reopen result = TryReopen(attempt);
            if (result == Reopen::Opened) {
                break;
            }
            if (result == Reopen::Stopped) {
                running = false;
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
    std::scoped_lock control{control_mutex};
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        current = std::exchange(live_backend, nullptr);
    }
    if (current) {
        DestroyBackend(current);
    }
}

StandbySinkStream::Reopen StandbySinkStream::TryReopen(u32 attempt) {
    u64 seen{};
    {
        std::scoped_lock lock{state_mutex};
        if (state == State::Stopped) {
            return Reopen::Stopped;
        }
        state = State::Reopening;
        seen = losses;
    }
    ++attempts;
    // Without any lock: opening may block while the device comes and goes, and the emulation's
    // Start and Stop must not wait for it.
    void* const opened = system.IsShuttingDown() ? nullptr : OpenBackend(attempt);
    std::scoped_lock control{control_mutex};
    if (opened) {
        bool usable;
        {
            std::scoped_lock lock{state_mutex};
            usable = state == State::Reopening && losses == seen && !failed;
        }
        // Started before it is published, still standing in: the stream stays ready throughout.
        if (usable && (paused || StartBackend(opened))) {
            std::scoped_lock lock{state_mutex};
            if (state == State::Reopening && losses == seen) {
                live_backend = opened;
                state = State::Live;
                ++reopens;
                LOG_INFO(Audio_Sink, "Stream {} reopened after {} attempts{}", name, attempt,
                         paused ? ", paused" : "");
                return Reopen::Opened;
            }
        }
        // Finalized, faulted, not started, or the new backend reported a loss: drop it.
        DestroyBackend(opened);
    }
    std::scoped_lock lock{state_mutex};
    if (state == State::Reopening) {
        state = State::Lost;
    }
    return state == State::Stopped ? Reopen::Stopped : Reopen::Failed;
}

bool StandbySinkStream::StandIn(std::stop_token stop_token,
                                std::chrono::steady_clock::time_point until) {
    while (true) {
        if (stop_token.stop_requested()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= until || failed) {
            return true;
        }
        if (paused) {
            // As a stopped device would, play nothing until Start, which signals it.
            std::unique_lock lock{state_mutex};
            wake.wait_until(lock, stop_token, until, [this] { return !paused.load(); });
            next_block = std::chrono::steady_clock::now();
            continue;
        }
        if (now < next_block) {
            if (!Common::StoppableTimedWait(stop_token, (std::min)(next_block, until) - now)) {
                return false;
            }
            continue;
        }
        ProcessAudioOutAndRender(stand_in_block, TargetSampleCount);
        // The clock runs on across reopen attempts, so a slow open is made up for afterwards; but
        // after a host stall skip all but 20 blocks of it.
        next_block = (std::max)(next_block + BlockPeriod, now - 20 * BlockPeriod);
    }
}

} // namespace AudioCore::Sink
