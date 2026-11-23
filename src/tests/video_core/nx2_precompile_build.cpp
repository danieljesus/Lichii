// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_precompile.h"
#include "video_core/nx2_precompile_build.h"

namespace {

using namespace VideoCommon::Nx2Precompile;

CachedEnv ComputeEnv(u64 code_word, std::vector<std::pair<u64, u8>> pointers) {
    CachedEnv env;
    env.stage = 5;
    env.code.resize(16);
    std::memcpy(env.code.data(), &code_word, sizeof(code_word));
    env.code_size = env.code.size();
    env.cbuf_values = {{0x100000010ULL, 7}};
    env.storage_pointer_types = std::move(pointers);
    env.workgroup_size = {32, 1, 1};
    env.shared_memory_size = 256;
    return env;
}

std::vector<u8> CacheFile(const std::vector<CachedPipeline>& records) {
    std::vector<u8> out{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h', 54, 0, 0, 0};
    for (const auto& record : records) {
        const u32 count = static_cast<u32>(record.envs.size());
        out.insert(out.end(), reinterpret_cast<const u8*>(&count),
                   reinterpret_cast<const u8*>(&count) + 4);
        for (const auto& env : record.envs) {
            const auto bytes = env.Serialize();
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        out.insert(out.end(), record.key.begin(), record.key.end());
    }
    return out;
}

} // Anonymous namespace

TEST_CASE("Nx2Precompile: cached environments read back as written", "[video_core]") {
    CachedPipeline compute{.envs = {ComputeEnv(0x1234, {{0x100000020ULL, 0}})},
                           .key = std::vector<u8>(24, 1)};
    CachedPipeline mesh;
    CachedEnv env;
    env.stage = 8;
    env.code = {1, 2, 3, 4};
    env.code_size = 4;
    env.texture_types = {{3, 2}};
    env.texture_pixel_formats = {{3, -1}};
    env.cbuf_replacements = {{5, 9}};
    env.sph.assign(0x50, 0xAB);
    env.has_mesh_fields = true;
    env.mesh_registers = {4, 5, 6};
    env.mesh_primitive_header.assign(0x50, 0xCD);
    mesh.envs = {env};
    mesh.key = std::vector<u8>(1056, 2);
    const auto file = CacheFile({compute, mesh});
    const auto records = ParseCacheFile(file);
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].IsCompute());
    REQUIRE(records[0].envs[0].Serialize() == compute.envs[0].Serialize());
    REQUIRE(records[0].envs[0].workgroup_size[0] == 32);
    REQUIRE(records[1].envs[0].Serialize() == env.Serialize());
    REQUIRE(records[1].envs[0].mesh_registers[2] == 6);
    REQUIRE(records[1].key == mesh.key);
    // A cut-off file is an error, not a short read.
    auto cut = file;
    cut.resize(cut.size() - 1);
    REQUIRE_THROWS(ParseCacheFile(cut));
}

TEST_CASE("Nx2Precompile: priors keep the majority answer when it is not mapped",
          "[video_core]") {
    // Three programs ask about bank 1 offset 0x20: two answer "not a pointer", one "mapped". The
    // same program asked twice counts once.
    std::vector<CachedPipeline> records;
    for (const u64 code : {1ULL, 2ULL, 3ULL, 1ULL}) {
        const u8 answer = code == 3 ? 1 : 0;
        records.push_back({.envs = {ComputeEnv(code, {{0x100000020ULL, answer}})},
                           .key = std::vector<u8>(24, 0)});
    }
    // And bank 1 offset 0x40, mapped by majority: no prior.
    records.push_back({.envs = {ComputeEnv(7, {{0x100000040ULL, 1}})}, .key = std::vector<u8>(24)});
    const auto priors = LearnPriors(records);
    REQUIRE(priors.size() == 1);
    REQUIRE(priors[0].stage == 5);
    REQUIRE(priors[0].bank == 1);
    REQUIRE(priors[0].offset == 0x20);
    REQUIRE(priors[0].answer == 0);
    REQUIRE_FALSE(priors[0].indirect);
}

