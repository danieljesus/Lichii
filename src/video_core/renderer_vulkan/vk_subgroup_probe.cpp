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

/// The workgroups the probe runs (IsSubgroupProbeShape) within the device's limits.
std::vector<Shape> ProbedShapes(const Device& device) {
    using Shader::Backend::SPIRV::IsSubgroupProbeShape;
    const u32 max_invocations = device.GetMaxComputeWorkGroupInvocations();
    const std::array<u32, 3> max_size = device.GetMaxComputeWorkGroupSize();
    const auto fits = [&](const Shape& shape) {
        return shape.x <= max_size[0] && shape.y <= max_size[1] && shape.z <= max_size[2] &&
               u64{shape.x} * shape.y * shape.z <= max_invocations;
    };
    std::vector<Shape> shapes;
    for (u32 x = GuestWarpSize; IsSubgroupProbeShape(x, 1, 1); x += GuestWarpSize) {
        if (fits({x, 1, 1})) {
            shapes.push_back({x, 1, 1});
        }
    }
    for (const auto& [x, y, z] : Shader::Backend::SPIRV::SUBGROUP_PROBE_PLANAR_SHAPES) {
        if (fits({x, y, z})) {
            shapes.push_back({x, y, z});
        }
    }
    return shapes;
}

/// A dispatch of a few invocations takes well under a millisecond: this only catches a hang.
constexpr u64 ProbeTimeout = u64{2'000'000'000}; // 2 s
} // Anonymous namespace

bool ProbeComputeSubgroupsAreGuestWarps(const Device& device) {
    using Shader::Backend::SPIRV::SUBGROUP_PROBE_VALID;
    if (device.SupportedSpirvVersion() < 0x00010300U) {
        LOG_WARNING(Render_Vulkan, "No SPIR-V 1.3 to probe compute subgroups with: the software "
                                   "HMMA keeps its shuffles");
        return false;
    }
    const std::vector<Shape> shapes = ProbedShapes(device);
    if (shapes.empty()) {
        LOG_WARNING(Render_Vulkan, "No compute workgroup to probe subgroups with within the "
                                   "device's limits: the software HMMA keeps its shuffles");
        return false;
    }
    try {
        for (const Shape shape : shapes) {
            const u32 invocations = shape.x * shape.y * shape.z;
            const std::vector<u32> code = Shader::Backend::SPIRV::EmitSubgroupLayoutProbe(
                device.SupportedSpirvVersion(), shape.x, shape.y, shape.z);
            std::vector<u32> words(invocations, 0);
            if (RunComputeProbe(device, code, words, ProbeTimeout) == ProbeResult::TimedOut) {
                LOG_WARNING(Render_Vulkan,
                            "Compute subgroup probe timed out (a {}x{}x{} workgroup did not finish "
                            "in {} s): the software HMMA keeps its shuffles",
                            shape.x, shape.y, shape.z, ProbeTimeout / 1'000'000'000);
                return false;
            }
            for (u32 index = 0; index < invocations; ++index) {
                // SubgroupSize is a constant on MoltenVK (its fixed subgroup size, 32, unless the
                // pipeline allows a varying size): the subgroup and the lane are what is probed.
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
