// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Long chains of forward branches to one label (an unrolled "drain until done" loop) must not
// become one nested selection per branch: Metal rejects a shader nested more than 256 brackets
// deep. The programs read their input from c0[0], store through the global pointer in c0[0x310]
// and exit through one label, so the same program can also be run on a device to compare the
// emitted code before and after a change (LICHII_TEST_GOTO_CHAIN_DUMP=<dir> writes the modules).

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
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
    /// R[dest] = c[index][R[addr] + offset], 32 or 64 bits
    void LoadConst(u32 dest, u32 offset, bool wide, u32 index = 0, u32 addr = 255) {
        Emit(0xef90000000070000ULL | dest | (u64{addr} << 8) | (u64{offset} << 20) |
             (u64{index} << 36) | (u64{wide ? 5U : 4U} << 48));
    }
    /// R[dest] = level of detail of the 2D texture whose handle is R[handle], at R[coord]
    void QueryLod(u32 dest, u32 coord, u32 handle) {
        Emit(0xdf60000000070000ULL | dest | (u64{coord} << 8) | (u64{handle} << 20) |
             (u64{1} << 28) | (u64{1} << 31));
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
        Emit(0x3660000000070007ULL | (u64{compare_op} << 49) | (u64{7} << 39) | (u64{imm} << 20) |
             (u64{src} << 8) | (u64{pred} << 3));
    }
    /// @P[pred] BRA target
    void Branch(u32 pred, const std::string& target) {
        insns.push_back({0xe24000000000000fULL | (u64{pred} << 16), target});
    }
    void Exit() {
        Emit(0xe30000000007000fULL);
    }
    /// @P[pred] KIL
    void Kill(u32 pred) {
        Emit(0xe33000000000000fULL | (u64{pred} << 16));
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

/// The chain in a fragment program, with an invocation discarded (KIL) halfway through it.
std::vector<u64> ChainWithKill(u32 steps, u32 kill_after) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    for (u32 step = 0; step < steps; ++step) {
        Step(a, step, "done");
        if (step == kill_after) {
            a.SetPred(1, 0, 2, 77);
            a.Kill(1);
        }
    }
    a.Label("done");
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

/// The chain with the global pointer reloaded from another constant halfway: the stores before
/// the reload go through c0[0x310], the ones after it through c0[0x320].
std::vector<u64> ChainWithPointerReload(u32 steps, u32 reload_after) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    for (u32 step = 0; step < steps; ++step) {
        Step(a, step, "done");
        if (step == reload_after) {
            a.LoadConst(2, 0x320, true);
        }
    }
    a.Label("done");
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

/// A constant set before the chain and changed in it: the constant buffer read through it before
/// the change is c0[0x310] and must stay a constant offset.
std::vector<u64> ChainWithConstantIndex(u32 steps, u32 change_after) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.AddImm(5, 255, 0x310);
    for (u32 step = 0; step < steps; ++step) {
        if (step == 1) {
            a.LoadConst(2, 0, true, 0, 5);
        }
        Step(a, step, "done");
        if (step == change_after) {
            a.AddImm(5, 5, 0x10);
            a.LoadConst(2, 0, true, 0, 5);
        }
    }
    a.Label("done");
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

/// The chain in a fragment program querying a bindless texture whose handle comes from c2[0x10],
/// then, halfway, from c2[0x18].
std::vector<u64> ChainWithTextureReload(u32 steps, u32 reload_after) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.AddImm(1, 255, 0);
    a.LoadConst(8, 0x10, false, 2);
    for (u32 step = 0; step < steps; ++step) {
        Step(a, step, "done");
        if (step == 5 || step == reload_after + 5) {
            a.QueryLod(10, 1, 8);
            a.Store(10, 0xf00 + step * 4);
        }
        if (step == reload_after) {
            a.LoadConst(8, 0x18, false, 2);
        }
    }
    a.Label("done");
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

class ChainEnvironment final : public Environment {
public:
    explicit ChainEnvironment(std::span<const u64> code_, Stage stage_ = Stage::Compute)
        : code{code_} {
        stage = stage_;
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

IR::Program Translate(ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
                      std::span<const u64> code, Stage stage) {
    ChainEnvironment env{code, stage};
    ObjectPool<Maxwell::Flow::Block> flow_pool;
    Maxwell::Flow::CFG cfg{env, flow_pool, Maxwell::Location{0x50}};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    host.needs_demote_reorder = true;
    return Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
}

/// Runs a translated program on the CPU, following its structured syntax list, for the integer
/// subset these programs use: c0 holds `input` at 0 and the output pointer 0x10000 at 0x310 and
/// 0x320; returns the words stored, by byte offset from the pointer the store went through.
/// Throws on an instruction it does not know, so a test cannot pass by skipping one.
std::map<u32, u32> Run(const IR::Program& program, u32 input) {
    using IR::Opcode;
    using Type = IR::AbstractSyntaxNode::Type;
    const auto cbuf = [&](u32 index, u32 offset) -> u32 {
        if (index != 0) {
            throw std::runtime_error("cbuf index");
        }
        return offset == 0 ? input : offset == 0x310 || offset == 0x320 ? 0x10000U : 0U;
    };
    std::unordered_map<const IR::Inst*, std::array<u64, 4>> values;
    const auto read = [&](const IR::Value& value) -> std::array<u64, 4> {
        const IR::Value resolved{value.Resolve()};
        if (!resolved.IsImmediate()) {
            return values.at(resolved.InstRecursive());
        }
        switch (resolved.Type()) {
        case IR::Type::U1:
            return {resolved.U1()};
        case IR::Type::U32:
            return {resolved.U32()};
        case IR::Type::U64:
            return {resolved.U64()};
        default:
            throw std::runtime_error("immediate type");
        }
    };
    std::map<u32, u32> out;
    std::unordered_map<const IR::Block*, size_t> node_of;
    const auto& list{program.syntax_list};
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].type == Type::Block) {
            node_of.emplace(list[i].data.block, i);
        }
    }
    const IR::Block* previous{};
    size_t at = 0;
    for (size_t steps = 0; at < list.size(); ++steps) {
        if (steps > 100000) {
            throw std::runtime_error("no termination");
        }
        const IR::AbstractSyntaxNode& node{list[at]};
        switch (node.type) {
        case Type::Block: {
            const IR::Block* const block{node.data.block};
            for (const IR::Inst& inst : *block) {
                const auto arg = [&](size_t index) { return read(inst.Arg(index))[0]; };
                std::array<u64, 4> result{};
                switch (inst.GetOpcode()) {
                case Opcode::Phi: {
                    bool found{};
                    for (size_t index = 0; index < inst.NumArgs(); ++index) {
                        if (inst.PhiBlock(index) == previous) {
                            result = read(inst.Arg(index));
                            found = true;
                        }
                    }
                    if (!found) {
                        throw std::runtime_error("phi without the predecessor");
                    }
                    break;
                }
                case Opcode::Identity:
                case Opcode::ConditionRef:
                case Opcode::BitCastU32F32:
                case Opcode::BitCastF32U32:
                    result = read(inst.Arg(0));
                    break;
                case Opcode::Prologue:
                case Opcode::Epilogue:
                case Opcode::Void:
                    break;
                case Opcode::UndefU1:
                case Opcode::UndefU32:
                    // Any value: a program whose result depends on it differs from the guest's
                    break;
                case Opcode::GetCbufU32:
                    result[0] = cbuf(static_cast<u32>(arg(0)), static_cast<u32>(arg(1)));
                    break;
                case Opcode::GetCbufU32x2:
                    result[0] = cbuf(static_cast<u32>(arg(0)), static_cast<u32>(arg(1)));
                    result[1] = cbuf(static_cast<u32>(arg(0)), static_cast<u32>(arg(1)) + 4);
                    break;
                case Opcode::CompositeExtractU32x2:
                    result[0] = read(inst.Arg(0))[arg(1)];
                    break;
                case Opcode::CompositeConstructU32x2:
                    result = {arg(0), arg(1)};
                    break;
                case Opcode::IAdd32:
                    result[0] = static_cast<u32>(arg(0) + arg(1));
                    break;
                case Opcode::ISub32:
                    result[0] = static_cast<u32>(arg(0) - arg(1));
                    break;
                case Opcode::IAdd64:
                    result[0] = arg(0) + arg(1);
                    break;
                case Opcode::PackUint2x32:
                    result[0] = read(inst.Arg(0))[0] | (read(inst.Arg(0))[1] << 32);
                    break;
                case Opcode::UnpackUint2x32:
                    result = {arg(0) & 0xffffffff, arg(0) >> 32};
                    break;
                case Opcode::BitwiseAnd32:
                    result[0] = arg(0) & arg(1);
                    break;
                case Opcode::ShiftRightLogical32:
                    result[0] = static_cast<u32>(arg(0)) >> arg(1);
                    break;
                case Opcode::IEqual:
                    result[0] = static_cast<u32>(arg(0)) == static_cast<u32>(arg(1));
                    break;
                case Opcode::INotEqual:
                    result[0] = static_cast<u32>(arg(0)) != static_cast<u32>(arg(1));
                    break;
                case Opcode::LogicalNot:
                    result[0] = !arg(0);
                    break;
                case Opcode::LogicalOr:
                    result[0] = arg(0) || arg(1);
                    break;
                case Opcode::LogicalAnd:
                    result[0] = arg(0) && arg(1);
                    break;
                case Opcode::SelectU32:
                    result[0] = arg(0) ? arg(1) : arg(2);
                    break;
                case Opcode::WriteStorage32:
                    out[static_cast<u32>(arg(1))] = static_cast<u32>(arg(2));
                    break;
                default:
                    throw std::runtime_error(fmt::format("opcode {}", inst.GetOpcode()));
                }
                values[&inst] = result;
            }
            previous = block;
            ++at;
            break;
        }
        case Type::If:
            at = read(node.data.if_node.cond)[0] ? at + 1 : node_of.at(node.data.if_node.merge);
            break;
        case Type::EndIf:
            at = node_of.at(node.data.end_if.merge);
            break;
        case Type::Loop:
            ++at;
            break;
        case Type::Break:
            at = read(node.data.break_node.cond)[0] ? node_of.at(node.data.break_node.merge)
                                                    : node_of.at(node.data.break_node.skip);
            break;
        case Type::Repeat:
            at = read(node.data.repeat.cond)[0] ? node_of.at(node.data.repeat.loop_header)
                                                : node_of.at(node.data.repeat.merge);
            break;
        case Type::Return:
            return out;
        default:
            throw std::runtime_error("syntax node");
        }
    }
    return out;
}

