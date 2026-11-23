// SPDX-License-Identifier: GPL-3.0-or-later
// Emitting one IR::Program twice must give the same SPIR-V: Nx2Perf::Verify() emits a deferred
// pipeline's reference module and then the pipeline's module from the same program.

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/profile.h"

namespace {
using namespace Shader;

class StubEnvironment final : public Environment {
public:
    StubEnvironment() {
        stage = Stage::Compute;
    }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return {}; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

size_t Count(const std::vector<u32>& code, u32 opcode) {
    size_t n = 0;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        REQUIRE(count > 0);
        n += (code[i] & 0xffff) == opcode;
        i += count;
    }
    return n;
}

} // Anonymous namespace

TEST_CASE("Emitting one program twice gives the same SPIR-V", "[spirv_emission]") {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const IR::U32 a{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x110))};
    const IR::U32 b{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x114))};
    // An add whose carry is read (a 64-bit address's low half, IADD.CC): the carry is a
    // pseudo-operation the emission of the add defines.
    const IR::U32 sum{ir.IAdd(a, b)};
    const IR::U1 carry{ir.GetCarryFromOp(sum)};
    const IR::U32 value{ir.Select(carry, ir.Imm32(1), sum)};
    const IR::U64 address{ir.PackUint2x32(ir.CompositeConstruct(a, b))};
    ir.WriteGlobal32(address, value);

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    StubEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    Optimization::CollectShaderInfoPass(env, program);
    program.info.native_global_bounds = true;
    program.info.constant_buffer_descriptors = {{0, 1}};

    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    const std::vector<u32> first{Backend::SPIRV::EmitSPIRV(profile, program)};
    const std::vector<u32> second{Backend::SPIRV::EmitSPIRV(profile, program)};
    constexpr u32 OpIAddCarry = 149;
    INFO("first: " << first.size() << " words, " << Count(first, OpIAddCarry)
                   << " OpIAddCarry; second: " << second.size() << " words, "
                   << Count(second, OpIAddCarry) << " OpIAddCarry");
    CHECK(Count(first, OpIAddCarry) == 1);
    REQUIRE(first == second);
}
