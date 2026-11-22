// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

#include "common/logging.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

/// Reads of a driver pipeline cache a save makes, each after its own size query, before it settles
/// for what it got.
inline constexpr int PipelineCacheReadAttempts = 4;

/// Less than this is no cache: by the spec, a buffer smaller than the version one header gets no
/// bytes at all.
inline constexpr size_t MinPipelineCacheDataSize = sizeof(VkPipelineCacheHeaderVersionOne);

/// Driver pipeline cache data read for a save.
struct PipelineCacheData {
    std::unique_ptr<char[]> bytes;
    size_t size = 0;
    /// VK_SUCCESS: all of it, or, with less than a header, a size that holds no cache.
    /// VK_INCOMPLETE: with more than a header, part of it, a valid cache by the spec that lacks
    /// what was built during the read; with no more, nothing usable after every read. Otherwise
    /// the error a call returned.
    VkResult result = VK_SUCCESS;
    int reads = 0;

    /// All of the cache.
    [[nodiscard]] bool Whole() const {
        return result == VK_SUCCESS && size >= MinPipelineCacheDataSize;
    }
    /// Part of it: a header and at least some of what follows.
    [[nodiscard]] bool Partial() const {
        return result == VK_INCOMPLETE && size > MinPipelineCacheDataSize;
    }
    /// Worth saving over the file: a cache holds a header at least, and a live one never less.
    [[nodiscard]] bool Usable() const {
        return Whole() || Partial();
    }
};

/// Reads a driver pipeline cache through `read` (vkGetPipelineCacheData). Pipelines may build into
/// it between the size query and the read; the size is then stale and the read returns
/// VK_INCOMPLETE. A conforming driver gives back a smaller valid cache then, which is taken at
/// once, as Lichii always did: reading again would serialize the whole cache again for a result
/// as partial. MoltenVK gives none at all, and a larger buffer than the size query gave does not
/// help there: a build marks its cache dirty, which zeroes the size it serializes into, so any
/// buffer gets VK_INCOMPLETE and no bytes until the size is asked for again (asked 511 bytes, a
/// pipeline built, a read into 4607 returned 0). So the size is asked for again, and the cache
/// read again, only after a read that got nothing usable, up to PipelineCacheReadAttempts reads.
/// `progress` is called after each read: a read serializes the whole cache, gigabytes for a large
/// one, and a shutdown waits on it. The buffer is left uninitialized, and one is held at a time.
template <typename Read, typename Progress>
PipelineCacheData ReadPipelineCacheData(Read&& read, Progress&& progress) {
    for (int attempt = 1; attempt <= PipelineCacheReadAttempts; ++attempt) {
        size_t size = 0;
        if (const VkResult result = read(&size, nullptr); result != VK_SUCCESS) {
            return {.result = result, .reads = attempt};
        }
        // Less than a header is no cache: a live one always holds one.
        if (size < MinPipelineCacheDataSize) {
            return {.size = size, .reads = attempt};
        }
        const size_t asked = size;
        auto bytes = std::make_unique_for_overwrite<char[]>(size);
        const VkResult result = read(&size, bytes.get());
        progress();
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return {.result = result, .reads = attempt};
        }
        PipelineCacheData data{std::move(bytes), size, result, attempt};
        if (data.Usable() || result == VK_SUCCESS) {
            return data;
        }
        LOG_DEBUG(Render_Vulkan,
                  "Driver pipeline cache changed after its size ({} bytes) was read, read {} of {} "
                  "got {} bytes",
                  asked, attempt, PipelineCacheReadAttempts, size);
    }
    return {.result = VK_INCOMPLETE, .reads = PipelineCacheReadAttempts};
}

/// The timed flush of the driver pipeline cache: not sooner than `interval` after the last save
/// that worked (`last_saved`; the interval grows with the cache), nor than
/// PipelineCacheFlushRetryFloor after the last attempt (`last_attempt`), so a flush that fails
/// (a build overtook every read, a full disk) is not queued again as soon as it ends, though it
/// does not wait out the whole interval. A default time point is "never".
inline constexpr std::chrono::seconds PipelineCacheFlushRetryFloor{30};
[[nodiscard]] inline bool PipelineCacheFlushDue(std::chrono::steady_clock::time_point now,
                                                std::chrono::steady_clock::time_point last_saved,
                                                std::chrono::steady_clock::time_point last_attempt,
                                                std::chrono::seconds interval) {
    using TimePoint = std::chrono::steady_clock::time_point;
    if (last_saved != TimePoint{} && now - last_saved < interval) {
        return false;
    }
    return last_attempt == TimePoint{} || now - last_attempt >= PipelineCacheFlushRetryFloor;
}

} // namespace Vulkan
