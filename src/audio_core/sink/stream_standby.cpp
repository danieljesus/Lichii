// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <system_error>
#include <thread>
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

u32 Doublings(std::chrono::milliseconds from, std::chrono::milliseconds to) {
    u32 count = 0;
    for (auto delay = from; delay.count() > 0 && delay * 2 <= to; delay *= 2) {
        ++count;
    }
    return count;
}

s64 SteadyNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// For the backend calls made on the stream's own threads.
struct ComScope {
#ifdef _WIN32
    ComScope() : result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)} {}
    ~ComScope() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    HRESULT result;
#endif
};
} // Anonymous namespace

StandbySinkStream::DeviceChoice StandbySinkStream::ChooseDevice(u32 attempt, bool listed,
                                                                bool published_on_configured,
                                                                bool start_failed) {
    const bool may_take_default =
        attempt == 0 || attempt > ConfiguredDeviceAttempts || !published_on_configured;
    if (!listed) {
        return may_take_default ? DeviceChoice::Default : DeviceChoice::Wait;
    }
    if (!may_take_default) {
        return DeviceChoice::Configured;
    }
    return start_failed ? DeviceChoice::Default : DeviceChoice::ConfiguredThenDefault;
}

StandbySinkStream::StandbySinkStream(Core::System& system_, StreamType type_, Timing timing_)
    : SinkStream{system_, type_}, timing{timing_},
      max_stall_level{Doublings(timing_.first_retry, timing_.max_retry)} {}

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

bool StandbySinkStream::RenderFromBackend(const void* backend, std::span<s16> output,
                                          std::size_t num_frames) {
    // Counted before the check, so that once a stalled backend is unpublished and no callback is
    // counted, none plays the stream any more (RetireStalled).
    rendering.fetch_add(1);
    if (backend == nullptr || backend != live_backend.load()) {
        rendering.fetch_sub(1);
        // Opening or starting during a reopen, or unpublished as stalled, while the stand-in may
        // play the stream.
        std::fill(output.begin(), output.end(), s16{0});
        return false;
    }
    const s64 now = SteadyNow();
    if (!backend_asked.load(std::memory_order_acquire) &&
        now >= asks_from.load(std::memory_order_acquire)) {
        first_ask.store(now, std::memory_order_relaxed);
        backend_asked.store(true, std::memory_order_release);
        if (left_to_start.exchange(false)) {
            // It started after all: should it stall again, it is reopened once more.
            stall_level.store(max_stall_level, std::memory_order_relaxed);
        }
    } else if (const u32 level = stall_level.load(std::memory_order_relaxed); level != 0) {
        const auto sustained = timing.stall_check * StallThreshold(true, level);
        if (now - first_ask.load(std::memory_order_relaxed) >=
            std::chrono::duration_cast<std::chrono::nanoseconds>(sustained).count()) {
            stall_level.store(0, std::memory_order_relaxed);
        }
    }
    ProcessAudioOutAndRender(output, num_frames);
    rendering.fetch_sub(1);
    return true;
}

bool StandbySinkStream::IsStandingIn() const {
    const State current = state.load();
    return StandsIn() && !failed && (current == State::Lost || current == State::Reopening);
}

