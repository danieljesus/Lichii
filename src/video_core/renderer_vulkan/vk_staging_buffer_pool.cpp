// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stutter_trace.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using namespace Common::Literals;

// Maximum potential alignment of a Vulkan buffer
constexpr VkDeviceSize MAX_ALIGNMENT = 256;

// Stream buffer size in bytes
// *NIX drivers are more sensitive to increased buffers for streaming.
// Windows ones however, can intake bigger buffers and generally do not OOM.
// - GTX 960 on Windows will not OOM with 256mib
// - GT 1030 on ^NIX will OOM with 256mib
#if defined(__FreeBSD__)
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#else
// 128 MiB on Windows too: 256 MiB sat mostly unused in device memory on Switch 2 titles.
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#endif

// Unused space after the end of the stream buffer. Constant buffers are bound from it and the
// shaders declare each one at its full 64 KiB, so Metal checks 64 KiB from the binding offset
// against the end of the buffer ("argument c3[0] ... has space for 26624 bytes, but argument has a
// length(65536)"). The tail is never handed out, so every binding has the full 64 KiB behind it.
constexpr VkDeviceSize STREAM_BUFFER_TAIL = 64_KiB;

size_t GetStreamBufferSize(const Device& device) {
    if (!device.HasDebuggingToolAttached()) {
        if (device.IsIntegrated()) {
            // An integrated GPU's device-local host-visible heap can be no bigger than the stream
            // buffer (Radeon 780M: 256 MiB, 243 MiB budget), which then sat over that budget.
            // Leave a quarter of it free.
            VkDeviceSize heap_size = 0;
            ForEachDeviceLocalHostVisibleHeap(
                device, [&heap_size](size_t, VkMemoryHeap& heap) {
                    heap_size = (std::max)(heap_size, heap.size);
                });
            if (heap_size != 0 && heap_size <= MAX_STREAM_BUFFER_SIZE) {
                return Common::AlignUp(heap_size * 3 / 4, MAX_ALIGNMENT);
            }
        }
        return MAX_STREAM_BUFFER_SIZE;
    }

    VkDeviceSize size{0};
    bool has_device_local_host_visible_heap{};
    ForEachDeviceLocalHostVisibleHeap(device, [&size, &has_device_local_host_visible_heap](
                                                  size_t index, VkMemoryHeap& heap) {
        has_device_local_host_visible_heap = true;
        size = (std::max)(size, heap.size);
    });
    if (has_device_local_host_visible_heap) {
        // If rebar is not supported, cut the max heap size to 40%. This will allow 2 captures to be
        // loaded at the same time in RenderDoc. If rebar is supported, this shouldn't be an issue
        // as the heap will be much larger.
        if (size <= MAX_STREAM_BUFFER_SIZE) {
            size = size * 40 / 100;
        }
    } else {
        size = MAX_STREAM_BUFFER_SIZE;
    }
    return (std::min)(Common::AlignUp(size, MAX_ALIGNMENT), MAX_STREAM_BUFFER_SIZE);
}
// TEST ONLY: EDEN_NX2_TEST_STREAM_LOG=1 logs every wrap of the stream buffer, a wrap that finds
// the start still in use (with the next 64 requests), and any stream range handed out over one
// whose tick the GPU has not completed. EDEN_NX2_TEST_STREAM_DIVISOR=<n> (1 to 64) shrinks the
// stream buffer n times, so that it wraps more often.
bool TestStreamLog() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_TEST_STREAM_LOG");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

