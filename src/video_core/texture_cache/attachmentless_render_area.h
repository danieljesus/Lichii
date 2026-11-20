// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>

#include "common/common_types.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

/// The part of a guest scissor that bounds the framebuffer.
struct GuestScissor {
    bool enable;
    u32 max_x;
    u32 max_y;
};

/// The framebuffer a pass without attachments needs, `size` being the surface clip's.
///
/// Nothing is stored in such a pass, so only rasterisation needs the area: fragments (and their
/// storage writes and queries) come only from inside the scissors. Games leave the surface clip at
/// the hardware's 32768x32768 there, and an Apple GPU pays for every tile of that area
/// (2.7 ms for one 1x1 draw in Ocarina of Time, 47 ms the first time). So the size is cut to the
/// largest scissor any viewport can use: all of them, since a shader can pick the viewport.
/// It stays as it is when any scissor is off (then only the viewport bounds the draw), and with
/// a lower-left origin, whose viewports and scissors are flipped by the surface clip's height.
[[nodiscard]] constexpr Extent2D AttachmentlessRenderArea(Extent2D size, bool lower_left,
                                                          std::span<const GuestScissor> scissors,
                                                          u32 up_scale, u32 down_shift) {
    if (lower_left || scissors.empty()) {
        return size;
    }
    u32 width = 0;
    u32 height = 0;
    for (const GuestScissor& scissor : scissors) {
        if (!scissor.enable) {
            return size;
        }
        width = (std::max)(width, scissor.max_x);
        height = (std::max)(height, scissor.max_y);
    }
    // Scaled up as the host scissors are, rounding up, plus one for their rounding of each end.
    const auto scale = [&](u32 value) {
        const u32 slack = up_scale == 1 && down_shift == 0 ? 0 : 1;
        return ((value * up_scale + (1U << down_shift) - 1) >> down_shift) + slack;
    };
    return Extent2D{
        .width = (std::clamp)(scale(width), 1U, (std::max)(size.width, 1U)),
        .height = (std::clamp)(scale(height), 1U, (std::max)(size.height, 1U)),
    };
}

} // namespace VideoCommon
