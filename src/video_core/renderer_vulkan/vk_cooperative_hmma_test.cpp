// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_compute_probe.h"
#include "video_core/renderer_vulkan/vk_cooperative_hmma_test.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

using Shader::Backend::SPIRV::COOPERATIVE_HMMA_8X8X8_TEST_LANES;
using Shader::Backend::SPIRV::COOPERATIVE_HMMA_TEST_LANE_WORDS;

/// The half-precision encoding of a small integer. Products and sums of the test's integers stay
/// exact in half precision, so the results compare bit for bit, without any rounding involved.
u16 IntegerToHalf(s32 value) {
    if (value == 0) {
        return 0;
    }
    const u32 magnitude = static_cast<u32>(value < 0 ? -value : value);
    const u32 exponent = static_cast<u32>(std::bit_width(magnitude)) - 1;
    const u32 mantissa = (magnitude << (10 - exponent)) & 0x3ff;
    return static_cast<u16>((value < 0 ? 0x8000U : 0U) | ((exponent + 15) << 10) | mantissa);
}

u32 PackHalves(s32 low, s32 high) {
    return IntegerToHalf(low) | (static_cast<u32>(IntegerToHalf(high)) << 16);
}

struct Matrices {
    std::array<std::array<s32, 16>, 16> a{};
    std::array<std::array<s32, 8>, 16> b{};
    std::array<std::array<s32, 8>, 16> c{};
    std::array<std::array<s32, 8>, 16> d{};
};

Matrices MakeMatrices() {
    // Every element differs from most others, so that any misplaced element changes the results.
    u32 state = 0x2545f491;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<s32>((state >> 16) % 7) - 3;
    };
    Matrices m;
    for (auto& row : m.a) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (auto& row : m.b) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (auto& row : m.c) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (size_t row = 0; row < 16; ++row) {
        for (size_t column = 0; column < 8; ++column) {
            s32 sum = m.c[row][column];
            for (size_t k = 0; k < 16; ++k) {
                sum += m.a[row][k] * m.b[k][column];
            }
            m.d[row][column] = sum;
        }
    }
    return m;
}

/// Writes one lane's guest registers, in the layout of mma.m16n8k16: with g = lane / 4 and
/// t = lane % 4, a lane holds rows g and g + 8 of A, C and D, and column g of B. Every warp of
/// the workgroup gets the same matrices.
void WriteLane(const Matrices& m, u32 lane, std::span<u32> words) {
    lane %= GuestWarpSize;
    const size_t g = lane >> 2;
    const size_t t = lane & 3;
    const size_t k = t * 2;
    words[0] = PackHalves(m.a[g][k], m.a[g][k + 1]);
    words[1] = PackHalves(m.a[g + 8][k], m.a[g + 8][k + 1]);
    words[2] = PackHalves(m.a[g][k + 8], m.a[g][k + 9]);
    words[3] = PackHalves(m.a[g + 8][k + 8], m.a[g + 8][k + 9]);
    words[4] = PackHalves(m.b[k][g], m.b[k + 1][g]);
    words[5] = PackHalves(m.b[k + 8][g], m.b[k + 9][g]);
    words[6] = PackHalves(m.c[g][k], m.c[g][k + 1]);
    words[7] = PackHalves(m.c[g + 8][k], m.c[g + 8][k + 1]);
    words[8] = 0;
    words[9] = 0;
}

bool SameHalf(u32 got, s32 expected) {
    const u32 want = IntegerToHalf(expected);
    // A sum that cancels to zero may have either sign.
    return got == want || (expected == 0 && (got & 0x7fff) == 0);
}

u32 CountWrongLanes(const Matrices& m, u32 lanes, std::span<const u32> words) {
    u32 wrong = 0;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const size_t g = (lane % GuestWarpSize) >> 2;
        const size_t k = (lane & 3) * 2;
        const auto lane_words = words.subspan(lane * COOPERATIVE_HMMA_TEST_LANE_WORDS);
        const u32 d0 = lane_words[8];
        const u32 d1 = lane_words[9];
        if (!SameHalf(d0 & 0xffff, m.d[g][k]) || !SameHalf(d0 >> 16, m.d[g][k + 1]) ||
            !SameHalf(d1 & 0xffff, m.d[g + 8][k]) || !SameHalf(d1 >> 16, m.d[g + 8][k + 1])) {
            ++wrong;
        }
    }
    return wrong;
}

/// Runs `code` on the test's matrices in `lanes` lanes and counts the lanes with a wrong result.
u32 RunAndCountWrongLanes(const Device& device, std::span<const u32> code, u32 lanes) {
    const Matrices matrices = MakeMatrices();
    std::vector<u32> words(lanes * COOPERATIVE_HMMA_TEST_LANE_WORDS);
    for (u32 lane = 0; lane < lanes; ++lane) {
        WriteLane(matrices, lane,
                  std::span{words}.subspan(lane * COOPERATIVE_HMMA_TEST_LANE_WORDS,
                                           COOPERATIVE_HMMA_TEST_LANE_WORDS));
    }
    RunComputeProbe(device, code, words);
    return CountWrongLanes(matrices, lanes, words);
}

bool RunTest(const Device& device) {
    const std::vector<u32> code =
        Shader::Backend::SPIRV::EmitCooperativeHmmaTest(device.SupportedSpirvVersion());
    const u32 wrong = RunAndCountWrongLanes(device, code, GuestWarpSize);
    if (wrong != 0) {
        LOG_WARNING(Render_Vulkan,
                    "Cooperative matrices lay out elements differently on this driver ({} of {} "
                    "lanes wrong): guest tensor operations stay in shader emulation",
                    wrong, GuestWarpSize);
        return false;
    }
    LOG_INFO(Render_Vulkan, "Guest tensor operations run on cooperative matrices");
    return true;
}

bool Run8x8x8Test(const Device& device, const Shader::Profile& profile) {
    const std::vector<u32> code =
        Shader::Backend::SPIRV::EmitCooperativeHmma8x8x8Test(profile);
    const u32 wrong = RunAndCountWrongLanes(device, code, COOPERATIVE_HMMA_8X8X8_TEST_LANES);
    if (wrong != 0) {
        LOG_WARNING(Render_Vulkan,
                    "8x8x8 cooperative matrices gave wrong HMMA results on this driver ({} of {} "
                    "lanes wrong): guest tensor operations stay in shader emulation",
                    wrong, COOPERATIVE_HMMA_8X8X8_TEST_LANES);
        return false;
    }
    LOG_INFO(Render_Vulkan, "Guest tensor operations run on 8x8x8 cooperative matrices");
    return true;
}

} // Anonymous namespace

bool TestCooperativeHmma(const Device& device) {
    try {
        return RunTest(device);
    } catch (const std::exception& exception) {
        LOG_WARNING(Render_Vulkan,
                    "Cooperative matrix self-test failed ({}): guest tensor operations stay in "
                    "shader emulation",
                    exception.what());
        return false;
    }
}

bool TestCooperativeHmma8x8x8(const Device& device, const Shader::Profile& profile) {
    try {
        return Run8x8x8Test(device, profile);
    } catch (const std::exception& exception) {
        LOG_WARNING(Render_Vulkan,
                    "8x8x8 cooperative matrix self-test failed ({}): guest tensor operations stay "
                    "in shader emulation",
                    exception.what());
        return false;
    }
}

} // namespace Vulkan