VkDeviceSize TestStreamDivisor() {
    static const VkDeviceSize divisor = [] {
        const char* const value = std::getenv("EDEN_NX2_TEST_STREAM_DIVISOR");
        const unsigned long long parsed = value ? std::strtoull(value, nullptr, 10) : 1;
        return static_cast<VkDeviceSize>(parsed >= 1 && parsed <= 64 ? parsed : 1);
    }();
    return divisor;
}
} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Device& device_, MemoryAllocator& memory_allocator_,
                                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      stream_buffer_size{Common::AlignDown(GetStreamBufferSize(device) / TestStreamDivisor(),
                                           MAX_ALIGNMENT * StagingBufferPool::NUM_SYNCS)},
      region_size{stream_buffer_size /
                                                                   StagingBufferPool::NUM_SYNCS} {
    VkBufferCreateInfo stream_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = stream_buffer_size + STREAM_BUFFER_TAIL,
        // Transfer destination too: GPU-written list words are copied over their placeholders
        // in the constant buffers streamed here (Nx2ListSlots::PatchUniform).
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    stream_buffer = memory_allocator.CreateBuffer(stream_ci, MemoryUsage::Stream);
    if (device.NamesObjects()) {
        stream_buffer.SetObjectNameEXT("Stream Buffer");
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_buffer_address = device.GetLogical().GetBufferDeviceAddress(*stream_buffer);
    }
    stream_pointer = stream_buffer.Mapped();
    ASSERT_MSG(!stream_pointer.empty(), "Stream buffer must be host visible!");
    if (TestStreamLog() || TestStreamDivisor() != 1) {
        LOG_INFO(Render_Vulkan, "TEST stream buffer probe: size={:#x} region={:#x} divisor={} log={}",
                 stream_buffer_size, region_size, TestStreamDivisor(), TestStreamLog());
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    if (TestStreamLog()) {
        const std::thread::id caller = std::this_thread::get_id();
        if (test_owner_thread == std::thread::id{}) {
            test_owner_thread = caller;
        } else if (caller != test_owner_thread && test_other_thread_calls++ < 16) {
            LOG_ERROR(Render_Vulkan, "TEST staging request from a second thread: size={:#x} usage={}",
                      size, static_cast<u32>(usage));
        }
    }
    if (!deferred && usage == MemoryUsage::Upload && size <= region_size) {
        return GetStreamBuffer(size);
    }
    return GetStagingBuffer(size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    StagingBuffers& cache_level = GetCache(ref.usage)[ref.log2_level];
    auto node = cache_level.deferred.extract(ref.index);
    ASSERT(!node.empty());
    node.mapped().tick = scheduler.CurrentTick();
    cache_level.entries.push_back(std::move(node.mapped()));
}

void StagingBufferPool::TickFrame() {
    current_delete_level = (current_delete_level + 1) % NUM_LEVELS;

    ReleaseCache(MemoryUsage::DeviceLocal);
    ReleaseCache(MemoryUsage::Upload);
    ReleaseCache(MemoryUsage::Download);
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    if (AreRegionsActive(Region(free_iterator) + 1,
                         (std::min)(Region(iterator + size) + 1, NUM_SYNCS))) {
        // Avoid waiting for the previous usages to be free
        return GetStagingBuffer(size, MemoryUsage::Upload);
    }
    const u64 current_tick = scheduler.CurrentTick();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    free_iterator = (std::max)(free_iterator, iterator + size);

    if (iterator + size >= stream_buffer_size) {
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS,
                  current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;

        const bool start_busy = AreRegionsActive(0, Region(size) + 1);
        if (TestStreamLog()) {
            const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
            if (start_busy) {
                LOG_WARNING(Render_Vulkan,
                            "TEST stream wrap: start busy, cursor reset to 0 and request size={:#x} "
                            "falls back; current_tick={} gpu_tick={} start region ticks={}",
                            size, current_tick, gpu_tick,
                            fmt::join(sync_ticks.begin(), sync_ticks.begin() + Region(size) + 1, ","));
                test_stream_after_fallback = 64;
            } else {
                LOG_INFO(Render_Vulkan, "TEST stream wrap: current_tick={} gpu_tick={}", current_tick,
                         gpu_tick);
            }
        }
        if (start_busy) {
            // Avoid waiting for the previous usages to be free
            return GetStagingBuffer(size, MemoryUsage::Upload);
        }
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, MAX_ALIGNMENT);
    if (TestStreamLog()) {
        TestNoteStream(offset, size, current_tick);
    }
    return StagingBufferRef{
        .buffer = *stream_buffer,
        .device_address = stream_buffer_address,
        .offset = static_cast<VkDeviceSize>(offset),
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage{},
        .log2_level{},
        .index{},
    };
}

void StagingBufferPool::TestNoteStream(size_t offset, size_t size, u64 tick) {
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    if (gpu_tick != test_stream_pruned_tick) {
        test_stream_pruned_tick = gpu_tick;
        std::erase_if(test_stream_live, [gpu_tick](const auto& entry) {
            return entry.second.tick <= gpu_tick;
        });
    }
    const u64 serial = test_stream_serial++;
    // Stream requests are at most one region, so an overlapping range starts at most one region
    // below this one.
    const size_t window_begin = offset > region_size ? offset - region_size : 0;
    for (auto it = test_stream_live.lower_bound(window_begin);
         it != test_stream_live.end() && it->first < offset + size; ++it) {
        if (it->first + it->second.size > offset && test_stream_overlaps_logged < 400) {
            ++test_stream_overlaps_logged;
            LOG_ERROR(Render_Vulkan,
                      "TEST stream overlap: #{} [{:#x}, +{:#x}) tick={} over live #{} [{:#x}, +{:#x}) "
                      "tick={} gpu_tick={}",
                      serial, offset, size, tick, it->second.serial, it->first, it->second.size,
                      it->second.tick, gpu_tick);
        }
    }
    if (test_stream_after_fallback > 0) {
        --test_stream_after_fallback;
        LOG_WARNING(Render_Vulkan, "TEST stream request #{} after a busy wrap: [{:#x}, +{:#x}) tick={} gpu_tick={}",
                    serial, offset, size, tick, gpu_tick);
    }
    test_stream_live.emplace(offset, TestStreamRange{.size = size, .tick = tick, .serial = serial});
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [gpu_tick](u64 sync_tick) { return gpu_tick < sync_tick; });
};

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    if (const std::optional<StagingBufferRef> ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    StagingBuffers& cache_level = GetCache(usage)[Common::Log2Ceil(size)];

    const auto is_free = [this](const StagingBuffer& entry) {
        return scheduler.IsFree(entry.tick);
    };
    auto& entries = cache_level.entries;
    const auto hint_it = entries.begin() + cache_level.iterate_index;
    auto it = std::find_if(entries.begin() + cache_level.iterate_index, entries.end(), is_free);
    if (it == entries.end()) {
        it = std::find_if(entries.begin(), hint_it, is_free);
        if (it == hint_it) {
            return std::nullopt;
        }
    }
    const size_t position = static_cast<size_t>(std::distance(entries.begin(), it));
    const StagingBufferRef ref = it->Ref();
    if (!deferred) {
        cache_level.iterate_index = position + 1;
        it->tick = scheduler.CurrentTick();
        return ref;
    }
    // The last entry takes its place, so the next lookup starts there.
    cache_level.iterate_index = position;
    cache_level.deferred.emplace(it->index, std::move(*it));
    if (it != entries.end() - 1) {
        *it = std::move(entries.back());
    }
    entries.pop_back();
    return ref;
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred) {
    const VideoCommon::StutterTrace::SiteScope site{VideoCommon::StutterTrace::Site::StagingCreate};
    auto const log2_size = Common::Log2Ceil<u32>(u32(size));
    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 1ULL << log2_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, // mesh emulation dispatch and draw arguments
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer buffer = memory_allocator.TryCreateBuffer(buffer_ci, usage);
    if (!buffer) {
        // No memory left for it (#262: Mario Kart World on an 8 GB laptop with Intel UHD, a
        // 128 MiB upload). Free the idle staging buffers and try again, then wait for the GPU.
        ReleaseIdleBuffers();
        buffer = memory_allocator.TryCreateBuffer(buffer_ci, usage);
        if (!buffer) {
            ReleaseMemory();
            buffer = memory_allocator.TryCreateBuffer(buffer_ci, usage);
        }
    }
    if (!buffer) {
        static u32 logged{};
        if (logged++ == 0) {
            LogMemoryTrace(device);
        }
        // The submitted ticks are done, so the stream buffer after its last range is free unless
        // the open tick wrapped into it. The ranges before it may hold data for commands not
        // recorded yet, so it does not wrap.
        if (usage != MemoryUsage::Upload || deferred || iterator + size >= stream_buffer_size ||
            AreRegionsActive(Region(iterator), Region(iterator + size) + 1)) {
            LOG_CRITICAL(Render_Vulkan, "No memory for a {} byte staging buffer (usage {})", size,
                         static_cast<u32>(usage));
            throw vk::Exception(VK_ERROR_OUT_OF_DEVICE_MEMORY);
        }
        if (logged <= 32) {
            LOG_ERROR(Render_Vulkan,
                      "No memory for a {} byte upload buffer; using the stream buffer", size);
        }
        const size_t offset = iterator;
        free_iterator = (std::max)(free_iterator, offset + size);
        iterator = Common::AlignUp(offset + size, MAX_ALIGNMENT);
        return StagingBufferRef{
            .buffer = *stream_buffer,
            .device_address = stream_buffer_address,
            .offset = static_cast<VkDeviceSize>(offset),
            .mapped_span = stream_pointer.subspan(offset, size),
            .usage{},
            .log2_level{},
            .index{},
        };
    }
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::StagingCreates, 1);
    if (device.NamesObjects()) {
        ++buffer_index;
        buffer.SetObjectNameEXT(fmt::format("Staging Buffer {}", buffer_index).c_str());
    }
    const std::span<u8> mapped_span = buffer.Mapped();
    const VkDeviceAddress buffer_address =
        device.IsBufferDeviceAddressSupported()
            ? device.GetLogical().GetBufferDeviceAddress(*buffer)
            : VkDeviceAddress{};
    StagingBuffer entry{
        .buffer = std::move(buffer),
        .device_address = buffer_address,
        .mapped_span = mapped_span,
        .usage = usage,
        .log2_level = log2_size,
        .index = unique_ids++,
        .tick = scheduler.CurrentTick(),
    };
    const StagingBufferRef ref = entry.Ref();
    StagingBuffers& cache_level = GetCache(usage)[log2_size];
    if (deferred) {
        cache_level.deferred.emplace(entry.index, std::move(entry));
    } else {
        cache_level.entries.push_back(std::move(entry));
    }
    return ref;
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return device_local_cache;
    case MemoryUsage::Upload:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    default:
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return upload_cache;
    }
}

