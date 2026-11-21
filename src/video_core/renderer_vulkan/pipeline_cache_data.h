// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
    /// VK_SUCCESS: all of it. VK_INCOMPLETE with bytes: part of it, a valid cache by the spec
    /// that lacks what was built during the reads; without bytes, nothing usable. Otherwise the
    /// error a call returned.
    VkResult result = VK_SUCCESS;
    int reads = 0;

    [[nodiscard]] bool Usable() const {
        return result == VK_SUCCESS || (result == VK_INCOMPLETE && size != 0);
    }
};

/// Reads a driver pipeline cache through `read` (vkGetPipelineCacheData). Pipelines may build into
/// it between the size query and the read; the size is then stale and the read returns
/// VK_INCOMPLETE. A conforming driver gives back a smaller valid cache then, and MoltenVK none at
/// all. A larger buffer than the size query gave does not help on MoltenVK: a build marks its
/// cache dirty, which zeroes the size it serializes into, so any buffer gets VK_INCOMPLETE and no
/// bytes until the size is asked for again (asked 511 bytes, a pipeline built, a read into 4607
/// returned 0). So the size is asked for again before each read, and a whole read is taken at
/// once; when every read was overtaken, the last part read is taken, and with none the result is
/// not usable. `progress` is called after each read: a read serializes the whole cache, gigabytes
/// for a large one, and a shutdown waits on it. Each read gets its own buffer, left
/// uninitialized, so no stale bytes are copied; a part kept from an earlier read is held beside
/// it until the next one is known.
template <typename Read, typename Progress>
PipelineCacheData ReadPipelineCacheData(Read&& read, Progress&& progress) {
    PipelineCacheData partial{.result = VK_INCOMPLETE};
    for (int attempt = 1; attempt <= PipelineCacheReadAttempts; ++attempt) {
        size_t size = 0;
        if (const VkResult result = read(&size, nullptr); result != VK_SUCCESS) {
            return {.result = result, .reads = attempt};
        }
        if (size == 0) {
            return {.reads = attempt};
        }
        const size_t asked = size;
        auto bytes = std::make_unique_for_overwrite<char[]>(size);
        const VkResult result = read(&size, bytes.get());
        progress();
        if (result == VK_SUCCESS) {
            return {std::move(bytes), size, VK_SUCCESS, attempt};
        }
        if (result != VK_INCOMPLETE) {
            return {.result = result, .reads = attempt};
        }
        LOG_DEBUG(Render_Vulkan,
                  "Driver pipeline cache changed after its size ({} bytes) was read, read {} of {} "
                  "got {} bytes",
                  asked, attempt, PipelineCacheReadAttempts, size);
        if (size >= MinPipelineCacheDataSize) {
            partial = {std::move(bytes), size, VK_INCOMPLETE, attempt};
        }
    }
    partial.reads = PipelineCacheReadAttempts;
    return partial;
}

} // namespace Vulkan
