// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The cheat text parser (the .txt files in a mod's cheats folder), and the cheat VM that decodes
// and runs what it parses against a made-up process whose memory reads as zero.
// Run: build-fuzz/bin/fuzz_cheats -max_len=4096 corpus/cheats
// Seed: FUZZ_CHEATS_SEED=corpus/cheats build-fuzz/bin/fuzz_cheats writes made-up cheat files,
// one per opcode family, written by hand from the opcode formats (no game's cheats).

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/memory/cheat_engine.h"
#include "core/memory/dmnt_cheat_vm.h"
#include "fuzz/fuzz_common.h"
#include "fuzz/fuzz_seeds.h"

namespace {

// Ends a run after this many callbacks: a cheat loop can count to 2^64. The VM logs about 35 lines
// per instruction through CommandLog, so this is some thousands of instructions.
constexpr u64 CallBudget = 1 << 17;

struct Budget {};

class Callbacks final : public Core::Memory::DmntCheatVm::Callbacks {
public:
    void MemoryReadUnsafe(VAddr, void* data, u64 size) override {
        Spend();
        std::memset(data, 0, static_cast<std::size_t>(size));
    }
    void MemoryWriteUnsafe(VAddr, const void*, u64) override {
        Spend();
    }
    u64 HidKeysDown() override {
        Spend();
        return ~u64{0};
    }
    void PauseProcess() override {}
    void ResumeProcess() override {}
    void DebugLog(u8, u64) override {}
    void CommandLog(std::string_view) override {
        Spend();
    }

private:
    void Spend() {
        if (++calls > CallBudget) {
            throw Budget{};
        }
    }
    u64 calls{};
};

Fuzz::Seeds::Bytes Text(std::string_view text) {
    return {text.begin(), text.end()};
}

} // Anonymous namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    // Each a master code and one or two cheats in the dmnt text format: [name] or {name} and
    // whitespace-separated 8-digit hex words.
    Fuzz::Seeds::WriteIfAsked(
        "FUZZ_CHEATS_SEED",
        {
            {"store_static.txt", Text("{Master}\n04000000 00001000 00000001\n\n[Store]\n"
                                      "08000000 00002000 11112222 33334444\n")},
            {"conditional.txt", Text("[If]\n14010000 00003000 00000005\n"
                                     "02000000 00003004 00000007\n20000000\n")},
            {"loop.txt", Text("[Loop]\n40000000 00000000 00000004\n30000000 00000004\n"
                              "64010000 00000000 00000009\n31000000\n")},
            {"registers.txt", Text("[Registers]\n58010000 00004000\n72010000 00000003\n"
                                   "94110300 00000002\nA4110000\n")},
            {"keys.txt", Text("[Keys]\n80000010\n04000000 00005000 00000001\n20000000\n")},
            {"register_conditional.txt",
             Text("[RegCond]\nC0110400 00000002\nC1000000\nC2000003\nC3000000\n20000000\n")},
            {"process.txt", Text("[Process]\nFF000000\nFF100000\nFFF00000\n")},
        });
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    // A copy exactly as long as the input, as the cheat file is read: a read past the text is
    // then outside the allocation.
    const std::vector<char> text(data, data + size);
    const Core::Memory::TextCheatParser parser;
    auto cheats = parser.Parse(std::string_view{text.data(), text.size()});
    for (auto& cheat : cheats) {
        cheat.enabled = true;
    }
    (void)Core::Memory::DmntCheatVm::MainStaticStores(cheats);

    Core::Memory::DmntCheatVm vm{std::make_unique<Callbacks>()};
    if (!vm.LoadProgram(cheats)) {
        return 0;
    }
    Core::Memory::CheatProcessMetadata metadata{};
    metadata.main_nso_extents = {0x8000000, 0x1000000};
    metadata.heap_extents = {0x10000000, 0x1000000};
    metadata.alias_extents = {0x20000000, 0x1000000};
    metadata.aslr_extents = {0x8000000, 0x40000000};
    try {
        // Twice: the second run starts from the state the first left.
        vm.Execute(metadata);
        vm.Execute(metadata);
    } catch (const Budget&) {
    }
    return 0;
}
