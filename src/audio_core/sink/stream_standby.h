// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

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
 * owner paused the stream meanwhile. A fault its owner reports with MarkFaulted stays final.
 *
 * Only one thread consumes the stream: the lost backend is destroyed, which stops its callbacks,
 * before the standby plays a block, and it plays none once a new backend is published.
 *
 * The derived constructor calls OpenInitialBackend last, and the derived destructor calls
 * Finalize first: the standby worker calls the hooks.
 */
class StandbySinkStream : public SinkStream {
public:
    struct Timing {
        std::chrono::milliseconds first_retry{500};
        std::chrono::milliseconds max_retry{4000};
    };

    /// Failed reopens on a configured device before the backend should try the default one.
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

    /// Open a backend, or return null. attempt counts the failed reopens since the device was
    /// lost, 0 for the first open. Called without the backend lock, since it may block.
    virtual void* OpenBackend(u32 attempt) = 0;
    virtual bool StartBackend(void* backend) = 0;
    virtual bool StopBackend(void* backend) = 0;
    virtual void DestroyBackend(void* backend) = 0;

    /// Open the first backend. An output stream with no device yet stands in until one opens.
    void OpenInitialBackend();

    /// The backend lost its device. Takes no lock held around backend calls, so its callbacks can
    /// report it. Ignored once the stream is finalized.
    void BackendLost();

private:
    enum class Reopen { Opened, Failed, Abandoned };

    void Run(std::stop_token stop_token);
    /// Play or feed blocks until `until`. Returns false when stopping.
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
    std::atomic<u64> attempts{};
    std::atomic<u64> reopens{};

    std::mutex standby_mutex;
    std::condition_variable_any wake;
    /// Guarded by standby_mutex. The worker starts on the first loss.
    bool stopped{};
    std::jthread worker;
};

} // namespace AudioCore::Sink
