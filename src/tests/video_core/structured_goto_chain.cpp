// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Long chains of forward branches to one label (an unrolled "drain until done" loop) must not
// become one nested selection per branch: Metal rejects a shader nested more than 256 brackets
// deep. The programs read their input from c0[0], store through the global pointer in c0[0x310]
// and exit through one label, so the same program can also be run on a device to compare the
// emitted code before and after a change (LICHII_TEST_GOTO_CHAIN_DUMP=<dir> writes the modules).

#include <cstdlib>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"

namespace {
using namespace Shader;

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

/// Guest program with named labels; Branch() takes the label it jumps to.
struct Assembler {
    struct Insn {
        u64 word;
        std::string target; ///< BRA only
    };
    std::vector<Insn> insns;
    std::map<std::string, u32> labels;

    void Label(const std::string& name) {
        labels[name] = static_cast<u32>(insns.size());
    }
    void Emit(u64 word) {
        insns.push_back({word, {}});
    }
    /// R[dest] = c0[offset], 32 or 64 bits
    void LoadConst(u32 dest, u32 offset, bool wide) {
        Emit(0xef90000000070000ULL | dest | (u64{255} << 8) | (u64{offset} << 20) |
             (u64{wide ? 5U : 4U} << 48));
    }
    /// R[dest] = R[src] + imm
    void AddImm(u32 dest, u32 src, u32 imm) {
        Emit(0x1c00000000070000ULL | dest | (u64{src} << 8) | (u64{imm} << 20));
    }
    /// [R2:R3 + offset] = R[data], 32 bits
    void Store(u32 data, u32 offset) {
        Emit(0xeed8000000070000ULL | data | (u64{2} << 8) | (u64{1} << 45) | (u64{4} << 48) |
             (u64{offset} << 20));
    }
    /// P[pred] = R[src] (compare_op 2 == / 5 !=) imm
    void SetPred(u32 pred, u32 src, u32 compare_op, u32 imm) {
        Emit(0x3660000000070007ULL | (u64{compare_op} << 49) | (u64{7} << 39) |
             (u64{imm} << 20) | (u64{src} << 8) | (u64{pred} << 3));
    }
    /// @P[pred] BRA target
    void Branch(u32 pred, const std::string& target) {
        insns.push_back({0xe24000000000000fULL | (u64{pred} << 16), target});
    }
    void Exit() {
        Emit(0xe30000000007000fULL);
    }
    std::vector<u64> Code() const {
        std::vector<u64> code;
        for (u32 i = 0; i < insns.size(); ++i) {
            u64 word = insns[i].word;
            if (!insns[i].target.empty()) {
                const s32 offset = static_cast<s32>(Address(labels.at(insns[i].target))) -
                                   static_cast<s32>(Address(i) + 8);
                word |= (static_cast<u64>(static_cast<u32>(offset)) & 0xffffff) << 20;
            }
            code.push_back(word);
        }
        return code;
    }
};

/// One step of the chain: R0 += 1; out[step] = R0; leave for `exit` when R0 == 2 * step + 1.
void Step(Assembler& a, u32 step, const std::string& exit) {
    a.AddImm(0, 0, 1);
    a.Store(0, step * 4);
    a.SetPred(0, 0, 2, 2 * step + 1);
    a.Branch(0, exit);
}

/// R0 = input, R2:R3 = output pointer, then `steps` chain steps that all leave for "done";
/// "done" stores R0 last.
std::vector<u64> Chain(u32 steps) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    for (u32 step = 0; step < steps; ++step) {
        Step(a, step, "done");
    }
    a.Label("done");
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

/// The chain inside a loop that runs three times, with a branch out of the loop between two
/// chain branches: the branch out must leave the loop, not only the chain.
std::vector<u64> ChainWithLoopExit(u32 steps, u32 exit_after) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.AddImm(1, 255, 0);
    a.Label("head");
    a.AddImm(1, 1, 1);
    for (u32 step = 0; step < steps; ++step) {
        Step(a, step, "done");
        if (step == exit_after) {
            a.SetPred(1, 0, 2, 100);
            a.Branch(1, "after_loop");
        }
    }
    a.Label("done");
    a.Store(0, 0xff0);
    a.SetPred(2, 1, 5, 3);
    a.Branch(2, "head");
    a.Label("after_loop");
    a.Store(1, 0xff8);
    a.Store(0, 0xffc);
    a.Exit();
    return a.Code();
}