TEST_CASE("Nx2Precompile: the oracle's texture bound buffer is each stage's recorded majority",
          "[video_core]") {
    const auto env = [](u32 stage, u64 code_word, u32 texture_bound) {
        CachedEnv out;
        out.stage = stage;
        out.code.resize(16);
        std::memcpy(out.code.data(), &code_word, sizeof(code_word));
        out.code_size = out.code.size();
        out.texture_bound = texture_bound;
        out.sph.assign(0x50, 0);
        return out;
    };
    // Vertex programs 1 and 2 read their handles from buffer 0 and program 3 from 2; program 1 is
    // recorded three times and counts once. The pixel programs use 2.
    std::vector<CachedPipeline> records;
    const std::vector<std::pair<u64, u32>> vertices{{1, 0}, {1, 0}, {1, 0}, {2, 0}, {3, 2}};
    for (const auto& [vertex, bound] : vertices) {
        records.push_back({.envs = {env(0, vertex, bound), env(4, 100 + vertex, 2)},
                           .key = std::vector<u8>(1056, 0)});
    }
    const auto learned = LearnTextureBound(records);
    REQUIRE(learned.size() == 2);
    // A predicted vertex program whose lowering does not fix it gets 0, a pixel program 2.
    REQUIRE(OracleTextureBound(learned, 0) == 0);
    REQUIRE(OracleTextureBound(learned, 4) == 2);
    // A stage nothing was recorded for, and a build without recordings, keep the engine's 2.
    REQUIRE(OracleTextureBound(learned, 8) == 2);
    REQUIRE(OracleTextureBound(LearnTextureBound({}), 0) == 2);
}

TEST_CASE("Nx2Precompile: older shader archive versions nobody drew are left out",
          "[video_core]") {
    // Two versions of one archive, each with a vertex and a pixel program; the recorded play drew
    // the newer one's only.
    const std::vector<ExtractedProgram> programs{
        {.fingerprint = 10, .origin = "/Shader/Model.Product.100.bfsha.zs#BNSH@0x0#var0"},
        {.fingerprint = 11, .origin = "/Shader/Model.Product.100.bfsha.zs#BNSH@0x0#var0"},
        {.fingerprint = 20, .origin = "/Shader/Model.Product.110.bfsha.zs#BNSH@0x0#var0"},
        {.fingerprint = 21, .origin = "/Shader/Model.Product.110.bfsha.zs#BNSH@0x0#var0"},
    };
    const std::vector<ExtractedVariation> variations{
        {.label = "/Shader/Model.Product.100.bfsha.zs#var0", .programs = {0, 1}},
        {.label = "/Shader/Model.Product.110.bfsha.zs#var0", .programs = {2, 3}},
    };
    const std::vector<ScannedProgram> scanned{
        {.stage = "VertexB", .status = "ok", .code_hash = 0xA0},
        {.stage = "Fragment", .status = "ok", .code_hash = 0xA1},
        {.stage = "VertexB", .status = "ok", .code_hash = 0xB0},
        {.stage = "Fragment", .status = "ok", .code_hash = 0xB1},
    };
    CachedPipeline drawn{.envs = {}, .key = std::vector<u8>(1056, 0)};
    const u64 vertex = 0xB0, pixel = 0xB1;
    std::memcpy(drawn.key.data() + 8, &vertex, 8);
    std::memcpy(drawn.key.data() + 40, &pixel, 8);
    CachedEnv vertex_env;
    vertex_env.stage = 0;
    drawn.envs = {vertex_env};
    const auto tables = MakeGameTables(programs, variations, scanned, std::vector{drawn});
    REQUIRE(tables.left_out_files.size() == 1);
    REQUIRE(tables.rows[0].left_out);
    REQUIRE(tables.rows[1].left_out);
    REQUIRE_FALSE(tables.rows[2].left_out);
    REQUIRE(tables.pairs == std::vector<std::pair<u64, u64>>{{0xB0, 0xB1}});
    REQUIRE(tables.rows[0].fine == "/Shader/Model.Product.100.bfsha.zs#BNSH@0x0");
    REQUIRE(tables.rows[0].coarse == "/Shader/Model.Product.100.bfsha.zs");

    // Without recorded play, only the newest version stays.
    const auto unplayed = MakeGameTables(programs, variations, scanned, {});
    REQUIRE(unplayed.rows[0].left_out);
    REQUIRE_FALSE(unplayed.rows[3].left_out);
}

TEST_CASE("Nx2Precompile: every copy of a left-out program is left out", "[video_core]") {
    // The older version holds one program twice (rows 0 and 1, one fingerprint); without recorded
    // play it is left out, both copies with it, as game_tables.py marks them.
    const std::vector<ExtractedProgram> programs{
        {.fingerprint = 10, .origin = "/Shader/Model.Product.100.bfsha.zs#BNSH@0x0#var0"},
        {.fingerprint = 10, .origin = "/Shader/Model.Product.100.bfsha.zs#BNSH@0x100#var0"},
        {.fingerprint = 20, .origin = "/Shader/Model.Product.110.bfsha.zs#BNSH@0x0#var0"},
    };
    const std::vector<ExtractedVariation> variations{
        {.label = "/Shader/Model.Product.100.bfsha.zs#var0", .programs = {0}},
        {.label = "/Shader/Model.Product.110.bfsha.zs#var0", .programs = {2}},
    };
    const std::vector<ScannedProgram> scanned{
        {.stage = "VertexB", .status = "ok", .code_hash = 0xA0},
        {.stage = "VertexB", .status = "ok", .code_hash = 0xA0},
        {.stage = "VertexB", .status = "ok", .code_hash = 0xB0},
    };
    const auto tables = MakeGameTables(programs, variations, scanned, {});
    REQUIRE(tables.left_out_files.size() == 1);
    REQUIRE(tables.rows[0].left_out);
    REQUIRE(tables.rows[1].left_out);
    REQUIRE_FALSE(tables.rows[2].left_out);
}

