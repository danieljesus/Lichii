// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_cache_data.h"

namespace {

using Vulkan::PipelineCacheData;
using Vulkan::PipelineCacheReadAttempts;

constexpr size_t Header = Vulkan::MinPipelineCacheDataSize;

// A driver pipeline cache. `grow` pipelines build into it, one after each size query, before the
// read. A read into a smaller buffer gets nothing as on MoltenVK, or, `conforming`, as much as
// fits as the spec has it (whole entries of 8 bytes after the header).
struct FakeCache {
    std::vector<char> data = std::vector<char>(Header + 16, 1);
    bool conforming = false;
    int grow = 0;
    int calls = 0;

    VkResult Read(size_t* size, void* out) {
        ++calls;
        if (out == nullptr) {
            *size = data.size();
            if (grow > 0) {
                --grow;
                data.resize(data.size() + 8, static_cast<char>(data.size()));
            }
            return VK_SUCCESS;
        }
        if (*size >= data.size()) {
            std::memcpy(out, data.data(), data.size());
            *size = data.size();
            return VK_SUCCESS;
        }
        *size = conforming && *size >= Header ? Header + (*size - Header) / 8 * 8 : 0;
        std::memcpy(out, data.data(), *size);
        return VK_INCOMPLETE;
    }
};

PipelineCacheData Read(FakeCache& cache, int* progress = nullptr) {
    return Vulkan::ReadPipelineCacheData(
        [&cache](size_t* size, void* out) { return cache.Read(size, out); },
        [progress] {
            if (progress) {
                ++*progress;
            }
        });
}

std::vector<char> Bytes(const PipelineCacheData& data) {
    return {data.bytes.get(), data.bytes.get() + data.size};
}

} // Anonymous namespace

TEST_CASE("A driver pipeline cache that holds still is read once",
          "[video_core][vulkan_pipeline_cache]") {
    FakeCache cache;
    int progress = 0;
    const auto data = Read(cache, &progress);
    CHECK(data.Whole());
    CHECK(Bytes(data) == cache.data);
    CHECK(cache.calls == 2);
    CHECK(progress == 1);
}

TEST_CASE("A driver pipeline cache that reads back fewer bytes than asked is whole",
          "[video_core][vulkan_pipeline_cache]") {
    int calls = 0;
    const auto data = Vulkan::ReadPipelineCacheData(
        [&](size_t* size, void* out) {
            ++calls;
            if (out == nullptr) {
                *size = Header + 64;
            } else {
                *size = Header + 8;
                std::memset(out, 7, *size);
            }
            return VK_SUCCESS;
        },
        [] {});
    CHECK(data.Whole());
    CHECK(data.size == Header + 8);
    CHECK(calls == 2);
}

TEST_CASE("A driver pipeline cache smaller than a header is no data",
          "[video_core][vulkan_pipeline_cache]") {
    // A live cache always holds a header: a size of 0, or less than one, would replace a good
    // file with an empty cache.
    for (const size_t size : {size_t{0}, Header - 1}) {
        FakeCache cache{.data = std::vector<char>(size, 1)};
        const auto data = Read(cache);
        CHECK_FALSE(data.Usable());
        CHECK(data.result == VK_SUCCESS);
        CHECK(cache.calls == 1);
    }
}

TEST_CASE("A driver pipeline cache that returns nothing when it grows is asked again",
          "[video_core][vulkan_pipeline_cache]") {
    // MoltenVK: no bytes until the size is asked for again.
    FakeCache cache{.grow = 1};
    const auto data = Read(cache);
    CHECK(data.Whole());
    CHECK(Bytes(data) == cache.data);
    CHECK(cache.calls == 4);
}

TEST_CASE("A driver pipeline cache that returns nothing each time it grows is not saved",
          "[video_core][vulkan_pipeline_cache]") {
    // MoltenVK: what it returned would replace the file with an empty cache.
    FakeCache cache{.grow = 100};
    int progress = 0;
    const auto data = Read(cache, &progress);
    CHECK_FALSE(data.Usable());
    CHECK(data.result == VK_INCOMPLETE);
    CHECK(data.reads == PipelineCacheReadAttempts);
    CHECK(cache.calls == 2 * PipelineCacheReadAttempts);
    CHECK(progress == PipelineCacheReadAttempts);
}

TEST_CASE("A driver pipeline cache that returns part of itself when it grows saves that part",
          "[video_core][vulkan_pipeline_cache]") {
    // A conforming driver: the part is a valid, smaller cache, taken at once as nx2 took it,
    // rather than serializing the whole cache again for a result as partial.
    FakeCache cache{.conforming = true, .grow = 100};
    const auto data = Read(cache);
    REQUIRE(data.Partial());
    CHECK(data.size == Header + 16);
    CHECK(std::equal(data.bytes.get(), data.bytes.get() + data.size, cache.data.begin()));
    CHECK(cache.calls == 2);
}

TEST_CASE("A driver pipeline cache part of a header alone is asked again",
          "[video_core][vulkan_pipeline_cache]") {
    // A header and nothing after it holds no pipeline; it must not replace a full file.
    FakeCache cache{.data = std::vector<char>(Header, 1), .conforming = true, .grow = 100};
    const auto data = Read(cache);
    REQUIRE(data.Partial());
    CHECK(data.size == Header + 8);
    CHECK(cache.calls == 4);
}

TEST_CASE("A driver pipeline cache whose read fails is not saved",
          "[video_core][vulkan_pipeline_cache]") {
    int calls = 0;
    const auto failing = Vulkan::ReadPipelineCacheData(
        [&](size_t* size, void* out) {
            ++calls;
            *size = 64;
            return out == nullptr ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
        },
        [] {});
    CHECK_FALSE(failing.Usable());
    CHECK(failing.result == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(calls == 2);
    const auto failing_size = Vulkan::ReadPipelineCacheData(
        [](size_t*, void*) { return VK_ERROR_OUT_OF_HOST_MEMORY; }, [] {});
    CHECK_FALSE(failing_size.Usable());
    CHECK(failing_size.result == VK_ERROR_OUT_OF_HOST_MEMORY);
}

TEST_CASE("A failed driver pipeline cache flush waits for the retry floor, not the interval",
          "[video_core][vulkan_pipeline_cache]") {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const std::chrono::seconds interval{300}; // a 300 MB cache
    // Never saved nor tried: due.
    CHECK(Vulkan::PipelineCacheFlushDue(start, {}, {}, interval));
    // Saved a minute ago: not due until the interval has passed.
    CHECK_FALSE(Vulkan::PipelineCacheFlushDue(start + 60s, start, start, interval));
    CHECK(Vulkan::PipelineCacheFlushDue(start + 300s, start, start, interval));
    // Tried and failed: another 128 pipelines inside the floor do not queue it again...
    const auto failed = start + 400s;
    CHECK_FALSE(Vulkan::PipelineCacheFlushDue(failed + 10s, start, failed, interval));
    // ...the floor does, long before a whole interval from the failure.
    CHECK(Vulkan::PipelineCacheFlushDue(failed + Vulkan::PipelineCacheFlushRetryFloor, start,
                                        failed, interval));
}
