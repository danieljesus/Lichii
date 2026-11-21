// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace {

// A driver pipeline cache read as MoltenVK reads it: a buffer smaller than the data gets nothing
// and VK_INCOMPLETE. `grow` pipelines build into it after each size query, before the read.
struct FakeCache {
    std::vector<char> data;
    int grow = 0;
    int reads = 0;

    VkResult Read(size_t* size, void* out) {
        ++reads;
        if (out == nullptr) {
            *size = data.size();
            if (grow > 0) {
                --grow;
                data.push_back(static_cast<char>(data.size()));
            }
            return VK_SUCCESS;
        }
        if (*size < data.size()) {
            *size = 0;
            return VK_INCOMPLETE;
        }
        std::memcpy(out, data.data(), data.size());
        *size = data.size();
        return VK_SUCCESS;
    }
};

auto Reader(FakeCache& cache) {
    return [&cache](size_t* size, void* out) { return cache.Read(size, out); };
}

} // Anonymous namespace

TEST_CASE("A driver pipeline cache that grows between its size and its read is asked again",
          "[video_core][vulkan_pipeline_cache]") {
    FakeCache cache{.data{1, 2, 3}, .grow = 1};
    const auto data = Vulkan::ReadPipelineCacheData(Reader(cache));
    REQUIRE(data.has_value());
    CHECK(*data == std::vector<char>{1, 2, 3, 3});
    CHECK(cache.reads == 4);
}

TEST_CASE("A driver pipeline cache that never holds still is not read",
          "[video_core][vulkan_pipeline_cache]") {
    // Rather than saving what the driver returned: nothing at all, on MoltenVK.
    FakeCache cache{.data{1, 2, 3}, .grow = 100};
    CHECK_FALSE(Vulkan::ReadPipelineCacheData(Reader(cache), 3).has_value());
    CHECK(cache.reads == 6);
}

TEST_CASE("A driver pipeline cache that holds still is read once",
          "[video_core][vulkan_pipeline_cache]") {
    FakeCache cache{.data{1, 2, 3}};
    CHECK(Vulkan::ReadPipelineCacheData(Reader(cache)) == std::vector<char>{1, 2, 3});
    CHECK(cache.reads == 2);
    FakeCache empty;
    CHECK(Vulkan::ReadPipelineCacheData(Reader(empty)) == std::vector<char>{});
    CHECK(empty.reads == 1);
}

TEST_CASE("A driver pipeline cache whose read fails is not read",
          "[video_core][vulkan_pipeline_cache]") {
    int reads = 0;
    const auto failing = [&](size_t* size, void* out) {
        ++reads;
        *size = 4;
        return out == nullptr ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    };
    CHECK_FALSE(Vulkan::ReadPipelineCacheData(failing).has_value());
    CHECK(reads == 2);
    const auto failing_size = [](size_t*, void*) { return VK_ERROR_OUT_OF_HOST_MEMORY; };
    CHECK_FALSE(Vulkan::ReadPipelineCacheData(failing_size).has_value());
}
