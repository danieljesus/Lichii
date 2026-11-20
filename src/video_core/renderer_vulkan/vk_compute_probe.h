// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <limits>
#include <span>

#include "common/common_types.h"

namespace Vulkan {

class Device;

/// Waits for a probe without a bound.
constexpr u64 NoProbeTimeout = (std::numeric_limits<u64>::max)();

enum class ProbeResult {
    Finished,
    /// Still running after the timeout. Its objects are left alive, never freed, since the device
    /// may still use them.
    TimedOut,
};

/// Runs one workgroup of a compute shader, with full subgroups, on its own submission: `words` go
/// to a storage buffer at set 0 binding 0 and come back from it (when it finishes). For self-tests
/// while the renderer is being built, before the scheduler can flush and before anything else
/// submits to the queue. The shader must be valid with REQUIRE_FULL_SUBGROUPS (its x a multiple of
/// the subgroup size). Throws vk::Exception on a Vulkan error.
ProbeResult RunComputeProbe(const Device& device, std::span<const u32> code, std::span<u32> words,
                            u64 timeout_ns = NoProbeTimeout);

} // namespace Vulkan
