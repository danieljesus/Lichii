// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

/// Who driver pipeline cache data says it was made by: Vulkan starts it with a header naming the
/// vendor, device and pipelineCacheUUID (MoltenVK's changes with each build of it). Used to say
/// why a driver kept nothing of a file; whether it did is asked of the driver.
enum class PipelineCacheDataOrigin { ThisDriver, OtherDriver, NoData, Unknown };

/// NoData for data shorter than a version one header (a file of Lichii's header alone among
/// them), Unknown for a header of another version.
[[nodiscard]] inline PipelineCacheDataOrigin PipelineCacheDataMadeBy(
    std::span<const char> data, u32 vendor_id, u32 device_id,
    std::span<const u8, VK_UUID_SIZE> uuid) {
    // The header's fields are little endian whatever the host.
    static_assert(std::endian::native == std::endian::little);
    VkPipelineCacheHeaderVersionOne header;
    if (data.size() < sizeof(header)) {
        return PipelineCacheDataOrigin::NoData;
    }
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        header.headerSize < sizeof(header)) {
        return PipelineCacheDataOrigin::Unknown;
    }
    const bool same = header.vendorID == vendor_id && header.deviceID == device_id &&
                      std::ranges::equal(header.pipelineCacheUUID, uuid);
    return same ? PipelineCacheDataOrigin::ThisDriver : PipelineCacheDataOrigin::OtherDriver;
}

/// What loading the driver pipeline cache file came to.
enum class VulkanCacheLoad {
    /// The driver holds the file's data.
    Restored,
    /// No file, or one the load already removes (damaged, of another Lichii cache version, or
    /// rejected by the driver).
    None,
    /// Driver data shorter than a version one header: a save that read nothing left it.
    NoData,
    /// The driver was given the data and holds none of it: made by another driver version, or
    /// dropped for a reason of its own. vkCreatePipelineCache succeeds either way.
    DriverKeptNothing,
};

/// The size the driver reports for a cache created from `data_size` bytes of the file
/// (`held_after_create`, empty when the query failed), against an empty cache's
/// (`empty_size`). A driver that keeps nothing reports an empty cache's size.
[[nodiscard]] inline VulkanCacheLoad DecideVulkanCacheLoad(size_t data_size,
                                                           std::optional<size_t> held_after_create,
                                                           size_t empty_size) {
    if (data_size < sizeof(VkPipelineCacheHeaderVersionOne)) {
        return VulkanCacheLoad::NoData;
    }
    // A query that failed tells nothing: the data is taken as kept, as it always was.
    if (held_after_create && *held_after_create <= empty_size) {
        return VulkanCacheLoad::DriverKeptNothing;
    }
    return VulkanCacheLoad::Restored;
}

/// What the load does about it.
struct VulkanCacheLoadActions {
    /// Whether the cache counts as restored (vulkan_pipeline_cache_restored). One that is not
    /// drops the precache's progress and its "done" marker: the builds they vouch for are not in
    /// it, nothing else holds them on drivers without a cache of their own (MoltenVK among them),
    /// and the marker's stamp (driver version, device) does not change with the pipelineCacheUUID.
    bool restored;
    /// The file goes, as the damaged and rejected ones do, so it is not read again at every boot
    /// until a save replaces it.
    bool remove_file;
};

[[nodiscard]] constexpr VulkanCacheLoadActions ActionsFor(VulkanCacheLoad load) {
    switch (load) {
    case VulkanCacheLoad::Restored:
        return {.restored = true, .remove_file = false};
    case VulkanCacheLoad::None:
        return {.restored = false, .remove_file = false};
    case VulkanCacheLoad::NoData:
    case VulkanCacheLoad::DriverKeptNothing:
        return {.restored = false, .remove_file = true};
    }
    return {.restored = false, .remove_file = false};
}

} // namespace Vulkan
