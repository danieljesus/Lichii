// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "shader_recompiler/info_serialize.h"
#include "shader_recompiler/shader_info.h"

// A precompiled compute pipeline is created from the infos kept in the precompiled file, not from
// a fresh translation. The staged software HMMA needs full subgroups, which the pipeline asks for
// from info.requires_full_guest_warps: the kept info has to carry it.
TEST_CASE("Shader info: requires_full_guest_warps survives serialization", "[video_core]") {
    const auto round_trip = [](const Shader::Info& info) {
        std::vector<u8> bytes;
        Shader::SerializeInfo(info, bytes);
        std::span<const u8> in{bytes};
        Shader::Info read{};
        REQUIRE(Shader::DeserializeInfo(in, read));
        REQUIRE(in.empty());
        return std::make_pair(read, bytes);
    };

    Shader::Info plain{};
    Shader::Info staged{};
    staged.requires_full_guest_warps = true;

    const auto [plain_read, plain_bytes] = round_trip(plain);
    const auto [staged_read, staged_bytes] = round_trip(staged);
    REQUIRE_FALSE(plain_read.requires_full_guest_warps);
    REQUIRE(staged_read.requires_full_guest_warps);
    REQUIRE_FALSE(staged_read.uses_cooperative_matrix);
    REQUIRE(plain_bytes.size() == staged_bytes.size());
    REQUIRE(plain_bytes != staged_bytes);
}
