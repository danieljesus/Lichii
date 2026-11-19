// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace AudioCore::Sink {

class SinkStream;

/**
 * Stands in for a stream's host device after the backend lost it, such as a headset behind a
 * switch that went to another computer while Windows had no default device to move it to.
 *
 * Until the backend is back, the stream is played into nothing (or fed silence) in real time, as a
 * device's callback would, so the audio renderer stays paced and never waits forever for queue
 * space, which also blocked emulation shutdown. The backend is reopened with backoff.
 *
 * Only this worker consumes the stream while it stands in: the backend is released before the
 * first block is played, and reopen starts the new backend after the last one.
 */
class StreamStandby {
public:
    struct Timing {
        std::chrono::milliseconds first_retry{500};
        std::chrono::milliseconds max_retry{4000};
    };

    /// release drops the lost backend; reopen opens a new one and starts it unless the stream is
    /// paused, returning whether it is playing. Both run on the worker thread.
    StreamStandby(SinkStream& stream, std::function<void()> release, std::function<bool()> reopen,
                  Timing timing);
    StreamStandby(SinkStream& stream, std::function<void()> release, std::function<bool()> reopen)
        : StreamStandby(stream, std::move(release), std::move(reopen), Timing{}) {}
    ~StreamStandby();

    StreamStandby(const StreamStandby&) = delete;
    StreamStandby& operator=(const StreamStandby&) = delete;

    /// The backend lost its device. Takes no lock the backend holds: callable from its callbacks.
    void Lost();

    /// Whether the stream is standing in for a lost backend.
    bool IsActive() const {
        return active.load();
    }

    /// Stop standing in and join the worker. Must run before the backend is destroyed.
    void Shutdown();

    u64 ReopenAttempts() const {
        return attempts.load();
    }
    u64 Reopens() const {
        return reopens.load();
    }

private:
    void Run(std::stop_token stop_token);
    /// Play or feed one block while the stream is not paused. Returns false when stopping.
    bool StandIn(std::stop_token stop_token, std::chrono::steady_clock::time_point until);

    SinkStream& stream;
    std::function<void()> release;
    std::function<bool()> reopen;
    Timing timing;
    std::atomic<bool> active{};
    std::atomic<u64> losses{};
    std::atomic<u64> attempts{};
    std::atomic<u64> reopens{};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread worker;
};

} // namespace AudioCore::Sink
