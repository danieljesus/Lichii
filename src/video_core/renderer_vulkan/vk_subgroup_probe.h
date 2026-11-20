// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Vulkan {

class Device;

/// Whether the device's compute subgroups are 32-lane warps numbered in invocation order, as the
/// staged software HMMA needs and as Apple GPUs were measured to run them (Device::
/// AssumesAppleComputeSubgroupsAreGuestWarps): with full subgroups, in every workgroup the staged
/// HMMA may run with (x a multiple of 32 up to 1024 invocations; some with y and z too), every
/// invocation i must see SubgroupSize 32, SubgroupId i / 32 and lane i % 32. Needs
/// computeFullSubgroups and SPIR-V 1.3.
[[nodiscard]] bool ProbeComputeSubgroupsAreGuestWarps(const Device& device);

} // namespace Vulkan
