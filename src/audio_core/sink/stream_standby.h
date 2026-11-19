// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "audio_core/sink/sink_stream.h"
#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {

/**
 * A sink stream that stands in for its host device after the backend lost it, such as a headset
 * behind a switch that went to another computer while Windows had no default device to move the
 * stream to. Backends that give up on a lost device, as cubeb does, implement the hooks below.
 *
 * Until a backend opens again, the stream is played into nothing (or fed silence) in real time, as
 * a device's callback would, so the audio renderer stays paced and never waits forever for queue
 * space, which also blocked emulation shutdown. The backend is reopened with backoff, on the
 * default device once the configured one has failed a few times, and started again unless its
 * owner paused the stream meanwhile. A fault its owner reports with MarkFaulted, as sysmix does
 * for a guest fault, stays final.
 *
 * Only one thread consumes the stream: the lost backend is destroyed, which stops its callbacks,
 * before the standby plays a block, and it plays none once a new backend is published.
 *
 * The derived constructor calls OpenInitialBackend last, and the derived destructor must call
 * Finalize first: the standby worker calls the hooks, so it is joined there, while the derived
 * stream is still whole. The base destructor asserts that it was.
 */
class StandbySinkStream : public SinkStream {
public:
    struct Timing {
        std::chrono::milliseconds first_retry{500};
        std::chrono::milliseconds max_retry{4000};
    };

    /// Reopens on a configured device before the backend should try the default one.
    static constexpr u32 ConfiguredDeviceAttempts = 3;

    ~StandbySinkStream() override;

    void Start(bool resume = false) override;
    void Stop() override;
    void Finalize() override;
    /// A stream whose device was lost stays ready: it plays into nothing until it reopens.
    bool IsReady() const override;

    bool IsStandingIn() const {
        return active.load();
    }
    u64 ReopenAttempts() const {
        return attempts.load();
    }
    u64 Reopens() const {
        return reopens.load();
    }

protected:
    StandbySinkStream(Core::System& system_, StreamType type_, Timing timing_);
    StandbySinkStream(Core::System& system_, StreamType type_)
        : StandbySinkStream(system_, type_, Timing{}) {}

    /// Open a backend, or return null. attempt is 0 for the stream's first open, then numbers the
    /// reopens since the device was lost from 1. Called without the backend lock, since it may
    /// block.
    virtual void* OpenBackend(u32 attempt) = 0;
    virtual bool StartBackend(void* backend) = 0;
    virtual bool StopBackend(void* backend) = 0;
    virtual void DestroyBackend(void* backend) = 0;

    /// Open the first backend. A renderer or audio out stream with no device yet stands in until
    /// one opens, since their owners do not check for one. Sysmix and input streams stay not ready,
    /// as their owners fail the open or fall back to silence themselves.
    void OpenInitialBackend();

    /// The backend lost its device. Takes no lock held around backend calls, so its callbacks can
    /// report it. Ignored once the stream is finalized.
    void BackendLost();

private:
    enum class Reopen { Opened, Failed, Abandoned };

    void Run(std::stop_token stop_token);
    /// Play or feed blocks until `until`. Returns false when stopping. Worker thread only.
    bool StandIn(std::stop_token stop_token, std::chrono::steady_clock::time_point until);
    void ReleaseBackend();
    Reopen TryReopen(u32 attempt);
    void StopStandby();

    Timing timing;

    /// Serializes the backend's start, stop, release and publication.
    std::mutex backend_mutex;
    /// Written under backend_mutex; null while standing in.
    std::atomic<void*> published{};
    /// The current backend reported a loss and must not be started or stopped any more.
    std::atomic<bool> backend_broken{};

    std::atomic<bool> active{};
    /// The block the worker plays or feeds, sized when it starts.
    std::vector<s16> stand_in_block;
    std::atomic<u64> attempts{};
    std::atomic<u64> reopens{};

    std::mutex standby_mutex;
    std::condition_variable_any wake;
    /// Guarded by standby_mutex. The worker starts on the first loss.
    bool stopped{};
    std::jthread worker;
};

} // namespace AudioCore::Sink
