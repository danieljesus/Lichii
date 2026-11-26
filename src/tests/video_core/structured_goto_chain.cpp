// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Long chains of forward branches to one label (an unrolled "drain until done" loop) must not
// become one nested selection per branch: Metal rejects a shader nested more than 256 brackets
// deep. The programs read their input from c0[0], store through the global pointer in c0[0x310]
// and exit through one label, so the same program can also be run on a device to compare the
// emitted code before and after a change (LICHII_TEST_GOTO_CHAIN_DUMP=<dir> writes the modules).
//
// Not proposed upstream. The breakable blocks this branch adds hold up structurally, but they
// change which pointer or texture handle the trackers pick when it differs by path: both take
// the candidate nearest by phi depth, and flattening puts every path's value at the same depth,
// so candidates that differed now tie. In 20,000 random programs (seeds 1000-20999, 12,395 with
// a breakable block), 153,055 stores were lost only with the change and 35,777 only without it.
// Making it neutral means changing both trackers. Kept as a reference for the shape finder below.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
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
    /// P[pred] = R[src] (compare_op 1 <, 2 ==, 4 >, 5 !=) imm
    void SetPred(u32 pred, u32 src, u32 compare_op, u32 imm, bool is_signed = false) {
        Emit(0x3660000000070007ULL | (u64{compare_op} << 49) | (u64{is_signed} << 48) |
             (u64{7} << 39) | (u64{imm} << 20) | (u64{src} << 8) | (u64{pred} << 3));
    }
    /// R[dest] = R[a] + R[b]
    void AddReg(u32 dest, u32 a, u32 b) {
        Emit(0x5c10000000070000ULL | dest | (u64{a} << 8) | (u64{b} << 20));
    }
    /// @P[pred] BRA target
    void Branch(u32 pred, const std::string& target) {
        insns.push_back({0xe24000000000000fULL | (u64{pred} << 16), target});
    }
    /// @P[pred] EXIT (7: always; 8 + n: when P[n] is false)
    void Exit(u32 pred = 7) {
        Emit(0xe30000000000000fULL | (u64{pred} << 16));
    }
    /// BAR.SYNC 0
    void Barrier() {
        Emit(0xf0a8000000070000ULL | (u64{0x80} << 32) | (u64{1} << 43) | (u64{1} << 44) |
             (u64{7} << 39));
    }
    /// R[dest] = the invocation's x index in its workgroup
    void ThreadId(u32 dest) {
        Emit(0xf0c8000000070000ULL | dest | (u64{0x21} << 20));
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

/// The chain whose main path returns: the steps that do not branch away end in EXIT, and the
/// label is reached only through the branches (an "early exits to an error label" shape).
std::vector<u64> ChainEndingInExit(u32 steps) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    for (u32 step = 0; step < steps; ++step) {
        Step(a, step, "done");
    }
    a.Store(0, 0xfe0);
    a.Exit();
    a.Label("done");
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

/// A compute chain with uniform branches, a barrier after step 20 and, after step 40, a branch
/// on the invocation index: the barrier runs before any invocation can have left.
std::vector<u64> ChainWithBarrier(u32 steps, u32 barrier_after = 20) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.ThreadId(7);
    for (u32 step = 0; step < steps; ++step) {
        Step(a, step, "done");
        if (step == barrier_after) {
            a.Barrier();
        }
        if (step == 40) {
            a.SetPred(1, 7, 2, 3);
            a.Branch(1, "done");
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
                      std::span<const u64> code, Stage stage, bool demote_reorder = true) {
    ChainEnvironment env{code, stage};
    ObjectPool<Maxwell::Flow::Block> flow_pool;
    Maxwell::Flow::CFG cfg{env, flow_pool, Maxwell::Location{0x50}};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    host.needs_demote_reorder = demote_reorder;
    return Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
}

/// Constant buffer contents the programs read: c0[0] is the input, the global pointer in
/// c0[0x310 + 16 * n] points at its own 4 KiB buffer, 0x10000 * (n + 1), and every other word
/// is a fixed function of its place.
u32 CbufWord(u32 input, u32 index, u32 offset) {
    if (index == 0 && offset == 0) {
        return input;
    }
    if (index == 0 && offset >= 0x310 && offset < 0x400 && (offset - 0x310) % 16 == 0) {
        return 0x10000 * ((offset - 0x310) / 16 + 1);
    }
    if (index == 0 && offset >= 0x314 && offset < 0x400 && (offset - 0x314) % 16 == 0) {
        return 0;
    }
    return (index * 0x9e3779b1U) ^ (offset * 0x85ebca77U) ^ 0x165667b1U;
}

/// What a program stored, by guest address, and how many stores fell outside the storage buffer
/// they were tracked to (a GPU with robust buffer access drops them).
struct Stores {
    std::map<u32, u32> words;
    u32 lost{};
    bool operator==(const Stores&) const = default;
};

/// Runs a translated program on the CPU, one invocation, following its structured syntax list.
/// Phis take their values in parallel from the predecessor that was left; an undefined value
/// propagates and makes a branch or a store on it throw. A storage buffer store lands at its
/// buffer's pointer plus its offset when that is inside the buffer, and is lost otherwise; a
/// texture query returns the tracked descriptor as its value. Either way a wrongly tracked
/// handle or pointer changes the stores. Instructions it does not know throw, so a test cannot
/// pass by skipping one.
Stores Execute(const IR::Program& program, u32 input, u32 invocation = 0) {
    using IR::Opcode;
    using Type = IR::AbstractSyntaxNode::Type;
    struct Val {
        std::array<u64, 4> w{};
        bool undef{};
    };
    std::unordered_map<const IR::Inst*, Val> values;
    const auto read = [&](const IR::Value& value) -> Val {
        const IR::Value resolved{value.Resolve()};
        if (resolved.IsEmpty()) {
            return {};
        }
        if (!resolved.IsImmediate()) {
            return values.at(resolved.InstRecursive());
        }
        switch (resolved.Type()) {
        case IR::Type::U1:
            return {{resolved.U1()}};
        case IR::Type::U32:
            return {{resolved.U32()}};
        case IR::Type::U64:
            return {{resolved.U64()}};
        case IR::Type::F32:
            return {{std::bit_cast<u32>(resolved.F32())}};
        default:
            throw std::runtime_error("immediate type");
        }
    };
    const auto defined = [&](const IR::Value& value, const char* what) {
        const Val v{read(value)};
        if (v.undef) {
            throw std::runtime_error(fmt::format("{} on an undefined value", what));
        }
        return v.w[0];
    };
    const auto as_float = [](u64 word) { return std::bit_cast<f32>(static_cast<u32>(word)); };
    const auto as_word = [](f32 value) { return u64{std::bit_cast<u32>(value)}; };
    Stores out;
    bool demoted{};
    std::unordered_map<const IR::Block*, size_t> node_of;
    const auto& list{program.syntax_list};
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].type == Type::Block) {
            node_of.emplace(list[i].data.block, i);
        }
    }
    const auto store = [&](u64 binding, u64 offset, u64 value) {
        const auto& descriptor{program.info.storage_buffers_descriptors.at(binding)};
        if (static_cast<u32>(offset) >= 0x1000) {
            ++out.lost;
            return;
        }
        const u32 base{CbufWord(input, descriptor.cbuf_index, descriptor.cbuf_offset)};
        out.words[base + static_cast<u32>(offset)] = static_cast<u32>(value);
    };
    const IR::Block* previous{};
    size_t at = 0;
    for (size_t steps = 0; at < list.size(); ++steps) {
        if (steps > 200000) {
            throw std::runtime_error("no termination");
        }
        const IR::AbstractSyntaxNode& node{list[at]};
        switch (node.type) {
        case Type::Block: {
            const IR::Block* const block{node.data.block};
            // Phis read the values the predecessor left, all before any of them is written
            std::vector<std::pair<const IR::Inst*, Val>> phis;
            for (const IR::Inst& inst : *block) {
                if (inst.GetOpcode() != Opcode::Phi) {
                    continue;
                }
                std::optional<Val> value;
                for (size_t index = 0; index < inst.NumArgs(); ++index) {
                    if (inst.PhiBlock(index) == previous) {
                        value = read(inst.Arg(index));
                    }
                }
                if (!value) {
                    throw std::runtime_error("phi without the predecessor");
                }
                phis.emplace_back(&inst, *value);
            }
            for (const auto& [inst, value] : phis) {
                values[inst] = value;
            }
            for (const IR::Inst& inst : *block) {
                const Opcode op{inst.GetOpcode()};
                if (op == Opcode::Phi) {
                    continue;
                }
                bool undef{};
                std::array<Val, 4> args{};
                for (size_t index = 0; index < std::min<size_t>(inst.NumArgs(), 4); ++index) {
                    args[index] = read(inst.Arg(index));
                    undef |= args[index].undef;
                }
                const auto arg = [&](size_t index) { return args[index].w[0]; };
                Val result{};
                switch (op) {
                case Opcode::Identity:
                case Opcode::ConditionRef:
                case Opcode::BitCastU32F32:
                case Opcode::BitCastF32U32:
                    result = args[0];
                    break;
                case Opcode::Prologue:
                case Opcode::Epilogue:
                case Opcode::Void:
                case Opcode::Barrier:
                case Opcode::WorkgroupMemoryBarrier:
                case Opcode::VotedBarrier:
                case Opcode::BarrierVoteInit:
                case Opcode::BarrierVote:
                    break;
                case Opcode::UndefU1:
                case Opcode::UndefU32:
                case Opcode::UndefU64:
                    result.undef = true;
                    break;
                case Opcode::DemoteToHelperInvocation:
                    demoted = true;
                    break;
                case Opcode::LocalInvocationId:
                    result.w = {invocation, 0, 0};
                    break;
                case Opcode::GetCbufU32:
                    result.w[0] = CbufWord(input, static_cast<u32>(arg(0)),
                                           static_cast<u32>(defined(inst.Arg(1), "cbuf read")));
                    break;
                case Opcode::GetCbufU32x2: {
                    const u32 offset{static_cast<u32>(defined(inst.Arg(1), "cbuf read"))};
                    result.w = {CbufWord(input, static_cast<u32>(arg(0)), offset),
                                CbufWord(input, static_cast<u32>(arg(0)), offset + 4)};
                    break;
                }
                case Opcode::CompositeExtractU32x2:
                case Opcode::CompositeExtractU32x3:
                case Opcode::CompositeExtractF32x4:
                    result.w[0] = args[0].w[arg(1)];
                    break;
                case Opcode::CompositeConstructU32x2:
                    result.w = {arg(0), arg(1)};
                    break;
                case Opcode::IAdd32:
                    result.w[0] = static_cast<u32>(arg(0) + arg(1));
                    break;
                case Opcode::ISub32:
                    result.w[0] = static_cast<u32>(arg(0) - arg(1));
                    break;
                case Opcode::IAdd64:
                    result.w[0] = arg(0) + arg(1);
                    break;
                case Opcode::PackUint2x32:
                    result.w[0] = args[0].w[0] | (args[0].w[1] << 32);
                    break;
                case Opcode::UnpackUint2x32:
                    result.w = {arg(0) & 0xffffffff, arg(0) >> 32};
                    break;
                case Opcode::BitwiseAnd32:
                    result.w[0] = arg(0) & arg(1);
                    break;
                case Opcode::BitwiseOr32:
                    result.w[0] = arg(0) | arg(1);
                    break;
                case Opcode::ShiftLeftLogical32:
                    result.w[0] = static_cast<u32>(arg(0) << arg(1));
                    break;
                case Opcode::ShiftRightLogical32:
                    result.w[0] = static_cast<u32>(arg(0)) >> arg(1);
                    break;
                case Opcode::IEqual:
                    result.w[0] = static_cast<u32>(arg(0)) == static_cast<u32>(arg(1));
                    break;
                case Opcode::INotEqual:
                    result.w[0] = static_cast<u32>(arg(0)) != static_cast<u32>(arg(1));
                    break;
                case Opcode::ULessThan:
                    result.w[0] = static_cast<u32>(arg(0)) < static_cast<u32>(arg(1));
                    break;
                case Opcode::UGreaterThan:
                    result.w[0] = static_cast<u32>(arg(0)) > static_cast<u32>(arg(1));
                    break;
                case Opcode::ULessThanEqual:
                    result.w[0] = static_cast<u32>(arg(0)) <= static_cast<u32>(arg(1));
                    break;
                case Opcode::UGreaterThanEqual:
                    result.w[0] = static_cast<u32>(arg(0)) >= static_cast<u32>(arg(1));
                    break;
                case Opcode::SLessThan:
                    result.w[0] = static_cast<s32>(arg(0)) < static_cast<s32>(arg(1));
                    break;
                case Opcode::SGreaterThan:
                    result.w[0] = static_cast<s32>(arg(0)) > static_cast<s32>(arg(1));
                    break;
                case Opcode::SLessThanEqual:
                    result.w[0] = static_cast<s32>(arg(0)) <= static_cast<s32>(arg(1));
                    break;
                case Opcode::SGreaterThanEqual:
                    result.w[0] = static_cast<s32>(arg(0)) >= static_cast<s32>(arg(1));
                    break;
                case Opcode::LogicalNot:
                    result.w[0] = !arg(0);
                    break;
                case Opcode::LogicalOr:
                case Opcode::LogicalAnd: {
                    // A known operand that decides the result hides an undefined other one
                    const bool is_or{op == Opcode::LogicalOr};
                    for (size_t index = 0; index < 2; ++index) {
                        if (!args[index].undef && (args[index].w[0] != 0) == is_or) {
                            result.w[0] = is_or;
                            undef = false;
                            goto logical_done;
                        }
                    }
                    result.w[0] = is_or ? (arg(0) || arg(1)) : (arg(0) && arg(1));
                logical_done:
                    break;
                }
                case Opcode::SelectU1:
                case Opcode::SelectU32:
                case Opcode::SelectF32:
                    result = args[defined(inst.Arg(0), "select") ? 1 : 2];
                    undef = result.undef;
                    break;
                case Opcode::FPMul32:
                    result.w[0] = as_word(as_float(arg(0)) * as_float(arg(1)));
                    break;
                case Opcode::FPAdd32:
                    result.w[0] = as_word(as_float(arg(0)) + as_float(arg(1)));
                    break;
                case Opcode::FPClamp32:
                    result.w[0] =
                        as_word(std::clamp(as_float(arg(0)), as_float(arg(1)), as_float(arg(2))));
                    break;
                case Opcode::FPMin32:
                    result.w[0] = as_word(std::min(as_float(arg(0)), as_float(arg(1))));
                    break;
                case Opcode::FPMax32:
                    result.w[0] = as_word(std::max(as_float(arg(0)), as_float(arg(1))));
                    break;
                case Opcode::ConvertU32F32:
                    result.w[0] = static_cast<u32>(std::max(0.0f, as_float(arg(0))));
                    break;
                case Opcode::ConvertS32F32:
                    result.w[0] = static_cast<u32>(static_cast<s32>(as_float(arg(0))));
                    break;
                case Opcode::BindlessImageQueryLod:
                case Opcode::BoundImageQueryLod:
                case Opcode::ImageQueryLod: {
                    const auto& descriptor{program.info.texture_descriptors.at(
                        inst.Flags<IR::TextureInstInfo>().descriptor_index)};
                    const f32 tag{
                        static_cast<f32>(descriptor.cbuf_index * 256 + descriptor.cbuf_offset / 8)};
                    result.w = {as_word(tag), as_word(tag), 0, 0};
                    break;
                }
                case Opcode::WriteStorage32:
                    if (undef) {
                        throw std::runtime_error("store of an undefined value");
                    }
                    if (!demoted) {
                        store(arg(0), arg(1), arg(2));
                    }
                    break;
                case Opcode::WriteGlobal32:
                    if (undef) {
                        throw std::runtime_error("store of an undefined value");
                    }
                    if (!demoted) {
                        out.words[static_cast<u32>(arg(0))] = static_cast<u32>(arg(1));
                    }
                    break;
                default:
                    throw std::runtime_error(fmt::format("opcode {}", op));
                }
                result.undef |= undef;
                values[&inst] = result;
            }
            previous = block;
            ++at;
            break;
        }
        case Type::If:
            at = defined(node.data.if_node.cond, "if") ? at + 1
                                                       : node_of.at(node.data.if_node.merge);
            break;
        case Type::EndIf:
            at = node_of.at(node.data.end_if.merge);
            break;
        case Type::Loop:
            ++at;
            break;
        case Type::Break:
            at = defined(node.data.break_node.cond, "break")
                     ? node_of.at(node.data.break_node.merge)
                     : node_of.at(node.data.break_node.skip);
            break;
        case Type::Repeat:
            at = defined(node.data.repeat.cond, "repeat") ? node_of.at(node.data.repeat.loop_header)
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

/// The words a chain program stored through c0[0x310] (at 0x10000), by byte offset.
std::map<u32, u32> Run(const IR::Program& program, u32 input) {
    const Stores stores{Execute(program, input)};
    if (stores.lost != 0) {
        throw std::runtime_error("a store fell outside its storage buffer");
    }
    std::map<u32, u32> out;
    for (const auto& [address, value] : stores.words) {
        if (address < 0x10000 || address >= 0x11000) {
            throw std::runtime_error(fmt::format("store at {:#x}", address));
        }
        out[address - 0x10000] = value;
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

/// Structural problems of a translated program: syntax-list blocks missing from program.blocks
/// (they would get no SPIR-V label) and phis without operands.
std::vector<std::string> IrProblems(const IR::Program& program) {
    std::vector<std::string> problems;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == IR::AbstractSyntaxNode::Type::Block &&
            std::ranges::find(program.blocks, node.data.block) == program.blocks.end()) {
            problems.push_back("syntax-list block not in program.blocks");
        }
    }
    for (const IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : *block) {
            if (inst.GetOpcode() == IR::Opcode::Phi && inst.NumArgs() == 0) {
                problems.push_back("phi without operands");
            }
        }
    }
    return problems;
}

/// SPIR-V operands that name id 0: labels, merge and continue targets, branch targets and phi
/// parents.
size_t ZeroIds(std::span<const u32> spirv) {
    constexpr u32 OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
                  OpBranch = 249, OpBranchConditional = 250;
    size_t zeros = 0;
    for (size_t at = 5; at < spirv.size(); at += spirv[at] >> 16) {
        const u32 count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
        const auto check = [&](size_t index) { zeros += spirv[at + index] == 0; };
        switch (opcode) {
        case OpLabel:
        case OpBranch:
        case OpSelectionMerge:
            check(1);
            break;
        case OpLoopMerge:
            check(1);
            check(2);
            break;
        case OpBranchConditional:
            check(2);
            check(3);
            break;
        case OpPhi:
            for (size_t index = 4; index < count; index += 2) {
                check(index);
            }
            break;
        default:
            break;
        }
    }
    return zeros;
}

/// Whether every SPIR-V phi has exactly one entry per predecessor of its block.
bool PhisMatchPredecessors(std::span<const u32> spirv) {
    constexpr u32 OpPhi = 245, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250,
                  OpSwitch = 251;
    std::unordered_map<u32, std::vector<u32>> predecessors;
    u32 block = 0;
    for (size_t at = 5; at < spirv.size(); at += spirv[at] >> 16) {
        const u32 count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
        if (opcode == OpLabel) {
            block = spirv[at + 1];
        } else if (opcode == OpBranch) {
            predecessors[spirv[at + 1]].push_back(block);
        } else if (opcode == OpBranchConditional) {
            predecessors[spirv[at + 2]].push_back(block);
            if (spirv[at + 3] != spirv[at + 2]) {
                predecessors[spirv[at + 3]].push_back(block);
            }
        } else if (opcode == OpSwitch) {
            predecessors[spirv[at + 2]].push_back(block);
            for (size_t index = 4; index < count; index += 2) {
                predecessors[spirv[at + index]].push_back(block);
            }
        }
    }
    for (size_t at = 5; at < spirv.size(); at += spirv[at] >> 16) {
        const u32 count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
        if (opcode == OpLabel) {
            block = spirv[at + 1];
        } else if (opcode == OpPhi) {
            std::vector<u32> parents;
            for (size_t index = 4; index < count; index += 2) {
                parents.push_back(spirv[at + index]);
            }
            std::vector<u32> expected{predecessors[block]};
            std::ranges::sort(parents);
            std::ranges::sort(expected);
            expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
            if (parents != expected) {
                return false;
            }
        }
    }
    return true;
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

TEST_CASE("A long chain whose main path returns", "[shader][structured_control_flow]") {
    for (const u32 steps : {60U, 70U}) {
        INFO("steps " << steps);
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const auto code = ChainEndingInExit(steps);
        const IR::Program program = Translate(inst_pool, block_pool, code, Stage::Compute);
        CHECK(IrProblems(program).empty());
        CHECK(ZeroIds(Emit(code, "exit" + std::to_string(steps))) == 0);
        for (u32 input = 0; input < 2 * steps + 8; ++input) {
            INFO("input " << input);
            std::map<u32, u32> expected = ReferenceChain(steps, input);
            if (input >= steps) { // no branch taken: the main path stores at 0xfe0 and returns
                expected.erase(0xff0);
                expected[0xfe0] = input + steps;
            }
            REQUIRE(Run(program, input) == expected);
        }
    }
}

std::map<IR::Opcode, size_t> Barriers(const IR::Program& program) {
    std::map<IR::Opcode, size_t> barriers;
    for (const IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : *block) {
            const IR::Opcode op{inst.GetOpcode()};
            if (op == IR::Opcode::Barrier || op == IR::Opcode::VotedBarrier ||
                op == IR::Opcode::WorkgroupMemoryBarrier) {
                ++barriers[op];
            }
        }
    }
    return barriers;
}

TEST_CASE("A barrier before a divergent break stays a barrier",
          "[shader][structured_control_flow]") {
    for (const u32 steps : {60U, 70U}) {
        INFO("steps " << steps);
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const IR::Program program =
            Translate(inst_pool, block_pool, ChainWithBarrier(steps), Stage::Compute);
        CHECK(Barriers(program) == std::map<IR::Opcode, size_t>{{IR::Opcode::Barrier, 1}});
    }
}

TEST_CASE("A barrier after a divergent break is treated as the nested form treats it",
          "[shader][structured_control_flow]") {
    std::vector<std::map<IR::Opcode, size_t>> results;
    for (const u32 steps : {60U, 70U}) {
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const IR::Program program =
            Translate(inst_pool, block_pool, ChainWithBarrier(steps, 50), Stage::Compute);
        results.push_back(Barriers(program));
    }
    CHECK(!results[0].contains(IR::Opcode::Barrier));
    CHECK(results[0] == results[1]);
}

namespace {
/// A random guest program: forward conditional and unconditional branches to shared labels,
/// counted backward branches (loops that end), predicated and unconditional EXIT, KIL in
/// fragment programs and barriers in compute ones, constant buffer loads, global pointers and
/// texture handles reloaded along the way, and branches on the invocation index.
struct FuzzProgram {
    std::vector<u64> code;
    Stage stage;
};

FuzzProgram GenerateProgram(u32 seed) {
    std::mt19937 rng{seed};
    const auto pick = [&](u32 n) { return static_cast<u32>(rng() % n); };
    const Stage stage{pick(3) == 0 ? Stage::Fragment : Stage::Compute};
    const u32 slots = 6 + pick(40);
    std::vector<u32> label_slot(1 + pick(6));
    for (u32& slot : label_slot) {
        slot = pick(slots + 1);
    }
    const u32 hot = pick(static_cast<u32>(label_slot.size()));
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.AddImm(9, 255, 0); // a 2D query's coordinates are R9 and R10
    a.AddImm(10, 255, 0);
    a.LoadConst(8, 0x10, false, 2);
    a.AddImm(5, 255, 0x310);
    if (stage == Stage::Compute) {
        a.ThreadId(7);
    } else {
        a.AddImm(7, 255, 1);
    }
    for (u32 counter = 0; counter < 4; ++counter) {
        a.AddImm(20 + counter, 255, 1 + pick(3));
    }
    for (u32 pred = 0; pred < 3; ++pred) {
        a.SetPred(pred, 0, 5, pick(16));
    }
    const auto name = [](size_t label) { return fmt::format("l{}", label); };
    const auto forward = [&](u32 slot) -> std::string {
        std::vector<size_t> after;
        for (size_t label = 0; label < label_slot.size(); ++label) {
            if (label_slot[label] > slot) {
                after.push_back(label);
            }
        }
        if (label_slot[hot] > slot && pick(2) == 0) {
            return name(hot);
        }
        return after.empty() || pick(5) == 0 ? "end" : name(after[pick(after.size())]);
    };
    for (u32 slot = 0; slot <= slots; ++slot) {
        for (size_t label = 0; label < label_slot.size(); ++label) {
            if (label_slot[label] == slot) {
                a.Label(name(label));
            }
        }
        if (slot == slots) {
            break;
        }
        const u32 kind = pick(100);
        const u32 pred = pick(3);
        if (kind < 15) {
            a.AddImm(0, 0, 1 + pick(7));
        } else if (kind < 27) {
            a.Store(0, 4 * pick(64));
        } else if (kind < 32) {
            a.LoadConst(4, 0x100 + 4 * pick(16), false);
            a.AddReg(0, 0, 4);
        } else if (kind < 36) {
            a.LoadConst(2, 0x310 + 0x10 * pick(4), true);
        } else if (kind < 39) {
            a.AddImm(5, 255, 0x310 + 0x10 * pick(4));
        } else if (kind < 42) {
            a.LoadConst(2, 0, true, 0, 5);
        } else if (kind < 47 && stage == Stage::Fragment) {
            if (pick(2) == 0) {
                a.LoadConst(8, 0x10 + 8 * pick(4), false, 2);
            }
            a.QueryLod(10, 9, 8);
            a.Store(10, 0xe00 + 4 * pick(32));
        } else if (kind < 58) {
            const bool invocation{stage == Stage::Compute && pick(4) == 0};
            const u32 compares[]{1, 2, 4, 5};
            a.SetPred(pred, invocation ? 7 : 0, compares[pick(4)], invocation ? pick(3) : pick(48));
        } else if (kind < 80) {
            a.Branch(pred | (pick(2) ? 8 : 0), forward(slot));
        } else if (kind < 84) {
            a.Branch(7, forward(slot));
        } else if (kind < 89) {
            std::vector<size_t> before;
            for (size_t label = 0; label < label_slot.size(); ++label) {
                if (label_slot[label] <= slot) {
                    before.push_back(label);
                }
            }
            if (!before.empty()) {
                const u32 counter = 20 + pick(4);
                a.AddImm(counter, counter, 0xffffffff);
                a.SetPred(4, counter, 4, 0, true);
                a.Branch(4, name(before[pick(before.size())]));
            }
        } else if (kind < 93) {
            a.Exit(pred | 8);
        } else if (kind < 94) {
            a.Exit();
        } else if (kind < 98 && stage == Stage::Fragment) {
            a.Kill(pred);
        } else if (kind < 98) {
            a.Barrier();
        } else {
            a.Store(0, 4 * pick(64));
        }
    }
    a.Label("end");
    a.Store(0, 0xff0);
    a.Exit();
    return {a.Code(), stage};
}

struct Translated {
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    IR::Program program;
};

/// Translates with the given breakable block threshold (0 off), restoring the default after.
std::unique_ptr<Translated> TranslateAt(const FuzzProgram& fuzz, u32 threshold,
                                        bool demote_reorder) {
    struct Threshold {
        explicit Threshold(u32 gotos) {
            Maxwell::SetBreakableBlockGotosForTest(gotos);
        }
        ~Threshold() {
            Maxwell::SetBreakableBlockGotosForTest(64);
        }
    } const set{threshold};
    auto result{std::make_unique<Translated>()};
    result->program =
        Translate(result->inst_pool, result->block_pool, fuzz.code, fuzz.stage, demote_reorder);
    return result;
}

std::vector<u32> EmitProgram(IR::Program& program) {
    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.min_ssbo_alignment = 16;
    profile.support_demote_to_helper_invocation = true;
    return Backend::SPIRV::EmitSPIRV(profile, program);
}

struct FuzzOutcome {
    bool rejected{}; ///< the program does not translate or run without the change
    std::string rejection{};
    bool breakable{};         ///< threshold 2 made a breakable block
    bool tracking_differs{};  ///< tracked textures or storage buffers differ
    bool more_voted{};        ///< a plain barrier became voted or demoted
    u32 lost_only_with{};     ///< stores lost with the change that land without it
    u32 lost_only_without{};  ///< stores lost without the change that land with it
    u32 values_by_tracking{}; ///< words stored with another value where tracking differs
    std::string finding{};    ///< a difference the change made
};

/// Compares a program translated with breakable blocks from 2 gotos with it translated without
/// them: translation, IR and SPIR-V structure, tracked textures and storage buffers, barriers,
/// and the stores of every input and invocation.
FuzzOutcome CheckProgram(const FuzzProgram& fuzz) {
    FuzzOutcome outcome;
    std::unique_ptr<Translated> off, on;
    try {
        off = TranslateAt(fuzz, 0, false);
    } catch (const std::exception& error) {
        outcome.rejected = true;
        outcome.rejection = fmt::format("translation: {}", error.what());
        return outcome;
    }
    try {
        on = TranslateAt(fuzz, 2, false);
    } catch (const std::exception& error) {
        outcome.finding = fmt::format("translation throws: {}", error.what());
        return outcome;
    }
    for (const IR::AbstractSyntaxNode& node : on->program.syntax_list) {
        outcome.breakable |=
            node.type == IR::AbstractSyntaxNode::Type::Repeat && node.data.repeat.never;
    }
    if (!IrProblems(off->program).empty()) {
        outcome.rejected = true;
        outcome.rejection = "IR problems without the change";
        return outcome;
    }
    if (const auto problems{IrProblems(on->program)}; !problems.empty()) {
        outcome.finding = problems.front();
        return outcome;
    }
    // The descriptor each texture query uses, not only the set of descriptors
    const auto textures = [](const IR::Program& program) {
        std::vector<std::pair<u32, u32>> result;
        for (const IR::Block* const block : program.blocks) {
            for (const IR::Inst& inst : *block) {
                if (inst.GetOpcode() == IR::Opcode::ImageQueryLod) {
                    const auto& descriptor{program.info.texture_descriptors.at(
                        inst.Flags<IR::TextureInstInfo>().descriptor_index)};
                    result.emplace_back(descriptor.cbuf_index, descriptor.cbuf_offset);
                }
            }
        }
        std::ranges::sort(result);
        return result;
    };
    const auto storage = [](const IR::Program& program) {
        std::vector<std::tuple<u32, u32, bool>> result;
        for (const auto& descriptor : program.info.storage_buffers_descriptors) {
            result.emplace_back(descriptor.cbuf_index, descriptor.cbuf_offset,
                                descriptor.is_written);
        }
        std::ranges::sort(result);
        return result;
    };
    // Tracking may legitimately differ (a pointer or handle merged from paths that the two
    // structures join at different places): the stores below decide whether it matters.
    outcome.tracking_differs = textures(off->program) != textures(on->program) ||
                               storage(off->program) != storage(on->program);
    // A barrier may become voted (or keep only its memory ordering) where a flattened merge is
    // marked divergent as a whole: more conservative, counted. The other way is a finding.
    {
        const auto plain = [](const IR::Program& program) {
            const auto barriers{Barriers(program)};
            const auto it{barriers.find(IR::Opcode::Barrier)};
            return it == barriers.end() ? size_t{0} : it->second;
        };
        if (plain(on->program) > plain(off->program)) {
            outcome.finding = "a barrier stays plain that was voted or demoted";
            return outcome;
        }
        outcome.more_voted = plain(on->program) < plain(off->program);
    }
    for (u32 input = 0; input < 48; ++input) {
        for (u32 invocation = 0; invocation < 3; ++invocation) {
            Stores expected;
            try {
                expected = Execute(off->program, input, invocation);
            } catch (const std::exception& error) {
                outcome.rejected = true;
                outcome.rejection = fmt::format("run: {}", error.what());
                return outcome;
            }
            try {
                const Stores actual{Execute(on->program, input, invocation)};
                // A word both store must be the same; a word only one stores is a store the
                // other lost to its storage buffer tracking (a pointer that differs by path,
                // merged where the two structures merge differently), counted, not a finding
                for (const auto& [address, value] : expected.words) {
                    const auto it{actual.words.find(address)};
                    if (it == actual.words.end()) {
                        ++outcome.lost_only_with;
                    } else if (it->second != value) {
                        // A store lost to tracking leaves an earlier one's value; a texture
                        // query on a differently tracked handle returns another texture
                        if (actual.lost == expected.lost && !outcome.tracking_differs) {
                            outcome.finding = fmt::format(
                                "stored value differs at {:#x} for input {} invocation {}", address,
                                input, invocation);
                            return outcome;
                        }
                        ++outcome.values_by_tracking;
                    }
                }
                for (const auto& [address, value] : actual.words) {
                    outcome.lost_only_without += !expected.words.contains(address);
                }
            } catch (const std::exception& error) {
                outcome.finding = fmt::format("run throws for input {}: {}", input, error.what());
                return outcome;
            }
        }
    }
    try {
        const std::vector<u32> spirv{EmitProgram(on->program)};
        if (ZeroIds(spirv) != 0) {
            outcome.finding = "SPIR-V names id 0";
            return outcome;
        }
        if (!PhisMatchPredecessors(spirv)) {
            outcome.finding = PhisMatchPredecessors(EmitProgram(off->program))
                                  ? "SPIR-V phi entries differ from the predecessors"
                                  : std::string{};
            if (!outcome.finding.empty()) {
                return outcome;
            }
        }
        // Demote reordering on: the structure must hold as well
        auto reordered{TranslateAt(fuzz, 2, true)};
        if (const auto problems{IrProblems(reordered->program)}; !problems.empty()) {
            outcome.finding = "with demote reordering: " + problems.front();
            return outcome;
        }
        if (ZeroIds(EmitProgram(reordered->program)) != 0) {
            outcome.finding = "with demote reordering: SPIR-V names id 0";
            return outcome;
        }
        // The default threshold leaves programs this small as they were
        auto standard{TranslateAt(fuzz, 64, false)};
        if (EmitProgram(standard->program) != EmitProgram(off->program)) {
            outcome.finding = "the default threshold changes a small program";
            return outcome;
        }
    } catch (const std::exception& error) {
        outcome.finding = fmt::format("emission throws: {}", error.what());
    }
    return outcome;
}

void Fuzz(u32 first_seed, u32 count) {
    u32 rejected = 0, breakable = 0, tracking = 0, worse = 0, better = 0, lost_with = 0,
        lost_without = 0, values = 0;
    std::map<std::string, std::vector<u32>> findings;
    std::map<std::string, std::pair<u32, u32>> rejections; // count, first seed
    u32 more_voted = 0;
    for (u32 seed = first_seed; seed < first_seed + count; ++seed) {
        const FuzzOutcome outcome{CheckProgram(GenerateProgram(seed))};
        rejected += outcome.rejected;
        if (outcome.rejected) {
            auto& entry{rejections[outcome.rejection.substr(0, 70)]};
            entry.second = entry.first++ == 0 ? seed : entry.second;
        }
        more_voted += outcome.more_voted;
        breakable += outcome.breakable;
        tracking += outcome.tracking_differs;
        worse += outcome.lost_only_with > outcome.lost_only_without;
        better += outcome.lost_only_with < outcome.lost_only_without;
        lost_with += outcome.lost_only_with;
        lost_without += outcome.lost_only_without;
        values += outcome.values_by_tracking > 0;
        if (!outcome.finding.empty()) {
            findings[outcome.finding.substr(0, 60)].push_back(seed);
        }
    }
    fmt::print("structurizer fuzz: seeds {}-{}: {} programs, {} rejected without the change, "
               "{} with a breakable block, {} tracking differently: {} losing more stores, {} "
               "fewer ({} stores lost only with the change, {} only without); {} kinds of "
               "findings\n",
               first_seed, first_seed + count - 1, count, rejected, breakable, tracking, worse,
               better, lost_with, lost_without, findings.size());
    fmt::print("  {} programs with a barrier voted that was plain, {} storing other values where "
               "tracking differs\n",
               more_voted, values);
    for (const auto& [why, entry] : rejections) {
        fmt::print("  rejected {}, first seed {}: {}\n", entry.first, entry.second, why);
    }
    for (const auto& [finding, seeds] : findings) {
        fmt::print("  {} seeds, first {}: {}\n", seeds.size(), seeds.front(), finding);
    }
    CHECK(findings.empty());
    CHECK(breakable > count / 4);
}
} // namespace

TEST_CASE("Random programs translate alike with and without breakable blocks",
          "[shader][structured_control_flow]") {
    Fuzz(1, 400);
}

TEST_CASE("Random programs, long run", "[.][structurizer_fuzz_long]") {
    Fuzz(1000, 20000);
}

TEST_CASE("Random program details", "[.][structurizer_fuzz_seed]") {
    const char* const text{std::getenv("LICHII_FUZZ_SEED")};
    const u32 seed{text ? static_cast<u32>(std::stoul(text)) : 1U};
    const FuzzProgram fuzz{GenerateProgram(seed)};
    fmt::print("seed {} stage {} words {}\n", seed, static_cast<u32>(fuzz.stage), fuzz.code.size());
    for (const u32 threshold : {0U, 2U}) {
        const auto translated{TranslateAt(fuzz, threshold, false)};
        fmt::print("== threshold {}: storage", threshold);
        for (const auto& d : translated->program.info.storage_buffers_descriptors) {
            fmt::print(" c{}[{:#x}]{}", d.cbuf_index, d.cbuf_offset, d.is_written ? "w" : "");
        }
        std::string shape;
        for (const IR::AbstractSyntaxNode& node : translated->program.syntax_list) {
            using Type = IR::AbstractSyntaxNode::Type;
            switch (node.type) {
            case Type::Block: {
                std::string marks;
                for (const IR::Inst& inst : *node.data.block) {
                    const IR::Opcode op{inst.GetOpcode()};
                    marks += op == IR::Opcode::Barrier             ? "B"
                             : op == IR::Opcode::VotedBarrier      ? "V"
                             : op == IR::Opcode::WriteStorage32    ? "s"
                             : op == IR::Opcode::LocalInvocationId ? "t"
                                                                   : "";
                }
                shape += "[" + marks + "]";
                break;
            }
            case Type::If:
                shape += " if{";
                break;
            case Type::EndIf:
                shape += "} ";
                break;
            case Type::Loop:
                shape += " do{";
                break;
            case Type::Break:
                shape += " brk ";
                break;
            case Type::Repeat:
                shape += node.data.repeat.never ? "}never " : "}while ";
                break;
            case Type::Return:
                shape += " ret ";
                break;
            case Type::Unreachable:
                shape += " unr ";
                break;
            }
        }
        std::string queries;
        for (const IR::Block* const block : translated->program.blocks) {
            for (const IR::Inst& inst : *block) {
                if (inst.GetOpcode() == IR::Opcode::ImageQueryLod) {
                    const auto& d{translated->program.info.texture_descriptors.at(
                        inst.Flags<IR::TextureInstInfo>().descriptor_index)};
                    queries += fmt::format(" c{}[{:#x}]", d.cbuf_index, d.cbuf_offset);
                }
            }
        }
        fmt::print("\nqueries:{}\nshape: {}\n{}\n", queries, shape,
                   IR::DumpProgram(translated->program));
    }
    const char* const input_text{std::getenv("LICHII_FUZZ_INPUT")};
    if (input_text) {
        const u32 input{static_cast<u32>(std::stoul(input_text))};
        for (const u32 threshold : {0U, 2U}) {
            const auto translated{TranslateAt(fuzz, threshold, false)};
            const Stores stores{Execute(translated->program, input, 0)};
            fmt::print("threshold {} input {}: lost {}:", threshold, input, stores.lost);
            for (const auto& [address, value] : stores.words) {
                fmt::print(" {:#x}={:#x}", address, value);
            }
            fmt::print("\n");
        }
    }
    fmt::print("outcome: {}\n", CheckProgram(fuzz).finding);
}
