// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A negative value converted to an unsigned fixed-point or integer value raised Invalid Operation
// even when it rounds to zero. In Arm's FPToFixed the value is rounded first, and only a result
// below zero is out of range; one that rounds to zero gives 0 with Inexact. An Apple M6 gives
// FCVTNU W0, H1 with H1 = 0x83FF (a negative denormal) and FPCR 0x04C00000 the result 0 and
// FPSR IXC (0x10).

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <fmt/format.h>

#include "common/common_types.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op/FPToFixed.h"
#include "dynarmic/common/fp/rounding_mode.h"
#include "dynarmic/interface/A64/a64.h"

namespace {

using Dynarmic::FP::FPCR;
using Dynarmic::FP::FPSR;
using Dynarmic::FP::RoundingMode;

constexpr u32 IOC = 0x01;
constexpr u32 IXC = 0x10;

enum Value : size_t { MinusZero, Denormal, MinusPoint4, MinusHalf, MinusPoint6, MinusOne, Values };

// Bit patterns of the inputs, in half, single and double precision.
constexpr std::array<u16, Values> Half{0x8000, 0x83FF, 0xB666, 0xB800, 0xB8CD, 0xBC00};
constexpr std::array<u32, Values> Single{0x80000000, 0x80000001, 0xBECCCCCD,
                                         0xBF000000, 0xBF19999A, 0xBF800000};
constexpr std::array<u64, Values> Double{0x8000000000000000, 0x800FFFFFFFFFFFFF,
                                         0xBFD999999999999A, 0xBFE0000000000000,
                                         0xBFE3333333333333, 0xBFF0000000000000};

struct Expected {
    u64 result;
    u32 fpsr;
};

constexpr std::array Modes{RoundingMode::ToNearest_TieEven, RoundingMode::TowardsPlusInfinity,
                           RoundingMode::TowardsMinusInfinity, RoundingMode::TowardsZero,
                           RoundingMode::ToNearest_TieAwayFromZero};
constexpr std::array<const char*, Modes.size()> ModeNames{"RN", "RP", "RM", "RZ", "RA"};

// The rounded value of each input under each mode: 0 or -1.
constexpr std::array<std::array<int, Modes.size()>, Values> Rounded{{
    {0, 0, 0, 0, 0},     // -0.0
    {0, 0, -1, 0, 0},    // negative denormal
    {0, 0, -1, 0, 0},    // -0.4
    {0, 0, -1, 0, -1},   // -0.5: ties to even give 0, ties away give -1
    {-1, 0, -1, 0, -1},  // -0.6
    {-1, -1, -1, -1, -1} // -1.0
}};

Expected ExpectedFor(size_t value, size_t mode, bool unsigned_, size_t ibits) {
    const bool exact = value == MinusZero || value == MinusOne;
    const int rounded = Rounded[value][mode];
    if (unsigned_ && rounded < 0) {
        return {0, IOC};
    }
    const u64 mask = ibits == 64 ? ~u64{0} : (u64{1} << ibits) - 1;
    return {static_cast<u64>(static_cast<s64>(rounded)) & mask, exact ? 0 : IXC};
}

template <typename FPT>
void CheckAll(const std::array<FPT, Values>& inputs) {
    const FPCR fpcr;
    for (size_t ibits : {16, 32, 64}) {
        for (bool unsigned_ : {true, false}) {
            for (size_t value = 0; value < Values; ++value) {
                for (size_t mode = 0; mode < Modes.size(); ++mode) {
                    FPSR fpsr;
                    const u64 result = Dynarmic::FP::FPToFixed<FPT>(
                        ibits, inputs[value], 0, unsigned_, fpcr, Modes[mode], fpsr);
                    const Expected expected = ExpectedFor(value, mode, unsigned_, ibits);
                    INFO(fmt::format("input {:#x}, {} bits, {}, {}", inputs[value], ibits,
                                     unsigned_ ? "unsigned" : "signed", ModeNames[mode]));
                    CHECK(result == expected.result);
                    CHECK(fpsr.Value() == expected.fpsr);
                }
            }
        }
    }
}

} // namespace

TEST_CASE("FPToFixed rounds a negative value before it checks the unsigned range",
          "[jit][fp_to_fixed]") {
    SECTION("half") {
        CheckAll(Half);
    }
    SECTION("single") {
        CheckAll(Single);
    }
    SECTION("double") {
        CheckAll(Double);
    }
}