/// The guest programs above, run on the CPU: the words each stores, by byte offset.
std::map<u32, u32> ReferenceChain(u32 steps, u32 input, std::optional<u32> entry = {}) {
    std::map<u32, u32> out;
    u32 r0 = input;
    for (u32 step = entry && r0 == 5 ? *entry : 0; step < steps; ++step) {
        out[step * 4] = ++r0;
        if (r0 == 2 * step + 1) {
            break;
        }
    }
    out[0xff0] = r0;
    return out;
}

std::map<u32, u32> ReferenceLoopExit(u32 steps, u32 exit_after, u32 input) {
    std::map<u32, u32> out;
    u32 r0 = input, r1 = 0;
    for (;;) {
        ++r1;
        bool leave{};
        for (u32 step = 0; step < steps; ++step) {
            out[step * 4] = ++r0;
            if (r0 == 2 * step + 1) {
                break;
            }
            if (step == exit_after && r0 == 100) {
                leave = true;
                break;
            }
        }
        if (leave) {
            break;
        }
        out[0xff0] = r0;
        if (r1 == 3) {
            break;
        }
    }
    out[0xff8] = r1;
    out[0xffc] = r0;
    return out;
}

/// The constant buffer offsets of the storage buffers a program uses, sorted.
std::vector<u32> StorageBufferOffsets(const IR::Program& program) {
    std::vector<u32> offsets;
    for (const auto& descriptor : program.info.storage_buffers_descriptors) {
        offsets.push_back(descriptor.cbuf_offset);
    }
    std::ranges::sort(offsets);
    return offsets;
}

