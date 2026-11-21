// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
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
    CHECK(data.result == VK_SUCCESS);
    CHECK(Bytes(data) == cache.data);
    CHECK(cache.calls == 2);
    CHECK(progress == 1);
    FakeCache empty{.data{}};
    const auto none = Read(empty);
    CHECK(none.Usable());
    CHECK(none.size == 0);
    CHECK(empty.calls == 1);
}

TEST_CASE("A driver pipeline cache that grows between its size and its read is asked again",
          "[video_core][vulkan_pipeline_cache]") {
    for (const bool conforming : {false, true}) {
        FakeCache cache{.conforming = conforming, .grow = 1};
        const auto data = Read(cache);
        CHECK(data.result == VK_SUCCESS);
        CHECK(Bytes(data) == cache.data);
        CHECK(cache.calls == 4);
    }
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

TEST_CASE("A driver pipeline cache that returns part of itself each time it grows saves the last "
          "part",
          "[video_core][vulkan_pipeline_cache]") {
    // A conforming driver: each part is a valid, smaller cache, as the save always took.
    FakeCache cache{.conforming = true, .grow = 100};
    const auto data = Read(cache);
    REQUIRE(data.Usable());
    CHECK(data.result == VK_INCOMPLETE);
    const size_t last_size = Header + 16 + 8 * (PipelineCacheReadAttempts - 1);
    CHECK(data.size == last_size);
    CHECK(std::equal(data.bytes.get(), data.bytes.get() + data.size, cache.data.begin()));
    CHECK(cache.calls == 2 * PipelineCacheReadAttempts);
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