TEST_CASE("FPToFixed scales by the fraction bits before it rounds a negative value",
          "[jit][fp_to_fixed]") {
    // -2^-5 with 4 fraction bits is -0.5 units: ties to even and towards zero give 0, towards
    // minus infinity and ties away give -1.
    constexpr u32 input = 0xBD000000;
    const FPCR fpcr;
    for (size_t mode = 0; mode < Modes.size(); ++mode) {
        INFO(ModeNames[mode]);
        const bool below = Modes[mode] == RoundingMode::TowardsMinusInfinity ||
                           Modes[mode] == RoundingMode::ToNearest_TieAwayFromZero;
        FPSR fpsr;
        CHECK(Dynarmic::FP::FPToFixed<u32>(32, input, 4, true, fpcr, Modes[mode], fpsr) == 0);
        CHECK(fpsr.Value() == (below ? IOC : IXC));
        // With no fraction bits it is -0.03125, which only towards minus infinity rounds below 0.
        FPSR whole;
        CHECK(Dynarmic::FP::FPToFixed<u32>(32, input, 0, true, fpcr, Modes[mode], whole) == 0);
        CHECK(whole.Value() == (Modes[mode] == RoundingMode::TowardsMinusInfinity ? IOC : IXC));
    }
}

TEST_CASE("FPToFixed does not take a negative value rounded up for an overflow",
          "[jit][fp_to_fixed]") {
    // -2147483647.5 and -32767.5 towards zero or plus infinity are -2147483647 and -32767, which
    // fit; the overflow check added the round-up to their magnitude.
    const FPCR fpcr;
    for (const auto mode : {RoundingMode::TowardsZero, RoundingMode::TowardsPlusInfinity}) {
        FPSR fpsr;
        CHECK(Dynarmic::FP::FPToFixed<u64>(32, 0xC1DFFFFFFFE00000, 0, false, fpcr, mode, fpsr) ==
              0x80000001);
        CHECK(fpsr.Value() == IXC);
        FPSR fpsr16;
        CHECK(Dynarmic::FP::FPToFixed<u32>(16, 0xC6FFFF00, 0, false, fpcr, mode, fpsr16) ==
              0x8001);
        CHECK(fpsr16.Value() == IXC);
    }
    // Below the range they still overflow.
    FPSR fpsr;
    CHECK(Dynarmic::FP::FPToFixed<u64>(32, 0xC1E0000000200000, 0, false, fpcr,
                                       RoundingMode::TowardsPlusInfinity, fpsr) == 0x80000000);
    CHECK(fpsr.Value() == IOC);
}

namespace {

constexpr u64 CodeBase = 0x1000;
constexpr u32 Svc = 0xD4000001;

class Environment final : public Dynarmic::A64::UserCallbacks {
public:
    Dynarmic::A64::Jit* jit{};
    std::vector<u32> code;