TEST_CASE("Nx2Precompile: numbered archives that are not versions are all kept", "[video_core]") {
    // Effect_01 and Effect_02 are two archives, not two versions of one: neither is left out,
    // drawn or not.
    const std::vector<ExtractedProgram> programs{
        {.fingerprint = 10, .origin = "/Shader/Effect_01.bfsha#BNSH@0x0#var0"},
        {.fingerprint = 20, .origin = "/Shader/Effect_02.bfsha#BNSH@0x0#var0"},
    };
    const std::vector<ExtractedVariation> variations{
        {.label = "/Shader/Effect_01.bfsha#var0", .programs = {0}},
        {.label = "/Shader/Effect_02.bfsha#var0", .programs = {1}},
    };
    const std::vector<ScannedProgram> scanned{
        {.stage = "VertexB", .status = "ok", .code_hash = 0xA0},
        {.stage = "VertexB", .status = "ok", .code_hash = 0xB0},
    };
    const auto tables = MakeGameTables(programs, variations, scanned, {});
    REQUIRE(tables.left_out_files.empty());
    REQUIRE_FALSE(tables.rows[0].left_out);
    REQUIRE_FALSE(tables.rows[1].left_out);
}

TEST_CASE("Nx2Precompile: graphics key hashes cover the state the key's flags name",
          "[video_core]") {
    std::vector<u8> key(1056, 0);
    // Dynamic vertex input: the hash covers the 112-byte prefix of the state only.
    const u32 raw1 = 1U << 5;
    std::memcpy(key.data() + 48, &raw1, 4);
    const u64 base = GraphicsKeyHash(key);
    key[48 + 200] = 1;
    REQUIRE(GraphicsKeyHash(key) == base);
    key[48 + 100] = 1;
    REQUIRE(GraphicsKeyHash(key) != base);
}

TEST_CASE("Nx2Precompile: canonical keys leave the driver out", "[video_core]") {
    // A vertex/pixel pipeline with dynamic vertex input, recorded twice: in a cache of the format
    // before v2 with a driver's id and version, and in a v2 cache without them.
    CachedEnv vertex;
    vertex.stage = 0;
    vertex.sph.assign(0x50, 0);
    CachedEnv pixel;
    pixel.stage = 4;
    pixel.sph.assign(0x50, 0);
    CachedPipeline v2{.envs = {vertex, pixel}, .key = std::vector<u8>(1056, 0)};
    const u32 raw1 = 1U << 5;
    std::memcpy(v2.key.data() + 48, &raw1, 4);
    v2.key[48 + 8] = 3; // a colour format: the state is not all zero
    CachedPipeline old_format = v2;
    const std::array<u32, 2> driver{0x13, 0x6800000};
    std::memcpy(old_format.key.data() + 48 + 16, driver.data(), 8);
    REQUIRE(old_format.key != v2.key);
    REQUIRE(CanonicalKey(old_format) == CanonicalKey(v2));
    REQUIRE(CanonicalKey(v2)[48 + 8] == 3);
}

TEST_CASE("Nx2Precompile: the pixel class tells programs that discard apart", "[video_core]") {
    std::vector<u8> sph(0x50, 0);
    const u32 omap = 0xF;
    std::memcpy(sph.data() + 0x48, &omap, 4);
    const auto writes = PixelClass(sph);
    const u32 kills_pixels = 1U << 15;
    std::memcpy(sph.data(), &kills_pixels, 4);
    const auto discards = PixelClass(sph);
    REQUIRE(writes != discards);
    // Other header bits do not change the class.
    const u32 kills_and_mrt = kills_pixels | (1U << 14);
    std::memcpy(sph.data(), &kills_and_mrt, 4);
    REQUIRE(PixelClass(sph) == discards);
}

TEST_CASE("Nx2Precompile: build takes --states 1 to 16 only", "[video_core]") {
    const auto run = [](std::vector<std::string> args) {
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        return VideoCommon::Nx2Precompile::RunTool(static_cast<int>(argv.size()), argv.data());
    };
    // Each is refused while the options are read, before the game is opened.
    for (const char* states : {"0", "17", "-1", "4x", "abc", "", "4294967297"}) {
        INFO(states);
        REQUIRE(run({"nx2-precompile", "build", "--game", "missing.nup", "--out", "out.nx2prep",
                     "--cache", "missing.bin", "--states", states}) != 0);
    }
    // Only build takes it.
    REQUIRE(run({"nx2-precompile", "scan", "--pkg", "missing", "--out", "out", "--states", "4"}) !=
            0);
}
