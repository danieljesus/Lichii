// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// An NRO through the checks of its layout (ValidateNroLayout) and the loader's reading of its
// assets (icon, NACP, RomFS), as the game list does. Loading it into a process is not covered:
// that needs a kernel per input, and every size the load uses is the layout's.
// Run: build-fuzz/bin/fuzz_nro -max_len=65536 -rss_limit_mb=2048 -malloc_limit_mb=2048 corpus/nro

#include <memory>
#include <string>
#include <vector>

#include "core/file_sys/control_metadata.h"
#include "core/loader/nro.h"
#include "fuzz/fuzz_common.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    const std::vector<u8> bytes(data, data + size);
    std::string_view reason;
    if (const auto layout = Loader::ValidateNroLayout(bytes, &reason)) {
        // Every extent the loader copies into or protects lies in the image. An empty one, which
        // the layout exempts from its rules, keeps the header's offset and holds nothing.
        for (const auto& segment : layout->segments) {
            if (segment.size != 0 && segment.offset + segment.size > layout->image_size) {
                __builtin_trap();
            }
        }
        if (layout->file_size > size || layout->image_size > 0xFFFFFFFFull) {
            __builtin_trap();
        }
    }
    auto file = std::make_shared<FileSys::VectorVfsFile>(bytes, "fuzz.nro");
    Loader::AppLoader_NRO loader{file};
    (void)Loader::AppLoader_NRO::IdentifyType(file);
    std::vector<u8> icon;
    (void)loader.ReadIcon(icon);
    std::string title;
    (void)loader.ReadTitle(title);
    u64 program_id{};
    (void)loader.ReadProgramId(program_id);
    FileSys::NACP nacp;
    (void)loader.ReadControlData(nacp);
    FileSys::VirtualFile romfs;
    if (loader.ReadRomFS(romfs) == Loader::ResultStatus::Success && romfs) {
        Fuzz::Budget budget;
        Fuzz::ReadAll(romfs, budget);
    }
    return 0;
}