    std::optional<u32> MemoryReadCode(u64 address) override {
        const u64 index = (address - CodeBase) / 4;
        if (address < CodeBase || index >= code.size()) {
            return std::nullopt;
        }
        return code[index];
    }
    u64 MemoryRead(u64, size_t) override {
        FAIL("Unexpected read");
        return 0;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        FAIL("Unexpected read");
        return {};
    }
    void MemoryWrite(u64, u64, size_t) override {
        FAIL("Unexpected write");
    }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {
        FAIL("Unexpected write");
    }
    bool MemoryWriteExclusive64(u64, u64, u64) override {
        FAIL("Unexpected write");
        return false;
    }
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override {
        FAIL("Unexpected exception");
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

struct Conversion {
    u32 fpsr;
    u64 result;
};

Conversion RunConversion(u32 instruction, u64 input, u32 fpcr) {
    Environment env;
    env.code = {instruction, Svc};
    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    Dynarmic::A64::Jit jit{config};
    env.jit = &jit;
    jit.SetVector(1, {input, 0});
    jit.SetRegister(0, ~u64{0});
    jit.SetFpcr(fpcr);
    jit.SetFpsr(0);
    jit.SetPC(CodeBase);
    jit.Run();
    return {jit.GetFpsr(), jit.GetRegister(0)};
}

// FCVT<mode>U W0, <H|S|D>1: rmode 00 N, 01 P, 10 M, 11 Z; opcode 001 (U).
constexpr u32 FcvtU(u32 ftype, u32 rmode) {
    return 0x1E210020 | (ftype << 22) | (rmode << 19);
}
// FCVTAU W0, <H|S|D>1.
constexpr u32 FcvtAU(u32 ftype) {
    return 0x1E250020 | (ftype << 22);
}

} // namespace

TEST_CASE("The JIT converts a negative value that rounds to zero to unsigned with Inexact",
          "[jit][fp_to_fixed]") {
    // The reference result from an Apple M6.
    const auto reference = RunConversion(FcvtU(3, 0), 0x83FF, 0x04C00000);
    CHECK(reference.result == 0);
    CHECK(reference.fpsr == IXC);

    // These conversions reach FP::FPToFixed through the x64 fallback: every half-precision one,
    // and FCVTAU, which has no SSE rounding mode. The SSE path for the other single and double
    // conversions saturates without raising Invalid Operation, which this does not cover.
    // FPSR.IOC and IXC are sticky; RunConversion starts from 0 each time.
    constexpr std::array<u32, 5> RModeOf{0, 1, 2, 3, 0};
    for (u32 ftype : {3u, 0u, 1u}) {
        for (size_t value = 0; value < Values; ++value) {
            const u64 input = ftype == 3   ? u64{Half[value]}
                              : ftype == 0 ? u64{Single[value]}
                                           : Double[value];
            for (size_t mode = 0; mode < Modes.size(); ++mode) {
                if (ftype != 3 && mode != 4) {
                    continue;
                }
                const u32 instruction =
                    mode == 4 ? FcvtAU(ftype) : FcvtU(ftype, RModeOf[mode]);
                INFO(fmt::format("ftype {}, input {:#x}, {}", ftype, input, ModeNames[mode]));
                const auto got = RunConversion(instruction, input, 0);
                const Expected expected = ExpectedFor(value, mode, true, 32);
                CHECK(got.result == expected.result);
                CHECK(got.fpsr == expected.fpsr);
            }
        }
    }
}

namespace {

struct Variant {
    u32 instruction;
    bool double_input;
    size_t ibits;
    bool unsigned_;
    RoundingMode mode;
    size_t fbits;
    std::string name;
};

// FCVT{N,P,M,Z,A}{S,U} and FCVTZ{S,U} to fixed point, <W|X>0 from <S|D>1.
std::vector<Variant> AllVariants() {
    constexpr std::array<RoundingMode, 4> RModes{
        RoundingMode::ToNearest_TieEven, RoundingMode::TowardsPlusInfinity,
        RoundingMode::TowardsMinusInfinity, RoundingMode::TowardsZero};
    std::vector<Variant> variants;
    for (u32 ftype : {0u, 1u}) {
        for (u32 sf : {0u, 1u}) {
            for (u32 u : {0u, 1u}) {
                const size_t ibits = sf ? 64 : 32;
                const auto name = [&](const char* op, size_t fbits) {
                    return fmt::format("FCVT{}{} {}0, {}1{}", op, u ? 'U' : 'S', sf ? 'X' : 'W',
                                       ftype ? 'D' : 'S',
                                       fbits ? fmt::format(", #{}", fbits) : std::string{});
                };
                const u32 base = (sf << 31) | (ftype << 22) | (u << 16) | 0x20;
                for (u32 rmode = 0; rmode < 4; ++rmode) {
                    variants.push_back({0x1E200000 | base | (rmode << 19), ftype == 1, ibits,
                                        u == 1, RModes[rmode], 0,
                                        name(std::array{"N", "P", "M", "Z"}[rmode], 0)});
                }
                variants.push_back({0x1E240000 | base, ftype == 1, ibits, u == 1,
                                    RoundingMode::ToNearest_TieAwayFromZero, 0, name("A", 0)});
                for (u32 fbits : {1u, 8u, sf ? 63u : 32u}) {
                    variants.push_back({0x1E180000 | base | ((64 - fbits) << 10), ftype == 1,
                                        ibits, u == 1, RoundingMode::TowardsZero, fbits,
                                        name("Z", fbits)});
                }
            }
        }
    }
    return variants;
}

// Values around each range's ends, values that round across zero, and the special values.
std::vector<double> TestValues() {
    std::vector<double> values{0.0, 0.4, 0.5, 1.5, 2.5, 1234.567, 1e30, 1e300};
    for (int bits : {15, 16, 31, 32, 53, 63, 64}) {
        const double edge = std::ldexp(1.0, bits);
        for (double delta : {-1.5, -1.0, -0.5, 0.0, 0.5, 1.0}) {
            values.push_back(edge + delta);
        }
        values.push_back(std::nextafter(edge, 0.0));
        values.push_back(std::nextafter(edge, 2 * edge));
    }
    const auto positive = values;
    for (const double value : positive) {
        values.push_back(-value);
    }
    for (const double value : {0.4, 0.5, 0.6, 1.0}) {
        values.push_back(-value / 256); // the fbits = 8 cases
    }
    values.push_back(std::numeric_limits<double>::infinity());
    values.push_back(-std::numeric_limits<double>::infinity());
    return values;
}

std::vector<u64> Inputs(bool double_input) {
    std::vector<u64> inputs;
    for (const double value : TestValues()) {
        inputs.push_back(double_input ? std::bit_cast<u64>(value)
                                      : u64{std::bit_cast<u32>(static_cast<float>(value))});
    }
    if (double_input) {
        inputs.insert(inputs.end(), {0x0000000000000001, 0x8000000000000001, 0x7FF8000000000000,
                                     0xFFF8000000000000, 0x7FF0000000000001});
    } else {
        inputs.insert(inputs.end(), {0x00000001, 0x80000001, 0x7FC00000, 0xFFC00000, 0x7F800001});
    }
    return inputs;
}

} // namespace

TEST_CASE("The JIT converts single and double values as FP::FPToFixed does", "[jit][fp_to_fixed]") {
    // Each variant's result and FPSR are compared with the model's. The SSE path for single and
    // double inputs saturated without raising Invalid Operation and skipped Inexact for values
    // it clamped to zero.
    std::vector<std::string> mismatches;
    size_t checked = 0;
    for (const auto& variant : AllVariants()) {
        Environment env;
        env.code = {variant.instruction, Svc};
        Dynarmic::A64::UserConfig config;
        config.callbacks = &env;
        config.enable_cycle_counting = false;
        Dynarmic::A64::Jit jit{config};
        env.jit = &jit;
        for (const u64 input : Inputs(variant.double_input)) {
            jit.SetVector(1, {input, 0});
            jit.SetRegister(0, ~u64{0});
            jit.SetFpcr(0);
            jit.SetFpsr(0);
            jit.SetPC(CodeBase);
            jit.Run();
            FPSR fpsr;
            const u64 expected =
                variant.double_input
                    ? Dynarmic::FP::FPToFixed<u64>(variant.ibits, input, variant.fbits,
                                                   variant.unsigned_, FPCR{}, variant.mode, fpsr)
                    : Dynarmic::FP::FPToFixed<u32>(variant.ibits, static_cast<u32>(input),
                                                   variant.fbits, variant.unsigned_, FPCR{},
                                                   variant.mode, fpsr);
            ++checked;
            if (jit.GetRegister(0) != expected || jit.GetFpsr() != fpsr.Value()) {
                mismatches.push_back(fmt::format("{} of {:#x}: {:#x} FPSR {:#x}, expected {:#x} "
                                                 "FPSR {:#x}",
                                                 variant.name, input, jit.GetRegister(0),
                                                 jit.GetFpsr(), expected, fpsr.Value()));
            }
        }
    }
    INFO(checked << " conversions, " << mismatches.size() << " differ");
    for (size_t i = 0; i < std::min<size_t>(mismatches.size(), 40); ++i) {
        UNSCOPED_INFO(mismatches[i]);
    }
    CHECK(mismatches.empty());
}

namespace {

/// Guest memory for the benchmark's input arrays, read through fastmem.
class BenchArena {
public:
    static constexpr u32 AddressSpaceBits = 22;
    static constexpr size_t Size = size_t{1} << AddressSpaceBits;
    BenchArena() {
#ifdef _WIN32
        base = static_cast<u8*>(VirtualAlloc(nullptr, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
        void* const mapping =
            mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base = mapping == MAP_FAILED ? nullptr : static_cast<u8*>(mapping);
#endif
        REQUIRE(base != nullptr);
    }
    ~BenchArena() {
#ifdef _WIN32
        VirtualFree(base, 0, MEM_RELEASE);
#else
        munmap(base, Size);
#endif
    }
    BenchArena(const BenchArena&) = delete;
    BenchArena& operator=(const BenchArena&) = delete;
    u8* base{};
};

class BenchEnvironment final : public Dynarmic::A64::UserCallbacks {
public:
    Dynarmic::A64::Jit* jit{};
    std::vector<u32> code;
    u8* memory{};

    std::optional<u32> MemoryReadCode(u64 address) override {
        const u64 index = (address - CodeBase) / 4;
        if (address < CodeBase || index >= code.size()) {
            return std::nullopt;
        }
        return code[index];
    }
    u64 MemoryRead(u64 address, size_t size) override {
        u64 value{};
        std::memcpy(&value, memory + address, size);
        return value;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        return {};
    }
    void MemoryWrite(u64, u64, size_t) override {}
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {}
    bool MemoryWriteExclusive64(u64, u64, u64) override {
        return false;
    }
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override {}
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

} // namespace

TEST_CASE("FPToFixed JIT microbenchmark", "[.jit_fp_to_fixed_benchmark]") {
    // A guest loop: LDR <S|D>1, [X3], #size; the conversion (or a NOP); SUBS X2; B.NE. Each
    // row reads an array of 64 Ki values, 200 times, and prints the best of 5 runs in ns per
    // iteration. "mix" has 1 value in 16 a denormal and 1 in 16 a NaN.
    constexpr u32 Elements = 1 << 16;
    constexpr u32 Passes = 200;
    constexpr u64 ArrayBase = 0x100000;
    constexpr u32 Nop = 0xD503201F;
    constexpr u32 SubsX2 = 0xF1000442;
    constexpr u32 BneBack3 = 0x54FFFFA1;
    BenchArena arena;
    struct Row {
        const char* name;
        u32 instruction;
        bool double_input;
    };
    const std::array rows{
        Row{"NOP (S)", Nop, false},           Row{"FCVTZU W0, S1", 0x1E390020, false},
        Row{"FCVTNU W0, S1", 0x1E210020, false}, Row{"FCVTZS W0, S1", 0x1E380020, false},
        Row{"NOP (D)", Nop, true},            Row{"FCVTZU X0, D1", 0x9E790020, true},
        Row{"FCVTNU X0, D1", 0x9E610020, true},
    };
    enum class Fill { Normal, Mix, Negative, NaN };
    for (const Fill fill : {Fill::Normal, Fill::Mix, Fill::Negative, Fill::NaN}) {
        for (const auto& row : rows) {
            const u32 size = row.double_input ? 8 : 4;
            for (u32 i = 0; i < Elements; ++i) {
                double value = 1000.0 + (i % 977) * 0.37;
                if (fill == Fill::Negative) {
                    value = -0.25 - (i % 7) * 0.1;
                }
                u64 bits = row.double_input ? std::bit_cast<u64>(value)
                                            : std::bit_cast<u32>(static_cast<float>(value));
                if ((fill == Fill::Mix && i % 16 == 3) || fill == Fill::NaN) {
                    bits = row.double_input ? 0x7FF8000000000000 : 0x7FC00000;
                } else if (fill == Fill::Mix && i % 16 == 11) {
                    bits = row.double_input ? 0x000FFFFFFFFFFFFF : 0x007FFFFF;
                }
                std::memcpy(arena.base + ArrayBase + u64{i} * size, &bits, size);
            }
            BenchEnvironment env;
            env.memory = arena.base;
            const u32 load = (row.double_input ? 0xFC400461u : 0xBC400461u) | (size << 12);
            env.code = {load, row.instruction, SubsX2, BneBack3, Svc};
            Dynarmic::A64::UserConfig config;
            config.callbacks = &env;
            config.enable_cycle_counting = false;
            config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(arena.base);
            config.fastmem_address_space_bits = BenchArena::AddressSpaceBits;
            config.silently_mirror_fastmem = false;
            Dynarmic::A64::Jit jit{config};
            env.jit = &jit;
            double best = 1e30;
            for (int run = 0; run < 5; ++run) {
                const auto start = std::chrono::steady_clock::now();
                for (u32 pass = 0; pass < Passes; ++pass) {
                    jit.SetRegister(2, Elements);
                    jit.SetRegister(3, ArrayBase);
                    jit.SetFpcr(0);
                    jit.SetPC(CodeBase);
                    jit.Run();
                }
                const std::chrono::duration<double, std::nano> elapsed =
                    std::chrono::steady_clock::now() - start;
                best = std::min(best, elapsed.count() / (double(Elements) * Passes));
            }
            std::printf("%-9s %-16s %.3f ns/iteration\n",
                        std::array{"normal", "mix", "negative", "nan"}[static_cast<int>(fill)],
                        row.name, best);
        }
    }
}
