// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "common/common_types.h"
#include "video_core/gpu_trace/map_log.h"

namespace Core {
class System;
}
namespace Service::Nvidia {
struct NvFence;
}
namespace Tegra {
class MemoryManager;
struct CommandList;
struct FramebufferConfig;
} // namespace Tegra

namespace VideoCommon::GpuTrace {

/// From a request until the recording ends: the GPU thread calls OnCommand.
inline std::atomic<bool> g_requested{false};
/// While a recording runs (from its snapshot): the Note* hooks record.
inline std::atomic<bool> g_recording{false};
/// EDEN_NX2_GPU_TRACE=<dir>: a recording starts when <dir>/record.now appears.
inline const bool g_file_trigger = std::getenv("EDEN_NX2_GPU_TRACE") != nullptr;

struct RecordOptions {
    std::filesystem::path path;
    /// The recording ends after this many presents (180: 3 s at 60 fps).
    u32 presents = 180;
};

/// Asks the GPU thread to start recording at its next command with no suspended channel. Any
/// thread. False with `error` set while a recording is pending or running.
bool RequestRecording(const RecordOptions& options, std::string& error);

/// Ends the recording at the GPU thread's next command; a pending one is dropped.
void StopRecording();

struct RecorderStatus {
    bool requested{};
    bool recording{};
    u32 presents{};
    u32 presents_wanted{};
    u64 bytes{};
    std::string path;
    std::string last_trace;
    std::string last_error;
};
[[nodiscard]] RecorderStatus GetRecorderStatus();

/// The guest CPU wrote [pointer, pointer + size) of guest physical memory. Call after the store.
void NoteCpuWrite(const u8* pointer, size_t size);
/// The same, by device address, after a service wrote the range (GPU::InvalidateRegion).
void NoteDeviceWrite(DAddr address, u64 size);

/// The GPU thread stopped: writes what was recorded and releases the recorder.
void OnGpuShutdown();

void NoteGmmuMap(const Tegra::MemoryManager& memory_manager, const MapLog::Entry& entry);
void NoteGmmuUnmap(const Tegra::MemoryManager& memory_manager, GPUVAddr gpu_address, u64 size);

/// GPU::RequestComposite was asked for this present: it is recorded at the GPU tick after it.
void NotePresent(std::span<const Tegra::FramebufferConfig> layers,
                 std::span<const Service::Nvidia::NvFence> fences);

/// The GPU thread popped a command and is about to run it: `submit` for a command list,
/// `tick` for a GPU tick (which runs presents). Takes the snapshot when a recording is due and
/// records the memory the CPU wrote since the last command, then the command.
void OnCommand(Core::System& system, bool channels_pending, const Tegra::CommandList* submit,
               s32 channel, bool tick);

/// A requested recording waits for its snapshot, which needs a command boundary with no suspended
/// channel.
[[nodiscard]] inline bool WaitsForSnapshot() {
    return g_requested.load(std::memory_order_relaxed) &&
           !g_recording.load(std::memory_order_relaxed);
}

/// Whether the GPU thread holds back new commands so its suspended channels can drain for the
/// snapshot. Games that keep two channels waiting on each other (Ocarina of Time: one on a
/// semaphore, the other on a list copy) have one suspended at every command boundary while new
/// commands keep coming, so the snapshot never came. Held for at most HoldLimit at a time, since a
/// suspended channel may wait for a command still in the queue; then commands run for Backoff
/// before the next attempt. GPU thread only.
class SnapshotHold {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds HoldLimit{250};
    static constexpr std::chrono::milliseconds Backoff{1000};

    [[nodiscard]] bool Hold(Clock::time_point now, bool waits_for_snapshot) {
        if (!waits_for_snapshot) {
            holding_since.reset();
            resume_after = {};
            return false;
        }
        if (now < resume_after) {
            return false;
        }
        if (!holding_since) {
            holding_since = now;
        }
        if (now - *holding_since < HoldLimit) {
            return true;
        }
        holding_since.reset();
        resume_after = now + Backoff;
        return false;
    }

private:
    std::optional<Clock::time_point> holding_since;
    Clock::time_point resume_after{};
};

} // namespace VideoCommon::GpuTrace
