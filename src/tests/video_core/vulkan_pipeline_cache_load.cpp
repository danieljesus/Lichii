// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_cache_load.h"

namespace {

using Vulkan::PipelineCacheDataOrigin;
using Vulkan::VulkanCacheLoad;

constexpr size_t Header = sizeof(VkPipelineCacheHeaderVersionOne);

constexpr u32 Vendor = 0x106B;
constexpr u32 DeviceId = 0x1A020209;
constexpr std::array<u8, VK_UUID_SIZE> Uuid{0x38, 0x71, 0x93, 0x85, 1, 2,  3,  4,
                                            5,    6,    7,    8,    9, 10, 11, 12};

// Driver data as vkGetPipelineCacheData returns it: the version one header, then the driver's own.
std::vector<char> Data(u32 vendor, u32 device, const std::array<u8, VK_UUID_SIZE>& uuid) {
    const VkPipelineCacheHeaderVersionOne header{
        .headerSize = sizeof(VkPipelineCacheHeaderVersionOne),
        .headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE,
        .vendorID = vendor,
        .deviceID = device,
    };
    std::vector<char> data(sizeof(header) + 16, 0x5A);
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + offsetof(VkPipelineCacheHeaderVersionOne, pipelineCacheUUID),
                uuid.data(), uuid.size());
    return data;
}

PipelineCacheDataOrigin Origin(const std::vector<char>& data) {
    return Vulkan::PipelineCacheDataMadeBy(data, Vendor, DeviceId, Uuid);
}

} // Anonymous namespace

TEST_CASE("Driver pipeline cache data from this driver is this driver's",
          "[video_core][vulkan_pipeline_cache]") {
    CHECK(Origin(Data(Vendor, DeviceId, Uuid)) == PipelineCacheDataOrigin::ThisDriver);
}

TEST_CASE("Driver pipeline cache data from another driver build is another driver's",
          "[video_core][vulkan_pipeline_cache]") {
    // Another MoltenVK build: the same vendor and device, its UUID's first bytes changed.
    auto other_build = Uuid;
    other_build[0] = 0xBF;
    CHECK(Origin(Data(Vendor, DeviceId, other_build)) == PipelineCacheDataOrigin::OtherDriver);
    CHECK(Origin(Data(Vendor, DeviceId + 1, Uuid)) == PipelineCacheDataOrigin::OtherDriver);
    CHECK(Origin(Data(0x10DE, DeviceId, Uuid)) == PipelineCacheDataOrigin::OtherDriver);
}

TEST_CASE("Driver pipeline cache data shorter than its header holds nothing",
          "[video_core][vulkan_pipeline_cache]") {
    // A file of Lichii's header alone, as a save that read nothing from the driver left it.
    CHECK(Origin({}) == PipelineCacheDataOrigin::NoData);
    const auto data = Data(Vendor, DeviceId, Uuid);
    CHECK(Origin(std::vector<char>(data.begin(), data.begin() + 31)) ==
          PipelineCacheDataOrigin::NoData);
    CHECK(Origin(std::vector<char>(data.begin(), data.begin() + 32)) ==
          PipelineCacheDataOrigin::ThisDriver);
}

TEST_CASE("Driver pipeline cache data with a header of another version is left to the driver",
          "[video_core][vulkan_pipeline_cache]") {
    auto data = Data(Vendor, DeviceId, Uuid);
    auto other_version = data;
    const u32 version = 2;
    std::memcpy(other_version.data() + 4, &version, sizeof(version));
    CHECK(Origin(other_version) == PipelineCacheDataOrigin::Unknown);
    auto short_header = data;
    const u32 size = 16;
    std::memcpy(short_header.data(), &size, sizeof(size));
    CHECK(Origin(short_header) == PipelineCacheDataOrigin::Unknown);
}

TEST_CASE("A driver pipeline cache the driver kept is restored",
          "[video_core][vulkan_pipeline_cache]") {
    // The driver reports more than an empty cache of its own (MoltenVK: 36 bytes).
    const auto load = Vulkan::DecideVulkanCacheLoad(127845708, 127845708, 36);
    CHECK(load == VulkanCacheLoad::Restored);
    const auto actions = Vulkan::ActionsFor(load);
    CHECK(actions.restored);
    CHECK_FALSE(actions.remove_file);
    // A size query that failed tells nothing: taken as kept, as before.
    CHECK(Vulkan::DecideVulkanCacheLoad(4096, std::nullopt, 36) == VulkanCacheLoad::Restored);
}

TEST_CASE("A driver pipeline cache the driver kept none of is not restored, and goes",
          "[video_core][vulkan_pipeline_cache]") {
    // MoltenVK given 128 MB made by another build of it holds an empty cache's 36 bytes; so does
    // any driver that drops data for a reason of its own, whatever its header says.
    const auto load = Vulkan::DecideVulkanCacheLoad(127845708, 36, 36);
    CHECK(load == VulkanCacheLoad::DriverKeptNothing);
    const auto actions = Vulkan::ActionsFor(load);
    CHECK_FALSE(actions.restored);
    CHECK(actions.remove_file);
}

TEST_CASE("A driver pipeline cache file with no driver data is not restored, and goes",
          "[video_core][vulkan_pipeline_cache]") {
    // Lichii's header alone, as a save that read nothing left it, or less than Vulkan's header:
    // decided without giving it to the driver.
    for (const size_t size : {size_t{0}, Header - 1}) {
        const auto load = Vulkan::DecideVulkanCacheLoad(size, std::nullopt, 0);
        CHECK(load == VulkanCacheLoad::NoData);
        const auto actions = Vulkan::ActionsFor(load);
        CHECK_FALSE(actions.restored);
        CHECK(actions.remove_file);
    }
    CHECK(Vulkan::DecideVulkanCacheLoad(Header, 64, 36) == VulkanCacheLoad::Restored);
}

TEST_CASE("No driver pipeline cache file is not restored, and nothing is removed",
          "[video_core][vulkan_pipeline_cache]") {
    const auto actions = Vulkan::ActionsFor(VulkanCacheLoad::None);
    CHECK_FALSE(actions.restored);
    CHECK_FALSE(actions.remove_file);
}
