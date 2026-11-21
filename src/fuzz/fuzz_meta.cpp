// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The metadata readers that take a whole file: NACP, NPDM (main.npdm), CNMT, KIP1 and INI1.
// Run: build-fuzz/bin/fuzz_meta -max_len=32768 -rss_limit_mb=2048 -malloc_limit_mb=2048 corpus/meta
// Seed: FUZZ_META_SEED=corpus/meta build-fuzz/bin/fuzz_meta writes a made-up KIP1 with a layout
// the kernel accepts, an INI1 holding two of them and an NPDM; from an empty corpus the fuzzer
// never got past their magics to the layout checks.
// A KIP header can still ask for a large zero-filled .text, .rodata or .data with no stored bytes;
// those show up as out-of-memory reports, not crashes.

#include <cstring>
#include <memory>
#include <vector>

#include "core/file_sys/control_metadata.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/program_metadata.h"
#include "fuzz/fuzz_common.h"
#include "fuzz/fuzz_seeds.h"

namespace {

using Fuzz::Seeds::Bytes;

void Put32(Bytes& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

// A 64-bit KIP whose .text, .rodata and .data are 0x10 stored bytes each, on pages of their own,
// with .bss after them: the layout the kernel accepts.
Bytes Kip(char tag) {
    Bytes bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    std::memcpy(bytes.data() + 4, "SeedKip", 7);
    bytes[0x1F] = 0x8 | 0x10; // 64-bit, 39-bit address space
    const u32 sections[4][3] = {{0, 0x10, 0x10}, {0x1000, 0x10, 0x10}, {0x2000, 0x10, 0x10},
                                {0x3000, 0x1000, 0}};
    for (std::size_t i = 0; i < 4; ++i) {
        Put32(bytes, 0x20 + i * 0x10, sections[i][0]);
        Put32(bytes, 0x24 + i * 0x10, sections[i][1]);
        Put32(bytes, 0x28 + i * 0x10, sections[i][2]);
    }
    for (int i = 0; i < 3; ++i) {
        bytes.insert(bytes.end(), 0x10, static_cast<u8>(tag + i));
    }
    return bytes;
}

Bytes Ini() {
    const Bytes first = Kip('a');
    const Bytes second = Kip('x');
    Bytes bytes(0x10);
    Put32(bytes, 0, Common::MakeMagic('I', 'N', 'I', '1'));
    Put32(bytes, 4, static_cast<u32>(0x10 + first.size() + second.size()));
    Put32(bytes, 8, 2);
    bytes.insert(bytes.end(), first.begin(), first.end());
    bytes.insert(bytes.end(), second.begin(), second.end());
    return bytes;
}

// META, then ACI0 (its file access header and one kernel capability), then ACID (its file access
// control): what ProgramMetadata::Load reads.
Bytes Npdm() {
    constexpr u32 AciOffset = 0x80, AciSize = 0x40 + 0x1C + 4;
    constexpr u32 AcidOffset = 0x100, AcidSize = 0x240 + 0x2C;
    Bytes bytes(AcidOffset + AcidSize);
    Put32(bytes, 0, Common::MakeMagic('M', 'E', 'T', 'A'));
    bytes[0xC] = 0x1 | (3 << 1); // 64-bit instructions, 39-bit address space (Is39Bit)
    bytes[0xE] = 0x2C;           // main thread priority
    Put32(bytes, 0x1C, 0x100000); // main thread stack size
    Put32(bytes, 0x70, AciOffset);
    Put32(bytes, 0x74, AciSize);
    Put32(bytes, 0x78, AcidOffset);
    Put32(bytes, 0x7C, AcidSize);
    Put32(bytes, AciOffset, Common::MakeMagic('A', 'C', 'I', '0'));
    Put32(bytes, AciOffset + 0x20, 0x40); // file access header, after the ACI0 header
    Put32(bytes, AciOffset + 0x24, 0x1C);
    Put32(bytes, AciOffset + 0x30, 0x40 + 0x1C); // kernel capabilities: one thread info
    Put32(bytes, AciOffset + 0x34, 4);
    Put32(bytes, AciOffset + 0x40 + 0x1C, 0x30043F7);
    Put32(bytes, AcidOffset + 0x200, Common::MakeMagic('A', 'C', 'I', 'D'));
    Put32(bytes, AcidOffset + 0x220, 0x240); // file access control, after the ACID header
    Put32(bytes, AcidOffset + 0x224, 0x2C);
    return bytes;
}

} // Anonymous namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    Fuzz::Seeds::WriteIfAsked("FUZZ_META_SEED",
                              {{"kip.kip", Kip('a')}, {"ini.ini", Ini()}, {"npdm.npdm", Npdm()}});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    auto file =
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data, data + size), "fuzz.bin");
    {
        const FileSys::NACP nacp{file};
        (void)nacp.GetApplicationName();
        (void)nacp.GetDeveloperName();
        (void)nacp.GetVersionString();
        (void)nacp.GetSupportedLanguages();
    }
    {
        FileSys::ProgramMetadata npdm;
        (void)npdm.Load(file);
    }
    {
        const FileSys::CNMT cnmt{file};
        (void)cnmt.GetTitleID();
    }
    { const FileSys::KIP kip{file}; }
    { const FileSys::INI ini{file}; }
    return 0;
}
