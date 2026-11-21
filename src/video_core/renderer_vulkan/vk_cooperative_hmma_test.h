// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Shader {
struct Profile;
}

namespace Vulkan {

class Device;

/// Runs the HMMA.16816.F16 cooperative matrix lowering once on known matrices and compares every
/// lane's result. Vulkan leaves the per-lane order of a cooperative matrix's elements to the driver
/// and the lowering relies on it, so a driver with another order keeps the shader emulation.
[[nodiscard]] bool TestCooperativeHmma(const Device& device);

/// Runs HMMA.16816.F16 on known matrices through the lowering guest programs get with `profile`,
/// on 8x8x8 cooperative matrices through shared memory, in two warps of one workgroup, and
/// compares every lane's result with the CPU's. The device's matrices, its SIMD groups as guest
/// warps and the conversion to Metal (MoltenVK) all have to hold for it, so any of them failing
/// keeps the shader emulation. `profile` must run HMMA on 8x8x8 matrices.
[[nodiscard]] bool TestCooperativeHmma8x8x8(const Device& device, const Shader::Profile& profile);

} // namespace Vulkan
