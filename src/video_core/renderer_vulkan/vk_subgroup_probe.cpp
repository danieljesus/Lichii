// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <exception>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_compute_probe.h"
#include "video_core/renderer_vulkan/vk_subgroup_probe.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {
struct Shape {
    u32 x, y, z;
};

/// The workgroups the staged software HMMA may run with: x a multiple of 32, 32 to 1024
/// invocations. Every one-dimensional size, and some with y and z, within the device's limits.
std::vector<Shape> ProbedShapes(const Device& device) {
    const u32 max_invocations = (std::min)(device.GetMaxComputeWorkGroupInvocations(), 1024U);
    const std::array<u32, 3> max_size = device.GetMaxComputeWorkGroupSize();
    std::vector<Shape> shapes;
    for (u32 x = GuestWarpSize; x <= (std::min)(max_invocations, max_size[0]); x += GuestWarpSize) {
        shapes.push_back({x, 1, 1});
    }
    for (const Shape shape : {Shape{32, 4, 1}, Shape{64, 2, 2}, Shape{32, 8, 4}}) {
        if (shape.x * shape.y * shape.z <= max_invocations && shape.x <= max_size[0] &&
            shape.y <= max_size[1] && shape.z <= max_size[2]) {
            shapes.push_back(shape);
        }
    }
    return shapes;
}
} // Anonymous namespace

bool ProbeComputeSubgroupsAreGuestWarps(const Device& device) {
    using Shader::Backend::SPIRV::SUBGROUP_PROBE_VALID;
    if (device.SupportedSpirvVersion() < 0x00010300U) {
        LOG_WARNING(Render_Vulkan, "No SPIR-V 1.3 to probe compute subgroups with: the software "
                                   "HMMA keeps its shuffles");
        return false;
    }
    const std::vector<Shape> shapes = ProbedShapes(device);
    try {
        for (const Shape shape : shapes) {
            const u32 invocations = shape.x * shape.y * shape.z;
            const std::vector<u32> code = Shader::Backend::SPIRV::EmitSubgroupLayoutProbe(
                device.SupportedSpirvVersion(), shape.x, shape.y, shape.z);
            std::vector<u32> words(invocations, 0);
            RunComputeProbe(device, code, words);
            for (u32 index = 0; index < invocations; ++index) {
                const u32 expected = SUBGROUP_PROBE_VALID | GuestWarpSize |
                                     ((index / GuestWarpSize) << 8) |
                                     ((index % GuestWarpSize) << 16);
                if (words[index] != expected) {
                    LOG_WARNING(Render_Vulkan,
                                "Compute subgroups are not 32-lane warps in invocation order (a "
                                "{}x{}x{} workgroup: invocation {} has size {}, subgroup {}, lane "
                                "{}): the software HMMA keeps its shuffles",
                                shape.x, shape.y, shape.z, index, words[index] & 0xff,
                                (words[index] >> 8) & 0xff, (words[index] >> 16) & 0xff);
                    return false;
                }
            }
        }
    } catch (const std::exception& exception) {
        LOG_WARNING(Render_Vulkan,
                    "Compute subgroup probe failed ({}): the software HMMA keeps its shuffles",
                    exception.what());
        return false;
    }
    LOG_INFO(Render_Vulkan,
             "Compute subgroups are 32-lane warps in invocation order (probed with {} workgroups)",
             shapes.size());
    return true;
}

} // namespace Vulkan