/// A branch from before the chain into the middle of it.
std::vector<u64> ChainWithEntry(u32 steps, u32 entry) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.SetPred(3, 0, 2, 5);
    a.Branch(3, "entry");
    for (u32 step = 0; step < steps; ++step) {
        if (step == entry) {
            a.Label("entry");
        }
        Step(a, step, "done");
    }
    a.Label("done");
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

class ChainEnvironment final : public Environment {
public:
    explicit ChainEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Stage::Compute;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (address == Address(index)) {
                return code[index];
            }
        }
        FAIL("Read outside shader fixture at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    TextureType ReadTextureType(u32) override {
        return TextureType::Color2D;
    }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    bool HasNativeInstructions() const override {
        return false;
    }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};

std::vector<u32> Emit(std::span<const u64> code, const std::string& name) {
    ChainEnvironment env{code};
    ObjectPool<Maxwell::Flow::Block> flow_pool;
    Maxwell::Flow::CFG cfg{env, flow_pool, Maxwell::Location{0x50}};
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    auto program = Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.min_ssbo_alignment = 16;
    std::vector<u32> spirv = Backend::SPIRV::EmitSPIRV(profile, program);
    if (const char* const dir = std::getenv("LICHII_TEST_GOTO_CHAIN_DUMP")) {
        std::ofstream file{std::string{dir} + "/" + name + ".spv", std::ios::binary};
        file.write(reinterpret_cast<const char*>(spirv.data()),
                   static_cast<std::streamsize>(spirv.size() * sizeof(u32)));
    }
    return spirv;
}

/// Most structured constructs (selections and loops) open at once, in block order: a construct
/// opened by OpSelectionMerge or OpLoopMerge stays open until its merge block's label.
size_t MaxOpenConstructs(std::span<const u32> spirv) {
    constexpr u32 OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248;
    std::vector<u32> open;
    size_t most = 0;
    for (size_t at = 5; at < spirv.size();) {
        const u32 count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
        REQUIRE(count > 0);
        if (opcode == OpLabel) {
            std::erase(open, spirv[at + 1]);
        } else if (opcode == OpSelectionMerge || opcode == OpLoopMerge) {
            open.push_back(spirv[at + 1]);
            most = std::max(most, open.size());
        }
        at += count;
    }
    return most;
}
} // namespace

TEST_CASE("Short chains of forward branches to one label keep their selections",
          "[shader][structured_control_flow]") {
    // The last branch is next to its label and disappears; each other one nests.
    for (const u32 steps : {8U, 63U}) {
        const auto code = Chain(steps);
        CHECK(MaxOpenConstructs(Emit(code, "chain" + std::to_string(steps))) == steps - 1);
    }
}

TEST_CASE("Long chains of forward branches to one label do not nest a selection per branch",
          "[shader][structured_control_flow]") {
    for (const u32 steps : {64U, 300U}) {
        const auto code = Chain(steps);
        INFO("steps " << steps);
        CHECK(MaxOpenConstructs(Emit(code, "chain" + std::to_string(steps))) <= 4);
    }
}

TEST_CASE("A loop exit between chain branches still leaves the loop",
          "[shader][structured_control_flow]") {
    const auto code = ChainWithLoopExit(70, 30);
    // The branches before the loop exit cannot share a breakable block with the exit's break,
    // so up to 31 of them nest; the 39 after it do not.
    CHECK(MaxOpenConstructs(Emit(code, "loop_exit")) <= 40);
}

TEST_CASE("A branch into the middle of a long chain", "[shader][structured_control_flow]") {
    const auto code = ChainWithEntry(70, 40);
    CHECK(MaxOpenConstructs(Emit(code, "entry")) <= 8);
}