u64 StagingBufferPool::CachedBytes(MemoryUsage usage) {
    u64 bytes{};
    const StagingBuffersCache& cache = GetCache(usage);
    for (size_t level = 0; level < NUM_LEVELS; ++level) {
        const size_t count = cache[level].entries.size() + cache[level].deferred.size();
        if (count != 0) {
            bytes += u64{count} << level;
        }
    }
    return bytes;
}

void StagingBufferPool::ReleaseMemory() {
    // Waits for the submitted ticks only (#1993). This runs while a draw or dispatch is set up,
    // and a submit would leave its earlier bindings in the submitted command buffer. Buffers
    // handed out in the open tick stay busy, as they may feed commands not recorded yet.
    scheduler.Wait(scheduler.CurrentTick() - 1);
    ReleaseIdleBuffers();
}

void StagingBufferPool::ReleaseIdleBuffers() {
    for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {
        for (StagingBuffers& staging : *cache) {
            std::erase_if(staging.entries, [this](const StagingBuffer& entry) {
                return scheduler.IsFree(entry.tick);
            });
            staging.delete_index = 0;
            staging.iterate_index = 0;
        }
    }
}

void StagingBufferPool::ReleaseCache(MemoryUsage usage) {
    ReleaseLevel(GetCache(usage), current_delete_level);
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    constexpr size_t deletions_per_tick = 16;
    auto& staging = cache[log2];
    auto& entries = staging.entries;
    const size_t old_size = entries.size();

    // Keep two idle upload buffers per level up to 32 MiB (under 128 MiB in all). Each level is
    // swept every 64 frames, so after a pause each upload over 8 MiB (past the stream buffer's
    // regions) got a new dedicated allocation, written to for the first time (16 MiB on an
    // RTX 4080, idle system: 0.4 ms to create, 1.2 ms more for the first write).
    constexpr size_t KeepMaxLog2 = 25;
    constexpr size_t KeepPerLevel = 2;
    if (&cache == &upload_cache && log2 <= KeepMaxLog2) {
        size_t idle = 0;
        std::erase_if(entries, [this, &idle](const StagingBuffer& entry) {
            return scheduler.IsFree(entry.tick) && ++idle > KeepPerLevel;
        });
        staging.delete_index = 0;
        if (staging.iterate_index > entries.size()) {
            staging.iterate_index = 0;
        }
        return;
    }

    const auto is_deletable = [this](const StagingBuffer& entry) {
        return scheduler.IsFree(entry.tick);
    };
    // Deferred checkouts take entries out of the vector since the last sweep.
    const size_t begin_offset = (std::min)(staging.delete_index, old_size);
    const size_t end_offset = (std::min)(begin_offset + deletions_per_tick, old_size);
    const auto begin = entries.begin() + begin_offset;
    const auto end = entries.begin() + end_offset;
    entries.erase(std::remove_if(begin, end, is_deletable), end);

    const size_t new_size = entries.size();
    staging.delete_index += deletions_per_tick;
    if (staging.delete_index >= new_size) {
        staging.delete_index = 0;
    }
    if (staging.iterate_index > new_size) {
        staging.iterate_index = 0;
    }
}

} // namespace Vulkan