std::vector<u32> Emit(std::span<const u64> code, const std::string& name,
                      Stage stage = Stage::Compute) {
    ChainEnvironment env{code, stage};
    ObjectPool<Maxwell::Flow::Block> flow_pool;
    Maxwell::Flow::CFG cfg{env, flow_pool, Maxwell::Location{0x50}};
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    host.needs_demote_reorder = true;
    auto program = Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.min_ssbo_alignment = 16;
    profile.support_demote_to_helper_invocation = true;
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
/// Conditional branches out of a loop (to its merge) other than the loop's own back edge, and how
/// many of them come right after their own OpSelectionMerge.
std::pair<size_t, size_t> LoopExits(std::span<const u32> spirv) {
    constexpr u32 OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
                  OpBranchConditional = 250;
    std::unordered_map<u32, u32> header_of_merge;
    u32 block = 0;
    for (size_t at = 5; at < spirv.size(); at += spirv[at] >> 16) {
        const u32 opcode = spirv[at] & 0xffff;
        if (opcode == OpLabel) {
            block = spirv[at + 1];
        } else if (opcode == OpLoopMerge) {
            header_of_merge[spirv[at + 1]] = block;
        }
    }
    size_t exits = 0, selected = 0;
    u32 previous = 0;
    for (size_t at = 5; at < spirv.size(); at += spirv[at] >> 16) {
        const u32 opcode = spirv[at] & 0xffff;
        if (opcode == OpBranchConditional) {
            for (const u32 target : {spirv[at + 2], spirv[at + 3]}) {
                const auto loop = header_of_merge.find(target);
                const u32 other = target == spirv[at + 2] ? spirv[at + 3] : spirv[at + 2];
                if (loop != header_of_merge.end() && other != loop->second) {
                    ++exits;
                    selected += previous == OpSelectionMerge;
                }
            }
        }
        previous = opcode;
    }
    return {exits, selected};
}

/// Loop safety counters: private variables initialized to the iteration limit (0x2000).
size_t SafetyCounters(std::span<const u32> spirv) {
    constexpr u32 OpConstant = 43, OpVariable = 59, Private = 6;
    std::vector<u32> limits;
    size_t counters = 0;
    for (size_t at = 5; at < spirv.size(); at += spirv[at] >> 16) {
        const u32 count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
        if (opcode == OpConstant && count == 4 && spirv[at + 3] == 0x2000) {
            limits.push_back(spirv[at + 2]);
        } else if (opcode == OpVariable && count == 5 && spirv[at + 3] == Private &&
                   std::ranges::find(limits, spirv[at + 4]) != limits.end()) {
            ++counters;
        }
    }
    return counters;
}

/// Whether every demote (OpDemoteToHelperInvocation) is emitted inside a loop construct.
bool DemotesInsideLoops(std::span<const u32> spirv) {
    constexpr u32 OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpDemote = 5380;
    std::vector<std::pair<u32, bool>> open; // merge label, is a loop
    size_t demotes = 0;
    for (size_t at = 5; at < spirv.size();) {
        const u32 count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
        REQUIRE(count > 0);
        if (opcode == OpLabel) {
            std::erase_if(open, [&](const auto& entry) { return entry.first == spirv[at + 1]; });
        } else if (opcode == OpSelectionMerge || opcode == OpLoopMerge) {
            open.emplace_back(spirv[at + 1], opcode == OpLoopMerge);
        } else if (opcode == OpDemote) {
            ++demotes;
            if (std::ranges::none_of(open, [](const auto& entry) { return entry.second; })) {
                return false;
            }
        }
        at += count;
    }
    return demotes > 0;
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
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    const IR::Program program = Translate(inst_pool, block_pool, code, Stage::Compute);
    // Inputs 0-39 leave the chain at step 0-39 in the first pass; 69 leaves the loop at step 30
    // (R0 == 100); others reach it in a later pass or not at all.
    for (u32 input = 0; input < 420; ++input) {
        INFO("input " << input);
        REQUIRE(Run(program, input) == ReferenceLoopExit(70, 30, input));
    }
}

TEST_CASE("Long chains compute what the guest program does", "[shader][structured_control_flow]") {
    for (const u32 steps : {63U, 64U, 300U}) {
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const auto code = Chain(steps);
        const IR::Program program = Translate(inst_pool, block_pool, code, Stage::Compute);
        for (u32 input = 0; input < 2 * steps + 8; ++input) {
            INFO("steps " << steps << " input " << input);
            REQUIRE(Run(program, input) == ReferenceChain(steps, input));
        }
    }
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    const auto code = ChainWithEntry(70, 40);
    const IR::Program program = Translate(inst_pool, block_pool, code, Stage::Compute);
    for (u32 input = 0; input < 160; ++input) {
        INFO("entry input " << input);
        REQUIRE(Run(program, input) == ReferenceChain(70, input, 40));
    }
}

TEST_CASE("A branch into the middle of a long chain", "[shader][structured_control_flow]") {
    const auto code = ChainWithEntry(70, 40);
    CHECK(MaxOpenConstructs(Emit(code, "entry")) <= 8);
}

TEST_CASE("A discard inside a long chain stays inside its breakable block",
          "[shader][structured_control_flow]") {
    // Reordering demotes to the end of the program (needs_demote_reorder) must not take one out
    // of a breakable block: its condition is computed on a path the block may have left.
    const auto code = ChainWithKill(70, 40);
    const auto spirv = Emit(code, "kill", Stage::Fragment);
    CHECK(MaxOpenConstructs(spirv) <= 4);
    CHECK(DemotesInsideLoops(spirv));
}

TEST_CASE("A pointer reloaded inside a long chain keeps both storage buffers",
          "[shader][structured_control_flow]") {
    // 60 steps take the selections, 70 the breakable block: both must track the same.
    for (const u32 steps : {60U, 70U}) {
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const auto code = ChainWithPointerReload(steps, 30);
        const IR::Program program = Translate(inst_pool, block_pool, code, Stage::Compute);
        INFO("steps " << steps);
        CHECK(StorageBufferOffsets(program) == std::vector<u32>{0x310, 0x320});
    }
}

TEST_CASE("A constant set before a long chain is still a constant inside it",
          "[shader][structured_control_flow]") {
    // 60 steps take the selections, 70 the breakable block: both must track the same.
    for (const u32 steps : {60U, 70U}) {
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const auto code = ChainWithConstantIndex(steps, 30);
        const IR::Program program = Translate(inst_pool, block_pool, code, Stage::Compute);
        INFO("steps " << steps);
        CHECK(StorageBufferOffsets(program) == std::vector<u32>{0x310, 0x320});
    }
}

TEST_CASE("A texture handle reloaded inside a long chain keeps both textures",
          "[shader][structured_control_flow]") {
    for (const u32 steps : {60U, 70U}) {
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const auto code = ChainWithTextureReload(steps, 30);
        const IR::Program program = Translate(inst_pool, block_pool, code, Stage::Fragment);
        std::vector<u32> offsets;
        for (const auto& descriptor : program.info.texture_descriptors) {
            CHECK(descriptor.cbuf_index == 2);
            offsets.push_back(descriptor.cbuf_offset);
        }
        std::ranges::sort(offsets);
        INFO("steps " << steps);
        CHECK(offsets == std::vector<u32>{0x10, 0x18});
    }
}

TEST_CASE("Breaks out of a breakable block are selections and it has no loop safety counter",
          "[shader][structured_control_flow]") {
    // Without its own selection merge a conditional break leaves the rest of the block to the
    // false branch, and SPIR-V cross-compilers nest it under an else: depth again.
    const auto spirv = Emit(Chain(300), "chain300");
    const auto [exits, selected] = LoopExits(spirv);
    CHECK(exits >= 299);
    CHECK(selected == exits);
    CHECK(SafetyCounters(spirv) == 0);
    // A real loop keeps its counter; its own breaks are left as they were.
    CHECK(SafetyCounters(Emit(ChainWithLoopExit(70, 30), "loop_exit")) == 1);
}
