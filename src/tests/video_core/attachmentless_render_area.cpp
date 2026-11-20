// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/attachmentless_render_area.h"

namespace {
using VideoCommon::AttachmentlessRenderArea;
using VideoCommon::Extent2D;
using VideoCommon::GuestScissor;

constexpr Extent2D Max{32768, 32768};

std::array<GuestScissor, 16> AllScissors(u32 max_x, u32 max_y) {
    std::array<GuestScissor, 16> scissors;
    scissors.fill({true, max_x, max_y});
    return scissors;
}
} // Anonymous namespace

TEST_CASE("A pass without attachments is cut to its scissors",
          "[video_core][texture_cache][render_area]") {
    // Ocarina of Time: a 1x1 draw in a 32768x32768 surface clip.
    REQUIRE(AttachmentlessRenderArea(Max, false, AllScissors(1, 1), 1, 0) == Extent2D{1, 1});

    // The largest of all scissors, each dimension on its own: a shader can pick any viewport.
    auto scissors = AllScissors(16, 16);
    scissors[3] = {true, 640, 8};
    scissors[15] = {true, 8, 360};
    REQUIRE(AttachmentlessRenderArea(Max, false, scissors, 1, 0) == Extent2D{640, 360});

    // Never past the surface clip, never empty.
    REQUIRE(AttachmentlessRenderArea({256, 128}, false, AllScissors(65535, 65535), 1, 0) ==
            Extent2D{256, 128});
    REQUIRE(AttachmentlessRenderArea(Max, false, AllScissors(0, 0), 1, 0) == Extent2D{1, 1});
}

TEST_CASE("A pass without attachments keeps its size when the scissors do not bound it",
          "[video_core][texture_cache][render_area]") {
    // A scissor that is off leaves only the viewport to bound the draw.
    auto scissors = AllScissors(1, 1);
    scissors[9].enable = false;
    REQUIRE(AttachmentlessRenderArea(Max, false, scissors, 1, 0) == Max);

    // A lower-left origin flips viewports and scissors by the surface clip's height.
    REQUIRE(AttachmentlessRenderArea(Max, true, AllScissors(1, 1), 1, 0) == Max);

    REQUIRE(AttachmentlessRenderArea(Max, false, {}, 1, 0) == Max);
}

TEST_CASE("A pass without attachments is cut to its scissors when rescaled",
          "[video_core][texture_cache][render_area]") {
    // 2x: twice the scissor, plus one for the host scissors' rounding.
    REQUIRE(AttachmentlessRenderArea({65536, 65536}, false, AllScissors(100, 50), 2, 0) ==
            Extent2D{201, 101});
    // 0.75x (3 >> 2) rounds up: 101 * 3 / 4 = 75.75.
    REQUIRE(AttachmentlessRenderArea({24576, 24576}, false, AllScissors(101, 1), 3, 2) ==
            Extent2D{77, 2});
}
