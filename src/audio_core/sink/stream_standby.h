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
 * A sink stream whose backend can lose its device, such as a headset behind a switch that went to
 * another computer while Windows had no default device to move the stream to. Backends that give
 * up on a lost device, as cubeb does, implement the hooks below.
 *
 * A renderer or audio out stream stands in for the device until a backend opens again: it is
 * played into nothing in real time, as a device's callback would, so the audio renderer stays
 * paced and never waits forever for queue space, which also blocked emulation shutdown. Their
 * owners never check that a stream is ready, so a dead one left the game silent until restarted.
 * The backend is reopened with backoff, on the default device once the configured one has failed a
 * few times, and started again unless the owner paused the stream meanwhile.
 *
 * Input and sysmix streams do not stand in: their owners check IsReady and fall back to silence or
 * fail the guest's open, so a loss leaves them not ready, as it always did. A fault the owner
 * reports with MarkFaulted, as sysmix does for a guest fault, is final for every stream.
 *
 * Only one backend exists at a time: the lost one is destroyed, which stops its callbacks, before
 * the stand-in plays a block or a new one opens. So a loss reported while a reopen is under way
 * comes from the new backend, which is then dropped instead of published.
 *
 * The derived constructor calls OpenInitialBackend last, and the derived destructor must call
 * Finalize first: the worker calls the hooks, so it is joined there, while the derived stream is
 * still whole. The base destructor asserts that it was.
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
    /// A renderer or audio out stream stays ready while it stands in for a lost device.
    bool IsReady() const override;

protected:
    StandbySinkStream(Core::System& system_, StreamType type_, Timing timing_);
    StandbySinkStream(Core::System& system_, StreamType type_)
        : StandbySinkStream(system_, type_, Timing{}) {}

    /// Open a backend, or return null. attempt is 0 for the stream's first open, then numbers the
    /// reopens since the device was lost from 1. Called without any of the stream's locks, since it
    /// may block.
    virtual void* OpenBackend(u32 attempt) = 0;
    virtual bool StartBackend(void* backend) = 0;
    virtual bool StopBackend(void* backend) = 0;
    virtual void DestroyBackend(void* backend) = 0;

    /// Open the first backend. Renderer and audio out streams with no device yet stand in.
    void OpenInitialBackend();

    /// The backend lost its device. Takes no lock held around backend calls, so its callbacks can
    /// report it. Ignored once the stream is finalized.
    void BackendLost();

    /// For tests: standing in for a lost device, and the reopen counts.
    bool IsStandingIn() const;
    u64 ReopenAttempts() const {
        return attempts.load();
    }
    u64 Reopens() const {
        return reopens.load();
    }

private:
    enum class State : u8 {
        Live,      ///< The published backend plays the stream.
        Lost,      ///< No usable backend; a renderer or audio out stream stands in.
        Reopening, ///< The worker is opening a backend. Nothing plays while it opens; the
                   ///< stand-in makes up for the time afterwards.
        Stopped,   ///< Never opened (no backend context), or finalized.
    };
    enum class Reopen { Opened, Failed, Stopped };

    bool StandsIn() const {
        return type == StreamType::Render || type == StreamType::Out;
    }
    void Run(std::stop_token stop_token);
    /// Play blocks into nothing until `until`. Returns false when stopping. Worker thread only.
    bool StandIn(std::stop_token stop_token, std::chrono::steady_clock::time_point until);
    void ReleaseBackend();
    Reopen TryReopen(u32 attempt);
    /// Stop and join the worker, and return the state before.
    State StopWorker();

    Timing timing;

    /// Serializes Start, Stop, Finalize and the worker's release and publication: held around
    /// backend calls, except OpenBackend.
    std::mutex control_mutex;
    /// Guards the fields below. Never held around a backend call.
    mutable std::mutex state_mutex;
    std::condition_variable_any wake;
    std::atomic<State> state{State::Stopped};
    void* live_backend{};
    /// Losses reported, so that a reopen can tell its new backend reported one.
    u64 losses{};
    /// The worker starts on the first loss of a stream that stands in.
    std::jthread worker;

    /// Worker only: the block it plays, and when the next one is due.
    std::vector<s16> stand_in_block;
    std::chrono::steady_clock::time_point next_block{};

    std::atomic<u64> attempts{};
    std::atomic<u64> reopens{};
};

} // namespace AudioCore::Sink
