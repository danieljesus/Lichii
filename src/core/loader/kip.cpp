// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <climits>
#include <cstring>
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/random.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/program_metadata.h"
#include "core/core.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/kip.h"
#include "core/memory.h"

namespace Loader {

namespace {
// 64-bit: a 32-bit size from the header near 4 GiB would wrap.
constexpr u64 PageAlignSize(u64 size) {
    return (size + Core::Memory::LICHII_PAGEMASK) & ~Core::Memory::LICHII_PAGEMASK;
}
} // Anonymous namespace

AppLoader_KIP::AppLoader_KIP(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), kip(std::make_unique<FileSys::KIP>(file)) {}

AppLoader_KIP::~AppLoader_KIP() = default;

FileType AppLoader_KIP::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32_le magic{};
    if (in_file->GetSize() < sizeof(u32) || in_file->ReadObject(&magic) != sizeof(u32)) {
        return FileType::Error;
    }

    if (magic == Common::MakeMagic('K', 'I', 'P', '1')) {
        return FileType::KIP;
    }

    return FileType::Error;
}

FileType AppLoader_KIP::GetFileType() const {
    return (kip != nullptr && kip->GetStatus() == ResultStatus::Success) ? FileType::KIP
                                                                         : FileType::Error;
}

AppLoader::LoadResult AppLoader_KIP::Load(Kernel::KProcess& process,
                                          [[maybe_unused]] Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (kip == nullptr) {
        return {ResultStatus::ErrorNullFile, {}};
    }

    if (kip->GetStatus() != ResultStatus::Success) {
        return {kip->GetStatus(), {}};
    }

    const auto get_kip_address_space_type = [](const auto& kip_type) {
        return kip_type.Is64Bit()
                   ? (kip_type.Is39BitAddressSpace() ? FileSys::ProgramAddressSpaceType::Is39Bit
                                                     : FileSys::ProgramAddressSpaceType::Is36Bit)
                   : FileSys::ProgramAddressSpaceType::Is32Bit;
    };

    const auto address_space = get_kip_address_space_type(*kip);

    FileSys::ProgramMetadata metadata;
    metadata.LoadManual(kip->Is64Bit(), address_space, kip->GetMainThreadPriority(),
                        kip->GetMainThreadCpuCore(), kip->GetMainThreadStackSize(),
                        kip->GetTitleID(), 0xFFFFFFFFFFFFFFFF, 0x1FE00000,
                        kip->GetKernelCapabilities());

    // The KIP's constructor checked the layout in 64-bit arithmetic: .text at 0, .rodata, .data
    // and .bss after it in order, each section's bytes no more than its size, all inside
    // GetImageSize(). The loader used to size and fill the code set from the header's 32-bit
    // values without checking them, so a damaged or crafted .kip could make it copy outside the
    // buffer when launched. What is left to check is that the process can hold the image, before
    // anything is allocated: the code region counts pages in an s32, and the image cannot be
    // larger than the memory of the pool the process is created in.
    const u64 image_size = kip->GetImageSize();
    const auto pool = static_cast<Kernel::KMemoryManager::Pool>(
        FileSys::ProgramMetadata::GetDefault().GetPoolPartition());
    if (image_size == 0 || image_size / Core::Memory::LICHII_PAGESIZE > INT32_MAX ||
        image_size > system.Kernel().MemoryManager().GetSize(pool)) {
        LOG_ERROR(Loader, "KIP {} needs a {:#x} byte image, which the process cannot hold",
                  kip->GetName(), image_size);
        return {ResultStatus::ErrorBadKIPHeader, {}};
    }

    Kernel::CodeSet codeset;
    codeset.memory.resize(static_cast<std::size_t>(image_size));
    // Each segment spans its section's size from the header; the bytes the KIP holds for it may
    // be fewer, and the rest stays zero.
    const auto load_segment = [&codeset](Kernel::CodeSet::Segment& segment,
                                         std::span<const u8> data, u64 offset, u64 size) {
        ASSERT(data.size() <= size && offset + size <= codeset.memory.size());
        segment.addr = offset;
        segment.offset = offset;
        segment.size = static_cast<u32>(size);
        std::memcpy(codeset.memory.data() + offset, data.data(), data.size());
    };
    const u64 rx = kip->GetTextOffset();
    const u64 ro = kip->GetRODataOffset();
    const u64 rw = kip->GetDataOffset();
    load_segment(codeset.CodeSegment(), kip->GetTextSection(), rx,
                 PageAlignSize(kip->GetSectionSize(0)));
    load_segment(codeset.RODataSegment(), kip->GetRODataSection(), ro,
                 PageAlignSize(kip->GetSectionSize(1)));
    // .data and .bss share the last segment, to the end of the image.
    load_segment(codeset.DataSegment(), kip->GetDataSection(), rw, image_size - rw);

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), FileSys::ProgramMetadata::GetDefault(), codeset.memory.size(), 0, aslr_offset).IsError()) {
        return {ResultStatus::ErrorNotInitialized, {}};
    }
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    process.LoadModule(system.Kernel(), std::move(codeset), base_address);

    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", kip->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success,
            LoadParameters{kip->GetMainThreadPriority(), kip->GetMainThreadStackSize()}};
}

} // namespace Loader
