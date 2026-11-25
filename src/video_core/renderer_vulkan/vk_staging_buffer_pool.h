// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <climits>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_stream_ring.h"

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct StagingBufferRef {
    VkBuffer buffer;
    VkDeviceAddress device_address;
    VkDeviceSize offset;
    std::span<u8> mapped_span;
    MemoryUsage usage;
    u32 log2_level;
    u64 index;
};

class StagingBufferPool {
public:
    static constexpr size_t NUM_SYNCS = 16;

    explicit StagingBufferPool(const Device& device, MemoryAllocator& memory_allocator,
                               Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferRef Request(size_t size, MemoryUsage usage, bool deferred = false);
    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] VkBuffer StreamBuf() const noexcept {
        return *stream_buffer;
    }

    void TickFrame();

    /// Out of device memory: waits for the submitted work without submitting, then frees every
    /// staging buffer the GPU is done with. Those handed out since the last submit stay.
    void ReleaseMemory();

    /// Bytes of the cached staging buffers of a kind, idle or in use (EDEN_NX2_MEM_TRACE).
    [[nodiscard]] u64 CachedBytes(MemoryUsage usage);

private:
    struct StreamBufferCommit {
        size_t upper_bound;
        u64 tick;
    };

    struct StagingBuffer {
        vk::Buffer buffer;
        VkDeviceAddress device_address;
        std::span<u8> mapped_span;
        MemoryUsage usage;
        u32 log2_level;
        u64 index;
        u64 tick = 0;

        StagingBufferRef Ref() const noexcept {
            return {
                .buffer = *buffer,
                .device_address = device_address,
                .offset = 0,
                .mapped_span = mapped_span,
                .usage = usage,
                .log2_level = log2_level,
                .index = index,
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        /// Handed out deferred, by index, until FreeDeferred: lookups don't scan them. Tens of
        /// thousands of copies of GPU-written command list pages stay held in Hyrule Warriors:
        /// Age of Imprisonment fights, which made each dispatch's copies scan them all.
        std::unordered_map<u64, StagingBuffer> deferred;
        size_t delete_index = 0;
        size_t iterate_index = 0;
    };

    static constexpr size_t NUM_LEVELS = sizeof(size_t) * CHAR_BIT;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    StagingBufferRef GetStreamBuffer(size_t size);


    /// TEST ONLY (EDEN_NX2_TEST_STREAM_LOG): records a stream range handed out in `tick` and logs
    /// any overlap with a range whose tick the GPU has not completed yet.
    void TestNoteStream(size_t offset, size_t size, u64 tick);

    StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred = false);

    std::optional<StagingBufferRef> TryGetReservedBuffer(size_t size, MemoryUsage usage,
                                                         bool deferred);

    StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred);

    StagingBuffersCache& GetCache(MemoryUsage usage);

    void ReleaseCache(MemoryUsage usage);

    /// Frees every staging buffer the GPU is done with, of every kind (out of memory).
    void ReleaseIdleBuffers();

    void ReleaseLevel(StagingBuffersCache& cache, size_t log2);

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    vk::Buffer stream_buffer;
    VkDeviceAddress stream_buffer_address{};
    std::span<u8> stream_pointer;
    VkDeviceSize stream_buffer_size;

    StreamRing<NUM_SYNCS> stream_ring;

    // TEST ONLY: stream ranges whose tick the GPU has not completed, by offset.
    struct TestStreamRange {
        size_t size;
        u64 tick;
        u64 serial;
    };
    std::multimap<size_t, TestStreamRange> test_stream_live;
    u64 test_stream_serial = 0;
    u64 test_stream_pruned_tick = 0;
    u32 test_stream_overlaps_logged = 0;
    std::thread::id test_owner_thread{};
    u32 test_other_thread_calls = 0;
    u64 test_fallbacks_ahead = 0;   ///< stream requests that fell back: regions ahead busy
    u64 test_fallbacks_wrap = 0;    ///< stream requests that fell back: start busy at the wrap
    u64 test_fallbacks_logged = 0;
    u64 test_fallbacks_log_ns = 0;
    size_t test_cursor = 0;         ///< follows the ring's cursor, to tell a wrap
    bool test_busy_stretch = false; ///< the last stream request fell back at the wrap

    StagingBuffersCache device_local_cache;
    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level = 0;
    u64 buffer_index = 0;
    u64 unique_ids{};
};

} // namespace Vulkan
