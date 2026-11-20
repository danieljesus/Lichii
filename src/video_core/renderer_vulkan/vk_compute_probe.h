// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Vulkan {

class Device;

/// Runs one workgroup of a compute shader, with full subgroups, on its own submission: `words` go
/// to a storage buffer at set 0 binding 0 and come back from it. For self-tests while the renderer
/// is being built, before the scheduler can flush and before anything else submits to the queue.
/// Throws vk::Exception on a Vulkan error, or when it does not finish within 2 seconds.
void RunComputeProbe(const Device& device, std::span<const u32> code, std::span<u32> words);

} // namespace Vulkan
