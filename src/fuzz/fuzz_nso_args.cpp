// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// An NSO through AppLoader_NSO::LoadModule with program arguments of a length the input chooses,
// so that the argument block, sized from them and copied after the segments, is exercised along
// with the NSO's own segments. The first two bytes, little endian, are the arguments' length; the
// rest is the NSO. The module is laid out but not mapped into a process, as when its size is first
// worked out; one kernel serves every input.
// Run: build-fuzz/bin/fuzz_nso_args -max_len=65536 -rss_limit_mb=6144 -malloc_limit_mb=4096 \
//          corpus/nso_args
// The malloc limit sits above the largest pool an image may reserve; RSS is the ~1.9 GB base
// plus a 3 GB image plus margin. libFuzzer's fork driver exits at the first child OOM despite
// -ignore_ooms=1, so a run under lower limits ends in seconds.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/nso.h"
#include "fuzz/fuzz_common.h"

namespace {

struct KernelFixture {
    KernelFixture() {
        const auto home = Fuzz::UseScratchHome("fuzz_nso_args");
        // The system's NAND and SD folders, which it creates and writes, go there too.
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, home / "nand");
        Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, home / "sdmc");
        Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
        Settings::values.use_multi_core.SetValue(false);
        system.Initialize();
        bootstrap.emplace(system.Kernel());
        if (::Kernel::KThread::InitializeDummyThread(system, &*bootstrap, nullptr).IsError()) {
            std::abort();
        }
        system.Kernel().SetCurrentEmuThread(&*bootstrap);
        system.Kernel().Initialize();
    }
    Core::System system;
    std::optional<::Kernel::KThread> bootstrap;
};

KernelFixture* kernel{};

} // Anonymous namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    kernel = new KernelFixture{};
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    const std::size_t argument_size = data[0] | (std::size_t{data[1]} << 8);
    Settings::values.program_args.SetValue(std::string(argument_size, 'a'));
    auto file = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data + 2, data + size),
                                                         "main");
    ::Kernel::KProcess process{kernel->system.Kernel()};
    ::Kernel::KAutoObject::Create(&process);
    (void)Loader::AppLoader_NSO::LoadModule(process, kernel->system, *file, 0x8000000, true,
                                             false);
    kernel->system.Kernel().UnregisterKernelObject(&process);
    return 0;
}