void StandbySinkStream::OpenInitialBackend() {
    void* const opened = OpenBackend(0);
    {
        // As a reopen publishes: a loss reported once it is live has the worker release it, which
        // waits for this lock, so it is not destroyed before BackendPublished.
        std::scoped_lock control{control_mutex};
        bool published{};
        {
            std::scoped_lock lock{state_mutex};
            published = opened && losses == 0;
            if (published) {
                backend_asked = false;
                asks_from = SteadyNow();
                RestartStallCount();
                live_backend = opened;
                state = State::Live;
            } else {
                state = State::Lost;
            }
        }
        if (published) {
            BackendPublished(opened);
            return;
        }
        // It did not open, or reported a loss while it did.
        if (opened) {
            DestroyBackend(opened);
        }
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

bool StandbySinkStream::BackendIdle(u32 idle_checks) {
    {
        // The check and the transition under one lock: a loss reported meanwhile finds the
        // stream lost already, or this finds it no longer live.
        std::scoped_lock lock{state_mutex};
        // A stand-in or a reopen under way plays the stream; a paused or faulted one is not
        // expected to be asked for audio.
        if (state != State::Live || !StandsIn() || paused || failed || system.IsShuttingDown()) {
            return false;
        }
        const bool asked = backend_asked.load();
        const u32 level = stall_level.load();
        if (idle_checks < StallThreshold(asked, level)) {
            return false;
        }
        if (level > max_stall_level) {
            // Reopened at every threshold already, and slower than all of them: leave it to
            // start, or to report an error, rather than reopen it for ever.
            if (!left_to_start.exchange(true)) {
                LOG_WARNING(Audio_Sink,
                            "Stream {}: the backend has not asked for audio after {} reopens; "
                            "leaving it to start",
                            name, max_stall_level + 1);
            }
            return false;
        }
        stall_level = level + 1;
        state = State::Lost;
        // Unpublished now, so that its callbacks, should they come back, play silence; the worker
        // destroys it off the locks, as it may not respond.
        stalled_backend = live_backend.exchange(nullptr);
        const auto idle_ms = timing.stall_check.count() * idle_checks;
        if (asked) {
            LOG_WARNING(Audio_Sink,
                        "Stream {}: the backend stopped asking for audio {} ms ago; reopening",
                        name, idle_ms);
        } else {
            LOG_WARNING(Audio_Sink,
                        "Stream {}: the backend has not asked for audio in the {} ms since it "
                        "started; reopening",
                        name, idle_ms);
        }
        if (!worker.joinable()) {
            worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
        }
    }
    wake.notify_all();
    return true;
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
    // Not once a close has marked the stream stopped and paused it during StartBackend: a renderer
    // would wait for queue space nothing frees.
    const auto unpause = [this] {
        std::scoped_lock lock{state_mutex};
        if (state != State::Stopped) {
            paused = false;
        }
    };
    // A started backend may take a while to ask for audio again: it gets the grace of one that
    // has not asked yet. Only a request made once it has started counts: a callback trailing the
    // stop, which a backend can still make, came before.
    asks_from = std::numeric_limits<s64>::max();
    backend_asked = false;
    RestartStallCount();
    if (!StartBackend(current)) {
        LOG_ERROR(Audio_Sink, "Error starting stream {}", name);
        // A stream that stands in plays into nothing until it reopens; others stay not ready
        // and paused, as their owners expect of one that did not start.
        if (StandsIn()) {
            unpause();
        }
        BackendLost();
        return;
    }
    asks_from = SteadyNow();
    unpause();
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
    const bool playing = !paused;
    // The worker releases and reopens the backend: join it before destroying that. It pauses the
    // stream first, so a renderer is not left waiting for an open under way.
    const State before = StopWorker();
    std::scoped_lock control{control_mutex};
    void* current{};
    void* stalled{};
    {
        std::scoped_lock lock{state_mutex};
        current = live_backend.exchange(nullptr);
        stalled = std::exchange(stalled_backend, nullptr);
    }
    if (current) {
        if (playing && before == State::Live) {
            StopBackend(current);
        }
        DestroyBackend(current);
    }
    if (stalled) {
        // Found stalled just before the close, so the worker never took it. Its destroy may
        // block, as a close already waits for an open under way.
        DestroyBackend(stalled);
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
    SignalPause();
    if (joining.joinable()) {
        joining.request_stop();
        wake.notify_all();
        joining.join();
    }
    return before;
}

void StandbySinkStream::Run(std::stop_token stop_token) {
    Common::SetCurrentThreadName("AudioStandby");
    [[maybe_unused]] ComScope com;
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
        // consumes the stream, until a new backend is started. A stalled one may not respond, so
        // it is destroyed on a thread of its own while this one stands in.
        void* stalled{};
        {
            std::scoped_lock lock{state_mutex};
            stalled = std::exchange(stalled_backend, nullptr);
        }
        if (stalled) {
            if (!RetireStalled(stop_token, stalled)) {
                break;
            }
        } else {
            ReleaseBackend();
        }
        next_block = std::chrono::steady_clock::now();
        auto delay = timing.first_retry;
        for (u32 attempt = 1;; ++attempt) {
            if (!StandIn(stop_token, std::chrono::steady_clock::now() + delay)) {
                running = false;
                break;
            }
            // No other open while a stalled backend is being destroyed: its device's driver may
            // be what holds it, and an error it reports on the way must come before the reopen
            // counts losses. The stand-in plays on meanwhile.
            if (!retired && !StandIn(stop_token, std::chrono::steady_clock::time_point::max(),
                                     &retired)) {
                running = false;
                break;
            }
            if (retired && retiring.joinable()) {
                retiring.join();
            }
            if (failed) {
                // Faulted while standing in: stand down, and pause the stream so that nothing is
                // left waiting for queue space only the stand-in freed.
                std::scoped_lock control{control_mutex};
                SignalPause();
                running = false;
                break;
            }
            const Reopen result = TryReopen(stop_token, attempt);
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
    // A destroy that never returns holds the close here, as an open under way does.
    if (retiring.joinable()) {
        retiring.join();
    }
}

void StandbySinkStream::ReleaseBackend() {
    std::scoped_lock control{control_mutex};
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        current = live_backend.exchange(nullptr);
    }
    if (current) {
        DestroyBackend(current);
    }
}

bool StandbySinkStream::RetireStalled(std::stop_token stop_token, void* backend) {
    // Unpublished under state_mutex before this: a callback counted before that may still be
    // playing the stream, and none after it will. The stand-in cannot play the stream until it
    // ends, as the stream has one consumer at a time; it runs only this stream's code, so it ends
    // within a block unless its thread is held, as by a debugger. Wait for it asleep, and give up
    // only when stopping, leaving the backend to Finalize.
    const auto warn_at = std::chrono::steady_clock::now() + timing.stall_check * StallChecks;
    bool warned = false;
    while (rendering.load() != 0) {
        if (!warned && std::chrono::steady_clock::now() >= warn_at) {
            warned = true;
            LOG_WARNING(Audio_Sink,
                        "Stream {}: a callback of its stalled backend is still playing it; waiting "
                        "for it before standing in",
                        name);
        }
        if (!Common::StoppableTimedWait(stop_token, std::chrono::milliseconds{1})) {
            std::scoped_lock lock{state_mutex};
            stalled_backend = backend;
            return false;
        }
    }
    retired = false;
    const auto destroy = [this, backend] {
        DestroyBackend(backend);
        {
            std::scoped_lock lock{state_mutex};
            retired = true;
        }
        wake.notify_all();
    };
    try {
        retiring = std::jthread([destroy] {
            Common::SetCurrentThreadName("AudioRetire");
            [[maybe_unused]] ComScope com;
            destroy();
        });
    } catch (const std::system_error& error) {
        LOG_ERROR(Audio_Sink, "Stream {} could not start a thread to destroy its backend: {}",
                  name, error.what());
        destroy();
    }
    return true;
}

StandbySinkStream::Reopen StandbySinkStream::TryReopen(std::stop_token stop_token, u32 attempt) {
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
    void* opened{};
    std::atomic<bool> open_done{};
    std::jthread opener;
    try {
        // On a thread of its own, without any lock: opening may block while the device comes and
        // goes, and the stand-in plays on meanwhile. The backend consumes the stream only once it
        // is published.
        opener = std::jthread([this, attempt, &opened, &open_done] {
            Common::SetCurrentThreadName("AudioOpener");
            [[maybe_unused]] ComScope com;
            void* const result = system.IsShuttingDown() ? nullptr : OpenBackend(attempt);
            {
                std::scoped_lock lock{state_mutex};
                opened = result;
                open_done = true;
            }
            wake.notify_all();
        });
    } catch (const std::system_error& error) {
        // No thread for the open: try again later, standing in meanwhile.
        LOG_ERROR(Audio_Sink, "Stream {} could not start a thread to reopen: {}", name,
                  error.what());
    }
    if (opener.joinable()) {
        StandIn(stop_token, std::chrono::steady_clock::time_point::max(), &open_done);
        if (!open_done && failed) {
            // Faulted with the open under way: nothing frees queue space while the opener is
            // joined. A close has paused the stream already.
            std::scoped_lock control{control_mutex};
            SignalPause();
        }
        opener.join();
    }
    std::scoped_lock control{control_mutex};
    if (opened) {
        // Not finalized, faulted, or lost again by the new backend. Checked again once it starts.
        const auto usable = [this, seen] {
            std::scoped_lock lock{state_mutex};
            return state == State::Reopening && losses == seen && !failed;
        };
        // Started before it is published, still standing in: the stream stays ready throughout.
        if (usable() && !system.IsShuttingDown() && (paused || StartBackend(opened))) {
            bool published{};
            {
                std::scoped_lock lock{state_mutex};
                published = state == State::Reopening && losses == seen && !failed;
                if (published) {
                    // The checks a waiting renderer counted while this one opened and started,
                    // during which nothing played the stream, are not its own.
                    backend_asked = false;
                    asks_from = SteadyNow();
                    RestartStallCount();
                    live_backend = opened;
                    state = State::Live;
                }
            }
            if (published) {
                BackendPublished(opened);
                ++reopens;
                LOG_INFO(Audio_Sink, "Stream {} reopened after {} attempts{}", name, attempt,
                         paused ? ", paused" : "");
                return Reopen::Opened;
            }
        }
        // Closing, shutting down, faulted, not started, or the new backend reported a loss: drop
        // it.
        DestroyBackend(opened);
    }
    std::scoped_lock lock{state_mutex};
    if (state == State::Reopening) {
        state = State::Lost;
    }
    return state == State::Stopped ? Reopen::Stopped : Reopen::Failed;
}

bool StandbySinkStream::StandIn(std::stop_token stop_token,
                                std::chrono::steady_clock::time_point until,
                                const std::atomic<bool>* done) {
    const auto is_done = [done] { return done && done->load(); };
    while (true) {
        if (stop_token.stop_requested()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= until || failed || is_done()) {
            return true;
        }
        if (paused) {
            // As a stopped device would, play nothing until Start, which signals it.
            std::unique_lock lock{state_mutex};
            wake.wait_until(lock, stop_token, (std::min)(until, now + std::chrono::seconds{1}),
                            [this, &is_done] { return !paused.load() || is_done(); });
            next_block = std::chrono::steady_clock::now();
            continue;
        }
        if (now < next_block) {
            if (!Common::StoppableTimedWait(stop_token, (std::min)(next_block, until) - now)) {
                return false;
            }
            continue;
        }
        {
            // Under the lock Stop holds as it pauses: no block plays after Stop returns, when the
            // owner may clear the queue.
            std::scoped_lock control{control_mutex};
            if (paused) {
                continue;
            }
            ProcessAudioOutAndRender(stand_in_block, TargetSampleCount);
        }
        // The clock runs on while a backend starts, so that time is made up for afterwards; but
        // after a host stall skip all but 20 blocks of it.
        next_block = (std::max)(next_block + BlockPeriod, now - 20 * BlockPeriod);
    }
}

} // namespace AudioCore::Sink
